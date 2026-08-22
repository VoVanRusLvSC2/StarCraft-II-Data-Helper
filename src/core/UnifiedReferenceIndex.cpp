#include "core/UnifiedReferenceIndex.h"

#include "core/AssetFileRules.h"
#include "core/ScannedFileReader.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace
{

QString foldedKey(const QString &value)
{
    return value.trimmed().toCaseFolded();
}

bool isSc2TokenBoundary(const QChar ch)
{
    return !(ch.isLetterOrNumber() || ch == QLatin1Char('_'));
}

QRegularExpression tokenExpression(const QString &token)
{
    return QRegularExpression(QStringLiteral("(?<![A-Za-z0-9_])%1(?![A-Za-z0-9_])")
                                  .arg(QRegularExpression::escape(token)),
                              QRegularExpression::CaseInsensitiveOption);
}

int lineNumberAt(const QString &text, qsizetype offset)
{
    if (offset < 0)
        return -1;
    int line = 1;
    const qsizetype bounded = std::min(offset, text.size());
    for (qsizetype i = 0; i < bounded; ++i) {
        if (text.at(i) == QLatin1Char('\n'))
            ++line;
    }
    return line;
}

bool isSafeTextFile(const QString &relativePath, const ScannedFileInfo &file)
{
    const QString normalized = relativePath.toLower();
    const QString suffix = QFileInfo(relativePath).suffix().toLower();
    if (file.isXml || file.isSc2DataLike)
        return true;
    if (normalized == QStringLiteral("objects")
        || normalized.endsWith(QStringLiteral("/objects"))
        || normalized.contains(QStringLiteral("gamestrings"))
        || normalized.contains(QStringLiteral("objectstrings"))
        || normalized.contains(QStringLiteral("preload"))
        || normalized.contains(QStringLiteral("mapscript"))
        || normalized.contains(QStringLiteral("triggerlibs"))
        || normalized.contains(QStringLiteral(".sc2lib")))
        return true;
    static const QSet<QString> suffixes = {
        QStringLiteral("galaxy"),
        QStringLiteral("xml"),
        QStringLiteral("txt"),
        QStringLiteral("layout"),
        QStringLiteral("sc2layout"),
        QStringLiteral("trigger"),
        QStringLiteral("strings")
    };
    return suffixes.contains(suffix);
}

bool isPlacementFile(const QString &relativePath)
{
    const QString normalized = relativePath.trimmed().replace('\\', '/');
    return normalized.compare(QStringLiteral("Objects"), Qt::CaseInsensitive) == 0
        || normalized.endsWith(QStringLiteral("/Objects"), Qt::CaseInsensitive);
}

sc2dh::refs::ReferenceKind textKindForPath(const QString &relativePath)
{
    const QString normalized = relativePath.toLower();
    if (isPlacementFile(relativePath))
        return sc2dh::refs::ReferenceKind::PlacementRoot;
    if (normalized.contains(QStringLiteral("mapscript"))
        || normalized.contains(QStringLiteral("triggerlibs"))
        || normalized.contains(QStringLiteral(".sc2lib"))
        || normalized.endsWith(QStringLiteral(".galaxy"))
        || normalized.endsWith(QStringLiteral(".trigger")))
        return sc2dh::refs::ReferenceKind::ScriptText;
    return sc2dh::refs::ReferenceKind::WeakText;
}

bool isStrongTextPath(const QString &relativePath)
{
    const sc2dh::refs::ReferenceKind kind = textKindForPath(relativePath);
    return kind == sc2dh::refs::ReferenceKind::PlacementRoot
        || kind == sc2dh::refs::ReferenceKind::ScriptText;
}

QString unquote(QString value)
{
    value = value.trimmed();
    if (value.size() >= 2 && value.front() == QLatin1Char('"') && value.back() == QLatin1Char('"'))
        return value.mid(1, value.size() - 2);
    return value;
}

QStringList placementTargets(const QString &text, qsizetype *lastOffset = nullptr)
{
    QStringList targets;
    static const QRegularExpression field(QStringLiteral("\\b(?:Type|Unit|Doodad)\\b\\s*(?:=|:)\\s*(\"[^\"]*\"|[^\\s,;}]+)"),
                                          QRegularExpression::CaseInsensitiveOption);
    auto matches = field.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        targets << unquote(match.captured(1));
        if (lastOffset)
            *lastOffset = match.capturedStart(1);
    }
    targets.removeAll(QString());
    targets.removeDuplicates();
    return targets;
}

bool binaryContainsToken(const QByteArray &bytes, const QByteArray &needle)
{
    if (needle.isEmpty())
        return false;
    qsizetype pos = 0;
    while ((pos = bytes.indexOf(needle, pos)) >= 0) {
        const bool leftOk = pos == 0 || isSc2TokenBoundary(QChar::fromLatin1(bytes.at(pos - 1)));
        const qsizetype rightPos = pos + needle.size();
        const bool rightOk = rightPos >= bytes.size() || isSc2TokenBoundary(QChar::fromLatin1(bytes.at(rightPos)));
        if (leftOk && rightOk)
            return true;
        ++pos;
    }
    return false;
}

} // namespace

namespace sc2dh::refs
{

void UnifiedReferenceIndex::build(const AnalysisResult &analysis)
{
    m_records.clear();
    m_byId.clear();
    m_byAsset.clear();

    QSet<QString> knownIdKeys;
    QHash<QString, QString> canonicalIdByKey;
    for (const DataNode &node : analysis.nodes) {
        if (node.id.isEmpty())
            continue;
        const QString key = foldedKey(node.id);
        knownIdKeys.insert(key);
        canonicalIdByKey.insert(key, node.id);
    }

    QHash<QString, qint64> assetSizes;
    QHash<QString, QString> canonicalAssetByKey;
    for (const ScannedFileInfo &file : analysis.scannedFiles) {
        const QString relative = ScannedFileReader::relativePath(analysis.rootFolder, file.filePath);
        const QFileInfo info(relative);
        if (!sc2dh::asset::isAssetFile(info, relative))
            continue;
        QString normalized = relative;
        normalized.replace('\\', '/');
        canonicalAssetByKey.insert(foldedKey(normalized), normalized);
        canonicalAssetByKey.insert(foldedKey(info.fileName()), normalized);
        assetSizes.insert(normalized, file.size);
    }

    for (const DataNode &node : analysis.nodes) {
        for (const QString &reference : node.referencedIds) {
            if (reference.trimmed().isEmpty())
                continue;
            ReferenceRecord record;
            record.kind = ReferenceKind::TypedXml;
            record.strength = ReferenceStrength::Strong;
            record.targetId = reference.trimmed();
            record.sourceId = node.id;
            record.sourceType = node.elementName;
            record.sourceFile = node.sourceFile;
            record.lineNumber = node.lineNumber;
            record.detail = QStringLiteral("typed XML/catalog reference");
            record.rewritable = true;
            addRecord(record);
        }
    }

    ScannedFileReader reader(analysis);
    for (const ScannedFileInfo &file : analysis.scannedFiles) {
        const QString relative = ScannedFileReader::relativePath(analysis.rootFolder, file.filePath).replace('\\', '/');
        const bool text = isSafeTextFile(relative, file);

        QByteArray bytes;
        if (!reader.readBytes(file, 16ll * 1024ll * 1024ll, &bytes))
            continue;

        if (!text) {
            for (const QString &idKey : knownIdKeys) {
                const QString canonical = canonicalIdByKey.value(idKey);
                if (!binaryContainsToken(bytes, canonical.toUtf8()))
                    continue;
                ReferenceRecord record;
                record.kind = ReferenceKind::BinaryUnconfirmed;
                record.strength = ReferenceStrength::Blocking;
                record.targetId = canonical;
                record.sourceFile = relative;
                record.detail = QStringLiteral("binary file contains an ID token but cannot be safely rewritten");
                record.rewritable = false;
                addRecord(record);
            }
            continue;
        }

        const QString content = QString::fromUtf8(bytes);
        if (isPlacementFile(relative)) {
            qsizetype lastOffset = -1;
            for (const QString &target : placementTargets(content, &lastOffset)) {
                const QString key = foldedKey(target);
                if (!knownIdKeys.contains(key))
                    continue;
                ReferenceRecord record;
                record.kind = ReferenceKind::PlacementRoot;
                record.strength = ReferenceStrength::Strong;
                record.targetId = canonicalIdByKey.value(key, target);
                record.sourceFile = relative;
                record.lineNumber = lineNumberAt(content, lastOffset);
                record.detail = QStringLiteral("Objects placement/runtime root");
                record.rewritable = true;
                addRecord(record);
            }
        }

        const ReferenceKind textKind = textKindForPath(relative);
        const bool strongText = isStrongTextPath(relative);
        for (const QString &idKey : knownIdKeys) {
            const QString canonical = canonicalIdByKey.value(idKey);
            const QRegularExpression expression = tokenExpression(canonical);
            const QRegularExpressionMatch match = expression.match(content);
            if (!match.hasMatch())
                continue;
            ReferenceRecord record;
            record.kind = textKind;
            record.strength = strongText ? ReferenceStrength::Strong : ReferenceStrength::Weak;
            record.targetId = canonical;
            record.sourceFile = relative;
            record.lineNumber = lineNumberAt(content, match.capturedStart());
            record.detail = strongText ? QStringLiteral("token-aware text reference")
                                       : QStringLiteral("weak token-aware text match");
            record.rewritable = strongText;
            addRecord(record);
        }

        for (auto it = canonicalAssetByKey.cbegin(); it != canonicalAssetByKey.cend(); ++it) {
            const QString token = it.key();
            if (token.isEmpty())
                continue;
            const QRegularExpression expression = tokenExpression(token);
            const QRegularExpressionMatch match = expression.match(content);
            if (!match.hasMatch())
                continue;
            ReferenceRecord record;
            record.kind = ReferenceKind::AssetText;
            record.strength = ReferenceStrength::Strong;
            record.targetAsset = it.value();
            record.sourceFile = relative;
            record.lineNumber = lineNumberAt(content, match.capturedStart());
            record.detail = QStringLiteral("asset path/name text reference");
            record.rewritable = true;
            addRecord(record);
        }
    }
}

void UnifiedReferenceIndex::addRecord(const ReferenceRecord &record)
{
    const int index = m_records.size();
    m_records << record;
    if (!record.targetId.isEmpty())
        m_byId[foldedKey(record.targetId)] << index;
    if (!record.targetAsset.isEmpty())
        m_byAsset[foldedKey(record.targetAsset)] << index;
}

QVector<ReferenceRecord> UnifiedReferenceIndex::referencesToId(const QString &id) const
{
    QVector<ReferenceRecord> out;
    for (int index : m_byId.value(foldedKey(id)))
        out << m_records.at(index);
    return out;
}

QVector<ReferenceRecord> UnifiedReferenceIndex::referencesToAsset(const QString &assetPath) const
{
    QVector<ReferenceRecord> out;
    for (int index : m_byAsset.value(foldedKey(assetPath)))
        out << m_records.at(index);
    return out;
}

QVector<ReferenceRecord> UnifiedReferenceIndex::strongReferencesToId(const QString &id) const
{
    QVector<ReferenceRecord> out;
    for (const ReferenceRecord &record : referencesToId(id)) {
        if (record.strength == ReferenceStrength::Strong || record.strength == ReferenceStrength::Blocking)
            out << record;
    }
    return out;
}

bool UnifiedReferenceIndex::hasNonRewritableStrongReferenceToId(const QString &id) const
{
    for (const ReferenceRecord &record : strongReferencesToId(id)) {
        if (!record.rewritable)
            return true;
    }
    return false;
}

QString referenceKindName(ReferenceKind kind)
{
    switch (kind) {
    case ReferenceKind::TypedXml:
        return QStringLiteral("typed-xml");
    case ReferenceKind::ScriptText:
        return QStringLiteral("script-text");
    case ReferenceKind::PlacementRoot:
        return QStringLiteral("placement-root");
    case ReferenceKind::AssetText:
        return QStringLiteral("asset-text");
    case ReferenceKind::WeakText:
        return QStringLiteral("weak-text");
    case ReferenceKind::BinaryUnconfirmed:
        return QStringLiteral("binary-unconfirmed");
    }
    return QStringLiteral("weak-text");
}

QString referenceStrengthName(ReferenceStrength strength)
{
    switch (strength) {
    case ReferenceStrength::Strong:
        return QStringLiteral("strong");
    case ReferenceStrength::Weak:
        return QStringLiteral("weak");
    case ReferenceStrength::Blocking:
        return QStringLiteral("blocking");
    }
    return QStringLiteral("weak");
}

} // namespace sc2dh::refs
