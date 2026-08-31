#include "core/DeepCleanupService.h"

#include "core/AnalysisIndex.h"
#include "core/AssetFileRules.h"
#include "core/AssetOptimizationAnalyzer.h"
#include "core/AssetReferenceScanner.h"
#include "core/BackupManager.h"
#include "core/CatalogProtection.h"
#include "core/FolderAnalyzer.h"
#include "core/SemanticDuplicateAnalyzer.h"
#include "core/TriggerPerformanceAnalyzer.h"
#include "core/XmlCleanupUtils.h"
#include "core/XmlLoader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <pugixml.hpp>

#include <algorithm>
#include <sstream>

namespace
{
    QString relativePath(const QString &rootFolder, const QString &filePath)
    {
        if (!QDir::isAbsolutePath(filePath))
            return QDir::cleanPath(filePath).replace('\\', '/');
        const QFileInfo rootInfo(rootFolder);
        if (rootInfo.exists() && rootInfo.isFile())
            return QDir::cleanPath(filePath).replace('\\', '/');
        return QDir(rootFolder).relativeFilePath(filePath).replace('\\', '/');
    }

    QString absoluteCandidatePath(const QString &rootFolder, const QString &filePath)
    {
        return QDir::isAbsolutePath(filePath) ? filePath : QDir(rootFolder).absoluteFilePath(filePath);
    }

    bool containsToken(const QString &haystack, const QString &token)
    {
        if (token.trimmed().isEmpty())
            return false;
        const QRegularExpression expression(
            QStringLiteral("(?<![A-Za-z0-9_@.])%1(?![A-Za-z0-9_@.])")
                .arg(QRegularExpression::escape(token)),
            QRegularExpression::CaseInsensitiveOption);
        return expression.match(haystack).hasMatch();
    }

    QString readTextFileBestEffort(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(file.readAll());
    }

    bool assetIsReferenced(const QString &corpus, const QString &relative, const QFileInfo &info)
    {
        const QString slashPath = QDir::cleanPath(relative).replace('\\', '/');
        const QString backslashPath = QString(slashPath).replace('/', '\\');
        return containsToken(corpus, slashPath) || containsToken(corpus, backslashPath) || containsToken(corpus, info.fileName()) || containsToken(corpus, info.completeBaseName());
    }

    QStringList splitLocation(const QString &location)
    {
        return location.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    }

    bool parseSegment(const QString &segment, QString *name, int *index)
    {
        const int open = segment.lastIndexOf(QLatin1Char('['));
        const int close = segment.lastIndexOf(QLatin1Char(']'));
        if (open <= 0 || close <= open + 1)
            return false;
        bool ok = false;
        const int parsedIndex = segment.mid(open + 1, close - open - 1).toInt(&ok);
        if (!ok || parsedIndex <= 0)
            return false;
        *name = segment.left(open);
        *index = parsedIndex;
        return true;
    }

    pugi::xml_node childByNameAndIndex(const pugi::xml_node &parent, const QString &name, int index)
    {
        int current = 0;
        for (pugi::xml_node child = parent.first_child(); child; child = child.next_sibling())
        {
            if (child.type() != pugi::node_element || QString::fromUtf8(child.name()) != name)
                continue;
            ++current;
            if (current == index)
                return child;
        }
        return {};
    }

    pugi::xml_node findNodeByLocation(const pugi::xml_node &document, const QString &location)
    {
        pugi::xml_node currentParent = document;
        pugi::xml_node currentNode;
        for (const QString &segment : splitLocation(location))
        {
            QString name;
            int index = 0;
            if (!parseSegment(segment, &name, &index))
                return {};
            currentNode = childByNameAndIndex(currentParent, name, index);
            if (!currentNode)
                return {};
            currentParent = currentNode;
        }
        return currentNode;
    }

    QString buildPathFromNode(const pugi::xml_node &node)
    {
        QStringList segments;
        for (pugi::xml_node current = node; current && current.type() == pugi::node_element; current = current.parent())
        {
            int index = 1;
            for (pugi::xml_node sibling = current.previous_sibling(current.name()); sibling; sibling = sibling.previous_sibling(current.name()))
                ++index;
            segments.prepend(QStringLiteral("%1[%2]").arg(QString::fromUtf8(current.name())).arg(index));
            if (!current.parent() || current.parent().type() == pugi::node_document)
                break;
        }
        return QLatin1Char('/') + segments.join(QLatin1Char('/'));
    }

    QString serializeNode(const pugi::xml_node &node)
    {
        std::ostringstream stream;
        node.print(stream, "  ", pugi::format_raw, pugi::encoding_utf8);
        return QString::fromStdString(stream.str());
    }

    QStringList typedReferences(const QString &text)
    {
        static const QRegularExpression expression(
            QStringLiteral("\\b(?:Unit|Abil|Ability|Weapon|Effect|Behavior|Actor|Model|Sound|Button|Validator|Requirement|Upgrade|Mover),([A-Za-z0-9_@.]+)"));
        QStringList refs;
        auto matches = expression.globalMatch(text);
        while (matches.hasNext())
        {
            const QRegularExpressionMatch match = matches.next();
            refs << match.captured(1);
        }
        refs.removeDuplicates();
        return refs;
    }

    bool actorEventLike(const pugi::xml_node &node)
    {
        const QString name = QString::fromUtf8(node.name()).toLower();
        return name == QStringLiteral("on")
            || name == QStringLiteral("remove")
            || name == QStringLiteral("do")
            || name == QStringLiteral("event")
            || name.contains(QStringLiteral("term"));
    }

    bool isTrueAttribute(const pugi::xml_node &node, const char *name)
    {
        const QString value = QString::fromUtf8(node.attribute(name).value()).trimmed().toLower();
        return value == QStringLiteral("1") || value == QStringLiteral("true");
    }

    QString brokenActorEventReason(const pugi::xml_node &node)
    {
        if (isTrueAttribute(node, "removed"))
            return {};

        const QString nodeName = QString::fromUtf8(node.name()).toLower();
        if (nodeName != QStringLiteral("on"))
            return {};

        const pugi::xml_attribute termsAttr = node.attribute("Terms");
        const pugi::xml_attribute sendAttr = node.attribute("Send");
        const QString terms = QString::fromUtf8(termsAttr.value()).trimmed();
        const QString send = QString::fromUtf8(sendAttr.value()).trimmed();
        const bool missingTerms = !termsAttr || terms.isEmpty();
        const bool missingSend = !sendAttr || send.isEmpty();

        if (missingTerms && missingSend)
            return QStringLiteral("Actor event has no triggering Terms and no Send action.");
        if (missingSend)
            return QStringLiteral("Actor event has triggering Terms but no Send action.");
        if (missingTerms)
            return QStringLiteral("Actor event has a Send action but no triggering Terms.");
        return {};
    }

    void appendActorEventNodeCandidates(const pugi::xml_node &eventNode,
                                        const QString &filePath,
                                        const DataNode &actorNode,
                                        const QSet<QString> &ids,
                                        QVector<DeepCleanupCandidate> *candidates)
    {
        if (!eventNode || eventNode.type() != pugi::node_element || !candidates)
            return;
        if (actorEventLike(eventNode))
        {
            const QString xml = serializeNode(eventNode);
            const QString structuralReason = brokenActorEventReason(eventNode);
            if (!structuralReason.isEmpty())
            {
                DeepCleanupCandidate candidate;
                candidate.index = candidates->size();
                candidate.kind = DeepCleanupKind::BrokenActorEvent;
                candidate.action = DeepCleanupAction::RemoveXmlNode;
                candidate.state = CandidateState::Safe;
                candidate.recommended = true;
                candidate.filePath = filePath;
                candidate.label = QStringLiteral("%1 event in %2").arg(QString::fromUtf8(eventNode.name()), actorNode.id);
                candidate.xmlLocation = buildPathFromNode(eventNode);
                candidate.reason = structuralReason;
                candidate.detail = xml.left(600);
                candidates->append(candidate);
                return;
            }
            const QStringList refs = typedReferences(xml);
            if (!refs.isEmpty())
            {
                int existing = 0;
                for (const QString &ref : refs)
                    if (ids.contains(ref))
                        ++existing;
                DeepCleanupCandidate candidate;
                candidate.kind = DeepCleanupKind::BrokenActorEvent;
                candidate.index = candidates->size();
                candidate.filePath = filePath;
                candidate.label = QStringLiteral("%1 event in %2").arg(QString::fromUtf8(eventNode.name()), actorNode.id);
                candidate.xmlLocation = buildPathFromNode(eventNode);
                candidate.detail = xml.left(600);
                if (existing == 0)
                {
                    candidate.action = DeepCleanupAction::RemoveXmlNode;
                    candidate.state = CandidateState::Safe;
                    candidate.recommended = true;
                    candidate.reason = QStringLiteral("Actor event references only missing typed IDs: %1").arg(refs.join(QStringLiteral(", ")));
                    candidates->append(candidate);
                    return;
                }
                if (existing < refs.size())
                {
                    candidate.action = DeepCleanupAction::ReportOnly;
                    candidate.state = CandidateState::Risky;
                    candidate.recommended = false;
                    candidate.reason = QStringLiteral("Actor event has mixed existing and missing typed IDs: %1").arg(refs.join(QStringLiteral(", ")));
                    candidates->append(candidate);
                }
            }
        }

        for (pugi::xml_node child = eventNode.first_child(); child; child = child.next_sibling())
            if (child.type() == pugi::node_element)
                appendActorEventNodeCandidates(child, filePath, actorNode, ids, candidates);
    }

    void appendActorEventCandidates(const AnalysisResult &analysis,
                                    const QSet<QString> &ids,
                                    QVector<DeepCleanupCandidate> *candidates)
    {
        for (auto it = analysis.sourceXmlByFile.cbegin(); it != analysis.sourceXmlByFile.cend(); ++it)
        {
            pugi::xml_document document;
            const QByteArray bytes = it.value().toUtf8();
            if (!document.load_buffer(bytes.constData(), size_t(bytes.size())))
                continue;
            for (const DataNode &node : analysis.nodes)
            {
                if (node.sourceFile != it.key() || !node.elementName.startsWith(QStringLiteral("CActor"), Qt::CaseInsensitive))
                    continue;
                pugi::xml_node actor = findNodeByLocation(document, node.originalLocation);
                if (!actor)
                    continue;
                for (pugi::xml_node child = actor.first_child(); child; child = child.next_sibling())
                {
                    if (child.type() != pugi::node_element)
                        continue;
                    appendActorEventNodeCandidates(child, it.key(), node, ids, candidates);
                }
            }
        }
    }

    void appendRedundantChildNodeCandidates(const AnalysisResult &analysis,
                                            const QHash<QString, const DataNode *> &nodesByTypeAndId,
                                            QVector<DeepCleanupCandidate> *candidates)
    {
        for (const DataNode &node : analysis.nodes)
        {
            if (sc2dh::isProtectedCatalogNode(node))
                continue;
            const QString parentId = node.attributes.value(QStringLiteral("parent"));
            if (parentId.isEmpty())
                continue;
            const DataNode *parent = nodesByTypeAndId.value(AnalysisIndex::typeIdKey(node.elementName, parentId), nullptr);
            if (!parent)
                continue;

            pugi::xml_document nodeDocument;
            pugi::xml_document parentDocument;
            pugi::xml_node nodeRoot;
            pugi::xml_node parentRoot;
            if (!sc2dh::xmlcleanup::loadSerializedRoot(node.serializedXml, &nodeDocument, &nodeRoot) || !sc2dh::xmlcleanup::loadSerializedRoot(parent->serializedXml, &parentDocument, &parentRoot))
            {
                continue;
            }

            QHash<QString, int> inheritedChildCounts;
            for (pugi::xml_node child = parentRoot.first_child(); child; child = child.next_sibling())
            {
                if (child.type() != pugi::node_element)
                    continue;
                const QString key = sc2dh::xmlcleanup::canonicalNode(child, false);
                if (!key.isEmpty())
                    ++inheritedChildCounts[key];
            }

            for (pugi::xml_node child = nodeRoot.first_child(); child; child = child.next_sibling())
            {
                if (child.type() != pugi::node_element)
                    continue;
                const QString key = sc2dh::xmlcleanup::canonicalNode(child, false);
                auto inherited = inheritedChildCounts.find(key);
                if (inherited == inheritedChildCounts.end() || inherited.value() <= 0)
                    continue;
                --inherited.value();

                DeepCleanupCandidate candidate;
                candidate.index = candidates->size();
                candidate.kind = DeepCleanupKind::RedundantDefaultNode;
                candidate.action = DeepCleanupAction::RemoveXmlNode;
                candidate.state = CandidateState::Safe;
                candidate.filePath = node.sourceFile;
                candidate.label = QStringLiteral("%1.%2").arg(node.id, QString::fromUtf8(child.name()));
                candidate.xmlLocation = node.originalLocation + QLatin1Char('/') + sc2dh::xmlcleanup::locationSegmentForNode(child);
                candidate.reason = QStringLiteral("Child XML node is identical to inherited parent object %1.").arg(parent->id);
                candidate.detail = serializeNode(child).left(600);
                candidate.recommended = true;
                candidates->append(candidate);
            }
        }
    }

    class XmlInheritanceAnalyzer
    {
    public:
        void appendCandidates(const AnalysisResult &analysis,
                              const QHash<QString, const DataNode *> &nodesByTypeAndId,
                              QVector<DeepCleanupCandidate> *candidates) const
        {
            appendRedundantChildNodeCandidates(analysis, nodesByTypeAndId, candidates);
        }
    };

    QStringList localizationKeyTokens(const QString &key)
    {
        QStringList tokens = key.split(QRegularExpression(QStringLiteral("[/:.\\\\\\s]+")), Qt::SkipEmptyParts);
        tokens.removeDuplicates();
        return tokens;
    }

    bool keyLooksLikeObjectString(const QStringList &tokens)
    {
        static const QSet<QString> objectPrefixes = {
            QStringLiteral("Unit"), QStringLiteral("Abil"), QStringLiteral("Ability"), QStringLiteral("Weapon"),
            QStringLiteral("Effect"), QStringLiteral("Actor"), QStringLiteral("Button"), QStringLiteral("Behavior"),
            QStringLiteral("Validator"), QStringLiteral("Requirement"), QStringLiteral("Upgrade"), QStringLiteral("Model"),
            QStringLiteral("Sound"), QStringLiteral("Mover")};
        for (const QString &token : tokens)
            if (objectPrefixes.contains(token))
                return true;
        return false;
    }

    bool lineEndingIsCrLf(const QByteArray &bytes)
    {
        return bytes.contains("\r\n");
    }

    QByteArray joinLines(const QStringList &lines, const QByteArray &lineEnding, bool trailingNewline)
    {
        QByteArray output;
        for (int i = 0; i < lines.size(); ++i)
        {
            if (i > 0)
                output += lineEnding;
            output += lines.at(i).toUtf8();
        }
        if (trailingNewline)
            output += lineEnding;
        return output;
    }

    int locationDepth(const QString &location)
    {
        return splitLocation(location).size();
    }
}

QString deepCleanupKindName(DeepCleanupKind kind)
{
    switch (kind)
    {
    case DeepCleanupKind::UnusedAsset:
        return QStringLiteral("Unused asset");
    case DeepCleanupKind::LocalizationEntry:
        return QStringLiteral("Localization");
    case DeepCleanupKind::RedundantDefaultField:
        return QStringLiteral("Default field");
    case DeepCleanupKind::RedundantDefaultNode:
        return QStringLiteral("Default XML node");
    case DeepCleanupKind::BrokenActorEvent:
        return QStringLiteral("Actor event");
    case DeepCleanupKind::DependencyEntry:
        return QStringLiteral("Dependency review");
    case DeepCleanupKind::ArchiveTrash:
        return QStringLiteral("Archive trash");
    case DeepCleanupKind::AssetAudit:
        return QStringLiteral("Asset audit");
    case DeepCleanupKind::TriggerPerformance:
        return QStringLiteral("Trigger performance");
    case DeepCleanupKind::NearDuplicateObject:
        return QStringLiteral("Semantic duplicate");
    }
    return QStringLiteral("Deep cleanup");
}

QString deepCleanupActionName(DeepCleanupAction action)
{
    switch (action)
    {
    case DeepCleanupAction::DeleteFile:
        return QStringLiteral("Delete file");
    case DeepCleanupAction::RemoveTextLine:
        return QStringLiteral("Remove text line");
    case DeepCleanupAction::RemoveXmlNode:
        return QStringLiteral("Remove XML node");
    case DeepCleanupAction::RemoveXmlAttribute:
        return QStringLiteral("Remove XML attribute");
    case DeepCleanupAction::ReportOnly:
        return QStringLiteral("Review only");
    }
    return QStringLiteral("Review only");
}

void DeepCleanupService::populateCandidates(AnalysisResult *analysis) const
{
    if (!analysis)
        return;
    analysis->deepCleanupCandidates.clear();
    const AnalysisIndex index(*analysis);
    const QSet<QString> &ids = index.knownIds();
    const QString corpus = AssetReferenceScanner().buildCorpus(*analysis);

    auto append = [&](DeepCleanupCandidate candidate)
    {
        candidate.index = analysis->deepCleanupCandidates.size();
        analysis->deepCleanupCandidates.append(std::move(candidate));
    };

    for (const ScannedFileInfo &file : analysis->scannedFiles)
    {
        const QString rel = relativePath(analysis->rootFolder, file.filePath);
        const QFileInfo info(file.filePath);
        if (sc2dh::asset::isBackupOrTrashName(rel))
        {
            DeepCleanupCandidate candidate;
            candidate.kind = DeepCleanupKind::ArchiveTrash;
            candidate.action = DeepCleanupAction::DeleteFile;
            candidate.state = CandidateState::Safe;
            candidate.filePath = file.filePath;
            candidate.label = rel;
            candidate.reason = QStringLiteral("Temporary, report, backup or pending helper file is not game data.");
            candidate.bytes = file.size;
            candidate.recommended = true;
            append(candidate);
            continue;
        }
        if (sc2dh::asset::isAssetFile(info, rel) && !assetIsReferenced(corpus, rel, info))
        {
            DeepCleanupCandidate candidate;
            candidate.kind = DeepCleanupKind::UnusedAsset;
            candidate.filePath = file.filePath;
            candidate.label = rel;
            candidate.bytes = file.size;
            if (analysis->externalConsumersUnknown)
            {
                candidate.action = DeepCleanupAction::ReportOnly;
                candidate.state = CandidateState::Risky;
                candidate.reason = QStringLiteral("No local reference was found, but a standalone SC2Mod can be referenced by external maps/mods that were not analyzed together.");
                candidate.recommended = false;
            }
            else
            {
                candidate.action = DeepCleanupAction::DeleteFile;
                candidate.state = CandidateState::Safe;
                candidate.reason = QStringLiteral("Asset filename/path is not referenced by XML, trigger, script or text data.");
                candidate.recommended = true;
            }
            append(candidate);
        }
    }

    for (const ScannedFileInfo &file : analysis->scannedFiles)
    {
        const QString rel = relativePath(analysis->rootFolder, file.filePath);
        if (!file.isSc2DataLike || file.isXml || !sc2dh::asset::isLocalizationFile(rel) || file.size > 4 * 1024 * 1024)
            continue;
        QFile source(file.filePath);
        if (!source.open(QIODevice::ReadOnly))
            continue;
        const QString text = QString::fromUtf8(source.readAll());
        const QStringList lines = text.split(QRegularExpression(QStringLiteral("\\r?\\n")));
        for (int i = 0; i < lines.size(); ++i)
        {
            const QString line = lines.at(i);
            const QString trimmed = line.trimmed();
            if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')) || trimmed.startsWith(QStringLiteral("//")))
                continue;
            const int equals = line.indexOf(QLatin1Char('='));
            if (equals <= 0)
                continue;
            const QString key = line.left(equals).trimmed();
            const QStringList tokens = localizationKeyTokens(key);
            if (!keyLooksLikeObjectString(tokens))
                continue;
            bool referencesExisting = false;
            for (const QString &token : tokens)
            {
                if (ids.contains(token))
                {
                    referencesExisting = true;
                    break;
                }
            }
            if (referencesExisting)
                continue;
            DeepCleanupCandidate candidate;
            candidate.kind = DeepCleanupKind::LocalizationEntry;
            candidate.action = DeepCleanupAction::RemoveTextLine;
            candidate.state = CandidateState::Safe;
            candidate.filePath = file.filePath;
            candidate.label = key;
            candidate.lineNumber = i;
            candidate.reason = QStringLiteral("Localized object string does not match any existing data ID.");
            candidate.detail = line.left(600);
            candidate.recommended = true;
            append(candidate);
        }
    }

    for (const DataNode &node : analysis->nodes)
    {
        if (sc2dh::isProtectedCatalogNode(node))
            continue;
        const QString parentId = node.attributes.value(QStringLiteral("parent"));
        if (parentId.isEmpty())
            continue;
        const DataNode *parent = index.nodesByTypeAndId().value(AnalysisIndex::typeIdKey(node.elementName, parentId), nullptr);
        if (!parent)
            continue;
        for (auto it = node.attributes.cbegin(); it != node.attributes.cend(); ++it)
        {
            const QString attr = it.key();
            if (attr.compare(QStringLiteral("id"), Qt::CaseInsensitive) == 0 || attr.compare(QStringLiteral("parent"), Qt::CaseInsensitive) == 0 || attr.compare(QStringLiteral("default"), Qt::CaseInsensitive) == 0)
                continue;
            const auto parentAttr = parent->attributes.constFind(attr);
            if (parentAttr == parent->attributes.cend() || parentAttr.value() != it.value())
                continue;
            DeepCleanupCandidate candidate;
            candidate.kind = DeepCleanupKind::RedundantDefaultField;
            candidate.action = DeepCleanupAction::RemoveXmlAttribute;
            candidate.state = CandidateState::Safe;
            candidate.filePath = node.sourceFile;
            candidate.label = QStringLiteral("%1.%2").arg(node.id, attr);
            candidate.xmlLocation = node.originalLocation;
            candidate.attributeName = attr;
            candidate.reason = QStringLiteral("Attribute equals the local parent object value (%1).").arg(parent->id);
            candidate.detail = QStringLiteral("%1=\"%2\"").arg(attr, it.value());
            candidate.recommended = true;
            append(candidate);
        }
    }

    XmlInheritanceAnalyzer().appendCandidates(*analysis, index.nodesByTypeAndId(), &analysis->deepCleanupCandidates);
    appendActorEventCandidates(*analysis, ids, &analysis->deepCleanupCandidates);
    AssetOptimizationAnalyzer().appendCandidates(*analysis, &analysis->deepCleanupCandidates);
    TriggerPerformanceAnalyzer().appendCandidates(*analysis, &analysis->deepCleanupCandidates);
    SemanticDuplicateAnalyzer().appendCandidates(*analysis, &analysis->deepCleanupCandidates);

    for (const ScannedFileInfo &file : analysis->scannedFiles)
    {
        const QString rel = relativePath(analysis->rootFolder, file.filePath);
        const QString lower = rel.toLower();
        if (!lower.contains(QStringLiteral("documentinfo")) && !lower.contains(QStringLiteral("dependencies")))
            continue;
        const QString text = readTextFileBestEffort(file.filePath);
        if (!text.contains(QStringLiteral("SC2Mod"), Qt::CaseInsensitive) && !text.contains(QStringLiteral("Dependency"), Qt::CaseInsensitive))
            continue;
        DeepCleanupCandidate candidate;
        candidate.index = analysis->deepCleanupCandidates.size();
        candidate.kind = DeepCleanupKind::DependencyEntry;
        candidate.action = DeepCleanupAction::ReportOnly;
        candidate.state = CandidateState::Risky;
        candidate.filePath = file.filePath;
        candidate.label = rel;
        candidate.reason = QStringLiteral("Dependency metadata found. Dependency removal requires map/mod load testing and is review-only.");
        candidate.recommended = false;
        analysis->deepCleanupCandidates.append(candidate);
    }

    for (int index = 0; index < analysis->deepCleanupCandidates.size(); ++index)
        analysis->deepCleanupCandidates[index].index = index;
}

DeepCleanupApplyResult DeepCleanupService::apply(const AnalysisResult &analysis,
                                                 const QVector<int> &candidateIndexes,
                                                 const QString &rootFolder,
                                                 bool createBackup) const
{
    DeepCleanupApplyResult result;
    if (candidateIndexes.isEmpty())
    {
        result.error = QStringLiteral("No deep cleanup rows selected.");
        return result;
    }

    QSet<int> selected;
    for (int index : candidateIndexes)
        selected.insert(index);

    QVector<DeepCleanupCandidate> candidates;
    for (const DeepCleanupCandidate &candidate : analysis.deepCleanupCandidates)
    {
        if (!selected.contains(candidate.index))
            continue;
        if (candidate.state != CandidateState::Safe || candidate.action == DeepCleanupAction::ReportOnly)
        {
            ++result.reportOnlySkipped;
            continue;
        }
        candidates.append(candidate);
    }
    if (candidates.isEmpty())
    {
        result.success = true;
        return result;
    }

    const DestructiveOperationPermission permission = canApplyDestructiveChanges(analysis);
    if (!permission.allowed)
    {
        result.error = destructiveOperationPermissionText(permission);
        return result;
    }

    // Archive workspaces used to pass false here. They remain fully
    // transactional too: a verified backup inside that disposable workspace
    // is cheaper than allowing a partially changed workspace to reach archive
    // commit. Direct destructive applies therefore cannot be weakened by the
    // backup setting or this legacy flag.
    Q_UNUSED(createBackup);

    QHash<QString, QVector<DeepCleanupCandidate>> lineEdits;
    QHash<QString, QVector<DeepCleanupCandidate>> xmlEdits;
    QStringList filesToDelete;
    for (const DeepCleanupCandidate &candidate : candidates)
    {
        const QString abs = absoluteCandidatePath(rootFolder, candidate.filePath);
        switch (candidate.action)
        {
        case DeepCleanupAction::DeleteFile:
            if (!filesToDelete.contains(abs))
                filesToDelete.append(abs);
            break;
        case DeepCleanupAction::RemoveTextLine:
            lineEdits[abs].append(candidate);
            break;
        case DeepCleanupAction::RemoveXmlNode:
        case DeepCleanupAction::RemoveXmlAttribute:
            xmlEdits[abs].append(candidate);
            break;
        case DeepCleanupAction::ReportOnly:
            ++result.reportOnlySkipped;
            break;
        }
    }

    QHash<QString, QByteArray> plannedWrites;
    QSet<QString> stagedXmlFiles;
    QSet<QString> plannedDeletes;
    int plannedTextLinesRemoved = 0;
    int plannedXmlNodesRemoved = 0;
    int plannedXmlAttributesRemoved = 0;
    int plannedFilesDeleted = 0;

    const auto safeRelativePath = [&](const QString &absolutePath, QString *relative) {
        const QString normalized = QDir::cleanPath(relativePath(rootFolder, absolutePath)).replace('\\', '/');
        if (normalized.isEmpty() || normalized == QStringLiteral(".") || normalized == QStringLiteral("..")
            || normalized.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(normalized)) {
            result.error = QStringLiteral("Unsafe deep-cleanup path: %1").arg(absolutePath);
            return false;
        }
        if (relative)
            *relative = normalized;
        return true;
    };
    const auto addWrite = [&](const QString &absolutePath, const QByteArray &contents, bool xml) {
        QString relative;
        if (!safeRelativePath(absolutePath, &relative))
            return false;
        if (plannedDeletes.contains(relative) || plannedWrites.contains(relative)) {
            result.error = QStringLiteral("Conflicting deep-cleanup changes for %1.").arg(relative);
            return false;
        }
        plannedWrites.insert(relative, contents);
        if (xml)
            stagedXmlFiles.insert(relative);
        return true;
    };
    const auto addDelete = [&](const QString &absolutePath) {
        QString relative;
        if (!safeRelativePath(absolutePath, &relative))
            return false;
        if (plannedWrites.contains(relative) || plannedDeletes.contains(relative)) {
            result.error = QStringLiteral("Conflicting deep-cleanup changes for %1.").arg(relative);
            return false;
        }
        plannedDeletes.insert(relative);
        return true;
    };

    // Build every mutation before any source file is committed.
    for (auto it = lineEdits.cbegin(); it != lineEdits.cend(); ++it)
    {
        QFile file(it.key());
        if (!file.open(QIODevice::ReadOnly))
        {
            result.error = QStringLiteral("Unable to open text file for cleanup: %1").arg(it.key());
            return result;
        }
        const QByteArray bytes = file.readAll();
        file.close();
        const QByteArray ending = lineEndingIsCrLf(bytes) ? QByteArrayLiteral("\r\n") : QByteArrayLiteral("\n");
        const bool trailingNewline = bytes.endsWith('\n');
        QString text = QString::fromUtf8(bytes);
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        if (trailingNewline && text.endsWith(QLatin1Char('\n')))
            text.chop(1);
        const QStringList lines = text.split(QLatin1Char('\n'));
        QSet<int> removeLines;
        for (const DeepCleanupCandidate &candidate : it.value())
            if (candidate.lineNumber >= 0 && candidate.lineNumber < lines.size())
                removeLines.insert(candidate.lineNumber);
        QStringList kept;
        for (int line = 0; line < lines.size(); ++line)
            if (!removeLines.contains(line))
                kept.append(lines.at(line));
        if (!addWrite(it.key(), joinLines(kept, ending, trailingNewline), false))
            return result;
        plannedTextLinesRemoved += removeLines.size();
    }

    for (auto it = xmlEdits.cbegin(); it != xmlEdits.cend(); ++it)
    {
        QFile file(it.key());
        if (!file.open(QIODevice::ReadOnly))
        {
            result.error = QStringLiteral("Unable to open XML file for cleanup: %1").arg(it.key());
            return result;
        }
        const QByteArray bytes = file.readAll();
        file.close();
        pugi::xml_document document;
        const pugi::xml_parse_result parsed = document.load_buffer(bytes.constData(), size_t(bytes.size()));
        if (!parsed)
        {
            result.error = QStringLiteral("Unable to parse XML for cleanup: %1").arg(parsed.description());
            return result;
        }

        QVector<DeepCleanupCandidate> nodeCandidates;
        for (const DeepCleanupCandidate &candidate : it.value())
        {
            pugi::xml_node node = findNodeByLocation(document, candidate.xmlLocation);
            if (!node)
            {
                result.error = QStringLiteral("Unable to locate XML cleanup node: %1").arg(candidate.xmlLocation);
                return result;
            }
            if (candidate.action == DeepCleanupAction::RemoveXmlAttribute)
            {
                const QByteArray attributeName = candidate.attributeName.toUtf8();
                if (node.attribute(attributeName.constData()))
                {
                    node.remove_attribute(attributeName.constData());
                    ++plannedXmlAttributesRemoved;
                }
            }
            else if (candidate.action == DeepCleanupAction::RemoveXmlNode)
            {
                nodeCandidates.append(candidate);
            }
        }
        std::sort(nodeCandidates.begin(), nodeCandidates.end(), [](const DeepCleanupCandidate &left, const DeepCleanupCandidate &right)
                  {
            const int leftDepth = locationDepth(left.xmlLocation);
            const int rightDepth = locationDepth(right.xmlLocation);
            if (leftDepth != rightDepth)
                return leftDepth > rightDepth;
            return left.xmlLocation > right.xmlLocation; });
        QVector<pugi::xml_node> nodesToRemove;
        for (const DeepCleanupCandidate &candidate : nodeCandidates)
        {
            pugi::xml_node node = findNodeByLocation(document, candidate.xmlLocation);
            if (node)
                nodesToRemove.append(node);
        }
        for (const pugi::xml_node &node : nodesToRemove)
        {
            if (node.parent() && node.parent().remove_child(node))
                ++plannedXmlNodesRemoved;
        }

        std::ostringstream stream;
        document.save(stream, "  ", pugi::format_default, pugi::encoding_utf8);
        if (!addWrite(it.key(), QByteArray::fromStdString(stream.str()), true))
            return result;
    }

    for (const QString &filePath : filesToDelete)
    {
        if (!QFileInfo::exists(filePath))
            continue;
        if (!addDelete(filePath))
            return result;
        ++plannedFilesDeleted;
    }

    QVector<TransactionalFileChange> changes;
    QStringList writePaths = plannedWrites.keys();
    std::sort(writePaths.begin(), writePaths.end());
    for (const QString &relative : writePaths)
        changes.append({relative, plannedWrites.value(relative), false});
    QStringList deletePaths = plannedDeletes.values();
    std::sort(deletePaths.begin(), deletePaths.end());
    for (const QString &relative : deletePaths)
        changes.append({relative, {}, true});

    if (changes.isEmpty())
    {
        result.success = true;
        return result;
    }

    const auto stagedValidator = [stagedXmlFiles](const QString &stagingFolder, QString *validationError) {
        for (const QString &relative : stagedXmlFiles)
        {
            QFile file(QDir(stagingFolder).absoluteFilePath(relative));
            if (!file.open(QIODevice::ReadOnly))
            {
                if (validationError)
                    *validationError = QStringLiteral("Unable to reopen staged XML: %1").arg(relative);
                return false;
            }
            const QByteArray bytes = file.readAll();
            pugi::xml_document document;
            const pugi::xml_parse_result parsed = document.load_buffer(bytes.constData(), size_t(bytes.size()));
            if (!parsed)
            {
                if (validationError)
                    *validationError = QStringLiteral("Staged XML parse failed for %1: %2")
                                           .arg(relative, parsed.description());
                return false;
            }
        }
        return true;
    };
    const auto committedValidator = [rootFolder, stagedXmlFiles, plannedDeletes](QString *validationError) {
        for (const QString &relative : stagedXmlFiles)
        {
            QFile file(QDir(rootFolder).absoluteFilePath(relative));
            if (!file.open(QIODevice::ReadOnly))
            {
                if (validationError)
                    *validationError = QStringLiteral("Unable to reopen committed XML: %1").arg(relative);
                return false;
            }
            const QByteArray bytes = file.readAll();
            pugi::xml_document document;
            const pugi::xml_parse_result parsed = document.load_buffer(bytes.constData(), size_t(bytes.size()));
            if (!parsed)
            {
                if (validationError)
                    *validationError = QStringLiteral("Committed XML parse failed for %1: %2")
                                           .arg(relative, parsed.description());
                return false;
            }
        }
        for (const QString &relative : plannedDeletes)
        {
            if (QFileInfo::exists(QDir(rootFolder).absoluteFilePath(relative)))
            {
                if (validationError)
                    *validationError = QStringLiteral("Committed cleanup deletion remains: %1").arg(relative);
                return false;
            }
        }

        FolderAnalyzer analyzer;
        AnalysisResult rebuilt;
        QString reanalysisError;
        if (!analyzer.analyzeFolder(rootFolder, {}, &rebuilt, &reanalysisError))
        {
            if (validationError)
                *validationError = QStringLiteral("Post-cleanup analysis failed: %1").arg(reanalysisError);
            return false;
        }
        if (rebuilt.completeness != AnalysisCompleteness::Complete)
        {
            if (validationError)
                *validationError = QStringLiteral("Post-cleanup analysis is %1.")
                                       .arg(analysisCompletenessName(rebuilt.completeness));
            return false;
        }
        return true;
    };

    const FolderSaveTransactionResult transaction = BackupManager().applyFolderTransaction(
        rootFolder, changes, analysis.analysisReportText, analysis.plannedChangesReportText,
        stagedValidator, committedValidator);
    result.backupFolder = transaction.backupFolder;
    if (!transaction.success)
    {
        result.error = transaction.error;
        if (result.error.isEmpty())
            result.error = QStringLiteral("Deep cleanup transaction failed: %1")
                               .arg(operationErrorCodeName(transaction.errorCode));
        else if (transaction.errorCode != OperationErrorCode::None)
            result.error.prepend(QStringLiteral("[%1] ").arg(operationErrorCodeName(transaction.errorCode)));
        return result;
    }

    result.success = true;
    result.changedFiles = transaction.changedFiles;
    result.removedFiles = transaction.removedFiles;
    result.filesDeleted = plannedFilesDeleted;
    result.textLinesRemoved = plannedTextLinesRemoved;
    result.xmlNodesRemoved = plannedXmlNodesRemoved;
    result.xmlAttributesRemoved = plannedXmlAttributesRemoved;
    return result;
}
