#include "core/UnifiedReferenceIndex.h"

#include "core/AssetFileRules.h"
#include "core/ScannedFileReader.h"

#include <QFileInfo>
#include <QHash>
#include <QQueue>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace
{

QString foldedKey(const QString &value)
{
    const QString trimmed = value.trimmed();
    QString folded;
    folded.reserve(trimmed.size());
    for (const QChar character : trimmed)
        folded.append(character.toCaseFolded());
    return folded;
}

QString foldedCharacters(const QString &value)
{
    QString folded;
    folded.reserve(value.size());
    for (const QChar character : value)
        folded.append(character.toCaseFolded());
    return folded;
}

bool isSc2TokenBoundary(const QChar ch)
{
    return !(ch.isLetterOrNumber() || ch == QLatin1Char('_'));
}

class TokenSetMatcher
{
public:
    // first = text to scan for, second = stable key returned to the caller.
    explicit TokenSetMatcher(const QVector<QPair<QString, QString>> &patterns)
    {
        m_nodes.append(Node{});
        QSet<QString> seen;
        for (const auto &pattern : patterns) {
            if (pattern.first.isEmpty() || seen.contains(pattern.first))
                continue;
            seen.insert(pattern.first);
            const int index = m_patterns.size();
            m_patterns.append(pattern);
            int state = 0;
            for (const QChar character : pattern.first) {
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
            m_nodes[state].outputs.append(index);
        }

        QQueue<int> queue;
        for (auto edge = m_nodes[0].edges.cbegin(); edge != m_nodes[0].edges.cend(); ++edge)
            queue.enqueue(edge.value());
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

    QHash<QString, qsizetype> firstMatches(const QString &text) const
    {
        QHash<QString, qsizetype> found;
        int state = 0;
        for (qsizetype position = 0; position < text.size(); ++position) {
            const QChar character = text.at(position);
            while (state != 0 && !m_nodes[state].edges.contains(character))
                state = m_nodes[state].failure;
            const auto edge = m_nodes[state].edges.constFind(character);
            state = edge == m_nodes[state].edges.cend() ? 0 : edge.value();
            for (const int patternIndex : m_nodes[state].outputs) {
                const auto &pattern = m_patterns.at(patternIndex);
                if (found.contains(pattern.second))
                    continue;
                const qsizetype start = position - pattern.first.size() + 1;
                const qsizetype end = position + 1;
                const bool leftOk = start == 0 || isSc2TokenBoundary(text.at(start - 1));
                const bool rightOk = end >= text.size() || isSc2TokenBoundary(text.at(end));
                if (leftOk && rightOk)
                    found.insert(pattern.second, start);
            }
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
    QVector<QPair<QString, QString>> m_patterns;
};

class LineNumberIndex
{
public:
    explicit LineNumberIndex(const QString &text)
    {
        for (qsizetype i = 0; i < text.size(); ++i) {
            if (text.at(i) == QLatin1Char('\n'))
                m_newlines.append(i);
        }
    }

    int at(qsizetype offset) const
    {
        if (offset < 0)
            return -1;
        return int(std::lower_bound(m_newlines.cbegin(), m_newlines.cend(), offset)
                   - m_newlines.cbegin()) + 1;
    }

private:
    QVector<qsizetype> m_newlines;
};

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

    QVector<QPair<QString, QString>> foldedIdPatterns;
    QVector<QPair<QString, QString>> binaryIdPatterns;
    foldedIdPatterns.reserve(knownIdKeys.size());
    binaryIdPatterns.reserve(knownIdKeys.size());
    for (const QString &idKey : knownIdKeys) {
        const QString canonical = canonicalIdByKey.value(idKey);
        foldedIdPatterns.append({idKey, idKey});
        binaryIdPatterns.append({QString::fromLatin1(canonical.toUtf8()), idKey});
    }
    const TokenSetMatcher foldedIdMatcher(foldedIdPatterns);
    const TokenSetMatcher binaryIdMatcher(binaryIdPatterns);

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

    QVector<QPair<QString, QString>> assetPatterns;
    assetPatterns.reserve(canonicalAssetByKey.size());
    for (auto it = canonicalAssetByKey.cbegin(); it != canonicalAssetByKey.cend(); ++it)
        assetPatterns.append({it.key(), it.key()});
    const TokenSetMatcher assetMatcher(assetPatterns);

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
            const auto matches = binaryIdMatcher.firstMatches(QString::fromLatin1(bytes));
            for (auto match = matches.cbegin(); match != matches.cend(); ++match) {
                const QString idKey = match.key();
                const QString canonical = canonicalIdByKey.value(idKey);
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
        const LineNumberIndex lines(content);
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
                record.lineNumber = lines.at(lastOffset);
                record.detail = QStringLiteral("Objects placement/runtime root");
                record.rewritable = true;
                addRecord(record);
            }
        }

        const ReferenceKind textKind = textKindForPath(relative);
        const bool strongText = isStrongTextPath(relative);
        const QString foldedContent = foldedCharacters(content);
        const auto idMatches = foldedIdMatcher.firstMatches(foldedContent);
        for (auto match = idMatches.cbegin(); match != idMatches.cend(); ++match) {
            const QString idKey = match.key();
            const QString canonical = canonicalIdByKey.value(idKey);
            ReferenceRecord record;
            record.kind = textKind;
            record.strength = strongText ? ReferenceStrength::Strong : ReferenceStrength::Weak;
            record.targetId = canonical;
            record.sourceFile = relative;
            record.lineNumber = lines.at(match.value());
            record.detail = strongText ? QStringLiteral("token-aware text reference")
                                       : QStringLiteral("weak token-aware text match");
            record.rewritable = strongText;
            addRecord(record);
        }

        const auto assetMatches = assetMatcher.firstMatches(foldedContent);
        for (auto match = assetMatches.cbegin(); match != assetMatches.cend(); ++match) {
            ReferenceRecord record;
            record.kind = ReferenceKind::AssetText;
            record.strength = ReferenceStrength::Strong;
            record.targetAsset = canonicalAssetByKey.value(match.key());
            record.sourceFile = relative;
            record.lineNumber = lines.at(match.value());
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
