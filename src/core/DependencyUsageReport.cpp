#include "core/DependencyUsageReport.h"

#include "core/AssetFileRules.h"
#include "core/CatalogProtection.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QQueue>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>

#include <algorithm>

namespace
{

QString normalized(QString path)
{
    return QDir::cleanPath(path).replace('\\', '/');
}

QString sortedKey(QString value)
{
    return value.toCaseFolded();
}

QString nodeLabel(const DataNode &node)
{
    const QString type = node.elementName.isEmpty() ? QStringLiteral("Unknown") : node.elementName;
    const QString id = node.id.isEmpty() ? QStringLiteral("<no id>") : node.id;
    return QStringLiteral("%1(%2)").arg(type, id);
}

QString dependencyRootForSource(const QString &sourceFile)
{
    QString path = normalized(sourceFile);
    const QString lower = path.toLower();
    const QStringList markers = {
        QStringLiteral(".sc2mod"),
        QStringLiteral(".sc2campaign")
    };
    for (const QString &marker : markers) {
        const int index = lower.indexOf(marker);
        if (index >= 0)
            return path.left(index + marker.size());
    }

    const QStringList roots = {
        QStringLiteral("Mods/"),
        QStringLiteral("Campaigns/")
    };
    for (const QString &root : roots) {
        const int index = lower.indexOf(root.toLower());
        if (index >= 0) {
            const int nextSlash = path.indexOf(QLatin1Char('/'), index + root.size());
            if (nextSlash > index)
                return path.left(nextSlash);
        }
    }
    return {};
}

QString dependencyNameFromPath(const QString &path)
{
    const QString normalizedPath = normalized(path);
    const QString fileName = QFileInfo(normalizedPath).fileName();
    if (!fileName.isEmpty())
        return fileName;
    const QStringList parts = normalizedPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return parts.isEmpty() ? QStringLiteral("Unknown dependency") : parts.last();
}

QStringList uniqueSorted(QStringList values)
{
    for (QString &value : values)
        value = value.trimmed();
    values.removeAll(QString());
    values.removeDuplicates();
    std::sort(values.begin(), values.end(), [](const QString &left, const QString &right) {
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
    return values;
}

QJsonArray stringArray(QStringList values)
{
    values = uniqueSorted(values);
    QJsonArray array;
    for (const QString &value : values)
        array.append(value);
    return array;
}

QJsonObject countsObject(const QHash<QString, int> &counts)
{
    QStringList keys;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it)
        keys << it.key();
    keys = uniqueSorted(keys);
    QJsonObject object;
    for (const QString &key : keys)
        object.insert(key, counts.value(key));
    return object;
}

void appendUnique(QStringList *values, const QString &value)
{
    if (!values || value.trimmed().isEmpty())
        return;
    values->append(value.trimmed());
    values->removeDuplicates();
}

QHash<QString, QVector<int>> buildLocalInbound(const QVector<DataNode> &nodes)
{
    QHash<QString, QVector<int>> inbound;
    for (int i = 0; i < nodes.size(); ++i) {
        const DataNode &node = nodes.at(i);
        if (node.id.isEmpty() || sc2dh::isDependencyCatalogSource(node.sourceFile))
            continue;
        for (const QString &reference : node.referencedIds) {
            if (!reference.isEmpty())
                inbound[reference].append(i);
        }
    }
    return inbound;
}

QString chainFromLocalToDependency(const QVector<DataNode> &nodes,
                                   int localIndex,
                                   int dependencyIndex)
{
    if (localIndex < 0 || localIndex >= nodes.size()
        || dependencyIndex < 0 || dependencyIndex >= nodes.size())
        return {};
    return QStringLiteral("%1 -> %2").arg(nodeLabel(nodes.at(localIndex)),
                                          nodeLabel(nodes.at(dependencyIndex)));
}

void addChainsForDependencyNode(const QVector<DataNode> &nodes,
                                const QHash<QString, QVector<int>> &localInbound,
                                int dependencyIndex,
                                QStringList *directUsers,
                                QStringList *chains)
{
    const DataNode &dependencyNode = nodes.at(dependencyIndex);
    const QVector<int> direct = localInbound.value(dependencyNode.id);
    for (int localIndex : direct) {
        const QString chain = chainFromLocalToDependency(nodes, localIndex, dependencyIndex);
        appendUnique(directUsers, chain);
        appendUnique(chains, chain);
    }
}

QHash<int, QStringList> dependencyUsageChains(const QVector<DataNode> &nodes,
                                              const QVector<int> &dependencyIndexes,
                                              const QHash<QString, QVector<int>> &localInbound)
{
    QHash<QString, int> dependencyIndexById;
    for (int index : dependencyIndexes) {
        if (index < 0 || index >= nodes.size())
            continue;
        const QString id = nodes.at(index).id;
        if (!id.isEmpty() && !dependencyIndexById.contains(id))
            dependencyIndexById.insert(id, index);
    }

    QHash<int, QStringList> chainsByIndex;
    QQueue<int> queue;
    for (int dependencyIndex : dependencyIndexes) {
        if (dependencyIndex < 0 || dependencyIndex >= nodes.size())
            continue;
        const DataNode &dependencyNode = nodes.at(dependencyIndex);
        for (int localIndex : localInbound.value(dependencyNode.id)) {
            const QString chain = chainFromLocalToDependency(nodes, localIndex, dependencyIndex);
            appendUnique(&chainsByIndex[dependencyIndex], chain);
        }
        if (!chainsByIndex.value(dependencyIndex).isEmpty())
            queue.enqueue(dependencyIndex);
    }

    while (!queue.isEmpty()) {
        const int sourceIndex = queue.dequeue();
        if (sourceIndex < 0 || sourceIndex >= nodes.size())
            continue;
        const QStringList sourceChains = chainsByIndex.value(sourceIndex);
        for (const QString &reference : nodes.at(sourceIndex).referencedIds) {
            const int targetIndex = dependencyIndexById.value(reference, -1);
            if (targetIndex < 0 || targetIndex >= nodes.size() || targetIndex == sourceIndex)
                continue;
            const int before = chainsByIndex.value(targetIndex).size();
            for (const QString &sourceChain : sourceChains) {
                appendUnique(&chainsByIndex[targetIndex],
                             sourceChain + QStringLiteral(" -> ") + nodeLabel(nodes.at(targetIndex)));
            }
            if (chainsByIndex.value(targetIndex).size() > before)
                queue.enqueue(targetIndex);
        }
    }
    return chainsByIndex;
}

QStringList metadataDependencyFiles(const AnalysisResult &analysis)
{
    QStringList files;
    for (const ScannedFileInfo &file : analysis.scannedFiles) {
        const QString rel = normalized(QDir(analysis.rootFolder).relativeFilePath(file.filePath));
        const QString lower = rel.toLower();
        if (lower.contains(QStringLiteral("documentinfo"))
            || lower.contains(QStringLiteral("dependencies"))) {
            files << rel;
        }
    }
    return uniqueSorted(files);
}

bool isPossibleDependencyImport(const ScannedFileInfo &file)
{
    const QString rel = normalized(file.filePath);
    if (!sc2dh::isDependencyCatalogSource(rel))
        return false;
    return sc2dh::asset::isAssetFile(QFileInfo(rel), rel);
}

} // namespace

namespace sc2dh
{

DependencyUsageReport DependencyUsageReportBuilder::build(const AnalysisResult &analysis) const
{
    DependencyUsageReport report;
    QHash<QString, int> indexByRoot;
    QHash<QString, QVector<int>> dependencyNodeIndexesByRoot;
    QHash<QString, int> knownIdToNodeIndex;

    for (int i = 0; i < analysis.nodes.size(); ++i) {
        const DataNode &node = analysis.nodes.at(i);
        if (!node.id.isEmpty() && !knownIdToNodeIndex.contains(node.id))
            knownIdToNodeIndex.insert(node.id, i);

        const QString root = dependencyRootForSource(node.sourceFile);
        if (root.isEmpty())
            continue;
        if (!indexByRoot.contains(root)) {
            DependencyUsageEntry entry;
            entry.path = root;
            entry.name = dependencyNameFromPath(root);
            entry.confidence = QStringLiteral("High");
            indexByRoot.insert(root, report.dependencies.size());
            report.dependencies.append(entry);
        }
        dependencyNodeIndexesByRoot[root].append(i);
    }

    const QStringList metadataFiles = metadataDependencyFiles(analysis);
    if (!metadataFiles.isEmpty()) {
        if (report.dependencies.isEmpty()) {
            DependencyUsageEntry entry;
            entry.name = QStringLiteral("Unknown dependency");
            entry.path = QStringLiteral("Unknown provenance");
            entry.confidence = QStringLiteral("Low");
            entry.metadataFiles = metadataFiles;
            report.dependencies.append(entry);
            indexByRoot.insert(entry.path, 0);
        } else {
            for (DependencyUsageEntry &entry : report.dependencies)
                entry.metadataFiles = metadataFiles;
        }
    }

    const QHash<QString, QVector<int>> localInbound = buildLocalInbound(analysis.nodes);
    for (auto it = dependencyNodeIndexesByRoot.cbegin(); it != dependencyNodeIndexesByRoot.cend(); ++it) {
        DependencyUsageEntry &entry = report.dependencies[indexByRoot.value(it.key())];
        const QHash<int, QStringList> reachableChains =
            dependencyUsageChains(analysis.nodes, it.value(), localInbound);
        for (int nodeIndex : it.value()) {
            const DataNode &node = analysis.nodes.at(nodeIndex);
            const QString type = node.elementName.isEmpty() ? QStringLiteral("Unknown") : node.elementName;
            entry.availableObjectsByType[type] += 1;
            if (!reachableChains.value(nodeIndex).isEmpty())
                entry.usedObjectsByType[type] += 1;
            addChainsForDependencyNode(analysis.nodes, localInbound, nodeIndex,
                                       &entry.directLocalUsers, &entry.usageChains);
            for (const QString &chain : reachableChains.value(nodeIndex))
                appendUnique(&entry.usageChains, chain);
        }
    }

    QSet<QString> unknown;
    for (const DataNode &node : analysis.nodes) {
        if (isDependencyCatalogSource(node.sourceFile))
            continue;
        for (const QString &reference : node.referencedIds) {
            if (reference.isEmpty() || knownIdToNodeIndex.contains(reference))
                continue;
            unknown.insert(reference);
        }
    }
    report.unknownProvenanceIds = uniqueSorted(unknown.values());
    if (!report.unknownProvenanceIds.isEmpty()) {
        if (report.dependencies.isEmpty()) {
            DependencyUsageEntry entry;
            entry.name = QStringLiteral("Unknown dependency");
            entry.path = QStringLiteral("Unknown provenance");
            entry.confidence = QStringLiteral("Low");
            entry.unresolvedExternalIds = report.unknownProvenanceIds;
            report.dependencies.append(entry);
        } else {
            for (DependencyUsageEntry &entry : report.dependencies)
                entry.unresolvedExternalIds = report.unknownProvenanceIds;
        }
    }

    for (const ScannedFileInfo &file : analysis.scannedFiles) {
        if (!isPossibleDependencyImport(file))
            continue;
        const QString relative = normalized(QDir(analysis.rootFolder).relativeFilePath(file.filePath));
        const QString root = dependencyRootForSource(relative);
        if (root.isEmpty() || !indexByRoot.contains(root))
            continue;
        report.dependencies[indexByRoot.value(root)].possibleImportFiles
            << relative;
    }

    for (DependencyUsageEntry &entry : report.dependencies) {
        entry.metadataFiles = uniqueSorted(entry.metadataFiles);
        entry.directLocalUsers = uniqueSorted(entry.directLocalUsers);
        entry.usageChains = uniqueSorted(entry.usageChains);
        entry.unresolvedExternalIds = uniqueSorted(entry.unresolvedExternalIds);
        entry.possibleImportFiles = uniqueSorted(entry.possibleImportFiles);
        if (entry.directLocalUsers.isEmpty() && entry.usedObjectsByType.isEmpty()
            && entry.unresolvedExternalIds.isEmpty())
            entry.confidence = QStringLiteral("Low");
        else if (entry.directLocalUsers.isEmpty())
            entry.confidence = QStringLiteral("Medium");
    }

    std::sort(report.dependencies.begin(), report.dependencies.end(),
              [](const DependencyUsageEntry &left, const DependencyUsageEntry &right) {
                  return left.path.compare(right.path, Qt::CaseInsensitive) < 0;
              });
    report.notes << QStringLiteral("Dependency cleanup is report-only; dependencies are never removed automatically.");
    if (analysis.externalConsumersUnknown)
        report.notes << QStringLiteral("External consumers are unknown; absence of local usage is not proof of unused dependency data.");
    return report;
}

QJsonObject DependencyUsageReportBuilder::toJson(const DependencyUsageReport &report) const
{
    QJsonArray dependencies;
    for (const DependencyUsageEntry &entry : report.dependencies) {
        dependencies.append(QJsonObject{
            {QStringLiteral("name"), entry.name},
            {QStringLiteral("path"), entry.path},
            {QStringLiteral("confidence"), entry.confidence},
            {QStringLiteral("metadataFiles"), stringArray(entry.metadataFiles)},
            {QStringLiteral("directLocalUsers"), stringArray(entry.directLocalUsers)},
            {QStringLiteral("usageChains"), stringArray(entry.usageChains)},
            {QStringLiteral("unresolvedExternalIds"), stringArray(entry.unresolvedExternalIds)},
            {QStringLiteral("usedObjectsByType"), countsObject(entry.usedObjectsByType)},
            {QStringLiteral("availableObjectsByType"), countsObject(entry.availableObjectsByType)},
            {QStringLiteral("possibleImportFiles"), stringArray(entry.possibleImportFiles)}
        });
    }
    return QJsonObject{
        {QStringLiteral("dependencies"), dependencies},
        {QStringLiteral("unknownProvenanceIds"), stringArray(report.unknownProvenanceIds)},
        {QStringLiteral("notes"), stringArray(report.notes)}
    };
}

QString DependencyUsageReportBuilder::toText(const DependencyUsageReport &report) const
{
    QString output;
    QTextStream stream(&output);
    stream << "Dependency Usage Report\n";
    stream << "=======================\n\n";
    if (report.dependencies.isEmpty()) {
        stream << "No dependency usage metadata or dependency catalog sources were found.\n";
    }
    for (const DependencyUsageEntry &entry : report.dependencies) {
        stream << "Dependency: " << entry.name << '\n';
        stream << "Path: " << entry.path << '\n';
        stream << "Confidence: " << entry.confidence << '\n';
        stream << "Metadata files: " << uniqueSorted(entry.metadataFiles).join(QStringLiteral(", ")) << '\n';
        stream << "Used objects by type:\n";
        QStringList typeKeys;
        for (auto it = entry.usedObjectsByType.cbegin(); it != entry.usedObjectsByType.cend(); ++it)
            typeKeys << it.key();
        typeKeys = uniqueSorted(typeKeys);
        if (typeKeys.isEmpty()) {
            stream << "  - none proven\n";
        } else {
            for (const QString &type : typeKeys)
                stream << "  - " << type << ": " << entry.usedObjectsByType.value(type) << '\n';
        }
        stream << "Available dependency objects by type:\n";
        QStringList availableTypeKeys;
        for (auto it = entry.availableObjectsByType.cbegin(); it != entry.availableObjectsByType.cend(); ++it)
            availableTypeKeys << it.key();
        availableTypeKeys = uniqueSorted(availableTypeKeys);
        if (availableTypeKeys.isEmpty()) {
            stream << "  - none proven\n";
        } else {
            for (const QString &type : availableTypeKeys)
                stream << "  - " << type << ": " << entry.availableObjectsByType.value(type) << '\n';
        }
        stream << "Direct local users:\n";
        for (const QString &user : uniqueSorted(entry.directLocalUsers))
            stream << "  - " << user << '\n';
        if (entry.directLocalUsers.isEmpty())
            stream << "  - none proven\n";
        stream << "Usage chains:\n";
        for (const QString &chain : uniqueSorted(entry.usageChains))
            stream << "  - " << chain << '\n';
        if (entry.usageChains.isEmpty())
            stream << "  - none proven\n";
        stream << "Unresolved external IDs:\n";
        for (const QString &id : uniqueSorted(entry.unresolvedExternalIds))
            stream << "  - " << id << '\n';
        if (entry.unresolvedExternalIds.isEmpty())
            stream << "  - none\n";
        stream << "Possible dependency imports:\n";
        for (const QString &file : uniqueSorted(entry.possibleImportFiles))
            stream << "  - " << file << '\n';
        if (entry.possibleImportFiles.isEmpty())
            stream << "  - none proven\n";
        stream << '\n';
    }
    if (!report.unknownProvenanceIds.isEmpty())
        stream << "Unknown provenance IDs: " << uniqueSorted(report.unknownProvenanceIds).join(QStringLiteral(", ")) << "\n\n";
    stream << "Notes:\n";
    for (const QString &note : uniqueSorted(report.notes))
        stream << "  - " << note << '\n';
    return output;
}

bool DependencyUsageReportBuilder::writeJson(const QString &path,
                                             const DependencyUsageReport &report,
                                             QString *errorMessage) const
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Unable to write dependency JSON report: %1").arg(path);
        return false;
    }
    const QByteArray bytes = QJsonDocument(toJson(report)).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Unable to commit dependency JSON report: %1").arg(path);
        return false;
    }
    return true;
}

bool DependencyUsageReportBuilder::writeText(const QString &path,
                                             const DependencyUsageReport &report,
                                             QString *errorMessage) const
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Unable to write dependency text report: %1").arg(path);
        return false;
    }
    const QByteArray bytes = toText(report).toUtf8();
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Unable to commit dependency text report: %1").arg(path);
        return false;
    }
    return true;
}

} // namespace sc2dh
