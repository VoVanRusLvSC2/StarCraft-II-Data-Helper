#include "core/DecorationMapCopyService.h"

#include "core/Sc2Archive.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

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
        || fileName == QStringLiteral("mapinfo")
        || fileName == QStringLiteral("documentinfo")
        || fileName == QStringLiteral("preloadassetdb.txt");
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
                           const QSet<QString> &phraseKeys)
{
    auto matches = tokenExpression().globalMatch(text);
    while (matches.hasNext()) {
        const QString token = matches.next().captured(1).toCaseFolded();
        if (tokenKeys.contains(token))
            addReference(context, token, entryName);
    }

    const QString foldedText = text.toCaseFolded();
    for (const QString &key : phraseKeys) {
        if (foldedText.contains(key))
            addReference(context, key, entryName);
    }
}

sc2dh::decor::DecorationSafetyContext buildArchiveSafetyContext(const Sc2Archive &archive,
                                                                const QByteArray &objectsBytes,
                                                                const sc2dh::decor::DecorOptimizedMapRequest &request,
                                                                QStringList *warnings,
                                                                QString *error)
{
    sc2dh::decor::DecorationSafetyContext context;
    sc2dh::decor::DecorationStreamingPlanner planner;
    const QVector<sc2dh::decor::DoodadPlacement> doodads = planner.parseObjects(objectsBytes);

    QSet<QString> tokenKeys;
    QSet<QString> phraseKeys;
    for (const sc2dh::decor::DoodadPlacement &doodad : doodads) {
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
    if (tokenKeys.isEmpty() && phraseKeys.isEmpty())
        return context;

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
            addReferencesForAllKeys(&context, tokenKeys, phraseKeys, entry);
            if (warnings)
                *warnings << QStringLiteral("Decoration safety: treated all doodads as referenced by undecodable text entry %1.")
                                 .arg(entry);
            continue;
        }
        scanTextForDoodadKeys(&context, entry, text, tokenKeys, phraseKeys);
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
    if (QFileInfo::exists(result.outputArchivePath)) {
        if (!request.overwriteExisting) {
            result.error = QStringLiteral("Output archive already exists.");
            return result;
        }
        if (!QFile::remove(result.outputArchivePath)) {
            result.error = QStringLiteral("Unable to remove existing output archive.");
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
    result.patch = planner.prepareArchivePatch(objectsBytes,
                                               mapScriptBytes,
                                               request.zones,
                                               safetyContext,
                                               request.galaxyOptions,
                                               request.objectsEntry,
                                               request.mapScriptEntry,
                                               request.runtimeEntry);
    result.warnings += result.patch.warnings;
    if (!result.patch.valid) {
        result.error = result.patch.error;
        return result;
    }

    if (!archive.saveCopy(result.outputArchivePath, result.patch.replacementEntries, {}, &error)) {
        QFile::remove(result.outputArchivePath);
        result.error = QStringLiteral("Unable to save decoration optimized copy: %1").arg(error);
        return result;
    }

    Sc2Archive verification;
    if (!verification.load(result.outputArchivePath, &error)) {
        QFile::remove(result.outputArchivePath);
        result.error = QStringLiteral("Decoration optimized copy verification failed: %1").arg(error);
        return result;
    }
    for (auto it = result.patch.replacementEntries.cbegin(); it != result.patch.replacementEntries.cend(); ++it) {
        QByteArray actual;
        if (!verification.readEntry(it.key(), &actual, &error)) {
            QFile::remove(result.outputArchivePath);
            result.error = QStringLiteral("Decoration optimized copy is missing %1: %2").arg(it.key(), error);
            return result;
        }
        if (actual != it.value()) {
            QFile::remove(result.outputArchivePath);
            result.error = QStringLiteral("Decoration optimized copy verification failed for %1.").arg(it.key());
            return result;
        }
    }

    result.removedDoodads = result.patch.artifacts.removedDoodadIndices.size();
    result.success = true;
    return result;
}

} // namespace sc2dh::decor
