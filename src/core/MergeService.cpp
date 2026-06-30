#include "core/MergeService.h"

#include "core/BackupManager.h"
#include "core/CatalogProtection.h"
#include "core/FolderAnalyzer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

#include <pugixml.hpp>

#include <algorithm>
#include <sstream>

namespace {

QRegularExpression idExpression(const QString &id)
{
    return QRegularExpression(QStringLiteral("(?<![A-Za-z0-9_@])%1(?![A-Za-z0-9_@])")
                                  .arg(QRegularExpression::escape(id)));
}

QRegularExpression redirectExpression(const QHash<QString, QString> &redirects)
{
    QStringList ids = redirects.keys();
    std::sort(ids.begin(), ids.end(), [](const QString &left, const QString &right) {
        return left.size() > right.size();
    });
    QStringList escaped;
    escaped.reserve(ids.size());
    for (const QString &id : ids)
        escaped << QRegularExpression::escape(id);
    return QRegularExpression(QStringLiteral("(?<![A-Za-z0-9_@])(%1)(?![A-Za-z0-9_@])")
                                  .arg(escaped.join(QLatin1Char('|'))));
}

int replaceRedirectTokens(QString *value,
                          const QHash<QString, QString> &redirects,
                          const QRegularExpression &expression)
{
    if (!value || value->isEmpty() || redirects.isEmpty())
        return 0;
    QString output;
    qsizetype last = 0;
    int replacements = 0;
    auto matches = expression.globalMatch(*value);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        output += value->mid(last, match.capturedStart() - last);
        const QString oldId = match.captured(1);
        if (!sc2dh::isSafeAutomaticObjectId(oldId) || sc2dh::isReservedCatalogToken(oldId)) {
            output += oldId;
            last = match.capturedEnd();
            continue;
        }
        output += redirects.value(oldId, oldId);
        last = match.capturedEnd();
        ++replacements;
    }
    if (replacements > 0) {
        output += value->mid(last);
        *value = output;
    }
    return replacements;
}

bool shouldRewriteReferenceValue(pugi::xml_node node, const QString &fieldName, const QString &value)
{
    if (sc2dh::isNonReferenceCatalogFieldName(fieldName) || sc2dh::looksLikeCatalogFilterList(value))
        return false;

    for (pugi::xml_node current = node; current && current.type() == pugi::node_element; current = current.parent()) {
        if (sc2dh::isNonReferenceCatalogFieldName(QString::fromUtf8(current.name())))
            return false;
    }
    return true;
}

QString relativePath(const QString &root, const QString &file)
{
    return QDir(root).relativeFilePath(file);
}

bool loadFile(const QString &path, QByteArray *bytes, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Unable to read %1").arg(path);
        return false;
    }
    *bytes = file.readAll();
    return true;
}

QString nodePath(const pugi::xml_node &node)
{
    QStringList parts;
    for (pugi::xml_node current = node; current && current.type() == pugi::node_element; current = current.parent()) {
        int index = 1;
        for (pugi::xml_node previous = current.previous_sibling(current.name()); previous;
             previous = previous.previous_sibling(current.name())) {
            ++index;
        }
        parts.prepend(QStringLiteral("%1[%2]").arg(QString::fromUtf8(current.name())).arg(index));
    }
    return QStringLiteral("/") + parts.join(QLatin1Char('/'));
}

pugi::xml_node findObject(pugi::xml_document &doc, const QString &element, const QString &id)
{
    const pugi::xpath_node_set matches = doc.select_nodes("//*[@id]");
    for (const pugi::xpath_node &match : matches) {
        pugi::xml_node node = match.node();
        if (QString::fromUtf8(node.name()) == element
            && QString::fromUtf8(node.attribute("id").value()) == id) {
            return node;
        }
    }
    return {};
}

bool isTopLevelCatalogIdentity(const pugi::xml_node &node)
{
    pugi::xml_node parent = node.parent();
    return parent
        && parent.type() == pugi::node_element
        && QString::fromUtf8(parent.name()) == QStringLiteral("Catalog")
        && node.attribute("id");
}

struct RewriteStats {
    int fields = 0;
    int references = 0;
    QStringList changes;
};

void rewriteNode(pugi::xml_node node,
                 const QHash<QString, QString> &redirects,
                 const QRegularExpression &redirectRegex,
                 const QString &file,
                 const QSet<QString> &removedIdentityLocations,
                 RewriteStats *stats)
{
    if (node.type() == pugi::node_element) {
        for (pugi::xml_attribute attribute : node.attributes()) {
            // Top-level catalog identities are deleted separately; duplicate
            // merge redirects references only and must not rename surviving
            // objects whose IDs happen to contain a removed token.
            if (QString::fromUtf8(attribute.name()) == QStringLiteral("id")
                && (removedIdentityLocations.contains(nodePath(node)) || isTopLevelCatalogIdentity(node))) {
                continue;
            }
            QString value = QString::fromUtf8(attribute.value());
            const QString before = value;
            if (!shouldRewriteReferenceValue(node, QString::fromUtf8(attribute.name()), value))
                continue;
            const int replacements = replaceRedirectTokens(&value, redirects, redirectRegex);
            if (replacements > 0) {
                attribute.set_value(value.toUtf8().constData());
                ++stats->fields;
                stats->references += replacements;
                stats->changes.append(QStringLiteral("%1 %2 @%3: %4 -> %5")
                                          .arg(file, nodePath(node), QString::fromUtf8(attribute.name()), before, value));
            }
        }
    }
    if (node.type() == pugi::node_pcdata || node.type() == pugi::node_cdata) {
        QString value = QString::fromUtf8(node.value());
        const QString before = value;
        if (!shouldRewriteReferenceValue(node.parent(), QString(), value))
            return;
        const int replacements = replaceRedirectTokens(&value, redirects, redirectRegex);
        if (replacements > 0) {
            node.set_value(value.toUtf8().constData());
            ++stats->fields;
            stats->references += replacements;
            stats->changes.append(QStringLiteral("%1 %2 text: %3 -> %4")
                                      .arg(file, nodePath(node.parent()), before, value));
        }
    }
    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling()) {
        rewriteNode(child, redirects, redirectRegex, file, removedIdentityLocations, stats);
    }
}

bool restoreBackup(const QString &root, const QString &backup, const QStringList &relativeFiles, QString *error)
{
    if (backup.startsWith(QStringLiteral("disabled"), Qt::CaseInsensitive))
        return false;
    bool ok = true;
    for (const QString &relative : relativeFiles) {
        const QString source = QDir(backup).absoluteFilePath(relative);
        const QString target = QDir(root).absoluteFilePath(relative);
        QFile::remove(target);
        if (!QFile::copy(source, target)) {
            ok = false;
            if (error) *error += QStringLiteral(" Rollback failed for %1.").arg(target);
        }
    }
    return ok;
}

} // namespace

int MergeService::replaceIdTokens(QString *value, const QString &oldId, const QString &newId)
{
    if (!value || oldId.isEmpty() || oldId == newId || !sc2dh::isSafeAutomaticObjectId(oldId)
        || sc2dh::isReservedCatalogToken(oldId)) return 0;
    const QRegularExpression expression = idExpression(oldId);
    int count = 0;
    auto iterator = expression.globalMatch(*value);
    while (iterator.hasNext()) { iterator.next(); ++count; }
    if (count) value->replace(expression, newId);
    return count;
}

int MergeService::countIdTokens(const QString &value, const QString &id)
{
    if (!sc2dh::isSafeAutomaticObjectId(id) || sc2dh::isReservedCatalogToken(id))
        return 0;
    int count = 0;
    auto iterator = idExpression(id).globalMatch(value);
    while (iterator.hasNext()) { iterator.next(); ++count; }
    return count;
}

MergePreview MergeService::preview(const AnalysisResult &analysis, const MergeRequest &request) const
{
    MergePreview preview;
    if (request.keepNodeIndex < 0 || request.keepNodeIndex >= analysis.nodes.size()) {
        preview.warnings << QStringLiteral("A keep object must be selected.");
        return preview;
    }
    const DataNode &keep = analysis.nodes[request.keepNodeIndex];
    if (sc2dh::isProtectedCatalogNode(keep)) {
        preview.warnings << QStringLiteral("%1 is an editor/runtime catalog object and cannot be merged safely.").arg(keep.id);
        return preview;
    }
    if (!sc2dh::isSafeAutomaticObjectId(keep.id)) {
        preview.warnings << QStringLiteral("%1 has a numeric or unsafe ID and cannot be merged automatically.").arg(keep.id);
        return preview;
    }
    preview.keptId = keep.id;
    QHash<QString, QString> redirects;
    QHash<QString, QSet<QString>> removedLocations;
    QSet<QString> files;
    for (int index : request.removeNodeIndices) {
        if (index < 0 || index >= analysis.nodes.size() || index == request.keepNodeIndex) {
            preview.warnings << QStringLiteral("Invalid remove selection.");
            continue;
        }
        const DataNode &remove = analysis.nodes[index];
        if (sc2dh::isProtectedCatalogNode(remove)) {
            preview.warnings << QStringLiteral("%1 is an editor/runtime catalog object and cannot be removed by duplicate merge.").arg(remove.id);
            continue;
        }
        if (!sc2dh::isSafeAutomaticObjectId(remove.id)) {
            preview.warnings << QStringLiteral("%1 has a numeric or unsafe ID and cannot be removed by duplicate merge.").arg(remove.id);
            continue;
        }
        bool relatedMergeGroup = false;
        for (const DuplicateContentGroup &group : analysis.duplicateContentGroups) {
            if (group.mergeCandidate && group.nodeIndices.contains(request.keepNodeIndex) && group.nodeIndices.contains(index)) {
                relatedMergeGroup = true;
                break;
            }
        }
        if (!relatedMergeGroup && remove.id != keep.id && remove.elementName == keep.elementName && remove.contentHash == keep.contentHash) {
            preview.warnings << QStringLiteral("%1 and %2 have unrelated IDs; identical body reuse is allowed and merge is not suggested.")
                                    .arg(remove.id, keep.id);
            continue;
        }
        if (remove.id == keep.id || remove.elementName != keep.elementName || remove.contentHash != keep.contentHash) {
            preview.warnings << QStringLiteral("%1 is not a different-ID exact body duplicate of %2.").arg(remove.id, keep.id);
            continue;
        }
        redirects.insert(remove.id, keep.id);
        removedLocations[remove.sourceFile].insert(remove.originalLocation);
        preview.removedIds << remove.id;
        files.insert(remove.sourceFile);
        ++preview.nodesDeleted;
    }
    if (redirects.isEmpty()) {
        preview.warnings << QStringLiteral("Select at least one exact duplicate to remove.");
        return preview;
    }
    const QRegularExpression redirectsRegex = redirectExpression(redirects);

    for (const ScannedFileInfo &info : analysis.scannedFiles) {
        if (!info.isXml) continue;
        QByteArray bytes;
        QString error;
        if (!loadFile(info.filePath, &bytes, &error)) {
            preview.warnings << error;
            continue;
        }
        pugi::xml_document doc;
        if (!doc.load_buffer(bytes.constData(), size_t(bytes.size()))) {
            preview.warnings << QStringLiteral("Cannot parse %1.").arg(info.filePath);
            continue;
        }
        RewriteStats stats;
        rewriteNode(doc, redirects, redirectsRegex, info.filePath, removedLocations.value(info.filePath), &stats);
        if (stats.fields) files.insert(info.filePath);
        preview.fieldsChanged += stats.fields;
        preview.referencesRedirected += stats.references;
        preview.changes += stats.changes;
    }
    preview.filesChanged = files.values();
    std::sort(preview.filesChanged.begin(), preview.filesChanged.end());
    preview.riskLevel = preview.warnings.isEmpty()
        ? (preview.referencesRedirected > 100 ? QStringLiteral("medium") : QStringLiteral("low"))
        : QStringLiteral("high");
    preview.valid = preview.warnings.isEmpty();
    preview.reportText = QStringLiteral("Merge Preview Result\nKept object: %1\nRemoved objects: %2\nFiles changed: %3\nFields changed: %4\nReferences redirected: %5\nNodes deleted: %6\nWarnings: %7\nRisk level: %8\n")
                             .arg(preview.keptId, preview.removedIds.join(QStringLiteral(", ")))
                             .arg(preview.filesChanged.size()).arg(preview.fieldsChanged)
                             .arg(preview.referencesRedirected).arg(preview.nodesDeleted)
                             .arg(preview.warnings.isEmpty() ? QStringLiteral("none") : preview.warnings.join(QStringLiteral("; ")),
                                  preview.riskLevel);
    for (const QString &change : preview.changes) preview.reportText += QStringLiteral("- %1\n").arg(change);
    return preview;
}

MergeApplyResult MergeService::apply(const AnalysisResult &analysis,
                                     const MergeRequest &request,
                                     const QString &rootFolder,
                                     const QSet<QString> &whitelistIds) const
{
    Q_UNUSED(whitelistIds);
    MergeApplyResult result;
    const MergePreview plan = preview(analysis, request);
    if (!plan.valid) { result.error = plan.warnings.join(QStringLiteral("; ")); return result; }

    QHash<QString, QString> redirects;
    QHash<QString, QVector<const DataNode *>> removals;
    for (int index : request.removeNodeIndices) {
        const DataNode &node = analysis.nodes[index];
        redirects.insert(node.id, plan.keptId);
        removals[node.sourceFile].append(&node);
    }
    const QRegularExpression redirectsRegex = redirectExpression(redirects);

    QHash<QString, QByteArray> staged;
    RewriteStats totals;
    QString error;
    for (const ScannedFileInfo &info : analysis.scannedFiles) {
        if (!info.isXml) continue;
        QByteArray bytes;
        if (!loadFile(info.filePath, &bytes, &error)) { result.error = error; return result; }
        pugi::xml_document doc;
        const auto parsed = doc.load_buffer(bytes.constData(), size_t(bytes.size()));
        if (!parsed) { result.error = QStringLiteral("Cannot parse %1: %2").arg(info.filePath, parsed.description()); return result; }
        RewriteStats fileStats;
        QSet<QString> identityLocations;
        for (const DataNode *remove : removals.value(info.filePath)) identityLocations.insert(remove->originalLocation);
        rewriteNode(doc, redirects, redirectsRegex, info.filePath, identityLocations, &fileStats);
        int deleted = 0;
        for (const DataNode *remove : removals.value(info.filePath)) {
            pugi::xml_node node = findObject(doc, remove->elementName, remove->id);
            if (!node || !node.parent().remove_child(node)) {
                result.error = QStringLiteral("Unable to delete %1 from %2.").arg(remove->id, info.filePath);
                return result;
            }
            ++deleted;
        }
        if (fileStats.fields || deleted) {
            std::ostringstream stream;
            doc.save(stream, "  ", pugi::format_default, pugi::encoding_utf8);
            const QByteArray output = QByteArray::fromStdString(stream.str());
            const QRegularExpressionMatch remaining = redirectsRegex.match(QString::fromUtf8(output));
            if (remaining.hasMatch()) {
                result.warnings << QStringLiteral("Duplicate merge kept residual old ID token %1 in %2 for manual review.")
                                       .arg(remaining.captured(1), info.filePath);
            }
            staged.insert(info.filePath, output);
            totals.fields += fileStats.fields;
            totals.references += fileStats.references;
        }
    }
    if (staged.isEmpty()) { result.error = QStringLiteral("Merge produced no file changes."); return result; }

    QStringList relativeFiles;
    for (const QString &file : staged.keys()) relativeFiles << relativePath(rootFolder, file);
    std::sort(relativeFiles.begin(), relativeFiles.end());
    BackupManager backups;
    if (!backups.createFolderBackup(rootFolder, relativeFiles, analysis.analysisReportText,
                                     plan.reportText, &result.backupFolder, &result.error)) return result;
    if (m_failureInjectionStep == QStringLiteral("after-backup")) {
        result.error = QStringLiteral("Injected failure after backup.");
        return result;
    }

    QStringList committed;
    for (auto it = staged.cbegin(); it != staged.cend(); ++it) {
        QSaveFile file(it.key());
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || file.write(it.value()) != it.value().size() || !file.commit()) {
            result.error = QStringLiteral("Failed to commit %1.").arg(it.key());
            restoreBackup(rootFolder, result.backupFolder, committed, &result.error);
            return result;
        }
        committed << relativePath(rootFolder, it.key());
    }
    if (m_failureInjectionStep == QStringLiteral("after-commit")) {
        result.error = QStringLiteral("Injected failure after commit.");
        restoreBackup(rootFolder, result.backupFolder, relativeFiles, &result.error);
        return result;
    }

    FolderAnalyzer analyzer;
    AnalysisResult rebuilt;
    if (!analyzer.analyzeFolder(rootFolder, whitelistIds, &rebuilt, &error)) {
        result.error = QStringLiteral("Registry rebuild failed: %1").arg(error);
        restoreBackup(rootFolder, result.backupFolder, relativeFiles, &result.error);
        return result;
    }
    for (const QString &removed : plan.removedIds) {
        for (const DataNode &node : rebuilt.nodes) {
            if (node.id == removed || node.referencedIds.contains(removed)) {
                result.warnings << QStringLiteral("Post-merge audit still sees residual old ID token %1 in refreshed analysis for manual review.")
                                       .arg(removed);
                break;
            }
        }
    }
    result.success = true;
    result.changedFiles = relativeFiles;
    result.referencesRedirected = totals.references;
    result.nodesDeleted = plan.nodesDeleted;
    return result;
}

MergeApplyResult MergeService::applyBatch(const AnalysisResult &analysis,
                                          const QVector<MergeRequest> &requests,
                                          const QString &rootFolder,
                                          const QSet<QString> &whitelistIds,
                                          const std::function<void(int, int, const QString &)> &progress) const
{
    MergeApplyResult result;
    if (requests.isEmpty()) {
        result.error = QStringLiteral("No duplicate merge requests selected.");
        return result;
    }

    QSet<QString> idsSelectedForRemoval;
    for (const MergeRequest &request : requests) {
        for (int index : request.removeNodeIndices) {
            if (index >= 0 && index < analysis.nodes.size())
                idsSelectedForRemoval.insert(analysis.nodes[index].id);
        }
    }

    QHash<QString, QString> redirects;
    QHash<QString, QVector<const DataNode *>> removals;
    QSet<int> removedNodeIndexes;
    QStringList removedIds;

    const auto isRelatedMergeCandidate = [&](int keepIndex, int removeIndex) {
        for (const DuplicateContentGroup &group : analysis.duplicateContentGroups) {
            if (group.mergeCandidate
                && group.nodeIndices.contains(keepIndex)
                && group.nodeIndices.contains(removeIndex)) {
                return true;
            }
        }
        return false;
    };

    for (const MergeRequest &request : requests) {
        if (request.keepNodeIndex < 0 || request.keepNodeIndex >= analysis.nodes.size()) {
            ++result.skippedMerges;
            result.warnings << QStringLiteral("Skipped duplicate merge with missing keep object.");
            continue;
        }
        const DataNode &keep = analysis.nodes[request.keepNodeIndex];
        if (sc2dh::isProtectedCatalogNode(keep)) {
            ++result.skippedMerges;
            result.warnings << QStringLiteral("Skipped duplicate merge for protected editor/runtime object %1.").arg(keep.id);
            continue;
        }
        if (!sc2dh::isSafeAutomaticObjectId(keep.id)) {
            ++result.skippedMerges;
            result.warnings << QStringLiteral("Skipped duplicate merge for numeric or unsafe ID %1.").arg(keep.id);
            continue;
        }
        if (idsSelectedForRemoval.contains(keep.id)) {
            ++result.skippedMerges;
            result.warnings << QStringLiteral("Skipped duplicate merge for %1 because its keep ID is also selected for removal.").arg(keep.id);
            continue;
        }
        for (int removeIndex : request.removeNodeIndices) {
            if (removeIndex < 0 || removeIndex >= analysis.nodes.size() || removeIndex == request.keepNodeIndex) {
                ++result.skippedMerges;
                continue;
            }
            if (removedNodeIndexes.contains(removeIndex)) {
                ++result.skippedMerges;
                continue;
            }
            const DataNode &remove = analysis.nodes[removeIndex];
            if (sc2dh::isProtectedCatalogNode(remove)) {
                ++result.skippedMerges;
                result.warnings << QStringLiteral("Skipped duplicate merge for protected editor/runtime object %1.").arg(remove.id);
                continue;
            }
            if (!sc2dh::isSafeAutomaticObjectId(remove.id)) {
                ++result.skippedMerges;
                result.warnings << QStringLiteral("Skipped duplicate merge for numeric or unsafe ID %1.").arg(remove.id);
                continue;
            }
            if (remove.id == keep.id
                || remove.elementName != keep.elementName
                || remove.contentHash != keep.contentHash
                || !isRelatedMergeCandidate(request.keepNodeIndex, removeIndex)) {
                ++result.skippedMerges;
                result.warnings << QStringLiteral("Skipped invalid duplicate merge %1 -> %2.").arg(remove.id, keep.id);
                continue;
            }
            redirects.insert(remove.id, keep.id);
            removals[remove.sourceFile].append(&remove);
            removedNodeIndexes.insert(removeIndex);
            removedIds << remove.id;
        }
    }

    if (redirects.isEmpty()) {
        result.success = true;
        result.error.clear();
        return result;
    }
    const QRegularExpression redirectsRegex = redirectExpression(redirects);

    QHash<QString, QByteArray> staged;
    RewriteStats totals;
    QString error;
    int fileIndex = 0;
    const int totalFiles = analysis.scannedFiles.size();
    for (const ScannedFileInfo &info : analysis.scannedFiles) {
        if (progress)
            progress(fileIndex, totalFiles, info.filePath);
        ++fileIndex;
        if (!info.isXml)
            continue;
        QByteArray bytes;
        if (!loadFile(info.filePath, &bytes, &error)) {
            result.error = error;
            return result;
        }
        pugi::xml_document doc;
        const auto parsed = doc.load_buffer(bytes.constData(), size_t(bytes.size()));
        if (!parsed) {
            result.error = QStringLiteral("Cannot parse %1: %2").arg(info.filePath, parsed.description());
            return result;
        }

        RewriteStats fileStats;
        QSet<QString> identityLocations;
        for (const DataNode *remove : removals.value(info.filePath))
            identityLocations.insert(remove->originalLocation);
        rewriteNode(doc, redirects, redirectsRegex, info.filePath, identityLocations, &fileStats);

        int deleted = 0;
        for (const DataNode *remove : removals.value(info.filePath)) {
            pugi::xml_node node = findObject(doc, remove->elementName, remove->id);
            if (!node || !node.parent().remove_child(node)) {
                result.error = QStringLiteral("Unable to delete %1 from %2.").arg(remove->id, info.filePath);
                return result;
            }
            ++deleted;
        }

        if (fileStats.fields || deleted) {
            std::ostringstream stream;
            doc.save(stream, "  ", pugi::format_default, pugi::encoding_utf8);
            const QByteArray output = QByteArray::fromStdString(stream.str());
            const QRegularExpressionMatch remaining = redirectsRegex.match(QString::fromUtf8(output));
            if (remaining.hasMatch()) {
                result.warnings << QStringLiteral("Duplicate merge kept residual old ID token %1 in %2 for manual review.")
                                       .arg(remaining.captured(1), info.filePath);
            }
            staged.insert(info.filePath, output);
            totals.fields += fileStats.fields;
            totals.references += fileStats.references;
        }
    }
    if (progress)
        progress(totalFiles, totalFiles, QString());

    if (staged.isEmpty()) {
        result.success = true;
        return result;
    }

    QStringList relativeFiles;
    for (const QString &file : staged.keys())
        relativeFiles << relativePath(rootFolder, file);
    std::sort(relativeFiles.begin(), relativeFiles.end());

    QString reportText = QStringLiteral("Batch Merge Apply\nRemoved objects: %1\nReferences redirected: %2\nFiles changed: %3\nWarnings: %4\n")
                             .arg(removedIds.size())
                             .arg(totals.references)
                             .arg(relativeFiles.size())
                             .arg(result.warnings.isEmpty() ? QStringLiteral("none") : result.warnings.join(QStringLiteral("; ")));
    BackupManager backups;
    if (!backups.createFolderBackup(rootFolder, relativeFiles, analysis.analysisReportText,
                                    reportText, &result.backupFolder, &result.error))
        return result;
    if (m_failureInjectionStep == QStringLiteral("after-backup")) {
        result.error = QStringLiteral("Injected failure after backup.");
        return result;
    }

    QStringList committed;
    for (auto it = staged.cbegin(); it != staged.cend(); ++it) {
        QSaveFile file(it.key());
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || file.write(it.value()) != it.value().size()
            || !file.commit()) {
            result.error = QStringLiteral("Failed to commit %1.").arg(it.key());
            restoreBackup(rootFolder, result.backupFolder, committed, &result.error);
            return result;
        }
        committed << relativePath(rootFolder, it.key());
    }
    if (m_failureInjectionStep == QStringLiteral("after-commit")) {
        result.error = QStringLiteral("Injected failure after commit.");
        restoreBackup(rootFolder, result.backupFolder, relativeFiles, &result.error);
        return result;
    }

    FolderAnalyzer analyzer;
    AnalysisResult rebuilt;
    if (!analyzer.analyzeFolder(rootFolder, whitelistIds, &rebuilt, &error)) {
        result.error = QStringLiteral("Registry rebuild failed: %1").arg(error);
        restoreBackup(rootFolder, result.backupFolder, relativeFiles, &result.error);
        return result;
    }
    for (const QString &removed : removedIds) {
        for (const DataNode &node : rebuilt.nodes) {
            if (node.id == removed || node.referencedIds.contains(removed)) {
                result.warnings << QStringLiteral("Post-merge audit still sees residual old ID token %1 in refreshed analysis for manual review.")
                                       .arg(removed);
                break;
            }
        }
    }

    result.success = true;
    result.changedFiles = relativeFiles;
    result.referencesRedirected = totals.references;
    result.nodesDeleted = removedIds.size();
    return result;
}
