#include "core/AssetReferenceScanner.h"

#include "core/AssetFileRules.h"
#include "core/ScannedFileReader.h"

#include <QFileInfo>
#include <QSet>
#include <QStringList>

namespace
{
bool isPrintableAscii(uchar value)
{
    return value >= 0x20 && value <= 0x7e;
}
}

QString AssetReferenceScanner::buildCorpus(const AnalysisResult &analysis) const
{
    QStringList parts;
    for (const QString &xml : analysis.sourceXmlByFile)
        parts << xml;

    ScannedFileReader reader(analysis);
    for (const ScannedFileInfo &file : analysis.scannedFiles) {
        if (!file.isSc2DataLike || file.isXml || file.size > 4 * 1024 * 1024)
            continue;
        const QString text = reader.readText(file, 4 * 1024 * 1024);
        if (!text.isEmpty())
            parts << text;
    }

    for (const ScannedFileInfo &file : analysis.scannedFiles) {
        const QString rel = ScannedFileReader::relativePath(analysis.rootFolder, file.filePath);
        if (!isBinaryAssetReferenceSource(rel) || file.size > 32 * 1024 * 1024)
            continue;
        QByteArray bytes;
        if (!reader.readBytes(file, 32 * 1024 * 1024, &bytes))
            continue;
        const QString extracted = extractPrintableAssetReferences(bytes);
        if (!extracted.isEmpty())
            parts << extracted;
    }

    return parts.join(QLatin1Char('\n'));
}

QString AssetReferenceScanner::extractPrintableAssetReferences(const QByteArray &bytes)
{
    QStringList strings;
    QByteArray current;
    current.reserve(128);
    auto flushAscii = [&] {
        if (current.size() >= 4) {
            const QString text = QString::fromLatin1(current);
            if (stringLooksLikeAssetReference(text))
                strings << text;
        }
        current.clear();
    };
    for (uchar value : bytes) {
        if (isPrintableAscii(value)) {
            if (current.size() < 512)
                current.append(char(value));
        } else {
            flushAscii();
        }
    }
    flushAscii();

    QString utf16;
    utf16.reserve(128);
    auto flushUtf16 = [&] {
        if (utf16.size() >= 4 && stringLooksLikeAssetReference(utf16))
            strings << utf16;
        utf16.clear();
    };
    for (int i = 0; i + 1 < bytes.size(); i += 2) {
        const uchar low = uchar(bytes.at(i));
        const uchar high = uchar(bytes.at(i + 1));
        if (high == 0 && isPrintableAscii(low)) {
            if (utf16.size() < 512)
                utf16.append(QChar(low));
        } else {
            flushUtf16();
        }
    }
    flushUtf16();

    strings.removeDuplicates();
    return strings.join(QLatin1Char('\n'));
}

bool AssetReferenceScanner::stringLooksLikeAssetReference(const QString &value)
{
    const QString lower = value.toLower();
    static const QStringList markers = {
        QStringLiteral(".dds"), QStringLiteral(".tga"), QStringLiteral(".png"), QStringLiteral(".jpg"),
        QStringLiteral(".jpeg"), QStringLiteral(".bmp"), QStringLiteral(".m3"), QStringLiteral(".ogg"),
        QStringLiteral(".wav"), QStringLiteral(".mp3"), QStringLiteral(".webm"), QStringLiteral(".mp4"),
        QStringLiteral(".sc2layout"), QStringLiteral(".layout")
    };
    for (const QString &marker : markers)
        if (lower.contains(marker))
            return true;
    return (lower.contains(QLatin1Char('/')) || lower.contains(QLatin1Char('\\')))
        && (lower.contains(QStringLiteral("assets")) || lower.contains(QStringLiteral("textures"))
            || lower.contains(QStringLiteral("models")) || lower.contains(QStringLiteral("sounds"))
            || lower.contains(QStringLiteral("ui")));
}

bool AssetReferenceScanner::isBinaryAssetReferenceSource(const QString &relative)
{
    const QString suffix = QFileInfo(relative).suffix().toLower();
    static const QSet<QString> extensions = {
        QStringLiteral("m3"), QStringLiteral("m3a"), QStringLiteral("m3h"), QStringLiteral("m3skl")
    };
    return extensions.contains(suffix) && !sc2dh::asset::isBackupOrTrashName(relative);
}
