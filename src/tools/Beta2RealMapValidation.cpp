#include "core/ArchiveCompressionService.h"
#include "core/DecorationMapCopyService.h"
#include "core/DecorationStreamingPlanner.h"
#include "core/MapRegionRepository.h"
#include "core/Sc2Archive.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>

#include <pugixml.hpp>

#include <algorithm>
#include <functional>

namespace
{

using sc2dh::decor::DecorOptimizedMapRequest;
using sc2dh::decor::DecorOptimizedMapResult;
using sc2dh::decor::DecorationMapCopyService;
using sc2dh::decor::DecorationStreamingPlanner;
using sc2dh::decor::DecorZone;
using sc2dh::region::MapRegionRepository;
using sc2dh::region::RegionReadResult;
using sc2dh::compression::ArchiveCompressionRequest;
using sc2dh::compression::ArchiveCompressionResult;
using sc2dh::compression::ArchiveCompressionService;

constexpr qint64 MaxDiagnosticEntryBytes = 1024ll * 1024ll * 1024ll;

QString normalizedPath(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool isSc2Document(const QFileInfo &info)
{
    const QString suffix = info.suffix();
    return info.isFile()
        && (suffix.compare(QStringLiteral("SC2Map"), Qt::CaseInsensitive) == 0
            || suffix.compare(QStringLiteral("SC2Mod"), Qt::CaseInsensitive) == 0);
}

bool fileSha256(const QString &path, QByteArray *digest, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Unable to read %1 for SHA-256: %2").arg(path, file.errorString());
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(4 * 1024 * 1024);
        if (chunk.isEmpty() && file.error() != QFile::NoError) {
            if (error)
                *error = QStringLiteral("SHA-256 read failed for %1: %2").arg(path, file.errorString());
            return false;
        }
        hash.addData(chunk);
    }
    if (digest)
        *digest = hash.result();
    return true;
}

QString shaHex(const QByteArray &digest)
{
    return QString::fromLatin1(digest.toHex());
}

QString safeSlug(QString name)
{
    name = QFileInfo(name).completeBaseName().toCaseFolded();
    QString slug;
    bool separator = false;
    for (const QChar ch : name) {
        if (ch.isLetterOrNumber()) {
            if (separator && !slug.isEmpty())
                slug += QLatin1Char('-');
            slug += ch;
            separator = false;
        } else {
            separator = true;
        }
        if (slug.size() >= 48)
            break;
    }
    return slug.isEmpty() ? QStringLiteral("sc2-document") : slug;
}

bool writeJson(const QString &path, const QJsonObject &object, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error)
            *error = QStringLiteral("Unable to create report directory for %1").arg(path);
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Unable to write %1: %2").arg(path, file.errorString());
        return false;
    }
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = QStringLiteral("Unable to commit %1").arg(path);
        return false;
    }
    return true;
}

QJsonArray stringsJson(const QStringList &values)
{
    QJsonArray result;
    for (const QString &value : values)
        result.append(value);
    return result;
}

QStringList discoverDocuments(const QString &root)
{
    QStringList paths;
    QDirIterator iterator(root,
                          QDir::Files | QDir::Readable | QDir::NoSymLinks,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QFileInfo info(iterator.next());
        if (isSc2Document(info))
            paths << normalizedPath(info.absoluteFilePath());
    }
    paths.removeDuplicates();
    paths.sort(Qt::CaseInsensitive);
    return paths;
}

QString normalizeDependency(QString value)
{
    value = value.trimmed().replace(QLatin1Char('\\'), QLatin1Char('/'));
    const int fileIndex = value.indexOf(QStringLiteral("file:"), 0, Qt::CaseInsensitive);
    if (fileIndex >= 0)
        value = value.mid(fileIndex + 5);
    while (value.startsWith(QLatin1Char('/')))
        value.remove(0, 1);
    const int query = value.indexOf(QLatin1Char('?'));
    if (query >= 0)
        value.truncate(query);
    return value.trimmed().toCaseFolded();
}

QStringList parseDependencies(const QByteArray &documentInfo, QStringList *errors)
{
    QStringList dependencies;
    pugi::xml_document document;
    const pugi::xml_parse_result parsed = document.load_buffer(
        documentInfo.constData(), size_t(documentInfo.size()), pugi::parse_default, pugi::encoding_utf8);
    if (!parsed) {
        if (errors) {
            *errors << QStringLiteral("DocumentInfo parse error at %1: %2")
                           .arg(parsed.offset)
                           .arg(QString::fromUtf8(parsed.description()));
        }
        return dependencies;
    }

    std::function<void(pugi::xml_node)> visit = [&](pugi::xml_node node) {
        for (pugi::xml_attribute attribute : node.attributes()) {
            const QString raw = QString::fromUtf8(attribute.value());
            if (!raw.contains(QStringLiteral("file:"), Qt::CaseInsensitive)
                && !raw.contains(QStringLiteral(".sc2mod"), Qt::CaseInsensitive)
                && !raw.contains(QStringLiteral(".sc2campaign"), Qt::CaseInsensitive)) {
                continue;
            }
            const QString dependency = normalizeDependency(raw);
            if (!dependency.isEmpty())
                dependencies << dependency;
        }
        for (pugi::xml_node child : node.children())
            visit(child);
    };
    visit(document.document_element());
    dependencies.removeDuplicates();
    dependencies.sort(Qt::CaseInsensitive);
    return dependencies;
}

bool isKnownGameDependency(const QString &dependency)
{
    static const QStringList prefixes{
        QStringLiteral("mods/core.sc2mod"),
        QStringLiteral("mods/liberty.sc2mod"),
        QStringLiteral("mods/swarm.sc2mod"),
        QStringLiteral("mods/void.sc2mod"),
        QStringLiteral("mods/libertymulti.sc2mod"),
        QStringLiteral("mods/swarmmulti.sc2mod"),
        QStringLiteral("mods/voidmulti.sc2mod"),
        QStringLiteral("mods/balancemulti.sc2mod"),
        QStringLiteral("mods/starcoop/starcoop.sc2mod"),
        QStringLiteral("mods/novastoryassets.sc2mod"),
        QStringLiteral("campaigns/liberty.sc2campaign"),
        QStringLiteral("campaigns/swarm.sc2campaign"),
        QStringLiteral("campaigns/void.sc2campaign"),
        QStringLiteral("campaigns/libertystory.sc2campaign"),
        QStringLiteral("campaigns/swarmstory.sc2campaign"),
        QStringLiteral("campaigns/voidstory.sc2campaign")
    };
    return std::any_of(prefixes.cbegin(), prefixes.cend(), [&](const QString &known) {
        return dependency == known || dependency.startsWith(known + QLatin1Char('/'));
    });
}

struct DependencyResolution
{
    QStringList resolved;
    QStringList missing;
};

DependencyResolution resolveDependencies(const QStringList &dependencies,
                                         const QString &sourcePath,
                                         const QHash<QString, QStringList> &corpusByName,
                                         const QString &gameRoot)
{
    DependencyResolution result;
    const bool gameDataPresent = QFileInfo::exists(QDir(gameRoot).absoluteFilePath(QStringLiteral("Data")));
    for (const QString &dependency : dependencies) {
        const QString baseName = QFileInfo(dependency).fileName().toCaseFolded();
        const QString beside = QDir(QFileInfo(sourcePath).absolutePath()).absoluteFilePath(QFileInfo(dependency).fileName());
        const QString inGame = QDir(gameRoot).absoluteFilePath(dependency);
        if (QFileInfo::exists(beside)) {
            result.resolved << QStringLiteral("%1 => %2").arg(dependency, normalizedPath(beside));
        } else if (QFileInfo::exists(inGame)) {
            result.resolved << QStringLiteral("%1 => %2").arg(dependency, normalizedPath(inGame));
        } else if (corpusByName.contains(baseName) && corpusByName.value(baseName).size() == 1) {
            result.resolved << QStringLiteral("%1 => %2").arg(dependency, corpusByName.value(baseName).first());
        } else if (isKnownGameDependency(dependency) && gameDataPresent) {
            result.resolved << QStringLiteral("%1 => INSTALLED_GAME_CASC").arg(dependency);
        } else {
            result.missing << dependency;
        }
    }
    return result;
}

QVector<DecorZone> zonesFromRegions(const RegionReadResult &regions)
{
    QVector<DecorZone> zones;
    QSet<int> usedIds;
    int generatedId = 1;
    for (const auto &region : regions.regions) {
        if (!region.geometry.supported)
            continue;
        DecorZone zone;
        bool idOk = false;
        zone.id = region.id.toInt(&idOk);
        if (!idOk)
            zone.id = 0;
        if (zone.id <= 0 || usedIds.contains(zone.id)) {
            while (usedIds.contains(generatedId))
                ++generatedId;
            zone.id = generatedId++;
        }
        usedIds.insert(zone.id);
        zone.name = region.name.isEmpty() ? QStringLiteral("Region_%1").arg(zone.id) : region.name;
        zone.xMin = region.geometry.bounds.xMin;
        zone.yMin = region.geometry.bounds.yMin;
        zone.xMax = region.geometry.bounds.xMax;
        zone.yMax = region.geometry.bounds.yMax;
        zone.geometry = region.geometry;
        zones << zone;
    }
    return zones;
}

struct Baseline
{
    bool archiveOpen = false;
    bool complete = false;
    qint64 durationMs = 0;
    qint64 peakWorkingSetBytes = -1;
    QStringList entries;
    QJsonArray entryInventory;
    QStringList parseErrors;
    QStringList warnings;
    QStringList dependencies;
    DependencyResolution dependencyResolution;
    bool hasObjects = false;
    bool hasRegions = false;
    bool hasMapScript = false;
    bool hasMapInfo = false;
    bool hasTerrain = false;
    int gameDataEntries = 0;
    QByteArray objects;
    RegionReadResult regionResult;
};

bool entryEquals(const QString &candidate, const QString &name)
{
    return candidate.trimmed().replace(QLatin1Char('\\'), QLatin1Char('/'))
               .compare(name, Qt::CaseInsensitive) == 0;
}

Baseline analyzeArchive(const QString &path,
                        const QHash<QString, QStringList> &corpusByName,
                        const QString &gameRoot)
{
    Baseline result;
    QElapsedTimer timer;
    timer.start();

    Sc2Archive archive;
    QString error;
    if (!archive.load(path, &error)) {
        result.parseErrors << error;
        result.durationMs = timer.elapsed();
        return result;
    }
    result.archiveOpen = true;
    result.entries = archive.allEntries();
    result.entries.sort(Qt::CaseInsensitive);

    QByteArray documentInfo;
    for (const QString &entry : result.entries) {
        const QString normalized = entry.trimmed().replace(QLatin1Char('\\'), QLatin1Char('/'));
        result.hasObjects = result.hasObjects || entryEquals(normalized, QStringLiteral("Objects"));
        result.hasRegions = result.hasRegions || entryEquals(normalized, QStringLiteral("Regions"));
        result.hasMapScript = result.hasMapScript || entryEquals(normalized, QStringLiteral("MapScript.galaxy"));
        result.hasMapInfo = result.hasMapInfo || entryEquals(normalized, QStringLiteral("MapInfo"));
        result.hasTerrain = result.hasTerrain || entryEquals(normalized, QStringLiteral("t3Terrain"));
        if (normalized.contains(QStringLiteral("GameData/"), Qt::CaseInsensitive)
            && normalized.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)) {
            ++result.gameDataEntries;
        }

        QByteArray bytes;
        QString readError;
        if (!archive.readEntry(entry, &bytes, &readError)) {
            result.parseErrors << QStringLiteral("Unreadable entry %1: %2").arg(entry, readError);
            continue;
        }
        QJsonObject item{
            {QStringLiteral("name"), entry},
            {QStringLiteral("logical_bytes"), double(bytes.size())},
            {QStringLiteral("logical_sha256"), shaHex(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256))}
        };
        result.entryInventory.append(item);

        if (entryEquals(normalized, QStringLiteral("Objects"))) {
            result.objects = bytes;
            pugi::xml_document xml;
            const auto parsed = xml.load_buffer(bytes.constData(), size_t(bytes.size()), pugi::parse_default,
                                                pugi::encoding_utf8);
            if (!parsed)
                result.parseErrors << QStringLiteral("Objects parse error at %1: %2")
                                          .arg(parsed.offset)
                                          .arg(QString::fromUtf8(parsed.description()));
        } else if (entryEquals(normalized, QStringLiteral("Regions"))) {
            result.regionResult = MapRegionRepository().parse(bytes, entry);
            result.parseErrors += result.regionResult.errors;
            result.warnings += result.regionResult.warnings;
        } else if (entryEquals(normalized, QStringLiteral("DocumentInfo"))) {
            documentInfo = bytes;
        } else if (normalized.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)) {
            pugi::xml_document xml;
            const auto parsed = xml.load_buffer(bytes.constData(), size_t(bytes.size()), pugi::parse_default,
                                                pugi::encoding_utf8);
            if (!parsed) {
                result.parseErrors << QStringLiteral("%1 parse error at %2: %3")
                                          .arg(entry)
                                          .arg(parsed.offset)
                                          .arg(QString::fromUtf8(parsed.description()));
            }
        }
        if (bytes.size() > MaxDiagnosticEntryBytes)
            result.warnings << QStringLiteral("Large logical entry read during inventory: %1 (%2 bytes)")
                                   .arg(entry)
                                   .arg(bytes.size());
    }

    if (!documentInfo.isEmpty())
        result.dependencies = parseDependencies(documentInfo, &result.parseErrors);
    else
        result.warnings << QStringLiteral("DocumentInfo is absent or empty; dependency inventory is incomplete.");
    result.dependencyResolution = resolveDependencies(result.dependencies, path, corpusByName, gameRoot);
    result.complete = result.archiveOpen && result.parseErrors.isEmpty()
        && result.dependencyResolution.missing.isEmpty();
    result.durationMs = timer.elapsed();
    // Kept explicit instead of relying on a platform-specific sampling hack.
    // A value of -1 means the measurement was not available in this run.
    result.peakWorkingSetBytes = -1;
    return result;
}

QJsonObject baselineJson(const Baseline &baseline)
{
    return QJsonObject{
        {QStringLiteral("archive_open"), baseline.archiveOpen},
        {QStringLiteral("analysis_complete"), baseline.complete},
        {QStringLiteral("analysis_duration_ms"), double(baseline.durationMs)},
        {QStringLiteral("peak_working_set_bytes"), double(baseline.peakWorkingSetBytes)},
        {QStringLiteral("entries_count"), baseline.entries.size()},
        {QStringLiteral("entries"), baseline.entryInventory},
        {QStringLiteral("objects_detected"), baseline.hasObjects},
        {QStringLiteral("regions_detected"), baseline.hasRegions},
        {QStringLiteral("map_script_detected"), baseline.hasMapScript},
        {QStringLiteral("map_info_detected"), baseline.hasMapInfo},
        {QStringLiteral("terrain_detected"), baseline.hasTerrain},
        {QStringLiteral("game_data_entries"), baseline.gameDataEntries},
        {QStringLiteral("dependencies"), stringsJson(baseline.dependencies)},
        {QStringLiteral("resolved_dependencies"), stringsJson(baseline.dependencyResolution.resolved)},
        {QStringLiteral("missing_dependencies"), stringsJson(baseline.dependencyResolution.missing)},
        {QStringLiteral("dependency_resolution"), baseline.dependencyResolution.missing.isEmpty()
             ? QStringLiteral("RESOLVED") : QStringLiteral("SKIPPED_MISSING_DEPENDENCY")},
        {QStringLiteral("parse_errors"), stringsJson(baseline.parseErrors)},
        {QStringLiteral("warnings"), stringsJson(baseline.warnings)}
    };
}

QJsonArray entryNamesJson(const QHash<QString, QByteArray> &entries)
{
    QStringList names = entries.keys();
    names.sort(Qt::CaseInsensitive);
    return stringsJson(names);
}

QString editorCategory(qint64 bytes)
{
    if (bytes < 8ll * 1024ll * 1024ll)
        return QStringLiteral("small");
    if (bytes < 80ll * 1024ll * 1024ll)
        return QStringLiteral("medium");
    return QStringLiteral("large");
}

QJsonObject validateOne(const QString &sourcePath,
                        bool requiredMission,
                        const QString &outputRoot,
                        const QHash<QString, QStringList> &corpusByName,
                        const QString &gameRoot,
                        bool inventoryOnly)
{
    QJsonObject report;
    const QFileInfo sourceInfo(sourcePath);
    QByteArray sourceHash;
    QString error;
    const bool sourceHashOk = fileSha256(sourcePath, &sourceHash, &error);
    const QString sourceHashHex = sourceHashOk ? shaHex(sourceHash) : QString();
    const QString folderName = safeSlug(sourceInfo.fileName()) + QLatin1Char('-')
        + (sourceHashHex.isEmpty() ? QStringLiteral("unhashed") : sourceHashHex.left(12));
    const QString documentOutput = QDir(outputRoot).absoluteFilePath(QStringLiteral("documents/") + folderName);
    QDir().mkpath(documentOutput);

    report.insert(QStringLiteral("source_path"), normalizedPath(sourcePath));
    report.insert(QStringLiteral("required_mission"), requiredMission);
    report.insert(QStringLiteral("source_sha256"), sourceHashHex);
    report.insert(QStringLiteral("source_bytes"), double(sourceInfo.size()));
    report.insert(QStringLiteral("category"), editorCategory(sourceInfo.size()));
    report.insert(QStringLiteral("complex_name"), sourceInfo.fileName().contains(QRegularExpression(QStringLiteral("[^A-Za-z0-9_. -]"))));
    report.insert(QStringLiteral("document_kind"), sourceInfo.suffix().compare(QStringLiteral("SC2Mod"), Qt::CaseInsensitive) == 0
                      ? QStringLiteral("SC2Mod") : QStringLiteral("SC2Map"));
    report.insert(QStringLiteral("output_folder"), normalizedPath(documentOutput));
    report.insert(QStringLiteral("editor_acceptance"), QStringLiteral("NOT_RUN"));
    report.insert(QStringLiteral("runtime_acceptance"), QStringLiteral("NOT_RUN"));
    report.insert(QStringLiteral("strong_missing_refs_before"), QJsonArray());
    report.insert(QStringLiteral("strong_missing_refs_after"), QJsonArray());

    if (!sourceHashOk) {
        report.insert(QStringLiteral("status"), QStringLiteral("BLOCKED"));
        report.insert(QStringLiteral("errors"), QJsonArray{error});
        writeJson(QDir(documentOutput).absoluteFilePath(QStringLiteral("report.json")), report, nullptr);
        return report;
    }

    const Baseline baseline = analyzeArchive(sourcePath, corpusByName, gameRoot);
    report.insert(QStringLiteral("baseline"), baselineJson(baseline));
    report.insert(QStringLiteral("analysis_complete_before"), baseline.complete);
    report.insert(QStringLiteral("parse_errors_before"), stringsJson(baseline.parseErrors));
    report.insert(QStringLiteral("dependency_resolution"), baseline.dependencyResolution.missing.isEmpty()
                      ? QStringLiteral("RESOLVED") : QStringLiteral("SKIPPED_MISSING_DEPENDENCY"));
    report.insert(QStringLiteral("missing_dependencies"), stringsJson(baseline.dependencyResolution.missing));

    if (!inventoryOnly && baseline.complete) {
        const QString extension = sourceInfo.suffix().isEmpty() ? QStringLiteral("SC2Map") : sourceInfo.suffix();
        const QString compressedPath = QDir(documentOutput).absoluteFilePath(
            sourceInfo.completeBaseName() + QStringLiteral("_BETA2_MAX_COMPRESSED.") + extension);
        ArchiveCompressionRequest compressionRequest;
        compressionRequest.sourceArchivePath = sourcePath;
        compressionRequest.outputArchivePath = compressedPath;
        const ArchiveCompressionResult compression =
            ArchiveCompressionService().compressCompatibleCopy(compressionRequest);
        QJsonObject compressionJson{
            {QStringLiteral("status"), compression.status},
            {QStringLiteral("success"), compression.success},
            {QStringLiteral("source_sha256_before"), shaHex(compression.sourceSha256Before)},
            {QStringLiteral("source_sha256_after"), shaHex(compression.sourceSha256After)},
            {QStringLiteral("output_sha256"), shaHex(compression.outputSha256)},
            {QStringLiteral("output_path"), QFileInfo::exists(compressedPath) ? normalizedPath(compressedPath) : QString()},
            {QStringLiteral("source_bytes"), double(compression.sourceBytes)},
            {QStringLiteral("output_bytes"), double(compression.outputBytes)},
            {QStringLiteral("saved_bytes"), double(compression.savedBytes)},
            {QStringLiteral("saved_percent"), compression.savedPercent},
            {QStringLiteral("predicted_temporary_bytes"), double(compression.predictedTemporaryBytes)},
            {QStringLiteral("available_bytes"), double(compression.availableBytes)},
            {QStringLiteral("entries_verified"), compression.entriesVerified},
            {QStringLiteral("source_unchanged"), compression.sourceUnchanged},
            {QStringLiteral("structural_verification"), compression.structuralVerification},
            {QStringLiteral("logical_entry_equality"), compression.logicalEntryEquality},
            {QStringLiteral("editor_acceptance"), compression.editorAcceptance},
            {QStringLiteral("strategy"), compression.strategy},
            {QStringLiteral("warnings"), stringsJson(compression.warnings)},
            {QStringLiteral("error"), compression.error}
        };
        report.insert(QStringLiteral("maximum_compatible_compression"), compressionJson);
        report.insert(QStringLiteral("compression_output_path"), compressionJson.value(QStringLiteral("output_path")));
    }

    if (!baseline.archiveOpen) {
        report.insert(QStringLiteral("status"), QStringLiteral("BLOCKED_ARCHIVE_OPEN"));
    } else if (!baseline.dependencyResolution.missing.isEmpty()) {
        report.insert(QStringLiteral("status"), QStringLiteral("SKIPPED_MISSING_DEPENDENCY"));
    } else if (!baseline.complete) {
        report.insert(QStringLiteral("status"), QStringLiteral("BLOCKED_INCOMPLETE_ANALYSIS"));
    } else if (inventoryOnly) {
        report.insert(QStringLiteral("status"), QStringLiteral("BASELINE_COMPLETE"));
    } else if (!baseline.hasObjects || !baseline.hasRegions || !baseline.hasMapScript) {
        report.insert(QStringLiteral("status"), QStringLiteral("NO_SAFE_GAIN"));
        report.insert(QStringLiteral("limitations"), QJsonArray{QStringLiteral("Objects, Regions, and MapScript.galaxy are all required for Region decor streaming.")});
    } else if (!baseline.regionResult.complete) {
        report.insert(QStringLiteral("status"), QStringLiteral("BLOCKED_INCOMPLETE_REGIONS"));
        report.insert(QStringLiteral("limitations"), stringsJson(baseline.regionResult.errors + baseline.regionResult.warnings));
    } else {
        const QVector<DecorZone> zones = zonesFromRegions(baseline.regionResult);
        if (zones.isEmpty()) {
            report.insert(QStringLiteral("status"), QStringLiteral("NO_SAFE_GAIN"));
            report.insert(QStringLiteral("limitations"), QJsonArray{QStringLiteral("No supported exact Regions were available.")});
        } else {
            const QString extension = sourceInfo.suffix().isEmpty() ? QStringLiteral("SC2Map") : sourceInfo.suffix();
            const QString outputPath = QDir(documentOutput).absoluteFilePath(
                sourceInfo.completeBaseName() + QStringLiteral("_BETA2_DECOR_OPTIMIZED.") + extension);
            DecorOptimizedMapRequest request;
            request.sourceArchivePath = sourcePath;
            request.outputArchivePath = outputPath;
            request.zones = zones;
            request.galaxyOptions.functionPrefix = QStringLiteral("SC2DH_Beta2Decor");
            request.galaxyOptions.batchLimit = 64;
            request.dryRun = true;
            const DecorOptimizedMapResult dryRun = DecorationMapCopyService().createOptimizedCopy(request);
            QJsonObject dryRunJson{
                {QStringLiteral("success"), dryRun.success},
                {QStringLiteral("selected_regions"), zones.size()},
                {QStringLiteral("objects_selected"), dryRun.removedDoodads},
                {QStringLiteral("entries_rewritten"), entryNamesJson(dryRun.patch.replacementEntries)},
                {QStringLiteral("round_trip_verified"), dryRun.patch.artifacts.roundTripVerified},
                {QStringLiteral("outside_scope_preserved"), dryRun.patch.artifacts.outsideScopePreserved},
                {QStringLiteral("source_unchanged"), dryRun.sourceUnchanged},
                {QStringLiteral("warnings"), stringsJson(dryRun.warnings)},
                {QStringLiteral("error"), dryRun.error}
            };
            report.insert(QStringLiteral("dry_run"), dryRunJson);

            if (!dryRun.success) {
                report.insert(QStringLiteral("status"), QStringLiteral("BLOCKED_DRY_RUN"));
            } else if (dryRun.removedDoodads <= 0) {
                report.insert(QStringLiteral("status"), QStringLiteral("NO_SAFE_GAIN"));
            } else {
                request.dryRun = false;
                request.overwriteExisting = false;
                const DecorOptimizedMapResult applied = DecorationMapCopyService().createOptimizedCopy(request);
                report.insert(QStringLiteral("output_path"), applied.success ? normalizedPath(outputPath) : QString());
                report.insert(QStringLiteral("objects_removed"), applied.removedDoodads);
                report.insert(QStringLiteral("entries_added"), applied.patch.replacementEntries.contains(request.runtimeEntry)
                                  ? QJsonArray{request.runtimeEntry} : QJsonArray());
                report.insert(QStringLiteral("entries_removed"), QJsonArray());
                report.insert(QStringLiteral("entries_rewritten"), entryNamesJson(applied.patch.replacementEntries));
                report.insert(QStringLiteral("galaxy_functions_added"), applied.removedDoodads > 0 ? 5 : 0);
                report.insert(QStringLiteral("source_unchanged"), applied.sourceUnchanged);
                report.insert(QStringLiteral("structural_verification"), applied.fullAnalysisVerified
                                  ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
                report.insert(QStringLiteral("semantic_verification"), applied.fullAnalysisVerified
                                  && applied.patch.artifacts.roundTripVerified
                                  && applied.patch.artifacts.outsideScopePreserved
                                  ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
                report.insert(QStringLiteral("warnings"), stringsJson(applied.warnings));
                if (!applied.success) {
                    report.insert(QStringLiteral("status"), QStringLiteral("BLOCKED_APPLY"));
                    report.insert(QStringLiteral("errors"), QJsonArray{applied.error});
                } else {
                    QByteArray outputHash;
                    QString hashError;
                    const bool outputHashOk = fileSha256(outputPath, &outputHash, &hashError);
                    const QFileInfo outputInfo(outputPath);
                    report.insert(QStringLiteral("output_sha256"), outputHashOk ? shaHex(outputHash) : QString());
                    report.insert(QStringLiteral("output_bytes"), double(outputInfo.size()));
                    report.insert(QStringLiteral("saved_bytes"), double(sourceInfo.size() - outputInfo.size()));
                    report.insert(QStringLiteral("saved_percent"), sourceInfo.size() > 0
                                      ? (double(sourceInfo.size() - outputInfo.size()) * 100.0 / double(sourceInfo.size())) : 0.0);
                    const Baseline after = analyzeArchive(outputPath, corpusByName, gameRoot);
                    report.insert(QStringLiteral("fresh_reanalysis"), baselineJson(after));
                    report.insert(QStringLiteral("analysis_complete_after"), after.complete);
                    report.insert(QStringLiteral("parse_errors_after"), stringsJson(after.parseErrors));
                    report.insert(QStringLiteral("status"), after.complete
                                      ? QStringLiteral("OPTIMIZED_COPY_VERIFIED")
                                      : QStringLiteral("BLOCKED_FRESH_REANALYSIS"));
                    if (!outputHashOk)
                        report.insert(QStringLiteral("errors"), QJsonArray{hashError});
                }
            }
        }
    }

    QByteArray sourceHashAfter;
    QString afterError;
    const bool afterHashOk = fileSha256(sourcePath, &sourceHashAfter, &afterError);
    const bool sourceUnchanged = afterHashOk && sourceHashAfter == sourceHash;
    report.insert(QStringLiteral("source_sha256_after"), afterHashOk ? shaHex(sourceHashAfter) : QString());
    report.insert(QStringLiteral("source_unchanged"), sourceUnchanged);
    if (!sourceUnchanged) {
        report.insert(QStringLiteral("status"), QStringLiteral("BLOCKED_SOURCE_CHANGED"));
        QJsonArray errors = report.value(QStringLiteral("errors")).toArray();
        errors.append(afterHashOk ? QStringLiteral("Source SHA-256 changed during validation.") : afterError);
        report.insert(QStringLiteral("errors"), errors);
    }

    writeJson(QDir(documentOutput).absoluteFilePath(QStringLiteral("report.json")), report, nullptr);
    return report;
}

QString usageHint()
{
    return QStringLiteral(
        "Example:\n"
        "  SC2Beta2RealMapValidation --corpus <folder> --required-map <map> "
        "--output <target/diag/beta2-real-maps>\n");
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("SC2Beta2RealMapValidation"));
    QCoreApplication::setApplicationVersion(QStringLiteral(SC2DH_VERSION_NUMBER));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("StarCraft-II-Data-Helper 3.0 Beta 2 real-map validation harness"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption corpusOption(QStringLiteral("corpus"), QStringLiteral("Recursively scan this corpus."), QStringLiteral("folder"));
    const QCommandLineOption requiredOption(QStringLiteral("required-map"), QStringLiteral("Required oracle map path."), QStringLiteral("map"));
    const QCommandLineOption outputOption(QStringLiteral("output"), QStringLiteral("Diagnostic output root."), QStringLiteral("folder"));
    const QCommandLineOption inventoryOption(QStringLiteral("inventory-only"), QStringLiteral("Create manifest and read-only baselines only."));
    parser.addOptions({corpusOption, requiredOption, outputOption, inventoryOption});
    parser.process(app);

    if (!parser.isSet(corpusOption) || !parser.isSet(requiredOption) || !parser.isSet(outputOption)) {
        QTextStream(stderr) << QStringLiteral("--corpus, --required-map, and --output are required.\n")
                            << usageHint();
        return 2;
    }

    const QString corpus = normalizedPath(parser.value(corpusOption));
    const QString requiredMap = normalizedPath(parser.value(requiredOption));
    const QString outputRoot = normalizedPath(parser.value(outputOption));
    if (!QFileInfo(corpus).isDir()) {
        QTextStream(stderr) << QStringLiteral("Corpus folder does not exist: %1\n").arg(corpus);
        return 3;
    }
    if (!QDir().mkpath(outputRoot)) {
        QTextStream(stderr) << QStringLiteral("Unable to create diagnostic output root: %1\n").arg(outputRoot);
        return 4;
    }

    QStringList corpusPaths = discoverDocuments(corpus);
    QStringList allPaths = corpusPaths;
    const bool requiredExists = QFileInfo(requiredMap).isFile();
    if (requiredExists && !allPaths.contains(requiredMap, Qt::CaseInsensitive))
        allPaths << requiredMap;

    QHash<QString, QStringList> corpusByName;
    for (const QString &path : allPaths)
        corpusByName[QFileInfo(path).fileName().toCaseFolded()] << path;

    QJsonArray manifestItems;
    QStringList manifestErrors;
    for (const QString &path : allPaths) {
        QByteArray hash;
        QString error;
        const bool hashed = fileSha256(path, &hash, &error);
        const QFileInfo info(path);
        manifestItems.append(QJsonObject{
            {QStringLiteral("path"), path},
            {QStringLiteral("bytes"), double(info.size())},
            {QStringLiteral("sha256"), hashed ? shaHex(hash) : QString()},
            {QStringLiteral("last_modified_utc"), info.lastModified().toUTC().toString(Qt::ISODateWithMs)},
            {QStringLiteral("kind"), info.suffix()},
            {QStringLiteral("required_mission"), path.compare(requiredMap, Qt::CaseInsensitive) == 0}
        });
        if (!hashed)
            manifestErrors << error;
    }
    QJsonObject manifest{
        {QStringLiteral("schema"), QStringLiteral("sc2dh.beta2.real-map-manifest.v1")},
        {QStringLiteral("created_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("corpus_root"), corpus},
        {QStringLiteral("corpus_count"), corpusPaths.size()},
        {QStringLiteral("required_map"), requiredMap},
        {QStringLiteral("required_map_exists"), requiredExists},
        {QStringLiteral("documents"), manifestItems},
        {QStringLiteral("errors"), stringsJson(manifestErrors)}
    };
    QString writeError;
    if (!writeJson(QDir(outputRoot).absoluteFilePath(QStringLiteral("manifest.json")), manifest, &writeError)) {
        QTextStream(stderr) << writeError << '\n';
        return 5;
    }

    const QString gameRoot = QStringLiteral("C:/Program Files (x86)/StarCraft II");
    QJsonArray reports;
    QJsonArray editorCandidates;
    int verifiedCopies = 0;
    int unchangedSources = 0;
    int blocked = 0;
    for (int index = 0; index < allPaths.size(); ++index) {
        const QString &path = allPaths.at(index);
        QTextStream(stdout) << QStringLiteral("[%1/%2] %3\n").arg(index + 1).arg(allPaths.size()).arg(path);
        QJsonObject report = validateOne(path,
                                         path.compare(requiredMap, Qt::CaseInsensitive) == 0,
                                         outputRoot,
                                         corpusByName,
                                         gameRoot,
                                         parser.isSet(inventoryOption));
        reports.append(report);
        if (report.value(QStringLiteral("source_unchanged")).toBool())
            ++unchangedSources;
        if (report.value(QStringLiteral("status")).toString() == QStringLiteral("OPTIMIZED_COPY_VERIFIED")) {
            ++verifiedCopies;
            editorCandidates.append(QJsonObject{
                {QStringLiteral("source_path"), report.value(QStringLiteral("source_path"))},
                {QStringLiteral("copy_path"), report.value(QStringLiteral("output_path"))},
                {QStringLiteral("required_mission"), report.value(QStringLiteral("required_mission"))},
                {QStringLiteral("category"), report.value(QStringLiteral("category"))},
                {QStringLiteral("complex_name"), report.value(QStringLiteral("complex_name"))},
                {QStringLiteral("document_kind"), report.value(QStringLiteral("document_kind"))},
                {QStringLiteral("editor_acceptance"), QStringLiteral("NOT_RUN")}
            });
        }
        const QString compressionCopy = report.value(QStringLiteral("compression_output_path")).toString();
        if (!compressionCopy.isEmpty()) {
            editorCandidates.append(QJsonObject{
                {QStringLiteral("source_path"), report.value(QStringLiteral("source_path"))},
                {QStringLiteral("copy_path"), compressionCopy},
                {QStringLiteral("operation"), QStringLiteral("maximum-compatible-compression")},
                {QStringLiteral("required_mission"), report.value(QStringLiteral("required_mission"))},
                {QStringLiteral("category"), report.value(QStringLiteral("category"))},
                {QStringLiteral("complex_name"), report.value(QStringLiteral("complex_name"))},
                {QStringLiteral("document_kind"), report.value(QStringLiteral("document_kind"))},
                {QStringLiteral("editor_acceptance"), QStringLiteral("NOT_RUN")}
            });
        }
        if (report.value(QStringLiteral("status")).toString().startsWith(QStringLiteral("BLOCKED")))
            ++blocked;
    }

    if (!requiredExists) {
        reports.append(QJsonObject{
            {QStringLiteral("source_path"), requiredMap},
            {QStringLiteral("required_mission"), true},
            {QStringLiteral("status"), QStringLiteral("MISSING_TEST_ASSET")},
            {QStringLiteral("editor_acceptance"), QStringLiteral("NOT_RUN")}
        });
    }

    const QJsonObject editorQueue{
        {QStringLiteral("schema"), QStringLiteral("sc2dh.beta2.editor-queue.v1")},
        {QStringLiteral("ownership_rule"), QStringLiteral("Only Editor instances started for copies materialized under this diagnostic root may be controlled.")},
        {QStringLiteral("timeout_seconds"), 200},
        {QStringLiteral("candidates"), editorCandidates}
    };
    writeJson(QDir(outputRoot).absoluteFilePath(QStringLiteral("editor-queue.json")), editorQueue, nullptr);

    const QJsonObject aggregate{
        {QStringLiteral("schema"), QStringLiteral("sc2dh.beta2.real-map-validation.v1")},
        {QStringLiteral("created_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("release_label"), QStringLiteral(SC2DH_VERSION_LABEL)},
        {QStringLiteral("corpus_count"), corpusPaths.size()},
        {QStringLiteral("required_map_exists"), requiredExists},
        {QStringLiteral("documents_processed"), reports.size()},
        {QStringLiteral("verified_optimized_copies"), verifiedCopies},
        {QStringLiteral("sources_proven_unchanged"), unchangedSources},
        {QStringLiteral("blocked_documents"), blocked},
        {QStringLiteral("editor_acceptance"), QStringLiteral("NOT_RUN")},
        {QStringLiteral("reports"), reports}
    };
    if (!writeJson(QDir(outputRoot).absoluteFilePath(QStringLiteral("aggregate-report.json")), aggregate, &writeError)) {
        QTextStream(stderr) << writeError << '\n';
        return 6;
    }

    QTextStream(stdout) << QJsonDocument(QJsonObject{
        {QStringLiteral("manifest"), QDir(outputRoot).absoluteFilePath(QStringLiteral("manifest.json"))},
        {QStringLiteral("aggregate"), QDir(outputRoot).absoluteFilePath(QStringLiteral("aggregate-report.json"))},
        {QStringLiteral("processed"), reports.size()},
        {QStringLiteral("verified_copies"), verifiedCopies},
        {QStringLiteral("blocked"), blocked}
    }).toJson(QJsonDocument::Compact) << '\n';
    return manifestErrors.isEmpty() ? 0 : 7;
}
