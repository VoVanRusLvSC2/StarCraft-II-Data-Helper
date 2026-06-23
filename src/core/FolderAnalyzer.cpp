#include "core/FolderAnalyzer.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QSaveFile>
#include <QRegularExpression>

#include <algorithm>

namespace
{

    QStringList tokenizeReferenceValue(const QString &value)
    {
        const QString normalized = value.trimmed();
        if (normalized.isEmpty())
        {
            return {};
        }

        if (normalized.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) || normalized.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive) || normalized.contains(QStringLiteral("://")))
        {
            return {};
        }

        QStringList tokens = normalized.split(QRegularExpression(QStringLiteral("[\\s,;|:/\\\\]+")), Qt::SkipEmptyParts);
        if (normalized.contains(QLatin1Char(',')))
        {
            const QString lastSegment = normalized.section(QLatin1Char(','), -1).trimmed();
            if (!lastSegment.isEmpty())
            {
                tokens.append(lastSegment);
            }
        }
        if (normalized.contains(QLatin1Char(' ')))
        {
            const QString lastWord = normalized.section(QLatin1Char(' '), -1).trimmed();
            if (!lastWord.isEmpty())
            {
                tokens.append(lastWord);
            }
        }

        tokens.removeAll(QString());
        return tokens;
    }

    bool isProtectedObject(const DataNode &node)
    {
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

    int tokenCount(const QString &text, const QString &id)
    {
        const QRegularExpression expression(QStringLiteral("(?<![A-Za-z0-9_])%1(?![A-Za-z0-9_])")
                                                .arg(QRegularExpression::escape(id)));
        int count = 0;
        auto matches = expression.globalMatch(text);
        while (matches.hasNext())
        {
            matches.next();
            ++count;
        }
        return count;
    }

    QString numberedIdBase(const QString &id)
    {
        static const QRegularExpression expression(QStringLiteral("^(.+?)(\\d+)$"));
        const QRegularExpressionMatch match = expression.match(id);
        return match.hasMatch() ? match.captured(1) : QString();
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
    static const QSet<QString> extensions = {
        QStringLiteral("xml"), QStringLiteral("txt"), QStringLiteral("json"), QStringLiteral("ini"),
        QStringLiteral("galaxy"), QStringLiteral("csv"), QStringLiteral("yaml"), QStringLiteral("yml")};
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

    QHash<QString, QVector<int>> idGroups;
    QHash<QString, QVector<int>> contentGroups;

    for (int i = 0; i < result->nodes.size(); ++i)
    {
        const DataNode &node = result->nodes[i];
        if (!node.id.isEmpty())
        {
            idGroups[node.id].append(i);
        }
        if (!node.elementName.isEmpty() && !node.contentHash.isEmpty())
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
        group.id = it.key();
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
            group.mergeCandidate = false;
            result->duplicateContentGroups.append(group);
        }
    }

    QHash<QString, int> inboundReferences;
    for (const DataNode &sourceNode : result->nodes)
    {
        for (const QString &reference : sourceNode.referencedIds)
        {
            if (!reference.isEmpty() && reference != sourceNode.id)
            {
                ++inboundReferences[reference];
            }
        }
    }

    QHash<QString, int> scriptReferences;
    for (const ScannedFileInfo &fileInfo : result->scannedFiles)
    {
        if (fileInfo.isXml || !fileInfo.isSc2DataLike)
            continue;
        QFile file(fileInfo.filePath);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QString text = QString::fromUtf8(file.readAll());
        for (const DataNode &node : result->nodes)
        {
            if (!node.id.isEmpty())
                scriptReferences[node.id] += tokenCount(text, node.id);
        }
    }

    for (int i = 0; i < result->nodes.size(); ++i)
    {
        DataNode &node = result->nodes[i];
        if (node.id.isEmpty())
            continue;
        UnusedCandidateInfo info;
        info.nodeIndex = i;
        info.incomingXmlReferences = inboundReferences.value(node.id);
        info.scriptReferences = scriptReferences.value(node.id);
        info.whitelisted = whitelistIds.contains(node.id);
        info.protectedObject = isProtectedObject(node);
        if (info.whitelisted)
            info.reason = QStringLiteral("Whitelisted");
        else if (info.protectedObject)
            info.reason = QStringLiteral("Root/editor/runtime protected");
        else if (info.incomingXmlReferences > 0)
            info.reason = QStringLiteral("Has incoming XML references");
        else if (info.scriptReferences > 0)
            info.reason = QStringLiteral("Has script/text references");
        else
            info.reason = QStringLiteral("No incoming XML or script/text references");

        if (info.whitelisted || info.protectedObject || info.incomingXmlReferences > 0 || info.scriptReferences > 0)
        {
            info.state = CandidateState::Blocked;
            info.riskLevel = QStringLiteral("high");
        }
        else
        {
            info.state = CandidateState::Safe;
            info.riskLevel = QStringLiteral("low");
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

    QSet<QString> knownIds;
    for (const DataNode &node : result->nodes)
    {
        if (!node.id.isEmpty())
        {
            knownIds.insert(node.id);
        }
    }

    const QRegularExpression quotedValueRe(QStringLiteral("\"([^\"]*)\""));
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
        QSet<QString> references;
        const auto addCandidate = [&](const QString &candidate)
        {
            for (const QString &token : tokenizeReferenceValue(candidate))
            {
                if (token.isEmpty() || token == node.id)
                {
                    continue;
                }
                if (knownIds.contains(token))
                {
                    references.insert(token);
                }
            }
        };

        for (auto it = node.attributes.cbegin(); it != node.attributes.cend(); ++it)
        {
            addCandidate(it.value());
        }

        auto matchIterator = quotedValueRe.globalMatch(node.serializedXml);
        while (matchIterator.hasNext())
        {
            addCandidate(matchIterator.next().captured(1));
        }
        // Text nodes and less conventional attribute quoting are still references.
        for (const QString &knownId : knownIds)
        {
            if (knownId != node.id && tokenCount(node.serializedXml, knownId) > 0)
                references.insert(knownId);
        }

        node.referencedIds = references.values();
        std::sort(node.referencedIds.begin(), node.referencedIds.end());
    }
    return true;
}

bool FolderAnalyzer::analyzeFolder(const QString &rootFolder,
                                   const QSet<QString> &whitelistIds,
                                   AnalysisResult *result,
                                   QString *errorMessage,
                                   const std::function<void(int, int, const QString &)> &progress,
                                   const std::function<bool()> &isCancelled) const
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

    if (!populateReferenceIds(result, [&]
                              { if (progress) progress(filePaths.size(), filePaths.size(), QString()); }, isCancelled))
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("Analysis canceled.");
        return false;
    }
    populateDuplicateAndCandidateFlags(result, whitelistIds);
    result->analysisReportText = buildAnalysisReport(*result);
    result->plannedChangesReportText = buildDryRunReport(*result, QVector<int>{});
    return true;
}

QString FolderAnalyzer::buildAnalysisReport(const AnalysisResult &result) const
{
    QString report;
    report += QStringLiteral("SC2 Data Helper Analysis Report\n");
    report += QStringLiteral("Root folder: %1\n").arg(result.rootFolder);
    report += QStringLiteral("Total files scanned: %1\n").arg(result.totalFilesScanned());
    report += QStringLiteral("Total XML files: %1\n").arg(result.totalXmlFiles());
    report += QStringLiteral("Total data nodes found: %1\n").arg(result.totalDataNodes());
    report += QStringLiteral("Duplicate ID groups: %1\n").arg(result.duplicateIdGroups.size());
    report += QStringLiteral("Duplicate XML content groups: %1\n").arg(result.duplicateContentGroups.size());
    report += QStringLiteral("Suspicious empty nodes: %1\n").arg(result.suspiciousEmptyNodeIndices.size());
    report += QStringLiteral("Possible unused candidates: %1\n").arg(result.possibleUnusedNodeIndices.size());
    int blockedUnused = 0;
    for (const UnusedCandidateInfo &info : result.unusedCandidates)
        if (info.state == CandidateState::Blocked)
            ++blockedUnused;
    report += QStringLiteral("Blocked unused objects: %1\n").arg(blockedUnused);
    report += QStringLiteral("Parse errors: %1\n\n").arg(result.parseErrors.size());

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
    for (const DuplicateContentGroup &group : result.duplicateContentGroups)
    {
        const DataNode &recommended = result.nodes[group.nodeIndices.front()];
        int redirectable = 0;
        for (int index : group.nodeIndices)
        {
            const QString id = result.nodes[index].id;
            for (const DataNode &source : result.nodes)
                redirectable += std::count(source.referencedIds.cbegin(), source.referencedIds.cend(), id);
        }
        report += QStringLiteral("- Hash: %1 | count: %2 | ID mask: %3 | classification: %4 | recommended keep: %5 | redirectable references: %6 | unsafe: %7\n")
                      .arg(group.contentHash.left(12))
                      .arg(group.nodeIndices.size())
                      .arg(group.commonIdMask,
                           group.mergeCandidate ? QStringLiteral("merge candidate") : QStringLiteral("allowed identical body"),
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

    report += QStringLiteral("\nBlocked unused objects\n");
    for (const UnusedCandidateInfo &info : result.unusedCandidates)
    {
        if (info.state != CandidateState::Blocked)
            continue;
        const DataNode &node = result.nodes[info.nodeIndex];
        report += QStringLiteral("- %1 | %2 | incoming: %3 | script: %4 | whitelist: %5 | risk: %6\n")
                      .arg(node.id, info.reason)
                      .arg(info.incomingXmlReferences)
                      .arg(info.scriptReferences)
                      .arg(info.whitelisted ? QStringLiteral("yes") : QStringLiteral("no"), info.riskLevel);
    }

    report += QStringLiteral("\nParse errors\n");
    for (const ParseErrorInfo &error : result.parseErrors)
    {
        report += QStringLiteral("- %1: %2\n").arg(error.filePath, error.message);
    }
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
        if (node.duplicateId || node.duplicateContent)
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

    if (selectedRows.isEmpty())
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("No rows selected.");
        }
        return false;
    }

    QHash<QString, QSet<QString>> locationsByFile;
    int plannedRemovals = 0;
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
        if (whitelistIds.contains(node.id) || candidate == result.unusedCandidates.cend() || candidate->state != CandidateState::Safe)
        {
            if (skippedNodes)
            {
                ++(*skippedNodes);
            }
            continue;
        }
        const int beforeCount = locationsByFile[node.sourceFile].size();
        locationsByFile[node.sourceFile].insert(node.originalLocation);
        if (locationsByFile[node.sourceFile].size() > beforeCount)
        {
            ++plannedRemovals;
        }
    }

    const QString analysisReport = buildAnalysisReport(result);
    const QString plannedChanges = buildPlannedChangesReport(result, selectedRows);

    QStringList changedFileList;
    for (auto it = locationsByFile.cbegin(); it != locationsByFile.cend(); ++it)
    {
        changedFileList.append(relativePath(rootFolder, it.key()));
    }
    QString backupRoot;
    BackupManager backupManager;
    if (!backupManager.createFolderBackup(rootFolder, changedFileList, analysisReport, plannedChanges, &backupRoot, errorMessage))
    {
        return false;
    }

    if (backupFolder)
    {
        *backupFolder = backupRoot;
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

    QStringList committedFiles;
    for (auto it = rewrittenFiles.cbegin(); it != rewrittenFiles.cend(); ++it)
    {
        QSaveFile saveFile(it.key());
        if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Failed to open file for safe write: %1").arg(it.key());
            }
            for (const QString &committed : committedFiles)
            {
                const QString backupPath = QDir(backupRoot).absoluteFilePath(relativePath(rootFolder, committed));
                QFile::remove(committed);
                QFile::copy(backupPath, committed);
            }
            return false;
        }
        if (saveFile.write(it.value()) != it.value().size())
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Failed to write file safely: %1").arg(it.key());
            }
            for (const QString &committed : committedFiles)
            {
                const QString backupPath = QDir(backupRoot).absoluteFilePath(relativePath(rootFolder, committed));
                QFile::remove(committed);
                QFile::copy(backupPath, committed);
            }
            return false;
        }
        if (!saveFile.commit())
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Failed to commit safe write: %1").arg(it.key());
            }
            for (const QString &committed : committedFiles)
            {
                const QString backupPath = QDir(backupRoot).absoluteFilePath(relativePath(rootFolder, committed));
                QFile::remove(committed);
                QFile::copy(backupPath, committed);
            }
            return false;
        }
        committedFiles.append(it.key());
    }

    if (changedFiles)
    {
        *changedFiles = changedFileList;
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
