#include "core/DecorationMapCopyService.h"

#include "core/BackupManager.h"
#include "core/FolderAnalyzer.h"
#include "core/GalaxyReferenceSpans.h"
#include "core/GalaxyDeclarationOrder.h"
#include "core/Sc2Archive.h"
#include "core/UnitStructureDefaults.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QQueue>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <algorithm>
#include <pugixml.hpp>

namespace
{

const QString DecorRestoreManifestEntry = QStringLiteral("sc2dh/decor_restore_v1.json");
const QString DecorActorCatalogEntry = QStringLiteral("Base.SC2Data/GameData/ActorData.xml");
const QString DecorModelCatalogEntry = QStringLiteral("Base.SC2Data/GameData/ModelData.xml");
const QString DecorImportEntry = QStringLiteral("sc2dh/decor_import.xml");

QByteArray sha256Hex(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

QByteArray compressedBase64(const QByteArray &bytes)
{
    return qCompress(bytes, 9).toBase64();
}

QByteArray restoredBytes(const QJsonObject &manifest, const QString &field)
{
    return qUncompress(QByteArray::fromBase64(manifest.value(field).toString().toLatin1()));
}

bool restorePreloadPatch(const Sc2Archive &source, QString galaxy,
                         QHash<QString, QByteArray> *replacements, QStringList *removals,
                         QString *error,
                         const QString &entry = QStringLiteral("Preload.xml"),
                         const QString &kind = QStringLiteral("preload"), const char *rootName = "Preload")
{
    const auto fail = [&](const QString &message) { if (error) *error = message; return false; };
    const QRegularExpression record(QStringLiteral("^// SC2DH %1 rollback ([01]) ([a-f0-9]{64})$").arg(kind), QRegularExpression::MultilineOption);
    if (!record.match(galaxy).hasMatch()) {
        QByteArray metadata;
        if (source.readEntry(DecorImportEntry, &metadata, nullptr)) {
            pugi::xml_document doc;
            if (!doc.load_buffer(metadata.constData(), size_t(metadata.size())) || !doc.child("SC2DHImport"))
                return fail(QStringLiteral("Invalid decoration import metadata."));
            galaxy += QLatin1Char('\n') + QString::fromUtf8(doc.child("SC2DHImport").child("Annotations").text().get());
        }
    }
    const auto match = record.match(galaxy);
    if (!match.hasMatch()) return true; // Exports before native variant preload support.
    QByteArray current;
    if (!source.readEntry(entry, &current, error)
        || sha256Hex(current) != match.captured(2).toLatin1())
        return fail(QStringLiteral("%1 changed after Galaxy export; no copy created.").arg(entry));
    QByteArray encoded;
    const QRegularExpression part(QStringLiteral("^// SC2DH %1 data ([A-Za-z0-9+/=]+)$").arg(kind), QRegularExpression::MultilineOption);
    auto matches = part.globalMatch(galaxy);
    while (matches.hasNext()) encoded += matches.next().captured(1).toLatin1();
    const QByteArray compressed = QByteArray::fromBase64(encoded);
    if (compressed.size() < 4) return fail(QStringLiteral("Invalid Galaxy preload cleanup data."));
    const auto *data = reinterpret_cast<const unsigned char *>(compressed.constData());
    const quint32 size = (quint32(data[0]) << 24) | (quint32(data[1]) << 16) | (quint32(data[2]) << 8) | data[3];
    if (size > 16 * 1024 * 1024) return fail(QStringLiteral("Galaxy preload cleanup data is too large."));
    const QByteArray original = qUncompress(compressed);
    if (original.size() != size) return fail(QStringLiteral("Invalid Galaxy preload cleanup data."));
    if (match.captured(1) == QStringLiteral("1")) {
        pugi::xml_document doc;
        if (!doc.load_buffer(original.constData(), size_t(original.size())) || !doc.child(rootName))
            return fail(QStringLiteral("Invalid original Preload.xml."));
        replacements->insert(entry, original);
    } else {
        if (!original.isEmpty()) return fail(QStringLiteral("Invalid absent-preload cleanup data."));
        *removals << entry;
    }
    return true;
}

bool verifyPreloadPatch(const Sc2Archive &archive, const QHash<QString, QByteArray> &replacements,
                        const QStringList &removals, const QString &entry = QStringLiteral("Preload.xml"))
{
    QByteArray actual;
    if (replacements.contains(entry)) return archive.readEntry(entry, &actual, nullptr) && actual == replacements.value(entry);
    if (removals.contains(entry)) return !archive.readEntry(entry, &actual, nullptr);
    return true;
}

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

bool archiveContainsEntry(const Sc2Archive &archive, const QString &wanted)
{
    const QString normalized = normalizedEntry(wanted);
    for (const QString &entry : archive.allEntries()) {
        if (normalizedEntry(entry) == normalized) return true;
    }
    return false;
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

    pugi::xml_document document;
    const QByteArray bytes = text.toUtf8();
    if (!document.load_buffer(bytes.constData(), size_t(bytes.size()))) return;
    for (auto record : document.document_element().children()) {
        const QString type = QString::fromUtf8(record.attribute("id").value()).toCaseFolded();
        if (!doodadTypes.contains(type)) continue;
        bool physical = QString::fromUtf8(record.name()).startsWith(QStringLiteral("CTerrainObject"));
        QVector<pugi::xml_node> pending;
        for (auto child : record.children()) pending << child;
        while (!pending.isEmpty()) {
            const auto field = pending.takeLast();
            const QString name = QString::fromUtf8(field.name()).toCaseFolded();
            const QString value = QString::fromUtf8(field.attribute("value").value()).trimmed().toCaseFolded();
            if ((name == QStringLiteral("footprint") || name == QStringLiteral("pathingfootprint")
                 || name == QStringLiteral("terrainobject")) && !value.isEmpty()
                && value != QStringLiteral("none") && value != QStringLiteral("0")) physical = true;
            for (auto child : field.children()) pending << child;
        }
        if (physical) context->staticOnlyReasonByDoodadType.insert(type,
            QStringLiteral("Static-only: native footprint/terrain registration in %1.").arg(entryName));
    }
}

void addUnitTypeReference(sc2dh::decor::DecorationSafetyContext *context,
                          const QString &key, const QString &entry)
{
    QStringList &files = context->referenceFilesByUnitType[key];
    if (!files.contains(entry))
        files << entry;
}

void protectAllUnitTypes(sc2dh::decor::DecorationSafetyContext *context,
                         const QSet<QString> &types, const QString &entry)
{
    for (const QString &type : types)
        addUnitTypeReference(context, type, entry);
}

void addDoodadTypeReference(sc2dh::decor::DecorationSafetyContext *context,
                            const QString &type, const QString &entry)
{
    auto &files = context->referenceFilesByDoodadType[type];
    if (!files.contains(entry)) files << entry;
}

void protectAllPlacementTypes(sc2dh::decor::DecorationSafetyContext *context,
                              const QSet<QString> &doodads, const QSet<QString> &units,
                              const QString &entry)
{
    protectAllUnitTypes(context, units, entry);
    for (const QString &type : doodads) addDoodadTypeReference(context, type, entry);
}

void scanGalaxyPlacementTypes(sc2dh::decor::DecorationSafetyContext *context,
                         const QVector<sc2dh::galaxy::Token> &tokens,
                         const QSet<QString> &doodads, const QSet<QString> &units,
                         const QString &entry)
{
    // Only placements without concrete script/type references are exported.
    // The lexer excludes comments; adjacent literal concatenations are resolved.
    for (int i = 0; i < tokens.size(); ++i) {
        if (!tokens[i].literal) continue;
        QString key = tokens[i].value.toCaseFolded();
        if (units.contains(key)) addUnitTypeReference(context, key, entry);
        if (doodads.contains(key)) addDoodadTypeReference(context, key, entry);
        for (int j = i + 1; j + 1 < tokens.size()
             && tokens[j].raw == QStringLiteral("+") && tokens[j + 1].literal; j += 2) {
            key += tokens[j + 1].value.toCaseFolded();
            if (units.contains(key)) addUnitTypeReference(context, key, entry);
            if (doodads.contains(key)) addDoodadTypeReference(context, key, entry);
        }
    }
}

void scanTriggerPlacementTypes(sc2dh::decor::DecorationSafetyContext *context,
                          const QString &text, const QSet<QString> &doodads, const QSet<QString> &units,
                          const QString &entry)
{
    if (doodads.isEmpty() && units.isEmpty())
        return;
    const QString fileName = QFileInfo(entry).fileName().toLower();
    const bool triggerFile = fileName == QStringLiteral("triggers")
        || fileName.endsWith(QStringLiteral(".sc2triggers"))
        || fileName.endsWith(QStringLiteral(".sc2lib"));
    pugi::xml_document document;
    const QByteArray utf8 = text.toUtf8();
    const bool valid = bool(document.load_buffer(utf8.constData(), utf8.size()));
    const bool triggerRoot = QString::fromUtf8(document.document_element().name()) == QStringLiteral("TriggerData");
    if (!triggerFile && !triggerRoot)
        return; // Catalog definitions, preload lists and localized text are not trigger uses.
    if (!valid) {
        protectAllPlacementTypes(context, doodads, units, entry);
        return;
    }
    QVector<pugi::xml_node> pending{document.document_element()};
    while (!pending.isEmpty()) {
        const auto node = pending.takeLast();
        const QString name = QString::fromUtf8(node.name());
        if (name.compare(QStringLiteral("Comment"), Qt::CaseInsensitive) == 0
            || QString::fromUtf8(node.attribute("Type").value()) == QStringLiteral("Comment"))
            continue;
        // Value carries GUI game links; ScriptCode carries custom Galaxy.
        if (name == QStringLiteral("Value") || name == QStringLiteral("ScriptCode")) {
            const QString value = QString::fromUtf8(node.text().get()).trimmed();
            const QString key = value.toCaseFolded();
            if (units.contains(key)) addUnitTypeReference(context, key, entry);
            if (doodads.contains(key)) addDoodadTypeReference(context, key, entry);
            bool complete = true;
            const auto tokens = sc2dh::galaxy::lex(value, &complete);
            scanGalaxyPlacementTypes(context, tokens, doodads, units, entry);
            if (!complete && name == QStringLiteral("ScriptCode"))
                protectAllPlacementTypes(context, doodads, units, entry);
        }
        for (auto child : node.children())
            if (child.type() == pugi::node_element)
                pending << child;
    }
}

QSet<int> mapInfoAiSlots(const QByteArray &bytes, bool *valid)
{
    // Versioned MapInfo headers have changed. The player table retains the
    // pid/controller/color/race/start/AI/decal record layout documented by
    // sc2reader. Accept one unambiguous complete table beginning with Neutral.
    *valid = false;
    if (bytes.size() < 32 || bytes.size() > 4 * 1024 * 1024 || bytes.left(4) != "IpaM") return {};
    const auto u32 = [&](qsizetype p) {
        return quint32(quint8(bytes[p])) | (quint32(quint8(bytes[p+1])) << 8)
            | (quint32(quint8(bytes[p+2])) << 16) | (quint32(quint8(bytes[p+3])) << 24);
    };
    int bestCount = 0; QSet<int> best;
    for (qsizetype offset = 16; offset + 4 < bytes.size(); ++offset) {
        const auto count = u32(offset);
        if (count < 2 || count > 16) continue;
        qsizetype p = offset + 4;
        QSet<int> pids, ai; bool ok = true;
        const auto string = [&]() -> QByteArray {
            const auto end = bytes.indexOf('\0', p);
            if (end < p || end-p > 1024) { ok = false; return {}; }
            const auto result = bytes.mid(p, end-p); p = end+1; return result;
        };
        for (quint32 i = 0; i < count && ok; ++i) {
            if (p+9 > bytes.size()) { ok = false; break; }
            const int pid = quint8(bytes[p++]); const auto control = u32(p); p += 4;
            const auto color = u32(p); p += 4;
            const auto race = string();
            if (pid > 15 || pids.contains(pid) || control > 4 || (color > 15 && color != 0xffffffff)
                || (i == 0 && (pid != 0 || control != 3))
                || (!race.isEmpty() && race != "Terr" && race != "Prot" && race != "Zerg" && race != "RAND" && race != "Random")) { ok = false; break; }
            pids.insert(pid);
            if (p+12 > bytes.size()) { ok = false; break; }
            p += 12; string();
            if (control == 2 || control == 4) ai.insert(pid);
        }
        if (!ok) continue;
        if (int(count) > bestCount) { bestCount = int(count); best = ai; *valid = true; }
        else if (int(count) == bestCount && ai != best) *valid = false;
    }
    return best;
}

void addAiBuildingSafety(const Sc2Archive &archive, sc2dh::decor::DecorationSafetyContext *context, QStringList *warnings)
{
    // Collect metadata even when the option is off: the GUI can enable it
    // after archive preflight without getting a different preview/export plan.
    QByteArray mapInfo;
    if (archive.readEntry(QStringLiteral("MapInfo"), &mapInfo, nullptr)) {
        bool valid = false;
        context->aiPlayerSlots.unite(mapInfoAiSlots(mapInfo, &valid));
        if (!valid) {
            for (int player = 0; player < 16; ++player) context->aiPlayerSlots.insert(player);
            if (warnings && context->keepAiBuildings) *warnings << QStringLiteral("Player controls could not be decoded; keeping all recognized buildings native.");
        }
    }
    QHash<QString, QString> parents; QHash<QString, bool> flags;
    for (const auto &type : sc2BuiltinStructureTypes()) flags.insert(type, true);
    for (const auto &entry : archive.allEntries()) {
        if (!entry.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)
            || !normalizedEntry(entry).contains(QStringLiteral("/GameData/"), Qt::CaseInsensitive)) continue;
        QByteArray data; pugi::xml_document doc;
        if (!archive.readEntry(entry, &data, nullptr) || !doc.load_buffer(data.constData(), size_t(data.size()))) continue;
        for (auto unit : doc.child("Catalog").children("CUnit")) {
            const QString type = QString::fromUtf8(unit.attribute("id").value()).toCaseFolded();
            if (type.isEmpty()) continue;
            if (unit.attribute("parent")) parents.insert(type, QString::fromUtf8(unit.attribute("parent").value()).toCaseFolded());
            for (auto field : unit.children("Attributes"))
                if (QString::fromUtf8(field.attribute("index").value()) == QStringLiteral("Structure")) flags.insert(type, field.attribute("value").as_bool());
        }
    }
    QSet<QString> all;
    for (auto i = flags.cbegin(); i != flags.cend(); ++i) all.insert(i.key());
    for (auto i = parents.cbegin(); i != parents.cend(); ++i) all.insert(i.key());
    for (const auto &type : all) {
        QString current = type; QSet<QString> seen;
        while (!flags.contains(current) && parents.contains(current) && !seen.contains(current)) { seen.insert(current); current = parents[current]; }
        if (flags.value(current, false)) context->structureUnitTypes.insert(type);
    }
}

sc2dh::decor::DecorationSafetyContext buildArchiveSafetyContext(const Sc2Archive &archive,
                                                                const QByteArray &objectsBytes,
                                                                const sc2dh::decor::DecorOptimizedMapRequest &request,
                                                                QStringList *warnings,
                                                                QString *error)
{
    sc2dh::decor::DecorationSafetyContext context = request.safetyContext;
    addAiBuildingSafety(archive, &context, warnings);
    sc2dh::decor::DecorationStreamingPlanner planner;
    const QVector<sc2dh::decor::DoodadPlacement> doodads = planner.parseObjects(objectsBytes);
    const QVector<sc2dh::decor::UnitPlacement> units = planner.parseUnits(objectsBytes);

    QSet<QString> tokenKeys;
    QSet<QString> phraseKeys;
    QSet<QString> doodadTypes;
    QSet<QString> unitTypes;
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
    if (request.mode == sc2dh::decor::DecorationOptimizationMode::RecreateActors) {
        for (const sc2dh::decor::UnitPlacement &unit : units) {
            if (!unit.type.trimmed().isEmpty())
                unitTypes.insert(unit.type.trimmed().toCaseFolded());
            for (const QString &value : {unit.id, unit.name}) {
                if (value == unit.name && value != unit.id && QRegularExpression(QStringLiteral("^[0-9]+$")).match(value).hasMatch())
                    continue; // An editor label such as "0" is not a reference to every zero in the archive.
                const QString key = value.trimmed().toCaseFolded();
                if (key.isEmpty())
                    continue;
                if (isTokenKey(value.trimmed()))
                    tokenKeys.insert(key);
                else if (key.size() >= 3)
                    phraseKeys.insert(key);
            }
        }
    }
    if (tokenKeys.isEmpty() && phraseKeys.isEmpty() && doodadTypes.isEmpty() && unitTypes.isEmpty())
        return context;
    const PhraseMatcher phraseMatcher(phraseKeys);

    for (const QString &entry : archive.allEntries()) {
        if (!isDecorationSafetyTextEntry(entry, request.objectsEntry, request.runtimeEntry))
            continue;

        QByteArray bytes;
        QString readError;
        if (!archive.readEntry(entry, &bytes, &readError)) {
            addReferencesForAllKeys(&context, tokenKeys, phraseKeys, entry);
            protectAllUnitTypes(&context, unitTypes, entry);
            if (warnings)
                *warnings << QStringLiteral("Decoration safety: treated all decor and units as referenced by unreadable text entry %1: %2")
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
            protectAllUnitTypes(&context, unitTypes, entry);
            if (warnings)
                *warnings << QStringLiteral("Decoration safety: treated all decor and units as referenced by undecodable text entry %1.")
                                 .arg(entry);
            continue;
        }
        scanTextForDoodadKeys(&context, entry, text, tokenKeys, phraseMatcher);
        if (entry.endsWith(QStringLiteral(".galaxy"), Qt::CaseInsensitive)) {
            // Keep every concrete doodad/unit type mentioned in executable
            // Galaxy, including stock actor helpers. Generic region operations
            // without an explicit type do not reference individual placements.
            bool complete = true;
            const auto tokens = sc2dh::galaxy::lex(text, &complete);
            scanGalaxyPlacementTypes(&context, tokens, doodadTypes, unitTypes, entry);
            if (!complete) {
                protectAllUnitTypes(&context, unitTypes, entry);
                for (const QString &key : doodadTypes)
                    context.staticOnlyReasonByDoodadType.insert(key,
                        QStringLiteral("Static-only: incomplete Galaxy safety scan in %1.").arg(entry));
            }
        } else {
            scanTriggerPlacementTypes(&context, text, doodadTypes, unitTypes, entry);
        }
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

DecorationSafetyContext DecorationMapCopyService::inspectSafety(const DecorOptimizedMapRequest &request, QString *error) const
{
    if (error)
        error->clear();
    Sc2Archive archive;
    QByteArray objects;
    if (!archive.load(request.sourceArchivePath, error)
        || !archive.readEntry(request.objectsEntry, &objects, error))
        return request.safetyContext;
    return buildArchiveSafetyContext(archive, objects, request, nullptr, error);
}

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

    if (request.mode == DecorationOptimizationMode::RecreateActors) {
        QByteArray existingRuntime;
        if (archive.readEntry(request.runtimeEntry, &existingRuntime, nullptr)
            && existingRuntime.contains("// SC2DH identities v2")) {
            result.error = QStringLiteral("This map already contains Galaxy placements; rebuild them before exporting again.");
            return result;
        }
        if (archiveContainsEntry(archive, DecorRestoreManifestEntry)) {
            result.error = QStringLiteral("This map already contains a decor restore manifest; restore that conversion before optimizing it again.");
            return result;
        }
        if (archiveContainsEntry(archive, DecorImportEntry)) {
            result.error = QStringLiteral("This map already contains decoration import metadata; restore it before exporting again.");
            return result;
        }
        const QByteArray optimizedObjects = result.patch.replacementEntries.value(request.objectsEntry);
        const QByteArray optimizedScript = result.patch.replacementEntries.value(request.mapScriptEntry);
        QByteArray galaxy = result.patch.replacementEntries.value(request.runtimeEntry);
        QString actorCatalogEntry = DecorActorCatalogEntry;
        bool actorCatalogExisted = false;
        QByteArray originalActorCatalog;
        if (!result.patch.artifacts.removedDoodadIndices.isEmpty()) {
            for (const QString &entry : archive.allEntries()) {
                if (normalizedEntry(entry).compare(DecorActorCatalogEntry, Qt::CaseInsensitive) == 0) {
                    actorCatalogEntry = entry;
                    actorCatalogExisted = true;
                    break;
                }
            }
            if (actorCatalogExisted && !archive.readEntry(actorCatalogEntry, &originalActorCatalog, &error)) {
                result.error = QStringLiteral("Unable to read original actor catalog: %1").arg(error);
                return result;
            }
            QByteArray runtimeActorCatalog;
            if (!planner.generateRuntimeActorCatalog(result.patch.artifacts.plan, originalActorCatalog,
                                                       &runtimeActorCatalog, &error)) {
                result.error = error;
                return result;
            }
            result.patch.replacementEntries.insert(actorCatalogEntry, runtimeActorCatalog);
            // Catalog cleanup metadata only: placements are never read from it.
            galaxy += QByteArray("// SC2DH catalog rollback ") + (actorCatalogExisted ? "1 " : "0 ")
                + sha256Hex(runtimeActorCatalog) + '\n';
            const QByteArray encodedCatalog = compressedBase64(originalActorCatalog);
            for (qsizetype i = 0; i < encodedCatalog.size(); i += 200)
                galaxy += QByteArray("// SC2DH catalog data ") + encodedCatalog.mid(i, 200) + '\n';
            result.patch.replacementEntries.insert(request.runtimeEntry, galaxy);
        }
        QByteArray modelActorCatalog = originalActorCatalog;
        if (modelActorCatalog.isEmpty()) archive.readEntry(DecorActorCatalogEntry, &modelActorCatalog, nullptr);
        QByteArray originalModelCatalog, runtimeModelCatalog;
        const bool modelCatalogExisted = archiveContainsEntry(archive, DecorModelCatalogEntry);
        if (modelCatalogExisted && !archive.readEntry(DecorModelCatalogEntry, &originalModelCatalog, &error)) {
            result.error = error; return result;
        }
        QStringList modelIds;
        if (!planner.generateRuntimeModelCatalog(result.patch.artifacts.plan, modelActorCatalog, originalModelCatalog,
                                                &runtimeModelCatalog, &modelIds, &error)) {
            result.error = error; return result;
        }
        if (!modelIds.isEmpty()) {
            result.patch.replacementEntries.insert(DecorModelCatalogEntry, runtimeModelCatalog);
            QString preloadFunction = QStringLiteral("void DecorOpt_PreloadVariants() {\n");
            for (const auto &model : modelIds) {
                QByteArray literal = QJsonDocument(QJsonArray{model}).toJson(QJsonDocument::Compact);
                literal = literal.mid(1, literal.size()-2);
                preloadFunction += QStringLiteral("    PreloadObject(c_gameCatalogModel, %1, false);\n").arg(QString::fromUtf8(literal));
            }
            preloadFunction += QStringLiteral("}\n\n");
            QString runtime = QString::fromUtf8(galaxy);
            const QString init = QStringLiteral("void DecorOpt_Init() {\n");
            const auto initIndex = runtime.indexOf(init);
            if (initIndex < 0) { result.error = QStringLiteral("Runtime initialization is missing."); return result; }
            const auto declarationsEnd = sc2dh::galaxy::afterFunctionDeclarations(runtime, QStringLiteral("DecorOpt_Init"));
            if (declarationsEnd < 0) { result.error = QStringLiteral("Runtime initialization declarations are invalid."); return result; }
            runtime.insert(declarationsEnd, QStringLiteral("\n    DecorOpt_PreloadVariants();"));
            runtime.insert(initIndex, preloadFunction);
            galaxy = runtime.toUtf8();
            galaxy += QByteArray("// SC2DH models rollback ") + (modelCatalogExisted ? "1 " : "0 ") + sha256Hex(runtimeModelCatalog) + '\n';
            const QByteArray encodedModels = compressedBase64(originalModelCatalog);
            for (qsizetype i = 0; i < encodedModels.size(); i += 200)
                galaxy += QByteArray("// SC2DH models data ") + encodedModels.mid(i, 200) + '\n';
            result.patch.replacementEntries.insert(request.runtimeEntry, galaxy);
        }
        const auto declarationErrors = sc2dh::galaxy::declarationOrderErrors(QString::fromUtf8(galaxy));
        if (!declarationErrors.isEmpty()) { result.error = declarationErrors.join(QLatin1Char('\n')); return result; }
        if (!result.patch.artifacts.removedDoodadIndices.isEmpty()
            || std::any_of(result.patch.artifacts.removedUnitIndices.cbegin(), result.patch.artifacts.removedUnitIndices.cend(),
                [&](int i) { return result.patch.artifacts.plan.units[i].variation >= 0; })) {
            const QString preloadEntry = QStringLiteral("Preload.xml");
            const bool existed = archiveContainsEntry(archive, preloadEntry);
            QByteArray originalPreload, runtimePreload;
            if ((existed && !archive.readEntry(preloadEntry, &originalPreload, &error))
                || !planner.generateRuntimePreload(result.patch.artifacts.plan, originalPreload, &runtimePreload, &error)) {
                result.error = error;
                return result;
            }
            result.patch.replacementEntries.insert(preloadEntry, runtimePreload);
            galaxy += QByteArray("// SC2DH preload rollback ") + (existed ? "1 " : "0 ")
                + sha256Hex(runtimePreload) + '\n';
            const QByteArray encoded = compressedBase64(originalPreload);
            for (qsizetype i = 0; i < encoded.size(); i += 200)
                galaxy += QByteArray("// SC2DH preload data ") + encoded.mid(i, 200) + '\n';
            result.patch.replacementEntries.insert(request.runtimeEntry, galaxy);
        }
        if (request.writeRestoreSidecars) {
            // Optional external bookkeeping and exact archive rollback.
            // This XML carries only import bookkeeping; placement geometry is
            // still read exclusively from the executable creation statements.
            QByteArray annotations;
            QByteArray cleanGalaxy;
            for (const QByteArray &line : galaxy.split('\n')) {
                if (line.startsWith("// SC2DH ")) annotations += line + '\n';
                else if (!line.trimmed().startsWith("//") || line.startsWith("// Generated by SC2 Data Helper Decoration Streaming.")) cleanGalaxy += line + '\n';
            }
            pugi::xml_document importDoc;
            auto importRoot = importDoc.append_child("SC2DHImport");
            importRoot.append_attribute("Version") = "1";
            importRoot.append_child("Annotations").text() = annotations.constData();
            struct ImportWriter : pugi::xml_writer {
                QByteArray bytes;
                void write(const void *data, size_t size) override { bytes.append(static_cast<const char *>(data), qsizetype(size)); }
            } importWriter;
            importDoc.save(importWriter, "    ", pugi::format_default, pugi::encoding_utf8);
            result.patch.replacementEntries.insert(DecorImportEntry, importWriter.bytes);
            galaxy = cleanGalaxy;
            result.patch.replacementEntries.insert(request.runtimeEntry, galaxy);
            QJsonObject manifest;
            manifest.insert(QStringLiteral("format"), QStringLiteral("SC2DH-DecorRestore"));
            manifest.insert(QStringLiteral("version"), 1);
            manifest.insert(QStringLiteral("objectsEntry"), request.objectsEntry);
            manifest.insert(QStringLiteral("mapScriptEntry"), request.mapScriptEntry);
            manifest.insert(QStringLiteral("runtimeEntry"), request.runtimeEntry);
            manifest.insert(QStringLiteral("originalObjects"), QString::fromLatin1(compressedBase64(objectsBytes)));
            manifest.insert(QStringLiteral("originalMapScript"), QString::fromLatin1(compressedBase64(mapScriptBytes)));
            manifest.insert(QStringLiteral("originalObjectsSha256"), QString::fromLatin1(sha256Hex(objectsBytes)));
            manifest.insert(QStringLiteral("originalMapScriptSha256"), QString::fromLatin1(sha256Hex(mapScriptBytes)));
            manifest.insert(QStringLiteral("optimizedObjectsSha256"), QString::fromLatin1(sha256Hex(optimizedObjects)));
            manifest.insert(QStringLiteral("optimizedMapScriptSha256"), QString::fromLatin1(sha256Hex(optimizedScript)));
            manifest.insert(QStringLiteral("runtimeSha256"), QString::fromLatin1(sha256Hex(galaxy)));
            if (!result.patch.artifacts.removedDoodadIndices.isEmpty()) {
                manifest.insert(QStringLiteral("actorCatalogEntry"), actorCatalogEntry);
                manifest.insert(QStringLiteral("originalActorCatalogExisted"), actorCatalogExisted);
                manifest.insert(QStringLiteral("originalActorCatalog"), QString::fromLatin1(compressedBase64(originalActorCatalog)));
                manifest.insert(QStringLiteral("originalActorCatalogSha256"), QString::fromLatin1(sha256Hex(originalActorCatalog)));
                manifest.insert(QStringLiteral("optimizedActorCatalogSha256"),
                                QString::fromLatin1(sha256Hex(result.patch.replacementEntries.value(actorCatalogEntry))));
            }
            manifest.insert(QStringLiteral("removedDoodads"), result.patch.artifacts.removedDoodadIndices.size());
            manifest.insert(QStringLiteral("removedUnits"), result.patch.artifacts.removedUnitIndices.size());
            result.patch.replacementEntries.insert(DecorRestoreManifestEntry,
                                                   QJsonDocument(manifest).toJson(QJsonDocument::Compact));
        } else {
            // A self-contained script import needs the original IDs/flags and
            // catalog cleanup record, not a backup of placement geometry.
            QByteArray compactGalaxy;
            for (const QByteArray &line : galaxy.split('\n')) {
                if (!line.trimmed().startsWith("//") || line.startsWith("// SC2DH ")
                    || line.startsWith("// Generated by SC2 Data Helper Decoration Streaming."))
                    compactGalaxy += line + '\n';
            }
            result.patch.replacementEntries.insert(request.runtimeEntry, compactGalaxy);
        }
    }

    if (request.guiAction.enabled) {
        if (request.mode != DecorationOptimizationMode::RecreateActors) {
            result.error = QStringLiteral("GUI create action requires Galaxy recreation mode.");
            return result;
        }
        QByteArray triggers;
        if (archiveContainsEntry(archive, QStringLiteral("Triggers"))
            && !archive.readEntry(QStringLiteral("Triggers"), &triggers, &error)) {
            result.error = error;
            return result;
        }
        const auto gui = buildDecorGuiAction(triggers, result.patch.replacementEntries.value(request.mapScriptEntry),
                                             request.runtimeEntry, result.patch.artifacts.plan,
                                             request.galaxyOptions, request.guiAction);
        if (!gui.valid) { result.error = gui.error; return result; }
        result.patch.replacementEntries.insert(QStringLiteral("Triggers"), gui.triggers);
        result.patch.replacementEntries.insert(request.mapScriptEntry, gui.mapScript);
        QStringList locales;
        for (const QString &entry : archive.allEntries()) {
            const QString normalized = normalizedEntry(entry).toLower();
            if (normalized.endsWith(QStringLiteral("/localizeddata/triggerstrings.txt"))) locales << entry;
        }
        if (locales.isEmpty()) locales << QStringLiteral("enUS.SC2Data/LocalizedData/TriggerStrings.txt");
        QStringList keys = gui.triggerStrings.keys();
        std::sort(keys.begin(), keys.end());
        QJsonObject guiRollback;
        const auto rememberGuiEntry = [&](const QString &entry, const QByteArray &replacement) {
            const bool existed = archiveContainsEntry(archive, entry);
            QByteArray original;
            if (existed && !archive.readEntry(entry, &original, &error)) return false;
            guiRollback.insert(entry, QJsonObject{
                {QStringLiteral("existed"), existed},
                {QStringLiteral("original"), QString::fromLatin1(compressedBase64(original))},
                {QStringLiteral("originalSha256"), QString::fromLatin1(sha256Hex(original))},
                {QStringLiteral("generatedSha256"), QString::fromLatin1(sha256Hex(replacement))}});
            return true;
        };
        if (request.writeRestoreSidecars && !rememberGuiEntry(QStringLiteral("Triggers"), gui.triggers)) {
            result.error = error; return result;
        }
        for (const QString &entry : locales) {
            QByteArray strings;
            if (archiveContainsEntry(archive, entry) && !archive.readEntry(entry, &strings, &error)) {
                result.error = error; return result;
            }
            if (!strings.isEmpty() && !strings.endsWith('\n')) strings += '\n';
            for (const QString &key : keys)
                strings += key.toUtf8() + '=' + gui.triggerStrings.value(key).toUtf8() + '\n';
            result.patch.replacementEntries.insert(entry, strings);
            if (request.writeRestoreSidecars && !rememberGuiEntry(entry, strings)) {
                result.error = error; return result;
            }
        }
        if (request.writeRestoreSidecars) {
            auto manifest = QJsonDocument::fromJson(result.patch.replacementEntries.value(DecorRestoreManifestEntry)).object();
            manifest.insert(QStringLiteral("guiRollback"), guiRollback);
            manifest.insert(QStringLiteral("optimizedMapScriptSha256"), QString::fromLatin1(sha256Hex(gui.mapScript)));
            result.patch.replacementEntries.insert(DecorRestoreManifestEntry, QJsonDocument(manifest).toJson(QJsonDocument::Compact));
        }
        result.warnings << QStringLiteral("GUI action '%1' with Region preset '%2' was added; %3 existing Custom Script call(s) became GUI actions.")
                               .arg(request.guiAction.actionName, request.guiAction.presetName).arg(gui.convertedCalls);
    }

    if (request.mode == DecorationOptimizationMode::VisibilityOnly) {
        result.visibilityControlledDoodads = result.patch.visibilityArtifacts.controlledDoodadIndices.size();
    } else {
        result.removedDoodads = result.patch.artifacts.removedDoodadIndices.size();
        result.removedUnits = result.patch.artifacts.removedUnitIndices.size();
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

DecorRestoreMapResult DecorationMapCopyService::restoreGalaxyCopy(const DecorRestoreMapRequest &request) const
{
    DecorRestoreMapResult result;
    result.outputArchivePath = QFileInfo(request.outputArchivePath).absoluteFilePath();
    if (request.sourceArchivePath.isEmpty() || request.outputArchivePath.isEmpty()
        || samePath(request.sourceArchivePath, request.outputArchivePath)) {
        result.error = QStringLiteral("Choose distinct source and output map archives.");
        return result;
    }
    if (QFileInfo::exists(result.outputArchivePath)) {
        result.error = QStringLiteral("Restored output already exists; choose a new file name.");
        return result;
    }
    QByteArray sourceHashBefore;
    QString error;
    if (!fileSha256(request.sourceArchivePath, &sourceHashBefore, &error)) {
        result.error = error;
        return result;
    }
    Sc2Archive source;
    if (!source.load(request.sourceArchivePath, &error)) {
        result.error = error;
        return result;
    }
    const QString objectsEntry = request.objectsEntry;
    const QString runtimeEntry = request.runtimeEntry;
    QByteArray currentObjects, runtime;
    if (!source.readEntry(objectsEntry, &currentObjects, &error)
        || !source.readEntry(runtimeEntry, &runtime, &error)) {
        result.error = QStringLiteral("Missing Objects or SC2DataHelper Galaxy: %1").arg(error);
        return result;
    }
    DecorationStreamingPlanner planner;
    QString importGalaxy = QString::fromUtf8(runtime);
    if (archiveContainsEntry(source, DecorImportEntry)) {
        QByteArray metadata;
        pugi::xml_document doc;
        if (!source.readEntry(DecorImportEntry, &metadata, &error) || metadata.size() > 16 * 1024 * 1024
            || !doc.load_buffer(metadata.constData(), size_t(metadata.size()))
            || QString::fromUtf8(doc.child("SC2DHImport").attribute("Version").value()) != QStringLiteral("1")) {
            result.error = QStringLiteral("Invalid decoration import metadata; no copy created.");
            return result;
        }
        importGalaxy += QLatin1Char('\n') + QString::fromUtf8(doc.child("SC2DHImport").child("Annotations").text().get());
    }
    QByteArray originalObjects;
    if (!planner.restoreObjectsFromGalaxy(currentObjects, importGalaxy, &originalObjects,
                                          &result.restoredDoodads, &result.restoredUnits, &error)) {
        result.error = error;
        return result;
    }
    const QByteArray staticRuntime = planner.staticPlacementGalaxy(QString::fromUtf8(runtime)).toUtf8();
    QByteArray actorCatalog;
    const bool hasCatalog = source.readEntry(DecorActorCatalogEntry, &actorCatalog, nullptr);
    const QRegularExpression record(QStringLiteral("^// SC2DH catalog rollback ([01]) ([a-f0-9]{64})(?: ([A-Za-z0-9+/=]+))?$"),QRegularExpression::MultilineOption);
    const auto match = record.match(importGalaxy);
    const bool restoreCatalog = match.hasMatch();
    bool removeCatalog = false;
    if (hasCatalog && result.restoredDoodads > 0 && !restoreCatalog) {
        result.error = QStringLiteral("Galaxy actor catalog cleanup record is missing; no copy created.");
        return result;
    }
    if (restoreCatalog) {
        if (!hasCatalog || sha256Hex(actorCatalog) != match.captured(2).toLatin1()) {
            result.error = QStringLiteral("Runtime actor catalog changed or its Galaxy cleanup record is missing; no copy created.");
            return result;
        }
        QByteArray encodedCatalog = match.captured(3).toLatin1();
        if (encodedCatalog.isEmpty()) {
            const QRegularExpression part(QStringLiteral("^// SC2DH catalog data ([A-Za-z0-9+/=]+)$"), QRegularExpression::MultilineOption);
            auto matches = part.globalMatch(importGalaxy);
            while (matches.hasNext()) encodedCatalog += matches.next().captured(1).toLatin1();
        }
        actorCatalog = qUncompress(QByteArray::fromBase64(encodedCatalog));
        removeCatalog = match.captured(1) == QStringLiteral("0");
        if (!removeCatalog) {
            pugi::xml_document doc;
            if (!doc.load_buffer(actorCatalog.constData(),size_t(actorCatalog.size())) || !doc.child("Catalog")) {
                result.error = QStringLiteral("Invalid Galaxy catalog cleanup data."); return result;
            }
        } else if (!actorCatalog.isEmpty()) {
            result.error = QStringLiteral("Invalid absent-catalog cleanup data."); return result;
        }
    }
    QByteArray sourceHashAfter;
    if (!fileSha256(request.sourceArchivePath, &sourceHashAfter, &error)
        || sourceHashAfter != sourceHashBefore) {
        result.error = QStringLiteral("Source map changed during restore preflight.");
        return result;
    }
    QString stagingPath;
    if (!makeStagingArchivePath(result.outputArchivePath, &stagingPath, &error)) {
        result.error = error;
        return result;
    }
    const auto discardStaging = [&] {
        QFile::remove(stagingPath);
        QFile::remove(stagingPath + QStringLiteral(".compact"));
    };
    QHash<QString, QByteArray> replacements{{objectsEntry, originalObjects}, {runtimeEntry, staticRuntime}};
    QStringList removals;
    if (archiveContainsEntry(source, DecorImportEntry)) removals << DecorImportEntry;
    if (archiveContainsEntry(source, DecorRestoreManifestEntry)) removals << DecorRestoreManifestEntry;
    if (restoreCatalog) {
        if (removeCatalog) removals << DecorActorCatalogEntry;
        else replacements.insert(DecorActorCatalogEntry, actorCatalog);
    }
    if (!restorePreloadPatch(source, importGalaxy, &replacements, &removals, &error)
        || !restorePreloadPatch(source, importGalaxy, &replacements, &removals, &error, DecorModelCatalogEntry, QStringLiteral("models"), "Catalog")) {
        discardStaging(); result.error = error; return result;
    }
    if (!source.saveCopy(stagingPath, replacements,
                         removals, &error)) {
        discardStaging();
        result.error = QStringLiteral("Could not stage restored map: %1").arg(error);
        return result;
    }
    Sc2Archive verification;
    QByteArray verifiedObjects;
    QByteArray verifiedRuntime;
    if (!verification.load(stagingPath, &error)
        || !verification.readEntry(objectsEntry, &verifiedObjects, &error)
        || !verification.readEntry(runtimeEntry, &verifiedRuntime, &error)
        || verifiedObjects != originalObjects || verifiedRuntime != staticRuntime
        || !verifyPreloadPatch(verification, replacements, removals)
        || !verifyPreloadPatch(verification, replacements, removals, DecorModelCatalogEntry)
        || archiveContainsEntry(verification, DecorRestoreManifestEntry)) {
        discardStaging();
        result.error = QStringLiteral("Galaxy import failed staged archive verification: %1").arg(error);
        return result;
    }
    if (hasCatalog) {
        QByteArray verifiedCatalog;
        const bool catalogMatches = removeCatalog
            ? !archiveContainsEntry(verification, DecorActorCatalogEntry)
            : verification.readEntry(DecorActorCatalogEntry, &verifiedCatalog, &error) && verifiedCatalog == actorCatalog;
        if (!catalogMatches) {
            discardStaging(); result.error = QStringLiteral("Catalog verification failed."); return result;
        }
    }
    DecorOptimizedMapResult analysis;
    if (!verifyArchiveWithFolderAnalysis(verification, &analysis, &error)) {
        discardStaging();
        result.error = QStringLiteral("Restored map failed full analysis: %1").arg(error);
        return result;
    }
    QByteArray stagedHash;
    if (!fileSha256(stagingPath, &stagedHash, &error)
        || !fileSha256(request.sourceArchivePath, &sourceHashAfter, &error)
        || sourceHashAfter != sourceHashBefore || QFileInfo::exists(result.outputArchivePath)) {
        discardStaging();
        result.error = QStringLiteral("Source or output changed during restore verification; no output was committed.");
        return result;
    }
    if (!replaceFileAtomicallyAndVerify(stagingPath, result.outputArchivePath, stagedHash, &error)) {
        discardStaging();
        result.error = QStringLiteral("Could not commit restored map: %1").arg(error);
        return result;
    }
    discardStaging();
    if (!fileSha256(request.sourceArchivePath, &sourceHashAfter, &error)
        || sourceHashAfter != sourceHashBefore) {
        QFile::remove(result.outputArchivePath);
        result.error = QStringLiteral("Source changed during final restore check; restored output was removed.");
        return result;
    }
    result.sourceUnchanged = true;
    result.success = true;
    return result;
}

DecorRestoreMapResult DecorationMapCopyService::restoreOptimizedCopy(const DecorRestoreMapRequest &request) const
{
    DecorRestoreMapResult result;
    result.outputArchivePath = QFileInfo(request.outputArchivePath).absoluteFilePath();
    if (request.sourceArchivePath.isEmpty() || request.outputArchivePath.isEmpty()
        || samePath(request.sourceArchivePath, request.outputArchivePath)) {
        result.error = QStringLiteral("Choose distinct source and output map archives.");
        return result;
    }
    if (QFileInfo::exists(result.outputArchivePath)) {
        result.error = QStringLiteral("Restored output already exists; choose a new file name.");
        return result;
    }
    QByteArray sourceHashBefore;
    QString error;
    if (!fileSha256(request.sourceArchivePath, &sourceHashBefore, &error)) {
        result.error = error;
        return result;
    }
    Sc2Archive source;
    if (!source.load(request.sourceArchivePath, &error)) {
        result.error = error;
        return result;
    }
    QByteArray manifestBytes;
    if (!source.readEntry(DecorRestoreManifestEntry, &manifestBytes, &error)) {
        result.error = QStringLiteral("This map has no SC2DataHelper decor restore data. Only maps created with the new reversible export can be restored.");
        return result;
    }
    const QJsonDocument document = QJsonDocument::fromJson(manifestBytes);
    if (!document.isObject()) {
        result.error = QStringLiteral("Decor restore data is malformed.");
        return result;
    }
    const QJsonObject manifest = document.object();
    if (manifest.value(QStringLiteral("format")).toString() != QStringLiteral("SC2DH-DecorRestore")
        || manifest.value(QStringLiteral("version")).toInt() != 1) {
        result.error = QStringLiteral("Unsupported decor restore data version.");
        return result;
    }
    const QString objectsEntry = manifest.value(QStringLiteral("objectsEntry")).toString();
    const QString mapScriptEntry = manifest.value(QStringLiteral("mapScriptEntry")).toString();
    const QString runtimeEntry = manifest.value(QStringLiteral("runtimeEntry")).toString();
    const QString actorCatalogEntry = manifest.value(QStringLiteral("actorCatalogEntry")).toString();
    const bool originalActorCatalogExisted = manifest.value(QStringLiteral("originalActorCatalogExisted")).toBool();
    const QByteArray originalActorCatalog = restoredBytes(manifest, QStringLiteral("originalActorCatalog"));
    if (objectsEntry.isEmpty() || mapScriptEntry.isEmpty() || runtimeEntry.isEmpty()
        || objectsEntry == mapScriptEntry || objectsEntry == runtimeEntry || mapScriptEntry == runtimeEntry
        || objectsEntry == DecorRestoreManifestEntry || mapScriptEntry == DecorRestoreManifestEntry
        || runtimeEntry == DecorRestoreManifestEntry) {
        result.error = QStringLiteral("Decor restore data contains invalid archive entry names.");
        return result;
    }
    if (!actorCatalogEntry.isEmpty()) {
        QByteArray current;
        if (normalizedEntry(actorCatalogEntry).compare(DecorActorCatalogEntry, Qt::CaseInsensitive) != 0
            || sha256Hex(originalActorCatalog) != manifest.value(QStringLiteral("originalActorCatalogSha256")).toString().toLatin1()
            || (originalActorCatalogExisted && originalActorCatalog.isEmpty())
            || !source.readEntry(actorCatalogEntry, &current, &error)
            || sha256Hex(current) != manifest.value(QStringLiteral("optimizedActorCatalogSha256")).toString().toLatin1()) {
            result.error = QStringLiteral("Actor catalog restore data is invalid or ActorData.xml changed after Galaxy export; no copy was created.");
            return result;
        }
    }
    const QByteArray originalObjects = restoredBytes(manifest, QStringLiteral("originalObjects"));
    const QByteArray originalMapScript = restoredBytes(manifest, QStringLiteral("originalMapScript"));
    if (originalObjects.isEmpty() || originalMapScript.isEmpty()
        || sha256Hex(originalObjects) != manifest.value(QStringLiteral("originalObjectsSha256")).toString().toLatin1()
        || sha256Hex(originalMapScript) != manifest.value(QStringLiteral("originalMapScriptSha256")).toString().toLatin1()) {
        result.error = QStringLiteral("Saved original Objects or MapScript failed integrity verification.");
        return result;
    }
    for (const auto &entry : {qMakePair(objectsEntry, QStringLiteral("optimizedObjectsSha256")),
                              qMakePair(mapScriptEntry, QStringLiteral("optimizedMapScriptSha256")),
                              qMakePair(runtimeEntry, QStringLiteral("runtimeSha256"))}) {
        QByteArray current;
        if (!source.readEntry(entry.first, &current, &error)
            || sha256Hex(current) != manifest.value(entry.second).toString().toLatin1()) {
            result.error = QStringLiteral("%1 changed after Galaxy export; reverse conversion would discard edits. No copy was created.")
                               .arg(entry.first);
            return result;
        }
    }
    QByteArray sourceHashAfter;
    if (!fileSha256(request.sourceArchivePath, &sourceHashAfter, &error)
        || sourceHashAfter != sourceHashBefore) {
        result.error = QStringLiteral("Source map changed during restore preflight.");
        return result;
    }
    QString stagingPath;
    if (!makeStagingArchivePath(result.outputArchivePath, &stagingPath, &error)) {
        result.error = error;
        return result;
    }
    const auto discardStaging = [&] {
        QFile::remove(stagingPath);
        QFile::remove(stagingPath + QStringLiteral(".compact"));
    };
    QHash<QString, QByteArray> replacements{{objectsEntry, originalObjects},
                                                   {mapScriptEntry, originalMapScript}};
    QStringList removals{runtimeEntry, DecorRestoreManifestEntry};
    const auto guiRollback = manifest.value(QStringLiteral("guiRollback")).toObject();
    for (auto it = guiRollback.begin(); it != guiRollback.end(); ++it) {
        const QString normalized = normalizedEntry(it.key()).toLower();
        if (normalized != QStringLiteral("triggers")
            && !normalized.endsWith(QStringLiteral("/localizeddata/triggerstrings.txt"))) {
            discardStaging(); result.error = QStringLiteral("Invalid GUI rollback entry."); return result;
        }
        const auto record = it.value().toObject();
        QByteArray generated;
        if (!source.readEntry(it.key(), &generated, &error)
            || sha256Hex(generated) != record.value(QStringLiteral("generatedSha256")).toString().toLatin1()) {
            discardStaging(); result.error = QStringLiteral("GUI trigger data changed after export; exact rollback refused."); return result;
        }
        const QByteArray compressed = QByteArray::fromBase64(record.value(QStringLiteral("original")).toString().toLatin1());
        if (compressed.size() < 4
            || (quint32(uchar(compressed[0])) << 24 | quint32(uchar(compressed[1])) << 16
                | quint32(uchar(compressed[2])) << 8 | quint32(uchar(compressed[3]))) > 64 * 1024 * 1024) {
            discardStaging(); result.error = QStringLiteral("Invalid GUI rollback payload."); return result;
        }
        const QByteArray original = qUncompress(compressed);
        if (sha256Hex(original) != record.value(QStringLiteral("originalSha256")).toString().toLatin1()) {
            discardStaging(); result.error = QStringLiteral("GUI rollback checksum mismatch."); return result;
        }
        if (record.value(QStringLiteral("existed")).toBool()) replacements.insert(it.key(), original);
        else if (original.isEmpty()) removals << it.key();
        else { discardStaging(); result.error = QStringLiteral("Invalid absent GUI entry rollback."); return result; }
    }
    if (archiveContainsEntry(source, DecorImportEntry)) removals << DecorImportEntry;
    if (!actorCatalogEntry.isEmpty()) {
        if (originalActorCatalogExisted) replacements.insert(actorCatalogEntry, originalActorCatalog);
        else removals << actorCatalogEntry;
    }
    QByteArray runtimeForPreload;
    if (!source.readEntry(runtimeEntry, &runtimeForPreload, &error)
        || !restorePreloadPatch(source, QString::fromUtf8(runtimeForPreload), &replacements, &removals, &error)
        || !restorePreloadPatch(source, QString::fromUtf8(runtimeForPreload), &replacements, &removals, &error, DecorModelCatalogEntry, QStringLiteral("models"), "Catalog")) {
        discardStaging(); result.error = error; return result;
    }
    if (!source.saveCopy(stagingPath, replacements,
                         removals, &error)) {
        discardStaging();
        result.error = QStringLiteral("Could not stage restored map: %1").arg(error);
        return result;
    }
    Sc2Archive verification;
    QByteArray verifiedObjects;
    QByteArray verifiedScript;
    if (!verification.load(stagingPath, &error)
        || !verification.readEntry(objectsEntry, &verifiedObjects, &error)
        || !verification.readEntry(mapScriptEntry, &verifiedScript, &error)
        || verifiedObjects != originalObjects || verifiedScript != originalMapScript
        || !verifyPreloadPatch(verification, replacements, removals)
        || !verifyPreloadPatch(verification, replacements, removals, DecorModelCatalogEntry)
        || archiveContainsEntry(verification, runtimeEntry)
        || archiveContainsEntry(verification, DecorRestoreManifestEntry)) {
        discardStaging();
        result.error = QStringLiteral("Staged reverse conversion failed archive verification: %1").arg(error);
        return result;
    }
    if (!actorCatalogEntry.isEmpty()) {
        QByteArray verifiedCatalog;
        const bool catalogMatches = originalActorCatalogExisted
            ? verification.readEntry(actorCatalogEntry, &verifiedCatalog, &error) && verifiedCatalog == originalActorCatalog
            : !archiveContainsEntry(verification, actorCatalogEntry);
        if (!catalogMatches) {
            discardStaging();
            result.error = QStringLiteral("Restored actor catalog failed archive verification.");
            return result;
        }
    }
    DecorOptimizedMapResult analysis;
    for (auto it = guiRollback.begin(); it != guiRollback.end(); ++it) {
        QByteArray verified;
        const bool exists = verification.readEntry(it.key(), &verified, nullptr);
        if ((replacements.contains(it.key()) && (!exists || verified != replacements.value(it.key())))
            || (removals.contains(it.key()) && exists)) {
            discardStaging(); result.error = QStringLiteral("GUI rollback verification failed."); return result;
        }
    }
    if (!verifyArchiveWithFolderAnalysis(verification, &analysis, &error)) {
        discardStaging();
        result.error = QStringLiteral("Restored map failed full analysis: %1").arg(error);
        return result;
    }
    QByteArray stagedHash;
    if (!fileSha256(stagingPath, &stagedHash, &error)
        || !fileSha256(request.sourceArchivePath, &sourceHashAfter, &error)
        || sourceHashAfter != sourceHashBefore || QFileInfo::exists(result.outputArchivePath)) {
        discardStaging();
        result.error = QStringLiteral("Source or output changed during restore verification; no output was committed.");
        return result;
    }
    if (!replaceFileAtomicallyAndVerify(stagingPath, result.outputArchivePath, stagedHash, &error)) {
        discardStaging();
        result.error = QStringLiteral("Could not commit restored map: %1").arg(error);
        return result;
    }
    discardStaging();
    if (!fileSha256(request.sourceArchivePath, &sourceHashAfter, &error)
        || sourceHashAfter != sourceHashBefore) {
        QFile::remove(result.outputArchivePath);
        result.error = QStringLiteral("Source changed during final restore check; restored output was removed.");
        return result;
    }
    result.sourceUnchanged = true;
    result.restoredDoodads = manifest.value(QStringLiteral("removedDoodads")).toInt();
    result.restoredUnits = manifest.value(QStringLiteral("removedUnits")).toInt();
    result.success = true;
    return result;
}

} // namespace sc2dh::decor
