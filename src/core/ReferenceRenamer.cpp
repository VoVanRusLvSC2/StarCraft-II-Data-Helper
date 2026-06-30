#include "core/ReferenceRenamer.h"

#include "core/BackupManager.h"
#include "core/CatalogProtection.h"
#include "core/FolderAnalyzer.h"
#include "core/MergeService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

#include <pugixml.hpp>

#include <algorithm>
#include <sstream>
#include <utility>

namespace {

QString nodePath(const pugi::xml_node &node)
{
    QStringList parts;
    for (pugi::xml_node current = node; current && current.type() == pugi::node_element; current = current.parent()) {
        int index = 1;
        for (pugi::xml_node previous = current.previous_sibling(current.name()); previous;
             previous = previous.previous_sibling(current.name())) ++index;
        parts.prepend(QStringLiteral("%1[%2]").arg(QString::fromUtf8(current.name())).arg(index));
    }
    return QStringLiteral("/") + parts.join(QLatin1Char('/'));
}

bool readFile(const QString &path, QByteArray *bytes, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { *error = QStringLiteral("Unable to read %1").arg(path); return false; }
    *bytes = file.readAll();
    return true;
}

QString fastLookupKey(const QString &left, const QString &right)
{
    return left + QChar(0x1f) + right;
}

bool isTopLevelCatalogIdentity(const pugi::xml_node &node)
{
    pugi::xml_node parent = node.parent();
    return parent
        && parent.type() == pugi::node_element
        && QString::fromUtf8(parent.name()) == QStringLiteral("Catalog")
        && node.attribute("id");
}

const QRegularExpression &renameTokenExpression()
{
    static const QRegularExpression expression(QStringLiteral("(?<![A-Za-z0-9_@])([A-Za-z0-9_@]+)(?![A-Za-z0-9_@])"));
    return expression;
}

int simultaneousReplace(QString *value, const QHash<QString, QString> &renames)
{
    if (!value || value->isEmpty() || renames.isEmpty())
        return 0;
    QString output;
    qsizetype last = 0;
    int replacements = 0;
    auto matches = renameTokenExpression().globalMatch(*value);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const QString oldId = match.captured(1);
        if (!sc2dh::isSafeAutomaticObjectId(oldId) || sc2dh::isReservedCatalogToken(oldId))
            continue;
        const auto replacement = renames.constFind(oldId);
        if (replacement == renames.cend())
            continue;
        output += value->mid(last, match.capturedStart() - last);
        output += replacement.value();
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
    if (fieldName.compare(QStringLiteral("parent"), Qt::CaseInsensitive) == 0)
        return false;
    if (sc2dh::isNonReferenceCatalogFieldName(fieldName) || sc2dh::looksLikeCatalogFilterList(value))
        return false;

    for (pugi::xml_node current = node; current && current.type() == pugi::node_element; current = current.parent()) {
        if (sc2dh::isNonReferenceCatalogFieldName(QString::fromUtf8(current.name())))
            return false;
    }
    return true;
}

struct RewriteResult {
    int identities = 0;
    int references = 0;
    QStringList changes;
};

struct PendingRename {
    QString oldId;
    QString newId;
    QString elementName;
    QString sourceFile;
    QString expectedLocation;
    QString actualLocation;
    bool found = false;
};

void locateIdentityTargets(pugi::xml_node node,
                           QHash<QString, int> *targetIndexByElementAndId,
                           QHash<QString, int> *targetIndexByLocation,
                           QVector<PendingRename> *targets)
{
    if (!targets || !targetIndexByElementAndId || !targetIndexByLocation)
        return;
    if (node.type() == pugi::node_element) {
        const pugi::xml_attribute idAttribute = node.attribute("id");
        if (idAttribute) {
            const QString id = QString::fromUtf8(idAttribute.value());
            const QString elementName = QString::fromUtf8(node.name());
            const QString path = nodePath(node);
            int targetIndex = targetIndexByElementAndId->value(fastLookupKey(elementName, id), -1);
            if (targetIndex < 0)
                targetIndex = targetIndexByLocation->value(path, -1);
            if (targetIndex >= 0 && targetIndex < targets->size() && !(*targets)[targetIndex].found) {
                PendingRename &target = (*targets)[targetIndex];
                if ((target.elementName == elementName && target.oldId == id)
                    || (target.expectedLocation == path && target.oldId == id)) {
                    target.found = true;
                    target.actualLocation = path;
                }
            }
        }
    }
    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling())
        locateIdentityTargets(child, targetIndexByElementAndId, targetIndexByLocation, targets);
}

void collectIdentityKeys(pugi::xml_node node, QSet<QString> *keys)
{
    if (!keys)
        return;
    if (node.type() == pugi::node_element) {
        const pugi::xml_attribute idAttribute = node.attribute("id");
        if (idAttribute)
            keys->insert(fastLookupKey(QString::fromUtf8(node.name()), QString::fromUtf8(idAttribute.value())));
    }
    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling())
        collectIdentityKeys(child, keys);
}

void rewrite(pugi::xml_node node, const QString &file, const QHash<QString, QString> &identityByLocation,
             const QHash<QString, QString> &renames, RewriteResult *result, bool collectChanges)
{
    if (node.type() == pugi::node_element) {
        const QString path = nodePath(node);
        for (pugi::xml_attribute attribute : node.attributes()) {
            const QString attributeName = QString::fromUtf8(attribute.name());
            if (attributeName.compare(QStringLiteral("id"), Qt::CaseInsensitive) == 0 && identityByLocation.contains(path)) {
                const QString before = QString::fromUtf8(attribute.value());
                const QString after = identityByLocation.value(path);
                attribute.set_value(after.toUtf8().constData());
                ++result->identities;
                if (collectChanges)
                    result->changes << QStringLiteral("%1 %2 @id: %3 -> %4 (object identity)").arg(file, path, before, after);
                continue;
            }
            if (attributeName.compare(QStringLiteral("id"), Qt::CaseInsensitive) == 0 && isTopLevelCatalogIdentity(node))
                continue;
            QString value = QString::fromUtf8(attribute.value());
            const QString before = value;
            if (!shouldRewriteReferenceValue(node, attributeName, value))
                continue;
            const int count = simultaneousReplace(&value, renames);
            if (count) {
                attribute.set_value(value.toUtf8().constData());
                result->references += count;
                if (collectChanges)
                    result->changes << QStringLiteral("%1 %2 @%3: %4 -> %5").arg(file, path, attributeName, before, value);
            }
        }
    } else if (node.type() == pugi::node_pcdata || node.type() == pugi::node_cdata) {
        QString value = QString::fromUtf8(node.value());
        const QString before = value;
        if (!shouldRewriteReferenceValue(node.parent(), QString(), value))
            return;
        const int count = simultaneousReplace(&value, renames);
        if (count) {
            node.set_value(value.toUtf8().constData());
            result->references += count;
            if (collectChanges)
                result->changes << QStringLiteral("%1 %2 text: %3 -> %4").arg(file, nodePath(node.parent()), before, value);
        }
    }
    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling())
        rewrite(child, file, identityByLocation, renames, result, collectChanges);
}

bool restore(const QString &root, const QString &backup, const QStringList &files, QString *error)
{
    if (backup.startsWith(QStringLiteral("disabled"), Qt::CaseInsensitive))
        return false;
    bool ok = true;
    for (const QString &relative : files) {
        const QString target = QDir(root).absoluteFilePath(relative);
        QFile::remove(target);
        if (!QFile::copy(QDir(backup).absoluteFilePath(relative), target)) {
            ok = false; *error += QStringLiteral(" Rollback failed for %1.").arg(target);
        }
    }
    return ok;
}

QString buildReport(const AnalysisResult &analysis, const RenamePlan &plan, const RewriteResult &rewriteResult,
                    const QStringList &files, const QStringList &warnings, const QStringList &conflicts,
                    const QString &finalResult)
{
    QString report = QStringLiteral("Rename To Standard Preview\nSelected family: %1\nRoot ID: %2\nTarget root ID: %3\nSource files: %4\n")
                         .arg(plan.family.rootId, plan.family.rootId, plan.targetRootId).arg(files.size());
    for (const QString &file : files) report += QStringLiteral("- %1\n").arg(file);
    report += QStringLiteral("\nDetected objects\n");
    for (const UnitFamilyObject &object : plan.family.objects) {
        const DataNode &node = analysis.nodes[object.nodeIndex];
        report += QStringLiteral("- %1 | %2 | role: %3 | confidence: %4 | %5\n")
                      .arg(node.id, node.elementName, unitFamilyRoleName(object.role), object.confidence, node.sourceFile);
    }
    report += QStringLiteral("\nNon-standard objects / rename plan\n");
    for (const RenamePlanItem &item : plan.items)
        report += QStringLiteral("- %1 -> %2 | %3 | confidence: %4 | risk: %5 | %6\n")
                      .arg(item.oldId, item.newId, unitFamilyRoleName(item.role), item.confidence, item.riskLevel, item.reason);
    report += QStringLiteral("\nManual review objects\n");
    for (const UnitFamilyObject &object : plan.manualReview)
        report += QStringLiteral("- %1 | %2\n").arg(analysis.nodes[object.nodeIndex].id, object.reason);
    report += QStringLiteral("\nReference update plan\n");
    for (const QString &change : rewriteResult.changes) report += QStringLiteral("- %1\n").arg(change);
    report += QStringLiteral("\nConflicts\n- %1\nWarnings\n- %2\nSkipped objects\n")
                  .arg(conflicts.isEmpty() ? QStringLiteral("none") : conflicts.join(QStringLiteral("\n- ")),
                       warnings.isEmpty() ? QStringLiteral("none") : warnings.join(QStringLiteral("\n- ")));
    QSet<int> planned;
    for (const RenamePlanItem &item : plan.items) planned.insert(item.nodeIndex);
    for (const UnitFamilyObject &object : plan.family.objects)
        if (!planned.contains(object.nodeIndex)) report += QStringLiteral("- %1 (already standard or manual review)\n").arg(analysis.nodes[object.nodeIndex].id);
    report += QStringLiteral("\nIdentities renamed: %1\nReferences updated: %2\nFinal result: %3\n")
                  .arg(rewriteResult.identities).arg(rewriteResult.references, 0, 10).arg(finalResult);
    return report;
}

bool prepare(const AnalysisResult &analysis, const RenamePlan &plan, QHash<QString, QByteArray> *staged,
             RewriteResult *totals, QStringList *files, QStringList *warnings, QString *error,
             bool collectChanges, const ReferenceRenamer::ProgressCallback &progress,
             QHash<QString, QString> *appliedRenames = nullptr)
{
    QHash<QString, QVector<PendingRename>> pendingByFile;
    QStringList unsafeIds;
    for (const RenamePlanItem &item : plan.items) {
        if (!item.selected || item.blocked) continue;
        if (!sc2dh::isSafeAutomaticObjectId(item.oldId) || sc2dh::isReservedCatalogToken(item.oldId)) {
            unsafeIds << item.oldId;
            continue;
        }
        const DataNode &node = analysis.nodes[item.nodeIndex];
        if (sc2dh::isProtectedCatalogNode(node)) {
            unsafeIds << item.oldId;
            continue;
        }
        PendingRename pending;
        pending.oldId = item.oldId;
        pending.newId = item.newId;
        pending.elementName = node.elementName;
        pending.sourceFile = node.sourceFile;
        pending.expectedLocation = node.originalLocation;
        pendingByFile[node.sourceFile].append(pending);
    }
    if (!unsafeIds.isEmpty() && warnings) {
        unsafeIds.removeDuplicates();
        std::sort(unsafeIds.begin(), unsafeIds.end());
        *warnings << QStringLiteral("Skipped %1 rename item(s) with numeric or unsafe IDs: %2")
                         .arg(unsafeIds.size())
                         .arg(unsafeIds.mid(0, 12).join(QStringLiteral(", "))
                              + (unsafeIds.size() > 12 ? QStringLiteral(", ...") : QString()));
    }
    if (pendingByFile.isEmpty()) {
        *error = QStringLiteral("No selected rename items are available.");
        return false;
    }

    QSet<QString> existingIdentityKeys;
    int fileIndex = 0;
    const int totalFiles = analysis.scannedFiles.size();
    for (const ScannedFileInfo &info : analysis.scannedFiles) {
        if (progress)
            progress(QStringLiteral("locate"), fileIndex, totalFiles, info.filePath);
        ++fileIndex;
        auto targetsIt = pendingByFile.find(info.filePath);
        if (targetsIt == pendingByFile.end())
            continue;
        QByteArray bytes;
        if (!readFile(info.filePath, &bytes, error)) return false;
        pugi::xml_document doc;
        const pugi::xml_parse_result parsed = doc.load_buffer(bytes.constData(), size_t(bytes.size()));
        if (!parsed) { *error = QStringLiteral("Cannot parse %1: %2").arg(info.filePath, parsed.description()); return false; }
        collectIdentityKeys(doc, &existingIdentityKeys);
        QHash<QString, int> targetIndexByElementAndId;
        QHash<QString, int> targetIndexByLocation;
        for (int targetIndex = 0; targetIndex < targetsIt.value().size(); ++targetIndex) {
            const PendingRename &target = targetsIt.value().at(targetIndex);
            targetIndexByElementAndId.insert(fastLookupKey(target.elementName, target.oldId), targetIndex);
            if (!target.expectedLocation.isEmpty())
                targetIndexByLocation.insert(target.expectedLocation, targetIndex);
        }
        locateIdentityTargets(doc, &targetIndexByElementAndId, &targetIndexByLocation, &targetsIt.value());
    }

    QHash<QString, QString> renames;
    QHash<QString, QHash<QString, QString>> identities;
    QStringList missing;
    QSet<QString> missingOldIds;
    for (auto it = pendingByFile.begin(); it != pendingByFile.end(); ++it) {
        for (const PendingRename &pending : std::as_const(it.value())) {
            if (!pending.found) {
                missing << pending.oldId;
                missingOldIds.insert(pending.oldId);
                continue;
            }
            renames.insert(pending.oldId, pending.newId);
            identities[it.key()].insert(pending.actualLocation, pending.newId);
        }
    }
    QSet<QString> movingFoundOldKeys;
    for (auto it = pendingByFile.cbegin(); it != pendingByFile.cend(); ++it) {
        for (const PendingRename &pending : std::as_const(it.value())) {
            if (pending.found)
                movingFoundOldKeys.insert(fastLookupKey(pending.elementName, pending.oldId));
        }
    }

    {
        bool removedDependent = true;
        while (removedDependent) {
            removedDependent = false;
            for (auto it = pendingByFile.begin(); it != pendingByFile.end(); ++it) {
                for (const PendingRename &pending : std::as_const(it.value())) {
                    if (!pending.found || !renames.contains(pending.oldId))
                        continue;
                    const bool targetStayedOccupied = existingIdentityKeys.contains(fastLookupKey(pending.elementName, pending.newId))
                        && (!movingFoundOldKeys.contains(fastLookupKey(pending.elementName, pending.newId))
                            || !renames.contains(pending.newId));
                    if (!missingOldIds.contains(pending.newId) && !targetStayedOccupied)
                        continue;
                    renames.remove(pending.oldId);
                    identities[it.key()].remove(pending.actualLocation);
                    missing << QStringLiteral("%1 (target %2 stayed occupied or was not moved)").arg(pending.oldId, pending.newId);
                    missingOldIds.insert(pending.oldId);
                    removedDependent = true;
                }
            }
        }
    }
    if (!missing.isEmpty() && warnings) {
        std::sort(missing.begin(), missing.end());
        *warnings << QStringLiteral("Skipped %1 rename item(s) because their XML identity could not be located after earlier apply steps: %2")
                         .arg(missing.size())
                         .arg(missing.mid(0, 12).join(QStringLiteral(", "))
                              + (missing.size() > 12 ? QStringLiteral(", ...") : QString()));
    }
    if (renames.isEmpty()) {
        *error = QStringLiteral("No selected object identity could be located safely.");
        return false;
    }
    if (appliedRenames)
        *appliedRenames = renames;

    fileIndex = 0;
    for (const ScannedFileInfo &info : analysis.scannedFiles) {
        if (progress)
            progress(QStringLiteral("rewrite"), fileIndex, totalFiles, info.filePath);
        ++fileIndex;
        if (!info.isXml) continue;
        QByteArray bytes;
        if (!readFile(info.filePath, &bytes, error)) return false;
        pugi::xml_document doc;
        const pugi::xml_parse_result parsed = doc.load_buffer(bytes.constData(), size_t(bytes.size()));
        if (!parsed) { *error = QStringLiteral("Cannot parse %1: %2").arg(info.filePath, parsed.description()); return false; }
        RewriteResult fileResult;
        rewrite(doc, info.filePath, identities.value(info.filePath), renames, &fileResult, collectChanges);
        if (fileResult.identities || fileResult.references) {
            std::ostringstream stream;
            doc.save(stream, "  ", pugi::format_default, pugi::encoding_utf8);
            staged->insert(info.filePath, QByteArray::fromStdString(stream.str()));
            files->append(info.filePath);
            totals->identities += fileResult.identities;
            totals->references += fileResult.references;
            totals->changes += fileResult.changes;
        }
    }
    int expectedIdentities = 0;
    for (auto it = identities.cbegin(); it != identities.cend(); ++it)
        expectedIdentities += it.value().size();
    if (totals->identities != expectedIdentities) {
        *error = QStringLiteral("Not every selected object identity could be rewritten safely.");
        return false;
    }
    if (progress)
        progress(QStringLiteral("rewrite"), totalFiles, totalFiles, QString());
    return true;
}

} // namespace

RenamePreviewReport ReferenceRenamer::preview(const AnalysisResult &analysis, const RenamePlan &plan) const
{
    RenamePreviewReport result;
    result.plan = plan;
    result.conflicts = plan.conflicts;
    result.warnings = plan.warnings;
    QHash<QString, QByteArray> staged;
    RewriteResult rewriteResult;
    QStringList prepareWarnings;
    QString error;
    if (plan.valid && !prepare(analysis, plan, &staged, &rewriteResult, &result.filesChanged,
                               &prepareWarnings, &error, true, {})) {
        const QString suffix = QFileInfo(analysis.rootFolder).suffix().toLower();
        const bool archive = suffix.startsWith(QStringLiteral("sc2"));
        if (!archive) {
            result.conflicts << error;
        } else {
            // Archive extraction is ephemeral; provide a serialized-node planning
            // preview while keeping Apply disabled at the UI boundary.
            QHash<QString, QString> renames;
            for (const RenamePlanItem &item : plan.items) {
                if (item.nodeIndex < 0 || item.nodeIndex >= analysis.nodes.size())
                    continue;
                const DataNode &node = analysis.nodes[item.nodeIndex];
                if (sc2dh::isSafeAutomaticObjectId(item.oldId) && !sc2dh::isProtectedCatalogNode(node))
                    renames.insert(item.oldId, item.newId);
            }
            rewriteResult.identities = plan.items.size();
            QSet<QString> files;
            for (const DataNode &node : analysis.nodes) {
                files.insert(node.sourceFile);
                QString value = node.serializedXml;
                int count = simultaneousReplace(&value, renames);
                if (renames.contains(node.id) && count > 0) --count;
                if (count > 0) rewriteResult.references += count;
            }
            result.filesChanged = files.values();
            result.warnings << QStringLiteral("Archive mode is preview-only; reference locations are estimated from extracted serialized XML.");
            error.clear();
        }
    }
    result.warnings.append(prepareWarnings);
    result.identitiesRenamed = rewriteResult.identities;
    result.referencesUpdated = rewriteResult.references;
    result.referenceChanges = rewriteResult.changes;
    result.valid = plan.valid && result.conflicts.isEmpty() && rewriteResult.identities > 0;
    result.reportText = buildReport(analysis, plan, rewriteResult, result.filesChanged, result.warnings,
                                    result.conflicts, QStringLiteral("Preview only; no files modified"));
    return result;
}

RenameApplyResult ReferenceRenamer::apply(const AnalysisResult &analysis, const RenamePlan &plan,
                                          const QString &rootFolder, const QSet<QString> &whitelistIds,
                                          const ProgressCallback &progress) const
{
    RenameApplyResult result;
    if (!plan.valid) {
        result.error = plan.conflicts.join(QStringLiteral("; "));
        return result;
    }
    QHash<QString, QByteArray> staged;
    RewriteResult rewriteResult;
    QStringList absoluteFiles;
    QStringList warnings;
    QHash<QString, QString> appliedRenames;
    if (!prepare(analysis, plan, &staged, &rewriteResult, &absoluteFiles,
                 &warnings, &result.error, false, progress, &appliedRenames)) return result;
    result.warnings = warnings;
    result.appliedRenames = appliedRenames;
    if (rewriteResult.identities <= 0) {
        result.error = QStringLiteral("No selected object identity could be renamed.");
        return result;
    }
    const QString previewReportText = buildReport(analysis, plan, rewriteResult, absoluteFiles,
                                                  plan.warnings + warnings, plan.conflicts,
                                                  QStringLiteral("Apply staged; no files committed yet"));
    QStringList relativeFiles;
    for (const QString &file : absoluteFiles) relativeFiles << QDir(rootFolder).relativeFilePath(file);
    std::sort(relativeFiles.begin(), relativeFiles.end());
    BackupManager backup;
    if (progress)
        progress(QStringLiteral("backup"), 0, 1, QString());
    if (!backup.createFolderBackup(rootFolder, relativeFiles, analysis.analysisReportText,
                                   previewReportText, &result.backupFolder, &result.error)) return result;
    if (m_failureInjectionStep == QStringLiteral("after-backup")) { result.error = QStringLiteral("Injected failure after backup."); return result; }
    QStringList committed;
    int writeIndex = 0;
    const int writeTotal = staged.size();
    for (auto it = staged.cbegin(); it != staged.cend(); ++it) {
        if (progress)
            progress(QStringLiteral("write"), writeIndex, writeTotal, it.key());
        ++writeIndex;
        QSaveFile file(it.key());
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || file.write(it.value()) != it.value().size() || !file.commit()) {
            result.error = QStringLiteral("Failed to commit %1.").arg(it.key());
            restore(rootFolder, result.backupFolder, committed, &result.error);
            return result;
        }
        committed << QDir(rootFolder).relativeFilePath(it.key());
    }
    if (progress)
        progress(QStringLiteral("verify"), 0, 1, QString());
    if (m_failureInjectionStep == QStringLiteral("after-commit")) {
        result.error = QStringLiteral("Injected failure after commit.");
        restore(rootFolder, result.backupFolder, relativeFiles, &result.error);
        return result;
    }
    FolderAnalyzer analyzer;
    AnalysisResult rebuilt;
    QString verifyError;
    if (!analyzer.analyzeFolder(rootFolder, whitelistIds, &rebuilt, &verifyError)) {
        result.error = QStringLiteral("Re-analysis failed: %1").arg(verifyError);
        restore(rootFolder, result.backupFolder, relativeFiles, &result.error);
        return result;
    }
    QSet<QString> rebuiltIds;
    for (const DataNode &node : rebuilt.nodes) rebuiltIds.insert(node.id);
    QSet<QString> newIds;
    for (auto it = appliedRenames.cbegin(); it != appliedRenames.cend(); ++it)
        newIds.insert(it.value());
    QSet<QString> oldIdsToCheck;
    QStringList postWarnings;
    for (const RenamePlanItem &item : plan.items) {
        if (appliedRenames.value(item.oldId) != item.newId)
            continue;
        if (!rebuiltIds.contains(item.newId))
            postWarnings << QStringLiteral("%1 -> %2 was written, but refreshed analysis did not expose the new ID.")
                                .arg(item.oldId, item.newId);
        if (!newIds.contains(item.oldId) && rebuiltIds.contains(item.oldId))
            postWarnings << QStringLiteral("%1 -> %2 was written, but refreshed analysis still exposes the old ID.")
                                .arg(item.oldId, item.newId);
        if (!newIds.contains(item.oldId))
            oldIdsToCheck.insert(item.oldId);
    }
    if (!oldIdsToCheck.isEmpty()) {
        for (const DataNode &node : rebuilt.nodes) {
            for (const QString &reference : node.referencedIds) {
                if (oldIdsToCheck.contains(reference)) {
                    postWarnings << QStringLiteral("A refreshed-analysis reference to old ID %1 remains in %2.")
                                        .arg(reference, node.id);
                    break;
                }
            }
        }
    }
    if (!postWarnings.isEmpty()) {
        if (postWarnings.size() > 20)
            postWarnings = postWarnings.mid(0, 20) << QStringLiteral("... %1 more post-rename verification warning(s).")
                                                       .arg(postWarnings.size() - 20);
        result.warnings << QStringLiteral("Post-rename verification reported non-fatal analysis mismatches:\n- %1")
                               .arg(postWarnings.join(QStringLiteral("\n- ")));
    }
    result.success = true;
    result.changedFiles = relativeFiles;
    result.identitiesRenamed = rewriteResult.identities;
    result.referencesUpdated = rewriteResult.references;
    result.finalReport = previewReportText + QStringLiteral("\nFinal result after apply: success\nBackup: %1\n").arg(result.backupFolder);
    return result;
}
