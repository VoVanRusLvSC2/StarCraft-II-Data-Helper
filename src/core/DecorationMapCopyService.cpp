#include "core/DecorationMapCopyService.h"

#include "core/BackupManager.h"
#include "core/FolderAnalyzer.h"
#include "core/Sc2Archive.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QQueue>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <algorithm>

namespace
{

QString defaultOutputPath(const QString &sourceArchivePath)
{
    const QFileInfo info(sourceArchivePath);
    const QString suffix = info.suffix().isEmpty() ? QStringLiteral("SC2Map") : info.suffix();
    return QDir(info.absolutePath()).absoluteFilePath(
        QStringLiteral("%1_DecorOptimized.%2").arg(info.completeBaseName(), suffix));
}

bool samePath(const QString &left, const QString &right)
{
    return QFileInfo(left).absoluteFilePath().compare(QFileInfo(right).absoluteFilePath(), Qt::CaseInsensitive) == 0;
}

bool fileSha256(const QString &path, QByteArray *hash, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Unable to read archive for verification: %1").arg(path);
        return false;
    }

    QCryptographicHash digest(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray bytes = file.read(1024 * 1024);
        if (bytes.isEmpty() && file.error() != QFileDevice::NoError) {
            if (error)
                *error = QStringLiteral("Unable to read archive completely for verification: %1").arg(path);
            return false;
        }
        digest.addData(bytes);
    }

    if (hash)
        *hash = digest.result();
    return true;
}

bool makeStagingArchivePath(const QString &outputPath, QString *stagingPath, QString *error)
{
    const QFileInfo outputInfo(outputPath);
    if (!QFileInfo::exists(outputInfo.absolutePath()) || !QFileInfo(outputInfo.absolutePath()).isDir()) {
        if (error)
            *error = QStringLiteral("Output folder does not exist: %1").arg(outputInfo.absolutePath());
        return false;
    }

    // Reserve a unique sibling name. Sc2Archive::saveCopy deliberately owns
    // and removes its target while constructing it, so the reservation must
    // be closed before handing that path over.
    QTemporaryFile reservation(
        QDir(outputInfo.absolutePath()).absoluteFilePath(outputInfo.fileName() + QStringLiteral(".sc2dh-stage-XXXXXX")));
    reservation.setAutoRemove(false);
    if (!reservation.open()) {
        if (error)
            *error = QStringLiteral("Unable to reserve a staging path beside the output archive.");
        return false;
    }
    const QString path = reservation.fileName();
    reservation.close();
    if (stagingPath)
        *stagingPath = path;
    return true;
}

bool replaceFileAtomicallyAndVerify(const QString &sourcePath,
                                    const QString &targetPath,
                                    const QByteArray &expectedHash,
                                    QString *error)
{
    QByteArray sourceHash;
    if (!fileSha256(sourcePath, &sourceHash, error))
        return false;
    if (sourceHash != expectedHash) {
        if (error)
            *error = QStringLiteral("Staged archive hash changed before commit.");
        return false;
    }

    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Unable to reopen staged archive: %1").arg(sourcePath);
        return false;
    }

    QSaveFile target(targetPath);
    // A direct-write fallback could truncate an existing output on a file
    // system that cannot atomically replace it. Failing safely is required.
    target.setDirectWriteFallback(false);
    if (!target.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Unable to start atomic output replacement: %1").arg(targetPath);
        return false;
    }

    while (!source.atEnd()) {
        const QByteArray chunk = source.read(1024 * 1024);
        if (chunk.isEmpty() && source.error() != QFileDevice::NoError) {
            if (error)
                *error = QStringLiteral("Unable to read staged archive while committing: %1").arg(sourcePath);
            return false;
        }
        if (!chunk.isEmpty() && target.write(chunk) != chunk.size()) {
            if (error)
                *error = QStringLiteral("Unable to write staged archive into atomic replacement: %1").arg(targetPath);
            return false;
        }
    }
    if (!target.commit()) {
        if (error)
            *error = QStringLiteral("Atomic output replacement could not be committed: %1").arg(targetPath);
        return false;
    }

    QByteArray committedHash;
    if (!fileSha256(targetPath, &committedHash, error))
        return false;
    if (committedHash != expectedHash) {
        if (error)
            *error = QStringLiteral("Committed output archive did not pass byte verification: %1").arg(targetPath);
        return false;
    }
    return true;
}

bool restoreExistingOutput(const QString &backupPath,
                           const QString &outputPath,
                           const QByteArray &originalHash,
                           QString *error)
{
    QByteArray currentHash;
    QString hashError;
    if (fileSha256(outputPath, &currentHash, &hashError) && currentHash == originalHash)
        return true;

    QString restoreError;
    if (!replaceFileAtomicallyAndVerify(backupPath, outputPath, originalHash, &restoreError)) {
        if (error) {
            *error = QStringLiteral("Existing output could not be restored. Its verified backup remains at %1. %2")
                         .arg(backupPath, restoreError);
        }
        return false;
    }
    return true;
}

bool normalizedExtractEntry(const QString &entryName, QString *relative, QString *error)
{
    QString normalized = QDir::cleanPath(entryName).replace('\\', '/');
    while (normalized.startsWith(QLatin1Char('/')))
        normalized.remove(0, 1);
    if (normalized.isEmpty() || normalized == QStringLiteral("."))
        return false;
    if (QDir::isAbsolutePath(normalized)
        || normalized == QStringLiteral("..")
        || normalized.startsWith(QStringLiteral("../"))
        || normalized.contains(QStringLiteral("/../"))
        || normalized.contains(QLatin1Char(':'))) {
        if (error)
            *error = QStringLiteral("Unsafe archive entry path: %1").arg(entryName);
        return false;
    }
    if (relative)
        *relative = normalized;
    return true;
}

bool extractArchiveForAnalysis(const Sc2Archive &archive,
                               const QString &targetFolder,
                               QString *error)
{
    QDir root(targetFolder);
    const QString rootPath = QDir::cleanPath(QFileInfo(targetFolder).absoluteFilePath()).replace('\\', '/');

    for (const QString &entry : archive.allEntries()) {
        QString relative;
        QString pathError;
        if (!normalizedExtractEntry(entry, &relative, &pathError)) {
            if (!pathError.isEmpty()) {
                if (error)
                    *error = pathError;
                return false;
            }
            continue;
        }
        if (relative.endsWith(QLatin1Char('/')))
            continue;

        QByteArray bytes;
        if (!archive.readEntry(entry, &bytes, error))
            return false;

        const QString outputPath = root.absoluteFilePath(relative);
        const QString outputAbs = QDir::cleanPath(QFileInfo(outputPath).absoluteFilePath()).replace('\\', '/');
        if (outputAbs != rootPath && !outputAbs.startsWith(rootPath + QLatin1Char('/'), Qt::CaseInsensitive)) {
            if (error)
                *error = QStringLiteral("Refused to extract archive entry outside verification folder: %1").arg(entry);
            return false;
        }
        const QFileInfo outputInfo(outputPath);
        if (!root.mkpath(root.relativeFilePath(outputInfo.absolutePath()))) {
            if (error)
                *error = QStringLiteral("Unable to create verification folder for archive entry: %1").arg(entry);
            return false;
        }

        QSaveFile file(outputPath);
        if (!file.open(QIODevice::WriteOnly)
            || file.write(bytes) != bytes.size()
            || !file.commit()) {
            if (error)
                *error = QStringLiteral("Unable to write verification copy of archive entry: %1").arg(entry);
            return false;
        }
    }

    return true;
}

bool verifyArchiveWithFolderAnalysis(const Sc2Archive &archive,
                                     sc2dh::decor::DecorOptimizedMapResult *result,
                                     QString *error)
{
    if (!result) {
        if (error)
            *error = QStringLiteral("Internal error: missing decoration result for analysis verification.");
        return false;
    }
    QTemporaryDir extracted;
    if (!extracted.isValid()) {
        if (error)
            *error = QStringLiteral("Unable to create temporary folder for optimized map analysis.");
        return false;
    }
    if (!extractArchiveForAnalysis(archive, extracted.path(), error))
        return false;

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    if (!analyzer.analyzeFolder(extracted.path(), {}, &analysis, error))
        return false;

    result->verifiedScannedFiles = analysis.scannedFiles.size();
    result->verifiedDataNodes = analysis.nodes.size();
    result->verificationParseErrors.clear();
    for (const ParseErrorInfo &parseError : analysis.parseErrors) {
        result->verificationParseErrors << QStringLiteral("%1: %2")
                                               .arg(QDir(extracted.path()).relativeFilePath(parseError.filePath),
                                                    parseError.message);
    }
    if (!result->verificationParseErrors.isEmpty()) {
        if (error)
            *error = QStringLiteral("Optimized map full analysis reported parse error(s): %1")
                         .arg(result->verificationParseErrors.join(QStringLiteral("; ")));
        return false;
    }

    result->fullAnalysisVerified = true;
    return true;
}

QString normalizedEntry(QString entry)
{
    return QDir::cleanPath(entry).replace('\\', '/').toCaseFolded();
}

bool isTokenKey(const QString &value)
{
    static const QRegularExpression expression(QStringLiteral("^[A-Za-z0-9_@]+$"));
    return expression.match(value).hasMatch();
}

const QRegularExpression &tokenExpression()
{
    static const QRegularExpression expression(QStringLiteral("(?<![A-Za-z0-9_@])([A-Za-z0-9_@]+)(?![A-Za-z0-9_@])"));
    return expression;
}

// Phrase-valued doodad names cannot use the token scanner above.  Checking
// every phrase with QString::contains() made safety preflight O(text * names)
// on real maps with large PreloadAssetDB/trigger files.  This compact
// Aho-Corasick matcher preserves the same case-folded substring semantics
// while scanning each text entry once.
class PhraseMatcher
{
public:
    explicit PhraseMatcher(const QSet<QString> &patterns)
    {
        m_nodes.append(Node{});
        for (const QString &pattern : patterns) {
            if (pattern.isEmpty())
                continue;
            const int patternIndex = m_patterns.size();
            m_patterns.append(pattern);
            int state = 0;
            for (const QChar character : pattern) {
                auto edge = m_nodes[state].edges.constFind(character);
                if (edge == m_nodes[state].edges.cend()) {
                    const int next = m_nodes.size();
                    m_nodes.append(Node{});
                    m_nodes[state].edges.insert(character, next);
                    state = next;
                } else {
                    state = edge.value();
                }
            }
            m_nodes[state].outputs.append(patternIndex);
        }

        QQueue<int> queue;
        for (auto edge = m_nodes[0].edges.cbegin(); edge != m_nodes[0].edges.cend(); ++edge) {
            queue.enqueue(edge.value());
            m_nodes[edge.value()].failure = 0;
        }
        while (!queue.isEmpty()) {
            const int state = queue.dequeue();
            const auto edges = m_nodes[state].edges;
            for (auto edge = edges.cbegin(); edge != edges.cend(); ++edge) {
                const QChar character = edge.key();
                const int next = edge.value();
                queue.enqueue(next);

                int fallback = m_nodes[state].failure;
                while (fallback != 0 && !m_nodes[fallback].edges.contains(character))
                    fallback = m_nodes[fallback].failure;
                const auto fallbackEdge = m_nodes[fallback].edges.constFind(character);
                if (fallbackEdge != m_nodes[fallback].edges.cend()
                    && fallbackEdge.value() != next) {
                    fallback = fallbackEdge.value();
                }
                m_nodes[next].failure = fallback;
                m_nodes[next].outputs += m_nodes[fallback].outputs;
            }
        }
    }

    QSet<QString> matches(const QString &text) const
    {
        QSet<QString> found;
        if (m_patterns.isEmpty())
            return found;
        int state = 0;
        for (const QChar character : text) {
            while (state != 0 && !m_nodes[state].edges.contains(character))
                state = m_nodes[state].failure;
            const auto edge = m_nodes[state].edges.constFind(character);
            state = edge == m_nodes[state].edges.cend() ? 0 : edge.value();
            for (const int patternIndex : m_nodes[state].outputs)
                found.insert(m_patterns.at(patternIndex));
            if (found.size() == m_patterns.size())
                break;
        }
        return found;
    }

private:
    struct Node
    {
        QHash<QChar, int> edges;
        int failure = 0;
        QVector<int> outputs;
    };
    QVector<Node> m_nodes;
    QVector<QString> m_patterns;
};

bool looksLikeUtf8Text(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return true;
    const qsizetype sampleSize = std::min<qsizetype>(bytes.size(), 8192);
    int printable = 0;
    int zeros = 0;
    for (qsizetype i = 0; i < sampleSize; ++i) {
        const uchar value = uchar(bytes.at(i));
        if (value == 0)
            ++zeros;
        if (value == '\r' || value == '\n' || value == '\t' || (value >= 32 && value < 127) || value >= 128)
            ++printable;
    }
    return zeros == 0 && printable >= (sampleSize * 85) / 100;
}

bool looksLikeUtf16LeText(const QByteArray &bytes)
{
    if (bytes.size() < 4 || bytes.size() % 2 != 0)
        return false;
    const qsizetype pairs = std::min<qsizetype>(bytes.size() / 2, 4096);
    int textPairs = 0;
    int zeroHigh = 0;
    for (qsizetype i = 0; i < pairs; ++i) {
        const uchar low = uchar(bytes.at(i * 2));
        const uchar high = uchar(bytes.at(i * 2 + 1));
        if (high == 0)
            ++zeroHigh;
        if (high == 0 && (low == '\r' || low == '\n' || low == '\t' || (low >= 32 && low < 127)))
            ++textPairs;
    }
    return zeroHigh >= (pairs * 70) / 100 && textPairs >= (pairs * 60) / 100;
}

bool decodeText(const QByteArray &bytes, QString *text)
{
    if (looksLikeUtf8Text(bytes)) {
        if (text)
            *text = QString::fromUtf8(bytes);
        return true;
    }
    if (looksLikeUtf16LeText(bytes)) {
        const auto *data = reinterpret_cast<const char16_t *>(bytes.constData());
        if (text)
            *text = QString::fromUtf16(data, bytes.size() / 2);
        return true;
    }
    return false;
}

bool isDecorationSafetyTextEntry(const QString &entryName,
                                 const QString &objectsEntry,
                                 const QString &runtimeEntry)
{
    const QString normalized = normalizedEntry(entryName);
    if (normalized == normalizedEntry(objectsEntry) || normalized == normalizedEntry(runtimeEntry))
        return false;

    const QString fileName = QFileInfo(normalized).fileName();
    static const QSet<QString> extensions = {
        QStringLiteral("galaxy"),
        QStringLiteral("xml"),
        QStringLiteral("txt"),
        QStringLiteral("sc2triggers"),
        QStringLiteral("sc2lib"),
        QStringLiteral("sc2layout"),
        QStringLiteral("layout")
    };
    if (extensions.contains(QFileInfo(fileName).suffix().toLower()))
        return true;
    return normalized.contains(QStringLiteral("localizeddata/"))
        || normalized.contains(QStringLiteral("gamestrings"))
        || normalized.contains(QStringLiteral("objectstrings"))
        || normalized.contains(QStringLiteral("triggerlibs/"))
        || normalized.contains(QStringLiteral("/scripts/"))
        || fileName == QStringLiteral("triggers")
        || fileName == QStringLiteral("mapinfo")
        || fileName == QStringLiteral("documentinfo")
        || fileName == QStringLiteral("preloadassetdb.txt");
}

bool isKnownNonExecutableMetadataEntry(const QString &entryName)
{
    const QString fileName = QFileInfo(normalizedEntry(entryName)).fileName();
    return fileName == QStringLiteral("mapinfo")
        || fileName == QStringLiteral("documentinfo")
        || fileName == QStringLiteral("preloadassetdb.txt");
}

bool isGameDataCatalogXmlEntry(const QString &entryName)
{
    const QString normalized = normalizedEntry(entryName);
    // Only XML catalog records can define a doodad's footprint/pathing. Map
    // preload manifests also contain asset IDs and the words "footprint" or
    // "pathing", but are not catalog definitions and would over-protect
    // unrelated placements.
    return normalized.startsWith(QStringLiteral("gamedata/"))
        || normalized.contains(QStringLiteral("/gamedata/"));
}

void addReference(sc2dh::decor::DecorationSafetyContext *context,
                  const QString &key,
                  const QString &entryName)
{
    if (!context || key.isEmpty())
        return;
    QStringList &files = context->referenceFilesByDoodadKey[key.toCaseFolded()];
    files << entryName;
    files.removeDuplicates();
}

void addReferencesForAllKeys(sc2dh::decor::DecorationSafetyContext *context,
                             const QSet<QString> &tokenKeys,
                             const QSet<QString> &phraseKeys,
                             const QString &entryName)
{
    for (const QString &key : tokenKeys)
        addReference(context, key, entryName);
    for (const QString &key : phraseKeys)
        addReference(context, key, entryName);
}

void scanTextForDoodadKeys(sc2dh::decor::DecorationSafetyContext *context,
                           const QString &entryName,
                           const QString &text,
                           const QSet<QString> &tokenKeys,
                           const PhraseMatcher &phraseMatcher)
{
    auto matches = tokenExpression().globalMatch(text);
    while (matches.hasNext()) {
        const QString token = matches.next().captured(1).toCaseFolded();
        if (tokenKeys.contains(token))
            addReference(context, token, entryName);
    }

    const QString foldedText = text.toCaseFolded();
    for (const QString &key : phraseMatcher.matches(foldedText))
        addReference(context, key, entryName);
}

void scanXmlForStaticOnlyDoodadTypes(sc2dh::decor::DecorationSafetyContext *context,
                                     const QString &entryName,
                                     const QString &text,
                                     const QSet<QString> &doodadTypes)
{
    if (!context || doodadTypes.isEmpty())
        return;

    const QString folded = text.toCaseFolded();
    for (const QString &typeKey : doodadTypes) {
        if (context->staticOnlyReasonByDoodadType.contains(typeKey))
            continue;

        const QString idNeedle = QStringLiteral("id=\"%1\"").arg(typeKey);
        int idIndex = folded.indexOf(idNeedle);
        if (idIndex < 0) {
            const QString idNeedleApostrophe = QStringLiteral("id='%1'").arg(typeKey);
            idIndex = folded.indexOf(idNeedleApostrophe);
        }
        if (idIndex < 0)
            continue;

        const qsizetype blockStart = std::max<qsizetype>(0, folded.lastIndexOf(QLatin1Char('<'), idIndex));
        int blockEnd = folded.indexOf(QStringLiteral("</"), idIndex);
        if (blockEnd < 0)
            blockEnd = int(std::min<qsizetype>(folded.size(), qsizetype(idIndex) + 3000));
        else
            blockEnd = int(std::min<qsizetype>(folded.size(), qsizetype(blockEnd) + 300));
        const QString block = folded.mid(blockStart, blockEnd - blockStart);
        if (!block.contains(QStringLiteral("footprint"))
            && !block.contains(QStringLiteral("pathing"))
            && !block.contains(QStringLiteral("blocker"))
            && !block.contains(QStringLiteral("destruct"))) {
            continue;
        }

        context->staticOnlyReasonByDoodadType.insert(
            typeKey,
            QStringLiteral("Static-only: pathing/gameplay dependency (doodad type has footprint/pathing data in %1).")
                .arg(entryName));
    }
}

sc2dh::decor::DecorationSafetyContext buildArchiveSafetyContext(const Sc2Archive &archive,
                                                                const QByteArray &objectsBytes,
                                                                const sc2dh::decor::DecorOptimizedMapRequest &request,
                                                                QStringList *warnings,
                                                                QString *error)
{
    sc2dh::decor::DecorationSafetyContext context = request.safetyContext;
    sc2dh::decor::DecorationStreamingPlanner planner;
    const QVector<sc2dh::decor::DoodadPlacement> doodads = planner.parseObjects(objectsBytes);

    QSet<QString> tokenKeys;
    QSet<QString> phraseKeys;
    QSet<QString> doodadTypes;
    for (const sc2dh::decor::DoodadPlacement &doodad : doodads) {
        const QString typeKey = doodad.type.trimmed().toCaseFolded();
        if (!typeKey.isEmpty())
            doodadTypes.insert(typeKey);
        for (const QString &value : {doodad.id, doodad.name}) {
            const QString key = value.trimmed().toCaseFolded();
            if (key.isEmpty())
                continue;
            if (isTokenKey(value.trimmed()))
                tokenKeys.insert(key);
            else if (key.size() >= 3)
                phraseKeys.insert(key);
        }
    }
    if (tokenKeys.isEmpty() && phraseKeys.isEmpty() && doodadTypes.isEmpty())
        return context;
    const PhraseMatcher phraseMatcher(phraseKeys);

    for (const QString &entry : archive.allEntries()) {
        if (!isDecorationSafetyTextEntry(entry, request.objectsEntry, request.runtimeEntry))
            continue;

        QByteArray bytes;
        QString readError;
        if (!archive.readEntry(entry, &bytes, &readError)) {
            addReferencesForAllKeys(&context, tokenKeys, phraseKeys, entry);
            if (warnings)
                *warnings << QStringLiteral("Decoration safety: treated all doodads as referenced by unreadable text entry %1: %2")
                                 .arg(entry, readError);
            continue;
        }

        QString text;
        if (!decodeText(bytes, &text)) {
            // MapInfo/DocumentInfo/PreloadAssetDb are map metadata rather
            // than executable trigger/script sources and do not address
            // individual ObjectDoodad placements. Treating their binary
            // payload as a reference to every doodad disables actor
            // recreation without adding safety. Unknown text-like entries
            // remain fail-closed below.
            if (isKnownNonExecutableMetadataEntry(entry)) {
                if (warnings) {
                    *warnings << QStringLiteral("Decoration safety: ignored opaque non-executable metadata entry %1.")
                                     .arg(entry);
                }
                continue;
            }
            addReferencesForAllKeys(&context, tokenKeys, phraseKeys, entry);
            if (warnings)
                *warnings << QStringLiteral("Decoration safety: treated all doodads as referenced by undecodable text entry %1.")
                                 .arg(entry);
            continue;
        }
        scanTextForDoodadKeys(&context, entry, text, tokenKeys, phraseMatcher);
        // Visibility-only mode keeps collision data intact, but hiding a
        // catalog-defined footprint/pathing doodad would still create an
        // invisible gameplay blocker. Keep those types static in both modes.
        if (entry.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)
            && isGameDataCatalogXmlEntry(entry))
            scanXmlForStaticOnlyDoodadTypes(&context, entry, text, doodadTypes);
    }

    Q_UNUSED(error);
    return context;
}

} // namespace

namespace sc2dh::decor
{

DecorOptimizedMapResult DecorationMapCopyService::createOptimizedCopy(const DecorOptimizedMapRequest &request) const
{
    DecorOptimizedMapResult result;
    result.dryRun = request.dryRun;
    result.outputArchivePath = request.outputArchivePath.isEmpty()
        ? defaultOutputPath(request.sourceArchivePath)
        : request.outputArchivePath;

    if (request.sourceArchivePath.isEmpty() || !QFileInfo::exists(request.sourceArchivePath)) {
        result.error = QStringLiteral("Source SC2 archive does not exist.");
        return result;
    }
    if (result.outputArchivePath.isEmpty()) {
        result.error = QStringLiteral("Output archive path is empty.");
        return result;
    }
    if (samePath(request.sourceArchivePath, result.outputArchivePath)) {
        result.error = QStringLiteral("Decoration optimized copy must not overwrite the source archive.");
        return result;
    }

    QString sourceHashError;
    if (!fileSha256(request.sourceArchivePath, &result.sourceSha256Before, &sourceHashError)) {
        result.error = QStringLiteral("Unable to hash source archive before preflight: %1").arg(sourceHashError);
        return result;
    }
    QSet<int> positiveZoneIds;
    for (const DecorZone &zone : request.zones) {
        if (zone.id > 0)
            positiveZoneIds.insert(zone.id);
    }
    if (positiveZoneIds.isEmpty()) {
        result.error = QStringLiteral("Create at least one positive decoration zone before creating an optimized map copy.");
        return result;
    }
    if (!request.dryRun && QFileInfo::exists(result.outputArchivePath)) {
        if (!request.overwriteExisting) {
            result.error = QStringLiteral("Output archive already exists.");
            return result;
        }
    }

    Sc2Archive archive;
    QString error;
    if (!archive.load(request.sourceArchivePath, &error)) {
        result.error = QStringLiteral("Unable to open source archive: %1").arg(error);
        return result;
    }

    QByteArray objectsBytes;
    if (!archive.readEntry(request.objectsEntry, &objectsBytes, &error)) {
        result.error = QStringLiteral("Unable to read Objects entry: %1").arg(error);
        return result;
    }
    QByteArray mapScriptBytes;
    if (!archive.readEntry(request.mapScriptEntry, &mapScriptBytes, &error)) {
        result.error = QStringLiteral("Unable to read MapScript entry: %1").arg(error);
        return result;
    }

    DecorationStreamingPlanner planner;
    QStringList safetyWarnings;
    const DecorationSafetyContext safetyContext =
        buildArchiveSafetyContext(archive, objectsBytes, request, &safetyWarnings, &error);
    result.warnings += safetyWarnings;
    if (request.mode == DecorationOptimizationMode::VisibilityOnly) {
        result.patch = planner.prepareVisibilityArchivePatch(objectsBytes,
                                                             mapScriptBytes,
                                                             request.zones,
                                                             safetyContext,
                                                             request.galaxyOptions,
                                                             request.objectsEntry,
                                                             request.mapScriptEntry,
                                                             request.runtimeEntry);
    } else {
        result.patch = planner.prepareArchivePatch(objectsBytes,
                                                   mapScriptBytes,
                                                   request.zones,
                                                   safetyContext,
                                                   request.galaxyOptions,
                                                   request.objectsEntry,
                                                   request.mapScriptEntry,
                                                   request.runtimeEntry);
    }
    result.warnings += result.patch.warnings;
    if (!result.patch.valid) {
        result.error = result.patch.error;
        return result;
    }
    if (result.patch.mode != request.mode) {
        result.error = QStringLiteral("Decoration archive patch mode does not match the requested operation.");
        return result;
    }
    if (request.mode == DecorationOptimizationMode::VisibilityOnly
        && result.patch.replacementEntries.contains(request.objectsEntry)) {
        result.error = QStringLiteral("Visibility-only decoration copy attempted to replace Objects.");
        return result;
    }

    if (request.mode == DecorationOptimizationMode::VisibilityOnly) {
        result.visibilityControlledDoodads = result.patch.visibilityArtifacts.controlledDoodadIndices.size();
    } else {
        result.removedDoodads = result.patch.artifacts.removedDoodadIndices.size();
    }

    if (!fileSha256(request.sourceArchivePath, &result.sourceSha256After, &sourceHashError)) {
        result.error = QStringLiteral("Unable to re-hash source archive after preflight: %1").arg(sourceHashError);
        return result;
    }
    result.sourceUnchanged = result.sourceSha256After == result.sourceSha256Before;
    if (!result.sourceUnchanged) {
        result.error = QStringLiteral("Source archive changed during preflight; no output was written.");
        return result;
    }
    if (request.dryRun) {
        result.success = true;
        return result;
    }

    QString stagingPath;
    if (!makeStagingArchivePath(result.outputArchivePath, &stagingPath, &error)) {
        result.error = QStringLiteral("Unable to create decoration optimized copy staging path: %1").arg(error);
        return result;
    }
    const auto discardStaging = [&]() {
        if (stagingPath.isEmpty())
            return;
        for (const QString &candidate : {
                 stagingPath,
                 stagingPath + QStringLiteral(".compact"),
                 stagingPath + QStringLiteral(".sc2dh.SC2Map"),
                 stagingPath + QStringLiteral(".sc2dh.SC2Map.compact")}) {
            QFile::remove(candidate);
        }
    };

    // Sc2Archive::saveCopy owns and can remove its target internally. Always
    // give it an isolated staging path, never the user-visible output path.
    if (!archive.saveCopy(stagingPath, result.patch.replacementEntries, {}, &error)) {
        discardStaging();
        result.error = QStringLiteral("Unable to stage decoration optimized copy; existing output was not changed: %1")
                           .arg(error);
        return result;
    }

    if (!fileSha256(request.sourceArchivePath, &result.sourceSha256After, &sourceHashError)
        || result.sourceSha256After != result.sourceSha256Before) {
        discardStaging();
        result.sourceUnchanged = false;
        result.error = sourceHashError.isEmpty()
            ? QStringLiteral("Source archive changed while the output was staged; no output was committed.")
            : QStringLiteral("Unable to prove source archive unchanged after staging: %1").arg(sourceHashError);
        return result;
    }
    result.sourceUnchanged = true;

    Sc2Archive verification;
    if (!verification.load(stagingPath, &error)) {
        discardStaging();
        result.error = QStringLiteral("Staged decoration optimized copy verification failed; existing output was not changed: %1")
                           .arg(error);
        return result;
    }
    for (auto it = result.patch.replacementEntries.cbegin(); it != result.patch.replacementEntries.cend(); ++it) {
        QByteArray actual;
        if (!verification.readEntry(it.key(), &actual, &error)) {
            discardStaging();
            result.error = QStringLiteral("Staged decoration optimized copy is missing %1; existing output was not changed: %2")
                               .arg(it.key(), error);
            return result;
        }
        if (actual != it.value()) {
            discardStaging();
            result.error = QStringLiteral("Staged decoration optimized copy verification failed for %1; existing output was not changed.")
                               .arg(it.key());
            return result;
        }
    }
    if (request.mode == DecorationOptimizationMode::VisibilityOnly) {
        QByteArray stagedObjects;
        if (!verification.readEntry(request.objectsEntry, &stagedObjects, &error)) {
            discardStaging();
            result.error = QStringLiteral("Staged visibility-only decoration copy is missing Objects; existing output was not changed: %1")
                               .arg(error);
            return result;
        }
        if (stagedObjects != objectsBytes) {
            discardStaging();
            result.error = QStringLiteral("Staged visibility-only decoration copy changed Objects; existing output was not changed.");
            return result;
        }
        result.objectsPreserved = true;
    }

    DecorOptimizedMapResult stagedVerification;
    if (!verifyArchiveWithFolderAnalysis(verification, &stagedVerification, &error)) {
        discardStaging();
        result.error = QStringLiteral("Staged decoration optimized copy full analysis failed; existing output was not changed: %1")
                           .arg(error);
        return result;
    }

    QByteArray stagedHash;
    if (!fileSha256(stagingPath, &stagedHash, &error)) {
        discardStaging();
        result.error = QStringLiteral("Unable to verify staged decoration optimized copy; existing output was not changed: %1")
                           .arg(error);
        return result;
    }

    const bool outputExistsAtCommit = QFileInfo::exists(result.outputArchivePath);
    if (outputExistsAtCommit && !request.overwriteExisting) {
        discardStaging();
        result.error = QStringLiteral("Output archive appeared while staging; it was not changed.");
        return result;
    }

    QByteArray originalOutputHash;
    if (outputExistsAtCommit) {
        BackupManager backupManager;
        if (!backupManager.createBackup(result.outputArchivePath,
                                        &result.previousOutputBackupPath,
                                        &error,
                                        true)) {
            discardStaging();
            result.error = QStringLiteral("Unable to create a verified backup of the existing output; it was not changed: %1")
                               .arg(error);
            return result;
        }
        if (!fileSha256(result.previousOutputBackupPath, &originalOutputHash, &error)) {
            discardStaging();
            result.error = QStringLiteral("Unable to verify the backup of the existing output; it was not changed: %1")
                               .arg(error);
            return result;
        }

        QByteArray currentOutputHash;
        if (!fileSha256(result.outputArchivePath, &currentOutputHash, &error)) {
            discardStaging();
            result.error = QStringLiteral("Unable to recheck the existing output after its backup was made; refusing to replace it. "
                                          "The verified pre-replacement backup remains at %1: %2")
                               .arg(result.previousOutputBackupPath, error);
            return result;
        }
        if (currentOutputHash != originalOutputHash) {
            discardStaging();
            result.error = QStringLiteral("Output changed after its backup was made; refusing to replace it. "
                                          "The verified pre-replacement backup remains at %1.")
                               .arg(result.previousOutputBackupPath);
            return result;
        }
    }

    if (!replaceFileAtomicallyAndVerify(stagingPath, result.outputArchivePath, stagedHash, &error)) {
        QString recoveryError;
        const bool restored = !outputExistsAtCommit
            || restoreExistingOutput(result.previousOutputBackupPath,
                                     result.outputArchivePath,
                                     originalOutputHash,
                                     &recoveryError);
        discardStaging();
        if (outputExistsAtCommit && !restored) {
            result.error = QStringLiteral("Unable to commit decoration optimized copy: %1\n%2")
                               .arg(error, recoveryError);
        } else if (outputExistsAtCommit) {
            result.error = QStringLiteral("Unable to commit decoration optimized copy; the existing output was restored from its verified backup: %1")
                               .arg(error);
        } else {
            result.error = QStringLiteral("Unable to commit decoration optimized copy; no pre-existing output was replaced: %1")
                               .arg(error);
        }
        return result;
    }

    // The committed file has the SHA-256 of the fully reopened and analysed
    // stage archive, so the staged semantic proof applies byte-for-byte to
    // the final output as well.
    result.fullAnalysisVerified = stagedVerification.fullAnalysisVerified;
    result.verifiedScannedFiles = stagedVerification.verifiedScannedFiles;
    result.verifiedDataNodes = stagedVerification.verifiedDataNodes;
    result.verificationParseErrors = stagedVerification.verificationParseErrors;
    discardStaging();

    if (!fileSha256(request.sourceArchivePath, &result.sourceSha256After, &sourceHashError)
        || result.sourceSha256After != result.sourceSha256Before) {
        result.sourceUnchanged = false;
        QString recoveryError;
        const bool recovered = outputExistsAtCommit
            ? restoreExistingOutput(result.previousOutputBackupPath,
                                    result.outputArchivePath,
                                    originalOutputHash,
                                    &recoveryError)
            : QFile::remove(result.outputArchivePath);
        result.error = sourceHashError.isEmpty()
            ? QStringLiteral("Source archive changed before final verification; the generated output was removed or restored.")
            : QStringLiteral("Unable to prove source archive unchanged after commit; the generated output was removed or restored: %1")
                  .arg(sourceHashError);
        if (!recovered)
            result.error += QStringLiteral(" Recovery failed: %1").arg(recoveryError);
        return result;
    }
    result.sourceUnchanged = true;
    result.success = true;
    return result;
}

} // namespace sc2dh::decor
