#include "core/ScannedFileReader.h"
#include "core/CatalogDataModel.h"
#include "core/DependencySourceResolver.h"
#include "core/LayeredDeclarationIndex.h"
#include "core/FolderAnalyzer.h"

#include "core/CatalogLinkSchema.h"
#include "core/DeepCleanupService.h"
#include "core/CatalogProtection.h"
#include "core/UnifiedReferenceIndex.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QIODevice>
#include <QRegularExpression>
#include <QQueue>

#include <algorithm>

namespace
{
    bool preserveDataCollectionRecords(const QByteArray &source, const QSet<QString> &removedIds,
                                       QByteArray *rewritten, QString *error)
    {
        Q_UNUSED(removedIds);
        Q_UNUSED(error);
        *rewritten = source;
        return true;
    }


    bool isProtectedObject(const DataNode &node)
    {
        if (sc2dh::isProtectedCatalogNode(node))
            return true;
        if (node.elementName.startsWith(QStringLiteral("CDataCollection"), Qt::CaseInsensitive))
            return true;
        static const QSet<QString> protectedIds = {
            QStringLiteral("root"), QStringLiteral("default"), QStringLiteral("editor"), QStringLiteral("runtime")};
        if (protectedIds.contains(node.id.toLower()) || node.id.endsWith(QStringLiteral("Root"), Qt::CaseInsensitive) || node.elementName.startsWith(QStringLiteral("CGame"), Qt::CaseInsensitive))
            return true;
        for (auto it = node.attributes.cbegin(); it != node.attributes.cend(); ++it)
        {
            const QString key = it.key().toLower();
            const QString value = it.value().toLower();
            if ((key == QStringLiteral("root") || key == QStringLiteral("editoronly") || key == QStringLiteral("runtime") || key == QStringLiteral("protected")) && (value == QStringLiteral("1") || value == QStringLiteral("true")))
                return true;
        }
        return false;
    }

    bool isExternalRootType(const DataNode &node)
    {
        const QString type = node.elementName.toLower();
        return type.contains(QStringLiteral("placed")) || type.contains(QStringLiteral("placement"))
            || type.startsWith(QStringLiteral("ctrigger")) || type.startsWith(QStringLiteral("cgame"))
            || type.startsWith(QStringLiteral("cmap")) || type.startsWith(QStringLiteral("cruntime"))
            || type.startsWith(QStringLiteral("ceditor"));
    }

    bool isPrimaryEntity(const DataNode &node)
    {
        const QString type = node.elementName.toLower();
        return type == QStringLiteral("cunit") || type.startsWith(QStringLiteral("cabil"))
            || type.startsWith(QStringLiteral("cweapon"))
            || (type.startsWith(QStringLiteral("cbehavior")) && !node.id.contains(QLatin1Char('@')));
    }

    QString usageLabel(const DataNode &node)
    {
        QString type = node.elementName;
        if (type.startsWith(QLatin1Char('C'))) type.remove(0, 1);
        if (type.contains(QStringLiteral("Placed"), Qt::CaseInsensitive)) type = QStringLiteral("Placed Unit");
        return QStringLiteral("%1(%2)").arg(type, node.id);
    }

    QHash<QString, int> countKnownScriptTokens(const QString &text, const QSet<QString> &knownIds)
    {
        QHash<QString, int> counts;
        static const QRegularExpression expression(QStringLiteral("[A-Za-z0-9_@.]+"));
        static const QRegularExpression scopedSeparator(QStringLiteral("[@.]"));
        auto matches = expression.globalMatch(text);
        while (matches.hasNext())
        {
            const QString token = matches.next().captured(0);
            if (knownIds.contains(token))
                ++counts[token];
            if (!token.contains(QLatin1Char('@')) && !token.contains(QLatin1Char('.')))
                continue;
            const QStringList parts = token.split(scopedSeparator, Qt::SkipEmptyParts);
            for (const QString &part : parts) {
                if (part != token && knownIds.contains(part))
                    ++counts[part];
            }
        }
        return counts;
    }

    QString numberedIdBase(const QString &id)
    {
        static const QRegularExpression expression(QStringLiteral("^(.+?)(\\d+)$"));
        const QRegularExpressionMatch match = expression.match(id);
        return match.hasMatch() ? match.captured(1) : QString();
    }

    QString attributeValue(const DataNode &node, const QString &name)
    {
        const auto exact = node.attributes.constFind(name);
        if (exact != node.attributes.cend())
            return exact.value();
        for (auto it = node.attributes.cbegin(); it != node.attributes.cend(); ++it)
            if (it.key().compare(name, Qt::CaseInsensitive) == 0)
                return it.value();
        return {};
    }

    void appendUniqueReference(DataNode *node, const QString &reference)
    {
        if (!node || reference.trimmed().isEmpty() || node->referencedIds.contains(reference))
            return;
        node->referencedIds.append(reference);
    }

    void appendImplicitActorUnitReferences(AnalysisResult *result)
    {
        if (!result)
            return;
        QHash<QString, QStringList> actorIdsByUnitId;
        for (const DataNode &node : result->nodes)
        {
            if (!node.elementName.startsWith(QStringLiteral("CActorUnit"), Qt::CaseInsensitive)
                || node.id.isEmpty())
                continue;
            const QString unitName = attributeValue(node, QStringLiteral("unitName")).trimmed();
            if (unitName.isEmpty())
                continue;
            actorIdsByUnitId[unitName].append(node.id);
        }
        for (DataNode &node : result->nodes)
        {
            if (!node.elementName.startsWith(QStringLiteral("CUnit"), Qt::CaseInsensitive))
                continue;
            for (const QString &actorId : actorIdsByUnitId.value(node.id))
                appendUniqueReference(&node, actorId);
            std::sort(node.referencedIds.begin(), node.referencedIds.end());
        }
    }

}

bool FolderAnalyzer::isXmlFile(const QFileInfo &info) const
{
    return info.isFile() && info.suffix().compare(QStringLiteral("xml"), Qt::CaseInsensitive) == 0;
}

bool FolderAnalyzer::isSc2DataLikeFile(const QFileInfo &info) const
{
    if (!info.isFile())
    {
        return false;
    }

    const QString fileName = info.fileName().toLower();
    if (fileName == QStringLiteral("analysis_report.txt") || fileName == QStringLiteral("planned_changes_report.txt") || fileName == QStringLiteral("rename_to_standard_preview.txt") || fileName == QStringLiteral("data_collection_preview.txt"))
        return false;
    static const QSet<QString> metadataFileNames = {
        QStringLiteral("mapinfo"),
        QStringLiteral("documentinfo"),
        QStringLiteral("componentlist.sc2components")
    };
    if (metadataFileNames.contains(fileName))
        return true;
    static const QSet<QString> extensions = {
        QStringLiteral("xml"), QStringLiteral("txt"), QStringLiteral("json"), QStringLiteral("ini"),
        QStringLiteral("galaxy"), QStringLiteral("csv"), QStringLiteral("yaml"), QStringLiteral("yml"),
        QStringLiteral("layout"), QStringLiteral("sc2layout"), QStringLiteral("fxa"),
        QStringLiteral("fxs"), QStringLiteral("fxh")};
    return extensions.contains(info.suffix().toLower());
}

QString FolderAnalyzer::relativePath(const QString &rootFolder, const QString &absolutePath) const
{
    return QDir(rootFolder).relativeFilePath(absolutePath);
}

QString FolderAnalyzer::nodeLocationDescription(const DataNode &node) const
{
    return QStringLiteral("%1 | %2 | %3 | %4")
        .arg(node.sourceFile, node.elementName, node.id, node.originalLocation);
}

void FolderAnalyzer::populateDuplicateAndCandidateFlags(AnalysisResult *result,
                                                        const QSet<QString> &whitelistIds) const
{
    result->duplicateIdGroups.clear();
    result->duplicateContentGroups.clear();
    result->suspiciousEmptyNodeIndices.clear();
    result->possibleUnusedNodeIndices.clear();
    result->unusedCandidates.clear();
    result->deepCleanupCandidates.clear();

    QHash<QString, QVector<int>> idGroups;
    QHash<QString, QVector<int>> contentGroups;

    for (int i = 0; i < result->nodes.size(); ++i)
    {
        const DataNode &node = result->nodes[i];
        if (!node.id.isEmpty())
        {
            idGroups[sc2dh::catalogIdentityKey(node.elementName, node.id)].append(i);
        }
        if (!node.elementName.isEmpty() && !node.contentHash.isEmpty() && !isProtectedObject(node)
            && sc2dh::isSafeAutomaticObjectId(node.id))
        {
            const QString bodyKey = node.elementName + QChar(0x1f) + node.contentHash;
            contentGroups[bodyKey].append(i);
        }

        const QString trimmed = node.serializedXml.trimmed();
        const bool isSelfClosing = trimmed.endsWith(QStringLiteral("/>"));
        const bool hasNoChildContent = trimmed.contains(QStringLiteral("></"));
        const bool looksEmpty = isSelfClosing || hasNoChildContent;
        if (looksEmpty)
        {
            result->suspiciousEmptyNodeIndices.append(i);
        }
    }

    for (auto it = idGroups.cbegin(); it != idGroups.cend(); ++it)
    {
        if (it.value().size() < 2)
        {
            continue;
        }

        DuplicateIdGroup group;
        group.id = result->nodes[it.value().front()].id;
        group.nodeIndices = it.value();

        QSet<QString> files;
        for (int index : it.value())
        {
            files.insert(result->nodes[index].sourceFile);
        }
        group.sameFile = files.size() == 1;
        group.crossFile = files.size() > 1;
        if (!group.sameFile)
        {
            continue;
        }

        result->duplicateIdGroups.append(group);
        for (int index : it.value())
        {
            result->nodes[index].duplicateId = true;
        }
    }

    for (auto it = contentGroups.cbegin(); it != contentGroups.cend(); ++it)
    {
        if (it.value().size() < 2)
        {
            continue;
        }
        QSet<QString> distinctIds;
        for (int index : it.value())
            distinctIds.insert(result->nodes[index].id);
        if (distinctIds.size() < 2)
            continue;

        const QVector<int> indices = it.value();
        QHash<QString, QVector<int>> numberedGroups;
        for (int index : indices)
        {
            const QString base = numberedIdBase(result->nodes[index].id);
            if (!base.isEmpty())
                numberedGroups[base.toCaseFolded()].append(index);
        }
        bool foundCandidate = false;
        for (auto numberedIt = numberedGroups.cbegin(); numberedIt != numberedGroups.cend(); ++numberedIt)
        {
            const QVector<int> component = numberedIt.value();
            if (component.size() < 2)
                continue;
            DuplicateContentGroup group;
            group.elementName = result->nodes[component.front()].elementName;
            group.contentHash = result->nodes[component.front()].contentHash;
            group.nodeIndices = component;
            group.commonIdMask = numberedIdBase(result->nodes[component.front()].id) + QStringLiteral("#");
            group.mergeCandidate = true;
            group.autoRecommended = false;
            result->duplicateContentGroups.append(group);
            foundCandidate = true;
            for (int index : component)
                result->nodes[index].duplicateContent = true;
        }
        if (!foundCandidate)
        {
            DuplicateContentGroup group;
            group.elementName = result->nodes[indices.front()].elementName;
            group.contentHash = result->nodes[indices.front()].contentHash;
            group.nodeIndices = indices;
            group.commonIdMask = QStringLiteral("unrelated IDs");
            group.mergeCandidate = true;
            group.autoRecommended = false;
            result->duplicateContentGroups.append(group);
            for (int index : indices)
                result->nodes[index].duplicateContent = true;
        }
    }


    QHash<QString, int> inboundReferences;
    QHash<QString, int> dataCollectionReferences;
    QHash<QString, QStringList> inboundSources, outboundTargets, collectionMemberships;
    QHash<QString, QVector<int>> scopedNodes, unscopedNodes;
    for (int i=0;i<result->nodes.size();++i) {
        const auto &node=result->nodes[i];
        if (node.id.isEmpty()) continue;
        scopedNodes[sc2dh::catalogIdentityKey(node.elementName,node.id)].append(i);
        unscopedNodes[node.id.toCaseFolded()].append(i);
    }
    const auto targetsFor = [&](const QString &reference) {
        return reference.startsWith(QStringLiteral("*")+QChar(0x1f))
            ? unscopedNodes.value(reference.section(QChar(0x1f),1)) : scopedNodes.value(reference);
    };
    for (const auto &sourceNode:result->nodes) {
        const QString sourceKey=sc2dh::catalogIdentityKey(sourceNode.elementName,sourceNode.id);
        for (const QString &reference:sourceNode.referenceKeys) {
            for(int targetIndex:targetsFor(reference)) {
                const auto &target=result->nodes[targetIndex];
                const QString targetKey=sc2dh::catalogIdentityKey(target.elementName,target.id);
                if(sourceKey==targetKey) continue;
                if(sourceNode.elementName.startsWith(QStringLiteral("CDataCollection"),Qt::CaseInsensitive)) {
                    ++dataCollectionReferences[targetKey]; collectionMemberships[targetKey].append(sourceNode.id);
                } else {
                    ++inboundReferences[targetKey]; inboundSources[targetKey].append(sourceNode.id);
                    outboundTargets[sourceKey].append(target.id);
                }
            }
        }
    }

    QHash<QString, int> scriptReferences;
    QHash<QString, QStringList> blockingScriptSources;
    QHash<QString, QStringList> externalSources;
    sc2dh::refs::UnifiedReferenceIndex unifiedReferences;
    unifiedReferences.build(*result);
    result->incompleteSources += unifiedReferences.coverageIssues();
    for (const sc2dh::refs::ReferenceRecord &reference : unifiedReferences.records()) {
        if (reference.targetId.isEmpty())
            continue;
        const bool rootReference =
            reference.kind == sc2dh::refs::ReferenceKind::ScriptText
            || reference.kind == sc2dh::refs::ReferenceKind::PlacementRoot
            || reference.kind == sc2dh::refs::ReferenceKind::BinaryUnconfirmed
            || (reference.kind == sc2dh::refs::ReferenceKind::TypedXml && reference.strength == sc2dh::refs::ReferenceStrength::Blocking);
        if (!rootReference)
            continue;
        const QVector<int> targetIndices=reference.targetCatalog.isEmpty()
            ? unscopedNodes.value(reference.targetId.toCaseFolded())
            : scopedNodes.value(reference.targetCatalog+QChar(0x1f)+reference.targetId.toCaseFolded());
        for(int targetIndex:targetIndices) {
            const auto &target=result->nodes[targetIndex];const QString targetKey=sc2dh::catalogIdentityKey(target.elementName,target.id);
            ++scriptReferences[targetKey];
            if(reference.strength == sc2dh::refs::ReferenceStrength::Blocking)blockingScriptSources[targetKey].append(reference.detail);
        }
        QString source = reference.sourceFile;
        if (reference.lineNumber > 0)
            source += QStringLiteral(":%1").arg(reference.lineNumber);
        if (reference.kind == sc2dh::refs::ReferenceKind::BinaryUnconfirmed)
            source += QStringLiteral(" (binary non-rewritable)");
        else if (reference.kind == sc2dh::refs::ReferenceKind::PlacementRoot)
            source += QStringLiteral(" (placement root)");
        for(int targetIndex:targetIndices) {
            const auto &target=result->nodes[targetIndex];externalSources[sc2dh::catalogIdentityKey(target.elementName,target.id)].append(source);
        }
    }

    // Reachability deliberately ignores Data Collection records: catalog grouping
    // is editor metadata and is not evidence that gameplay can reach an object.
    QHash<QString, QVector<int>> nodesById;
    for (int i = 0; i < result->nodes.size(); ++i)
        if (!result->nodes[i].id.isEmpty()) nodesById[result->nodes[i].id].append(i);
    QVector<bool> reachable(result->nodes.size(), false);
    QVector<int> predecessor(result->nodes.size(), -1);
    QStringList rootPrefix(result->nodes.size());
    QQueue<int> queue;
    for (int i = 0; i < result->nodes.size(); ++i) {
        const DataNode &node = result->nodes[i];
        const bool external = scriptReferences.value(sc2dh::catalogIdentityKey(node.elementName,node.id)) > 0;
        if (whitelistIds.contains(node.id) || isProtectedObject(node) || isExternalRootType(node) || external) {
            reachable[i] = true;
            rootPrefix[i] = external ? QStringLiteral("Script/Trigger/Placement") : usageLabel(node);
            queue.enqueue(i);
        }
    }
    while (!queue.isEmpty()) {
        const int sourceIndex = queue.dequeue();
        const DataNode &source = result->nodes[sourceIndex];
        for (const QString &targetId : source.referenceKeys) {
            if (source.elementName.startsWith(QStringLiteral("CDataCollection"), Qt::CaseInsensitive)) continue;
            for (int targetIndex : targetsFor(targetId)) {
                if (reachable[targetIndex]) continue;
                reachable[targetIndex] = true;
                predecessor[targetIndex] = sourceIndex;
                rootPrefix[targetIndex] = rootPrefix[sourceIndex];
                queue.enqueue(targetIndex);
            }
        }
    }

    for (int i = 0; i < result->nodes.size(); ++i)
    {
        DataNode &node = result->nodes[i];
        if (node.id.isEmpty())
            continue;
        UnusedCandidateInfo info;
        info.nodeIndex = i;
        info.incomingXmlReferences = inboundReferences.value(sc2dh::catalogIdentityKey(node.elementName,node.id));
        info.dataCollectionReferences = dataCollectionReferences.value(sc2dh::catalogIdentityKey(node.elementName,node.id));
        info.scriptReferences = scriptReferences.value(sc2dh::catalogIdentityKey(node.elementName,node.id));
        info.whitelisted = whitelistIds.contains(node.id);
        info.protectedObject = isProtectedObject(node);
        info.incomingXmlSources = inboundSources.value(sc2dh::catalogIdentityKey(node.elementName,node.id));
        info.outgoingXmlTargets = outboundTargets.value(sc2dh::catalogIdentityKey(node.elementName,node.id));
        info.dataCollectionMemberships = collectionMemberships.value(sc2dh::catalogIdentityKey(node.elementName,node.id));
        info.externalReferenceSources = externalSources.value(sc2dh::catalogIdentityKey(node.elementName,node.id));
        info.incomingXmlSources.removeDuplicates();
        info.outgoingXmlTargets.removeDuplicates();
        info.dataCollectionMemberships.removeDuplicates();
        info.externalReferenceSources.removeDuplicates();

        if (reachable[i]) {
            QVector<int> chain;
            for (int cursor = i; cursor >= 0; cursor = predecessor[cursor]) chain.prepend(cursor);
            if (!rootPrefix[i].isEmpty() && (chain.isEmpty() || rootPrefix[i] != usageLabel(result->nodes[chain.front()])))
                info.usagePath << rootPrefix[i];
            for (int pathIndex : chain) info.usagePath << usageLabel(result->nodes[pathIndex]);
            info.reason = QStringLiteral("Reachable from a gameplay/editor/runtime root: %1").arg(info.usagePath.join(QStringLiteral(" -> ")));
            info.usageState = (info.whitelisted || info.protectedObject || info.scriptReferences > 0)
                ? UsageState::Blocked : UsageState::Used;
            info.state = CandidateState::Blocked;
            info.removalSafety = RemovalSafety::Unsafe;
            if (!info.whitelisted && !info.protectedObject && !blockingScriptSources.value(sc2dh::catalogIdentityKey(node.elementName,node.id)).isEmpty()) {
                info.removalSafety = RemovalSafety::Unknown;
                info.reason = QStringLiteral("Unknown reference coverage: %1").arg(blockingScriptSources.value(sc2dh::catalogIdentityKey(node.elementName,node.id)).join(QStringLiteral("; ")));
            }
            info.riskLevel = QStringLiteral("high");
        } else {
            const bool disconnected = info.incomingXmlReferences == 0 && info.outgoingXmlTargets.isEmpty();
            if (disconnected) {
                info.usageState = UsageState::Disconnected;
                info.reason = info.dataCollectionReferences > 0
                    ? QStringLiteral("Disconnected from gameplay; referenced only by Data Collection")
                    : QStringLiteral("No incoming or outgoing gameplay references");
            } else {
                info.usageState = UsageState::UnusedSubgraph;
                info.reason = QStringLiteral("Linked subgraph is not reachable from any gameplay/editor/runtime root");
            }
            info.state = CandidateState::Safe;
            info.removalSafety = RemovalSafety::Safe;
            info.riskLevel = isPrimaryEntity(node)
                ? (disconnected ? QStringLiteral("low") : QStringLiteral("medium"))
                : QStringLiteral("medium");
            node.candidateUnused = true;
            result->possibleUnusedNodeIndices.append(i);
        }
        result->unusedCandidates.append(info);
    }
}

bool FolderAnalyzer::populateReferenceIds(AnalysisResult *result,
                                          const std::function<void()> &heartbeat,
                                          const std::function<bool()> &isCancelled) const
{
    if (!result)
    {
        return false;
    }

    for (int nodeIndex = 0; nodeIndex < result->nodes.size(); ++nodeIndex)
    {
        if (nodeIndex % 25 == 0)
        {
            if (heartbeat)
                heartbeat();
            if (isCancelled && isCancelled())
                return false;
        }
        DataNode &node = result->nodes[nodeIndex];
        node.referencedIds = sc2dh::extractCatalogLinkReferences(node).values();
        std::sort(node.referencedIds.begin(), node.referencedIds.end());
    }
    appendImplicitActorUnitReferences(result);
    return true;
}

bool FolderAnalyzer::analyzeFolder(const QString &rootFolder,
                                   const QSet<QString> &whitelistIds,
                                   AnalysisResult *result,
                                   QString *errorMessage,
                                   const std::function<void(int, int, const QString &)> &progress,
                                   const std::function<bool()> &isCancelled,
                                   const QStringList &dependencySearchRoots,
                                   const QHash<QString, QString> &dependencyHandleMappings) const
{
    if (!result)
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Internal error: result is null.");
        }
        return false;
    }

    QFileInfo rootInfo(rootFolder);
    if (!rootInfo.exists() || !rootInfo.isDir())
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Folder does not exist: %1").arg(rootFolder);
        }
        return false;
    }

    *result = AnalysisResult{};
    result->rootFolder = rootFolder;

    XmlLoader loader;
    QStringList filePaths;
    QDirIterator it(rootFolder, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString filePath = it.next();
        const QString relative = QDir(rootFolder).relativeFilePath(filePath);
        if (relative.startsWith(QStringLiteral("backup_"), Qt::CaseInsensitive) || relative.contains(QStringLiteral("/backup_"), Qt::CaseInsensitive))
        {
            continue;
        }
        filePaths.append(filePath);
        if (isCancelled && isCancelled())
        {
            if (errorMessage)
                *errorMessage = QStringLiteral("Analysis canceled.");
            return false;
        }
    }

    std::sort(filePaths.begin(), filePaths.end(), [](const QString &a, const QString &b) {
        const int folded = QString::compare(a, b, Qt::CaseInsensitive);
        return folded == 0 ? a < b : folded < 0;
    });
    QStringList manifestFiles;
    QHash<QString, QStringList> filesByRelativePath;
    for (const QString &path : filePaths) {
        const QString relative = QDir(rootFolder).relativeFilePath(path).replace('\\', '/');
        filesByRelativePath[relative.toCaseFolded()].append(path);
        if (relative.compare(QStringLiteral("Base.SC2Data/GameData.xml"), Qt::CaseInsensitive) == 0)
            manifestFiles.append(path);
    }
    QSet<QString> selectedGameDataFiles;
    bool hasParsedIncludeManifest = false;
    if (manifestFiles.size() > 1) {
        result->incompleteSources << QStringLiteral("Multiple local GameData include manifests require explicit selection.");
    } else if (manifestFiles.size() == 1) {
        const QString manifestPath = manifestFiles.first();
        QFile manifestFile(manifestPath);
        if (QFileInfo(manifestPath).size() > 16ll * 1024ll * 1024ll
            || !manifestFile.open(QIODevice::ReadOnly)) {
            result->incompleteSources << QStringLiteral("Local GameData include manifest is unreadable or oversized: %1")
                .arg(manifestPath);
        } else {
            QStringList includes;
            hasParsedIncludeManifest = sc2dh::parseGameDataCatalogIncludes(
                manifestFile.readAll(), manifestPath, &includes, &result->incompleteSources);
            for (const QString &include : includes) {
                const QString expected = QStringLiteral("Base.SC2Data/") + include;
                const QStringList matches = filesByRelativePath.value(expected.toCaseFolded());
                if (matches.size() != 1) {
                    result->incompleteSources << QStringLiteral("Local GameData include has %1 matching files: %2")
                        .arg(matches.size()).arg(expected);
                    continue;
                }
                selectedGameDataFiles.insert(matches.first().toCaseFolded());
            }
        }
    }

    for (int fileIndex = 0; fileIndex < filePaths.size(); ++fileIndex)
    {
        const QString filePath = filePaths[fileIndex];
        if (isCancelled && isCancelled())
        {
            if (errorMessage)
                *errorMessage = QStringLiteral("Analysis canceled.");
            return false;
        }
        if (progress)
            progress(fileIndex, filePaths.size(), filePath);
        QFileInfo info(filePath);

        ScannedFileInfo scanned;
        scanned.filePath = filePath;
        scanned.isXml = isXmlFile(info);
        scanned.isSc2DataLike = isSc2DataLikeFile(info);
        scanned.size = info.size();
        result->scannedFiles.append(scanned);

        constexpr qint64 maxIndexedSourceBytes = 16ll * 1024ll * 1024ll;
        if (scanned.size > maxIndexedSourceBytes)
        {
            result->unsupportedSources.append(
                QStringLiteral("%1: source exceeds the %2 MiB reference-index limit")
                    .arg(filePath)
                    .arg(maxIndexedSourceBytes / (1024 * 1024)));
        }
        else
        {
            QString revisionError;
            const SourceRevision revision = captureSourceRevision(filePath, &revisionError);
            if (!revisionError.isEmpty())
            {
                result->unreadableSources.append(revisionError);
            }
            else
            {
                result->sourceRevisions.append(revision);
            }
        }

        if (!scanned.isXml)
        {
            continue;
        }

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly))
        {
            ParseErrorInfo error;
            error.filePath = filePath;
            error.message = QStringLiteral("Unable to open file.");
            result->parseErrors.append(error);
            continue;
        }

        const QByteArray xmlBytes = file.readAll();
        file.close();
        result->sourceXmlByFile.insert(filePath, QString::fromUtf8(xmlBytes));

        const QString relative = QDir(rootFolder).relativeFilePath(filePath).replace('\\', '/');
        if (hasParsedIncludeManifest
            && relative.startsWith(QStringLiteral("Base.SC2Data/GameData/"), Qt::CaseInsensitive)
            && !selectedGameDataFiles.contains(filePath.toCaseFolded())) {
            result->inactiveGameDataSources.append(filePath);
            continue;
        }

        QVector<DataNode> fileNodes;
        QString parseError;
        if (!loader.extractNodes(filePath, xmlBytes, &fileNodes, &parseError))
        {
            ParseErrorInfo error;
            error.filePath = filePath;
            error.message = parseError;
            result->parseErrors.append(error);
            continue;
        }

        result->nodes += fileNodes;
    }
    if (progress)
        progress(filePaths.size(), filePaths.size(), QString());

    result->sourceDiscoveryComplete = true;

    return finalizeAnalysisResult(result, whitelistIds, errorMessage,
                                  [&] {
                                      if (progress)
                                          progress(filePaths.size(), filePaths.size(), QString());
                                  },
                                  isCancelled, dependencySearchRoots, dependencyHandleMappings);
}

bool FolderAnalyzer::finalizeAnalysisResult(AnalysisResult *result,
                                            const QSet<QString> &whitelistIds,
                                            QString *errorMessage,
                                            const std::function<void()> &heartbeat,
                                            const std::function<bool()> &isCancelled,
                                            const QStringList &dependencySearchRoots,
                                            const QHash<QString, QString> &dependencyHandleMappings) const
{
    const QByteArray settingsAtStart = optimizationSettingsFingerprint();
    result->optimizationSettingsRevision = settingsAtStart;
    // A declared mod is not a loaded dependency layer. The current resolver
    // supports an explicit local source tree; dependency stacks remain partial.
    ScannedFileReader metadataReader(*result);
    bool hasGameDataXml = false;
    QStringList unselectedGameDataPaths;
    for (const auto &file : result->scannedFiles) {
        const QString relative = ScannedFileReader::relativePath(result->rootFolder, file.filePath).replace('\\', '/');
        if (!file.isXml) continue;
        const bool localGameData = relative.startsWith(QStringLiteral("GameData/"), Qt::CaseInsensitive)
            || relative.startsWith(QStringLiteral("Base.SC2Data/GameData/"), Qt::CaseInsensitive);
        if (localGameData || relative.contains(QStringLiteral("/GameData/"), Qt::CaseInsensitive))
            hasGameDataXml = true;
        if (!localGameData && relative.contains(QStringLiteral("/GameData/"), Qt::CaseInsensitive)
            && unselectedGameDataPaths.size() < 8)
            unselectedGameDataPaths << relative;
    }
    int componentLists = 0;
    bool gameDataComponentActive = false;
    for (const auto &file : result->scannedFiles) {
        if (QFileInfo(file.filePath).fileName().compare(QStringLiteral("ComponentList.SC2Components"), Qt::CaseInsensitive) != 0)
            continue;
        ++componentLists;
        QByteArray bytes;
        if (!metadataReader.readBytes(file, 16ll * 1024ll * 1024ll, &bytes)) {
            result->incompleteSources << QStringLiteral("%1: component list is unreadable.").arg(file.filePath);
            continue;
        }
        pugi::xml_document components; QString parseError;
        if (!XmlLoader().loadDocument(bytes, &components, &parseError)
            || QString::fromUtf8(components.document_element().name()) != QStringLiteral("Components")) {
            result->incompleteSources << QStringLiteral("%1: component list is malformed: %2").arg(file.filePath, parseError);
            continue;
        }
        for (const pugi::xml_node child : components.document_element().children("DataComponent")) {
            if (QString::fromUtf8(child.attribute("Type").value()) != QStringLiteral("gada"))
                continue;
            const QString path = QString::fromUtf8(child.child_value()).trimmed().replace('\\', '/');
            if (path.compare(QStringLiteral("GameData"), Qt::CaseInsensitive) == 0
                || path.compare(QStringLiteral("Base.SC2Data/GameData"), Qt::CaseInsensitive) == 0)
                gameDataComponentActive = true;
            else
                result->incompleteSources << QStringLiteral("%1: GameData component path %2 is not resolved.")
                    .arg(file.filePath, path);
        }
    }
    if (componentLists > 1)
        result->incompleteSources << QStringLiteral("Multiple component lists require explicit active-layer selection.");
    if (hasGameDataXml && componentLists > 0 && !gameDataComponentActive)
        result->incompleteSources << QStringLiteral("GameData XML exists but is not listed as an active GameData component.");
    if (componentLists > 0 && !unselectedGameDataPaths.isEmpty())
        result->incompleteSources << QStringLiteral("GameData XML outside the selected local component requires layer resolution: %1")
            .arg(unselectedGameDataPaths.join(QStringLiteral(", ")));
    result->declaredDependencies.clear();
    QVector<ScannedFileInfo> documentInfoFiles;
    for(const auto &file:result->scannedFiles)
        if(QFileInfo(file.filePath).fileName().compare(QStringLiteral("DocumentInfo"),Qt::CaseInsensitive)==0)
            documentInfoFiles.append(file);
    std::sort(documentInfoFiles.begin(),documentInfoFiles.end(),[](const auto &a,const auto &b) {
        const int folded=QString::compare(a.filePath,b.filePath,Qt::CaseInsensitive);
        return folded==0 ? a.filePath<b.filePath : folded<0;
    });
    if(documentInfoFiles.size()>1)
        result->incompleteSources << QStringLiteral("Multiple DocumentInfo files require explicit active-layer selection.");
    for(const auto &file:documentInfoFiles) {
        QByteArray bytes;
        if(!metadataReader.readBytes(file,16ll*1024ll*1024ll,&bytes)) {
            result->incompleteSources << QStringLiteral("%1: DocumentInfo is unreadable.").arg(file.filePath);
            continue;
        }
        QVector<DependencyDeclaration> declarations;QString parseError;
        if(!sc2dh::parseDocumentInfoDependencies(bytes,file.filePath,&declarations,&parseError)) {
            result->incompleteSources << QStringLiteral("%1: %2").arg(file.filePath,parseError);continue;
        }
        for(const auto &declaration:declarations) {
            result->declaredDependencies.append(declaration);
            result->incompleteSources << QStringLiteral("%1: declared dependency layer not resolved: %2")
                .arg(file.filePath,declaration.raw);
        }
    }
    if(!sc2dh::locateDependencySources(result,dependencySearchRoots,dependencyHandleMappings,isCancelled,errorMessage)) return false;
    if(!sc2dh::loadDependencyLayerData(result,isCancelled,errorMessage)) return false;
    result->dependencySourcesLocated=sc2dh::dependencySourcesLocated(*result);
    result->hypotheticalLayerOrder=sc2dh::hypotheticalLayerOrder(*result);
    sc2dh::refreshCatalogSchema();
    const QByteArray schemaAtStart=sc2dh::catalogSchemaFingerprint();
    if (!populateReferenceIds(result, heartbeat, isCancelled))
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("Analysis canceled.");
        return false;
    }
    result->catalogSchemaRevision = schemaAtStart;
    if (!sc2dh::catalogSchemaAvailable())
        result->incompleteSources.append(QStringLiteral("Structural catalog schema index is missing or incompatible."));
    sc2dh::indexLayeredDeclarations(result);
    sc2dh::resolveCatalogReferences(result);
    sc2dh::markUnresolvedLayerEffects(result);
    result->referenceExtractionComplete = true;
    populateDuplicateAndCandidateFlags(result, whitelistIds);
    result->dependencyGraphComplete = result->declaredDependencies.isEmpty();
    DeepCleanupService().populateCandidates(result);
    QHash<QString, QStringList> unknownByCatalog;
    for (const auto &node:result->nodes) {
        if (!node.resolutionIssues.isEmpty()) unknownByCatalog[sc2dh::catalogIdentityScope(node.elementName)] += node.resolutionIssues;
        for (const QString &catalog:node.unknownReferenceCatalogs) unknownByCatalog[catalog] += node.resolutionIssues;
    }
    for (auto &candidate:result->unusedCandidates) {
        auto &node=result->nodes[candidate.nodeIndex];
        const auto issues=unknownByCatalog.value(sc2dh::catalogIdentityScope(node.elementName));
        if (candidate.state==CandidateState::Safe && !issues.isEmpty()) {
            candidate.state=CandidateState::Blocked;
            candidate.removalSafety=RemovalSafety::Unknown;
            candidate.usageState=UsageState::Blocked;
            candidate.reason=QStringLiteral("Unknown semantic coverage in this catalog: %1").arg(issues.join(QStringLiteral("; ")));
            node.candidateUnused=false;
            result->possibleUnusedNodeIndices.removeAll(candidate.nodeIndex);
        }
    }


    for (const SourceRevision &revision : result->sourceRevisions)
    {
        QString revisionError;
        if (sourceRevisionMatches(revision, &revisionError))
            continue;
        result->sourceChangedDuringAnalysis = true;
        if (!revisionError.isEmpty() && !result->incompleteSources.contains(revisionError))
            result->incompleteSources.append(revisionError);
    }
    if(schemaAtStart!=sc2dh::catalogSchemaFingerprint()) {
        result->sourceChangedDuringAnalysis=true;
        result->incompleteSources << QStringLiteral("Catalog schema changed during analysis.");
    }
    if (settingsAtStart != optimizationSettingsFingerprint()) {
        result->sourceChangedDuringAnalysis = true;
        result->incompleteSources << QStringLiteral("Optimization or backup settings changed during analysis.");
    }
    sc2dh::collectEditorLayeredScalarDiagnostics(result);
    result->incompleteSources.removeDuplicates();
    updateAnalysisCompleteness(result);
    enforceAnalysisCompletenessSafety(result);
    result->analysisReportText = buildAnalysisReport(*result);
    result->plannedChangesReportText = buildDryRunReport(*result, QVector<int>{});
    return true;
}

QString FolderAnalyzer::buildAnalysisReport(const AnalysisResult &result) const
{
    QString report;
    report += QStringLiteral("SC2 Data Helper Analysis Report\n");
    report += QStringLiteral("Root folder: %1\n").arg(result.rootFolder);
    report += QStringLiteral("Source and local graph coverage: %1\n").arg(analysisCompletenessName(result.completeness));
    if (!result.declaredDependencies.isEmpty()) {
        report += QStringLiteral("Dependency source archives located: %1 (diagnostic only)\n")
            .arg(result.dependencySourcesLocated ? QStringLiteral("yes") : QStringLiteral("no"));
        if (!result.hypotheticalLayerOrder.isEmpty())
            report += QStringLiteral("Hypothetical dependency-first order, not runtime proof: %1\n")
                .arg(result.hypotheticalLayerOrder.join(QStringLiteral(" -> ")));
    }
    for(const auto &diagnostic:result.editorLayeredScalars)
        report += QStringLiteral("Editor-observed scalar diagnostic (not runtime/Safe proof): %1/%2 %3=%4 from %5\n")
            .arg(diagnostic.object.catalog,diagnostic.object.id,diagnostic.field,
                 diagnostic.value,diagnostic.selectedDeclaration.source);
    int unresolvedDeclarations = 0, unknownCandidates = 0;
    for (const DataNode &node : result.nodes) if (!node.resolutionIssues.isEmpty()) ++unresolvedDeclarations;
    for (const auto &candidate : result.unusedCandidates) if (candidate.removalSafety == RemovalSafety::Unknown) ++unknownCandidates;
    report += QStringLiteral("Semantic coverage: %1 unresolved declarations, %2 Unknown removal candidates; supported subset only\n")
        .arg(unresolvedDeclarations).arg(unknownCandidates);
    report += QStringLiteral("Total files scanned: %1\n").arg(result.totalFilesScanned());
    report += QStringLiteral("Total XML files: %1\n").arg(result.totalXmlFiles());
    report += QStringLiteral("Total data nodes found: %1\n").arg(result.totalDataNodes());
    report += QStringLiteral("Duplicate ID groups: %1\n").arg(result.duplicateIdGroups.size());
    report += QStringLiteral("Duplicate XML content groups: %1\n").arg(result.duplicateContentGroups.size());
    report += QStringLiteral("Suspicious empty nodes: %1\n").arg(result.suspiciousEmptyNodeIndices.size());
    report += QStringLiteral("Possible unused candidates: %1\n").arg(result.possibleUnusedNodeIndices.size());
    report += QStringLiteral("Deep cleanup candidates: %1\n").arg(result.deepCleanupCandidates.size());
    int blockedUnused = 0;
    for (const UnusedCandidateInfo &info : result.unusedCandidates)
        if (info.state == CandidateState::Blocked)
            ++blockedUnused;
    report += QStringLiteral("Blocked unused data objects: %1\n").arg(blockedUnused);
    report += QStringLiteral("Parse errors: %1\n\n").arg(result.parseErrors.size());
    report += QStringLiteral("Unreadable sources: %1\n").arg(result.unreadableSources.size());
    report += QStringLiteral("Unsupported sources: %1\n").arg(result.unsupportedSources.size());
    report += QStringLiteral("Incomplete sources: %1\n\n").arg(result.incompleteSources.size());

    report += QStringLiteral("Duplicate IDs\n");
    for (const DuplicateIdGroup &group : result.duplicateIdGroups)
    {
        report += QStringLiteral("- ID: %1 | same file: %2 | cross file: %3 | count: %4\n")
                      .arg(group.id)
                      .arg(group.sameFile ? QStringLiteral("yes") : QStringLiteral("no"))
                      .arg(group.crossFile ? QStringLiteral("yes") : QStringLiteral("no"))
                      .arg(group.nodeIndices.size());
        for (int index : group.nodeIndices)
        {
            const DataNode &node = result.nodes[index];
            report += QStringLiteral("  - %1\n").arg(nodeLocationDescription(node));
        }
    }

    report += QStringLiteral("\nExact duplicate body groups\n");
    QHash<QString, int> incomingReferenceCount;
    for (const DataNode &source : result.nodes)
        for (const QString &reference : source.referencedIds)
            ++incomingReferenceCount[reference];
    for (const DuplicateContentGroup &group : result.duplicateContentGroups)
    {
        const DataNode &recommended = result.nodes[group.nodeIndices.front()];
        int redirectable = 0;
        for (int index : group.nodeIndices)
            redirectable += incomingReferenceCount.value(result.nodes[index].id);
        const QString classification = group.autoRecommended
            ? QStringLiteral("automatic merge candidate")
            : group.mergeCandidate ? QStringLiteral("manual merge review")
                                   : QStringLiteral("allowed identical body");
        report += QStringLiteral("- Hash: %1 | count: %2 | ID mask: %3 | classification: %4 | recommended keep: %5 | redirectable references: %6 | unsafe: %7\n")
                      .arg(group.contentHash.left(12))
                      .arg(group.nodeIndices.size())
                      .arg(group.commonIdMask,
                           classification,
                           recommended.id)
                      .arg(redirectable)
                      .arg(result.parseErrors.isEmpty() ? QStringLiteral("no") : QStringLiteral("yes (parse errors may hide references)"));
        for (int index : group.nodeIndices)
        {
            const DataNode &node = result.nodes[index];
            report += QStringLiteral("  - %1 | %2\n").arg(node.id, node.sourceFile);
        }
    }

    report += QStringLiteral("\nSuspicious empty nodes\n");
    for (int index : result.suspiciousEmptyNodeIndices)
    {
        const DataNode &node = result.nodes[index];
        report += QStringLiteral("- %1\n").arg(nodeLocationDescription(node));
    }

    report += QStringLiteral("\nPossible unused candidates\n");
    for (int index : result.possibleUnusedNodeIndices)
    {
        const DataNode &node = result.nodes[index];
        report += QStringLiteral("- %1\n").arg(nodeLocationDescription(node));
    }

    report += QStringLiteral("\nUsage classification\n");
    for (const UnusedCandidateInfo &info : result.unusedCandidates) {
        const DataNode &node = result.nodes[info.nodeIndex];
        const QString state = info.usageState == UsageState::Used ? QStringLiteral("Used")
            : info.usageState == UsageState::Disconnected ? QStringLiteral("Disconnected")
            : info.usageState == UsageState::UnusedSubgraph ? QStringLiteral("Unused subgraph")
            : info.usageState == UsageState::Risky ? QStringLiteral("Risky") : QStringLiteral("Blocked");
        report += QStringLiteral("- %1 | %2 | reason: %3 | path: %4 | incoming XML: %5 | external: %6 | collections: %7 | risk: %8\n")
                      .arg(state, node.id, info.reason, info.usagePath.join(QStringLiteral(" -> ")),
                           info.incomingXmlSources.join(QStringLiteral(", ")),
                           info.externalReferenceSources.join(QStringLiteral(", ")),
                           info.dataCollectionMemberships.join(QStringLiteral(", ")), info.riskLevel);
    }

    report += QStringLiteral("\nBlocked unused data objects\n");
    for (const UnusedCandidateInfo &info : result.unusedCandidates)
    {
        if (info.state != CandidateState::Blocked)
            continue;
        const DataNode &node = result.nodes[info.nodeIndex];
        report += QStringLiteral("- %1 | %2 | gameplay incoming: %3 | collection links: %4 | script: %5 | whitelist: %6 | risk: %7\n")
                      .arg(node.id, info.reason)
                      .arg(info.incomingXmlReferences)
                      .arg(info.dataCollectionReferences)
                      .arg(info.scriptReferences)
                      .arg(info.whitelisted ? QStringLiteral("yes") : QStringLiteral("no"), info.riskLevel);
    }

    report += QStringLiteral("\nDeep cleanup candidates\n");
    for (const DeepCleanupCandidate &candidate : result.deepCleanupCandidates)
    {
        const QString state = candidate.state == CandidateState::Safe ? QStringLiteral("Safe")
            : candidate.state == CandidateState::Risky ? QStringLiteral("Risky")
                                                       : QStringLiteral("Blocked");
        report += QStringLiteral("- %1 | %2 | %3 | %4 | %5 | %6\n")
                      .arg(state,
                           deepCleanupKindName(candidate.kind),
                           deepCleanupActionName(candidate.action),
                           candidate.label,
                           candidate.filePath,
                           candidate.reason);
    }

    report += QStringLiteral("\nParse errors\n");
    for (const ParseErrorInfo &error : result.parseErrors)
    {
        report += QStringLiteral("- %1: %2\n").arg(error.filePath, error.message);
    }
    report += QStringLiteral("\nAnalysis source diagnostics\n");
    for (const QString &source : result.unreadableSources)
        report += QStringLiteral("- Unreadable: %1\n").arg(source);
    for (const QString &source : result.unsupportedSources)
        report += QStringLiteral("- Unsupported: %1\n").arg(source);
    for (const QString &source : result.incompleteSources)
        report += QStringLiteral("- Incomplete: %1\n").arg(source);
    return report;
}

QString FolderAnalyzer::buildDryRunReport(const AnalysisResult &result, const QVector<int> &selectedRows) const
{
    QString report;
    report += QStringLiteral("Optimization Preview\n");
    report += QStringLiteral("Selected nodes: %1\n").arg(selectedRows.size());

    QHash<QString, QVector<const DataNode *>> byFile;
    for (int index : selectedRows)
    {
        if (index < 0 || index >= result.nodes.size())
        {
            continue;
        }
        const DataNode &node = result.nodes[index];
        byFile[node.sourceFile].append(&node);
    }

    QStringList files = byFile.keys();
    std::sort(files.begin(), files.end());
    for (const QString &file : files)
    {
        report += QStringLiteral("\nFile: %1\n").arg(file);
        for (const DataNode *node : byFile.value(file))
        {
            report += QStringLiteral("  - %1 | %2 | %3 | %4\n")
                          .arg(node->elementName, node->id, node->originalLocation, node->parentNode);
        }
    }

    report += QStringLiteral("\nAffected duplicates\n");
    int estimatedRemoved = 0;
    int duplicateAffected = 0;
    for (int index : selectedRows)
    {
        if (index < 0 || index >= result.nodes.size())
        {
            continue;
        }
        const DataNode &node = result.nodes[index];
        ++estimatedRemoved;
        if (node.duplicateId)
        {
            report += QStringLiteral("- %1 | %2\n").arg(node.id, node.sourceFile);
            ++duplicateAffected;
        }
    }

    report += QStringLiteral("\nEstimated removed nodes: %1\n").arg(estimatedRemoved);
    report += QStringLiteral("Duplicate rows affected: %1\n").arg(duplicateAffected);
    report += QStringLiteral("No automatic deletion is performed. Apply only after reviewing this preview.\n");
    return report;
}

QString FolderAnalyzer::buildPlannedChangesReport(const AnalysisResult &result, const QVector<int> &selectedRows) const
{
    QString report;
    report += QStringLiteral("Planned Changes Report\n");
    report += QStringLiteral("Selected rows: %1\n\n").arg(selectedRows.size());

    int count = 0;
    QHash<QString, int> filesToChange;
    for (int index : selectedRows)
    {
        if (index < 0 || index >= result.nodes.size())
        {
            continue;
        }
        const DataNode &node = result.nodes[index];
        ++count;
        filesToChange[node.sourceFile] += 1;
        report += QStringLiteral("- %1 | %2 | %3 | %4\n")
                      .arg(node.sourceFile, node.elementName, node.id, node.originalLocation);
    }
    report += QStringLiteral("\nFiles to change: %1\n").arg(filesToChange.size());
    report += QStringLiteral("Estimated removed nodes: %1\n").arg(count);
    return report;
}

bool FolderAnalyzer::applySelectedChanges(const AnalysisResult &result,
                                          const QVector<int> &selectedRows,
                                          const QString &rootFolder,
                                          const QSet<QString> &whitelistIds,
                                          QString *backupFolder,
                                          QString *errorMessage,
                                          QStringList *changedFiles,
                                          int *removedNodes,
                                          int *skippedNodes) const
{
    if (removedNodes)
    {
        *removedNodes = 0;
    }
    if (skippedNodes)
    {
        *skippedNodes = 0;
    }

    const DestructiveOperationPermission initialPermission = canApplyDestructiveChanges(result);
    if (!initialPermission.allowed)
    {
        if (errorMessage)
            *errorMessage = destructiveOperationPermissionText(initialPermission);
        return false;
    }

    if (selectedRows.isEmpty())
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("No rows selected.");
        }
        return false;
    }

    QVector<int> validRows;
    for (int row : selectedRows)
    {
        if (row < 0 || row >= result.nodes.size())
        {
            if (skippedNodes)
            {
                ++(*skippedNodes);
            }
            continue;
        }
        const DataNode &node = result.nodes[row];
        const auto candidate = std::find_if(result.unusedCandidates.cbegin(), result.unusedCandidates.cend(),
                                            [row](const UnusedCandidateInfo &info)
                                            { return info.nodeIndex == row; });
        if (whitelistIds.contains(node.id) || candidate == result.unusedCandidates.cend()
            || candidate->state != CandidateState::Safe
            || (candidate->usageState != UsageState::Disconnected
                && candidate->usageState != UsageState::UnusedSubgraph))
        {
            if (skippedNodes)
            {
                ++(*skippedNodes);
            }
            continue;
        }
        if (!validRows.contains(row))
            validRows.append(row);
    }

    bool changed=true;
    while(changed) {
        changed=false;
        QSet<int> selected;
        QSet<QString> selectedKeys,blocked;
        for(int row:validRows) {selected.insert(row); selectedKeys.insert(sc2dh::catalogIdentityKey(result.nodes[row].elementName,result.nodes[row].id));}
        for(int source=0;source<result.nodes.size();++source) {
            if(selected.contains(source) || result.nodes[source].elementName.startsWith(QStringLiteral("CDataCollection"),Qt::CaseInsensitive)) continue;
            for(const QString &reference:result.nodes[source].referenceKeys) {
                if(selectedKeys.contains(reference)) blocked.insert(reference);
                else if(reference.startsWith(QStringLiteral("*")+QChar(0x1f)))
                    for(const QString &target:selectedKeys)
                        if(target.section(QChar(0x1f),1)==reference.section(QChar(0x1f),1)) blocked.insert(target);
            }
        }
        QVector<int> kept;
        for(int row:validRows) {
            if(blocked.contains(sc2dh::catalogIdentityKey(result.nodes[row].elementName,result.nodes[row].id))) {changed=true; if(skippedNodes) ++*skippedNodes;}
            else kept.append(row);
        }
        validRows=kept;
    }

    QHash<QString, QSet<QString>> locationsByFile;
    QSet<QString> removedIds;
    QSet<QString> removedObjectKeys;
    int plannedRemovals = 0;
    for (int row : validRows) {
        const DataNode &node = result.nodes[row];
        const int beforeCount = locationsByFile[node.sourceFile].size();
        locationsByFile[node.sourceFile].insert(node.originalLocation);
        removedIds.insert(node.id);
        removedObjectKeys.insert(sc2dh::catalogIdentityKey(node.elementName,node.id));
        if (locationsByFile[node.sourceFile].size() > beforeCount)
            ++plannedRemovals;
    }

    if (locationsByFile.isEmpty()) {
        if (changedFiles)
            changedFiles->clear();
        if (removedNodes)
            *removedNodes = 0;
        return true;
    }

    QSet<QString> idsStillPresent;
    for (const DataNode &node : result.nodes) {
        if (!removedIds.contains(node.id)) continue;
        if (!locationsByFile.value(node.sourceFile).contains(node.originalLocation)) idsStillPresent.insert(node.id);
    }
    for (const QString &id : idsStillPresent) removedIds.remove(id);
    // Removing one duplicate declaration does not remove the catalog object.
    for (const DataNode &node : result.nodes)
        if (!locationsByFile.value(node.sourceFile).contains(node.originalLocation))
            removedObjectKeys.remove(sc2dh::catalogIdentityKey(node.elementName,node.id));

    const QString analysisReport = buildAnalysisReport(result);
    const QString plannedChanges = buildPlannedChangesReport(result, selectedRows);

    QSet<QString> collectionFiles;
    for (const DataNode &node : result.nodes) {
        if (!node.elementName.startsWith(QStringLiteral("CDataCollection"), Qt::CaseInsensitive)) continue;
        for (const QString &reference : node.referencedIds) {
            if (removedIds.contains(reference)) {
                collectionFiles.insert(node.sourceFile);
                break;
            }
        }
    }

    const DestructiveOperationPermission commitPermission = canApplyDestructiveChanges(result);
    if (!commitPermission.allowed)
    {
        if (errorMessage)
            *errorMessage = destructiveOperationPermissionText(commitPermission);
        return false;
    }

    QHash<QString, QByteArray> rewrittenFiles;
    XmlLoader loader;
    for (auto it = locationsByFile.cbegin(); it != locationsByFile.cend(); ++it)
    {
        const QString &filePath = it.key();
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly))
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Failed to open file for rewriting: %1").arg(filePath);
            }
            return false;
        }

        QByteArray rewritten;
        QString loadError;
        if (!loader.removeNodesByLocation(file.readAll(), it.value(), &rewritten, &loadError))
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("%1: %2").arg(filePath, loadError);
            }
            return false;
        }
        rewrittenFiles.insert(filePath, rewritten);
    }

    for (const QString &filePath : collectionFiles) {
        QByteArray source = rewrittenFiles.value(filePath);
        if (source.isEmpty()) {
            QFile file(filePath);
            if (!file.open(QIODevice::ReadOnly)) {
                if (errorMessage) *errorMessage = QStringLiteral("Failed to open Data Collection file: %1").arg(filePath);
                return false;
            }
            source = file.readAll();
        }
        QByteArray rewritten;
        QString collectionError;
        if (!preserveDataCollectionRecords(source, removedIds, &rewritten, &collectionError)) {
            if (errorMessage) *errorMessage = QStringLiteral("Failed to preserve Data Collection links in %1: %2").arg(filePath, collectionError);
            return false;
        }
        rewrittenFiles.insert(filePath, rewritten);
    }

    AnalysisResult verified;
    const auto validateCommitted = [&](QString *validationError) {
        QString verifyError;
        if (!analyzeFolder(rootFolder, whitelistIds, &verified, &verifyError)) {
            if (validationError) *validationError = QStringLiteral("Post-delete analysis failed: %1").arg(verifyError);
            return false;
        }
        if (verified.completeness != AnalysisCompleteness::Complete) {
            if (validationError) *validationError = QStringLiteral("Post-delete verification was incomplete: %1.")
                                                        .arg(analysisCompletenessName(verified.completeness));
            return false;
        }
        for (const DataNode &node : verified.nodes) {
            if (removedObjectKeys.contains(sc2dh::catalogIdentityKey(node.elementName,node.id))) {
                if (validationError) *validationError = QStringLiteral("Post-delete verification failed: ID %1 still exists.").arg(node.id);
                return false;
            }
            for (const QString &reference : node.referenceKeys) if (removedObjectKeys.contains(reference) || (reference.startsWith(QStringLiteral("*")+QChar(0x1f)) && removedIds.contains(reference.section(QChar(0x1f),1)))) {
                if (node.elementName.startsWith(QStringLiteral("CDataCollection"), Qt::CaseInsensitive))
                    continue;
                if (validationError) *validationError = QStringLiteral("Post-delete verification failed: %1 still references %2.").arg(node.id, reference);
                return false;
            }
        }
        QSet<QString> verifiedReachableIds;
        for (const UnusedCandidateInfo &info : verified.unusedCandidates) {
            if (info.nodeIndex < 0 || info.nodeIndex >= verified.nodes.size()) continue;
            if (info.usageState == UsageState::Used || info.usageState == UsageState::Blocked)
                verifiedReachableIds.insert(verified.nodes[info.nodeIndex].id);
        }
        for (const UnusedCandidateInfo &info : result.unusedCandidates) {
            if (info.nodeIndex < 0 || info.nodeIndex >= result.nodes.size()) continue;
            const QString id = result.nodes[info.nodeIndex].id;
            if ((info.usageState == UsageState::Used || info.usageState == UsageState::Blocked)
                && !info.usagePath.isEmpty()
                && !removedIds.contains(id) && !verifiedReachableIds.contains(id)) {
                if (validationError) *validationError = QStringLiteral("Post-delete verification failed: preserved object %1 lost its usage path.").arg(id);
                return false;
            }
        }
        return true;
    };

    QVector<TransactionalFileChange> transactionChanges;
    for (auto it = rewrittenFiles.cbegin(); it != rewrittenFiles.cend(); ++it) {
        TransactionalFileChange change;
        change.relativePath = relativePath(rootFolder, it.key());
        change.contents = it.value();
        transactionChanges.append(change);
    }
    const auto validateStaged = [&](const QString &stagingFolder, QString *validationError) {
        XmlLoader stagedLoader;
        for (const TransactionalFileChange &change : transactionChanges) {
            QFile file(QDir(stagingFolder).absoluteFilePath(change.relativePath));
            if (!file.open(QIODevice::ReadOnly)) {
                if (validationError) *validationError = QStringLiteral("Cannot reopen staged XML: %1").arg(change.relativePath);
                return false;
            }
            QVector<DataNode> stagedNodes;
            QString parseError;
            if (!stagedLoader.extractNodes(change.relativePath, file.readAll(), &stagedNodes, &parseError)) {
                if (validationError) *validationError = QStringLiteral("Staged XML validation failed for %1: %2")
                                                            .arg(change.relativePath, parseError);
                return false;
            }
        }
        return true;
    };

    const FolderSaveTransactionResult transaction = BackupManager().applyFolderTransaction(
        rootFolder, transactionChanges, analysisReport, plannedChanges, validateStaged, validateCommitted,
        {}, {}, result.optimizationSettingsRevision);
    if (!transaction.success) {
        if (errorMessage) *errorMessage = QStringLiteral("[%1] %2")
                                              .arg(operationErrorCodeName(transaction.errorCode), transaction.error);
        return false;
    }
    if (backupFolder)
        *backupFolder = transaction.backupFolder;

    if (changedFiles)
    {
        *changedFiles = transaction.changedFiles;
    }
    if (removedNodes)
    {
        *removedNodes = plannedRemovals;
    }
    if (skippedNodes && *skippedNodes < 0)
    {
        *skippedNodes = 0;
    }

    return true;
}
