#include "app/OptimizationWizardController.h"

#include "app/MainWindow.h"
#include "app/Sc2MessageDialog.h"

#include "core/ArchiveReferenceRewriter.h"
#include "core/DeepCleanupService.h"
#include "core/StandardNamePlanner.h"
#include "core/UnitFamilyDetector.h"

#include "ui/AnalysisProgressDialog.h"
#include "ui/FormatterPage.h"
#include "ui/OverviewPage.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMessageBox>
#include <QPair>
#include <QSettings>
#include <QTemporaryDir>

namespace
{
    DataCollectionMode configuredDataCollectionMode()
    {
        QSettings settings;
        return settings.value(QStringLiteral("dataCollection/mode"), QStringLiteral("UnitAbilWeapon")).toString().compare(QStringLiteral("UnitAbilWeapon"), Qt::CaseInsensitive) == 0
                   ? DataCollectionMode::UnitAbilWeapon
                   : DataCollectionMode::Unit;
    }

    bool persistentBackupsEnabledForUi()
    {
        return QSettings().value(QStringLiteral("backup/enabled"), true).toBool();
    }

    QString backupPrompt(const QString &withBackup, const QString &withoutBackup)
    {
        return persistentBackupsEnabledForUi() ? withBackup : withoutBackup;
    }

    QString archiveFolderReadOnlyMessage()
    {
        return QStringLiteral("Archive folder mode analyzes multiple maps/mods together and is read-only. Open a single SC2Map/SC2Mod archive or an extracted folder to apply changes.");
    }

    QString modeLabelFor(int kind)
    {
        switch (kind)
        {
        case 0:
            return QStringLiteral("Mode: folder analysis");
        case 1:
            return QStringLiteral("Mode: archive folder analysis (read-only)");
        case 2:
            return QStringLiteral("Mode: XML file analysis");
        case 3:
            return QStringLiteral("Mode: archive analysis (read-only)");
        default:
            return QStringLiteral("Mode: waiting for analysis");
        }
    }
}

namespace sc2dh::app
{
OptimizationWizardController::OptimizationWizardController(MainWindow &window)
    : m_window(window)
{
}

void OptimizationWizardController::applyPlan()
{
    MainWindow *window = &m_window;
    if (!window->m_dryRunPage)
        return;

    const OptimizationWizardSelection selection = window->m_dryRunPage->currentSelection();
    if (selection.unused.isEmpty() && selection.duplicates.isEmpty() && selection.importCleanup.isEmpty() && selection.deepCleanup.isEmpty()
        && selection.rename.isEmpty() && selection.collection.isEmpty())
    {
        if (window->m_wizardApplyAutomation)
        {
            window->finishWizardApplyAutomation(false, QStringLiteral("No optimization items were selected."));
            return;
        }
        showSc2MessageDialog(window,
                             QMessageBox::Information,
                             QStringLiteral("Optimization Wizard"),
                             QStringLiteral("Select at least one item before applying the optimization plan."),
                             QMessageBox::Ok,
                             660);
        return;
    }
    if (window->m_sourceKind == MainWindow::SourceKind::ArchiveFolder)
    {
        if (window->m_wizardApplyAutomation)
        {
            window->finishWizardApplyAutomation(false, archiveFolderReadOnlyMessage());
            return;
        }
        showSc2MessageDialog(window,
                             QMessageBox::Information,
                             QStringLiteral("Optimization Wizard"),
                             archiveFolderReadOnlyMessage(),
                             QMessageBox::Ok,
                             760);
        return;
    }

    if (!window->m_wizardApplyAutomation
        && showSc2MessageDialog(window,
                                QMessageBox::Question,
                                QStringLiteral("Apply Optimization Plan"),
                                QStringLiteral("Apply the selected optimization steps to files now, then rebuild the preview from the updated data?"),
                                QMessageBox::Yes | QMessageBox::No,
                                660) != QMessageBox::Yes)
    {
        return;
    }

    window->m_dryRunPage->setApplyingState(true, QStringLiteral("Applying selected optimization steps and saving files...\n\nThe wizard will rebuild the preview from updated files when the batch finishes."));
    AnalysisProgressDialog applyProgress(window);
    applyProgress.setTitleText(QStringLiteral("SC2 DATA APPLY"));
    applyProgress.setCancelVisible(false);
    applyProgress.setProgress(5, QStringLiteral("Preparing apply"), QStringLiteral("Building the selected optimization batch"));
    applyProgress.show();
    QApplication::processEvents();
    const auto updateApplyProgress = [&](int percent, const QString &primary, const QString &secondary = QString())
    {
        if (window->m_wizardApplyAutomation)
            window->appendWizardApplyAutomationLog(QStringLiteral("progress %1% | %2 | %3").arg(percent).arg(primary, secondary));
        applyProgress.setProgress(percent, primary, secondary);
        QApplication::processEvents();
    };

    int removedUnused = 0;
    int removedDuplicates = 0;
    int redirectedReferences = 0;
    int importCleanupChanged = 0;
    int deepCleanupChanged = 0;
    int renamedIds = 0;
    int collectionAdded = 0;
    int collectionReorganized = 0;
    QStringList warnings;
    QStringList notes;
    QString failure;
    QString archiveBackup;
    bool archiveAnalysisReady = false;
    int staleRenameRecommendations = 0;
    int renameConflictRecommendations = 0;
    int staleDuplicateRecommendations = 0;
    int staleUnusedRecommendations = 0;
    int dataCollectionUnavailable = 0;
    int dataCollectionNotApplicable = 0;
    int reviewOnlyCleanupSkipped = 0;
    int automaticFollowUpCleanupChanges = 0;
    int serviceSkippedRecommendations = 0;

    const auto reloadWorkingAnalysis = [window](const QString &rootFolder, AnalysisResult *analysis, QString *errorMessage)
    {
        return window->m_analyzer.analyzeFolder(rootFolder, window->m_whitelistIds, analysis, errorMessage);
    };
    const auto groupKey = [](const WizardNodeRef &ref)
    {
        return ref.sourceFile + QChar(0x1f) + ref.originalLocation + QChar(0x1f) + ref.elementName + QChar(0x1f) + ref.id;
    };
    const auto makeRenameProgress = [&](int basePercent, int spanPercent) -> ReferenceRenamer::ProgressCallback
    {
        return [&, basePercent, spanPercent](const QString &stage, int index, int total, const QString &file)
        {
            int percent = basePercent;
            if (total > 0)
                percent += (qBound(0, index, total) * spanPercent) / total;
            QString detail;
            if (stage == QStringLiteral("locate"))
                detail = QStringLiteral("Locating XML identities");
            else if (stage == QStringLiteral("rewrite"))
                detail = QStringLiteral("Rewriting XML IDs and references");
            else if (stage == QStringLiteral("backup"))
                detail = QStringLiteral("Creating rename backup");
            else if (stage == QStringLiteral("write"))
                detail = QStringLiteral("Saving renamed XML");
            else if (stage == QStringLiteral("verify"))
                detail = QStringLiteral("Verifying renamed IDs");
            else
                detail = QStringLiteral("Applying rename changes");
            if (!file.isEmpty())
                detail += QStringLiteral(": %1").arg(QFileInfo(file).fileName());
            updateApplyProgress(qBound(basePercent, percent, basePercent + spanPercent),
                                QStringLiteral("Applying rename changes"), detail);
        };
    };
    const auto archiveReferenceFilesForWorkspace = [](const AnalysisResult &analysis, const QString &rootFolder)
    {
        QStringList files;
        const QDir root(rootFolder);
        for (const ScannedFileInfo &file : analysis.scannedFiles)
        {
            if (file.isXml || !file.isSc2DataLike)
                continue;
            QString relative = root.relativeFilePath(file.filePath);
            relative = QDir::cleanPath(relative).replace('\\', '/');
            if (!relative.startsWith(QStringLiteral("../")) && !QDir::isAbsolutePath(relative))
                files << relative;
        }
        files.removeDuplicates();
        return files;
    };
    const auto buildCombinedRenamePlan = [window](const AnalysisResult &analysis,
                                                const QVector<WizardRenameSelection> &renameSelection,
                                                QStringList *planWarnings)
    {
        RenamePlan combined;
        combined.targetRootId = QStringLiteral("Batch");
        if (renameSelection.isEmpty())
            return combined;

        QHash<QString, QVector<WizardNodeRef>> renameByFamily;
        for (const WizardRenameSelection &item : renameSelection)
            renameByFamily[item.familyRootId].append(item.node);
        if (renameByFamily.isEmpty())
            return combined;

        const QVector<UnitFamily> families = UnitFamilyDetector().detect(analysis);
        QHash<QString, UnitFamily> familyByRoot;
        for (const UnitFamily &family : families)
            familyByRoot.insert(family.rootId, family);

        QSet<QString> existingIds;
        for (const DataNode &node : analysis.nodes)
            existingIds.insert(node.id);

        StandardNamePlanner planner;
        QVector<RenamePlanItem> candidates;
        QSet<int> selectedNodes;
        QHash<QString, int> proposedNewCounts;

        for (auto it = renameByFamily.cbegin(); it != renameByFamily.cend(); ++it)
        {
            const auto familyIt = familyByRoot.constFind(it.key());
            if (familyIt == familyByRoot.cend())
            {
                if (planWarnings)
                    *planWarnings << QStringLiteral("Rename recommendation became stale after earlier changes: %1").arg(it.key());
                continue;
            }
            QSet<int> includedNodeIndices;
            for (const WizardNodeRef &ref : it.value())
            {
                const int index = window->findNodeIndex(analysis, ref);
                if (index >= 0)
                    includedNodeIndices.insert(index);
            }
            if (includedNodeIndices.isEmpty())
                continue;

            const RenamePlan plan = planner.plan(analysis, familyIt.value(), familyIt.value().rootId, includedNodeIndices);
            if (!plan.valid)
            {
                if (planWarnings)
                    *planWarnings << QStringLiteral("Rename recommendation is no longer valid after refresh: %1").arg(it.key());
                continue;
            }
            if (combined.family.rootId.isEmpty())
                combined.family = plan.family;
            combined.warnings.append(plan.warnings);

            for (const RenamePlanItem &item : plan.items)
            {
                if (!item.selected || item.blocked || item.oldId == item.newId)
                    continue;
                if (selectedNodes.contains(item.nodeIndex))
                {
                    if (planWarnings)
                        *planWarnings << QStringLiteral("Duplicate rename recommendation ignored after refresh: %1").arg(item.oldId);
                    continue;
                }
                selectedNodes.insert(item.nodeIndex);
                candidates.append(item);
                ++proposedNewCounts[item.newId.toLower()];
            }
        }

        QVector<RenamePlanItem> filtered;
        filtered.reserve(candidates.size());
        for (const RenamePlanItem &item : candidates)
        {
            if (proposedNewCounts.value(item.newId.toLower()) > 1)
            {
                if (planWarnings)
                    *planWarnings << QStringLiteral("Rename conflict after refresh: %1 -> %2 uses a duplicate target ID.")
                                      .arg(item.oldId, item.newId);
                continue;
            }
            filtered.append(item);
        }

        bool changed = true;
        while (changed)
        {
            changed = false;
            QSet<QString> movingOldIds;
            for (const RenamePlanItem &item : filtered)
                movingOldIds.insert(item.oldId);

            QVector<RenamePlanItem> next;
            next.reserve(filtered.size());
            for (const RenamePlanItem &item : filtered)
            {
                if (existingIds.contains(item.newId) && !movingOldIds.contains(item.newId))
                {
                    if (planWarnings)
                        *planWarnings << QStringLiteral("Rename conflict after refresh: %1 -> %2 target ID is still occupied.")
                                          .arg(item.oldId, item.newId);
                    changed = true;
                    continue;
                }
                next.append(item);
            }
            filtered = next;
        }

        combined.items = filtered;
        combined.valid = !combined.items.isEmpty();
        if (!combined.valid)
            combined.conflicts << QStringLiteral("No safe rename items remained after batch validation.");
        return combined;
    };
    const auto collectionSkipReason = [](const DataCollectionPreviewReport &preview)
    {
        QStringList details = preview.warnings + preview.idConflicts;
        details.removeDuplicates();
        if (details.isEmpty())
            return QStringLiteral("preview is not valid for automatic apply");
        if (details.size() > 4)
            details = details.mid(0, 4) << QStringLiteral("...");
        return details.join(QStringLiteral("; "));
    };
    const auto automaticFollowUpDeepCleanupRows = [](const AnalysisResult &analysis)
    {
        QVector<int> rows;
        for (const DeepCleanupCandidate &candidate : analysis.deepCleanupCandidates)
        {
            if (candidate.kind == DeepCleanupKind::UnusedAsset)
                continue;
            if (candidate.state == CandidateState::Safe
                && candidate.recommended
                && candidate.action != DeepCleanupAction::ReportOnly)
            {
                rows.append(candidate.index);
            }
        }
        return rows;
    };
    const auto deepCleanupChangeCount = [](const DeepCleanupApplyResult &result)
    {
        return result.filesDeleted + result.textLinesRemoved + result.xmlNodesRemoved + result.xmlAttributesRemoved;
    };
    const auto recordRenamePlanNotes = [&](const QStringList &messages)
    {
        for (const QString &message : messages)
        {
            window->logLine(message);
            if (message.startsWith(QStringLiteral("Rename conflict after refresh:")))
                ++renameConflictRecommendations;
            else
                ++staleRenameRecommendations;
        }
    };
    const auto recordServiceMessages = [&](const QStringList &messages)
    {
        for (const QString &message : messages)
        {
            window->logLine(QStringLiteral("Optimization service message: %1").arg(message));
            if (message.contains(QStringLiteral("residual old ID token"), Qt::CaseInsensitive)
                || message.startsWith(QStringLiteral("Post-merge verification still sees"), Qt::CaseInsensitive)
                || message.startsWith(QStringLiteral("Post-rename verification reported non-fatal"), Qt::CaseInsensitive)
                || message.contains(QStringLiteral("saved anyway for manual review"), Qt::CaseInsensitive))
            {
                notes << message;
                continue;
            }
            if (message.startsWith(QStringLiteral("Skipped "), Qt::CaseInsensitive))
            {
                ++serviceSkippedRecommendations;
                continue;
            }
            warnings << message;
        }
    };

    if (window->m_sourceKind == MainWindow::SourceKind::ArchiveFile)
    {
        updateApplyProgress(15, QStringLiteral("Preparing archive workspace"), QStringLiteral("Materializing XML and listfile"));
        QTemporaryDir workspace;
        AnalysisResult current;
        QString error;
        if (!workspace.isValid() || !window->materializeArchiveAnalysis(workspace.path(), &current, &error))
        {
            failure = error;
        }
        else
        {
            QStringList changedFiles;
            QStringList removedFiles;

            if (!reloadWorkingAnalysis(workspace.path(), &current, &error))
            {
                failure = error;
            }
            else
            {
                window->applyArchiveReferenceSafety(&current);
            }

            if (failure.isEmpty() && !selection.importCleanup.isEmpty())
            {
                updateApplyProgress(20, QStringLiteral("Applying import cleanup"), QStringLiteral("Removing unused imported assets from the archive workspace"));
                const DeepCleanupApplyResult result = DeepCleanupService().apply(current, selection.importCleanup, workspace.path(), false);
                if (!result.success)
                {
                    failure = result.error;
                }
                else
                {
                    importCleanupChanged += result.filesDeleted;
                    reviewOnlyCleanupSkipped += result.reportOnlySkipped;
                    changedFiles.append(result.changedFiles);
                    removedFiles.append(result.removedFiles);
                    changedFiles.removeDuplicates();
                    removedFiles.removeDuplicates();
                    if (!reloadWorkingAnalysis(workspace.path(), &current, &error))
                    {
                        failure = error;
                    }
                    else
                    {
                        window->applyArchiveReferenceSafety(&current);
                    }
                }
            }

            if (failure.isEmpty() && !selection.deepCleanup.isEmpty())
            {
                updateApplyProgress(22, QStringLiteral("Applying deep cleanup"), QStringLiteral("Removing stale localization, redundant XML and broken actor events"));
                const DeepCleanupApplyResult result = DeepCleanupService().apply(current, selection.deepCleanup, workspace.path(), false);
                if (!result.success)
                {
                    failure = result.error;
                }
                else
                {
                    deepCleanupChanged += result.filesDeleted + result.textLinesRemoved + result.xmlNodesRemoved + result.xmlAttributesRemoved;
                    reviewOnlyCleanupSkipped += result.reportOnlySkipped;
                    changedFiles.append(result.changedFiles);
                    removedFiles.append(result.removedFiles);
                    changedFiles.removeDuplicates();
                    removedFiles.removeDuplicates();
                    if (!reloadWorkingAnalysis(workspace.path(), &current, &error))
                    {
                        failure = error;
                    }
                    else
                    {
                        window->applyArchiveReferenceSafety(&current);
                    }
                }
            }

            if (failure.isEmpty() && !selection.unused.isEmpty())
            {
                QVector<int> unusedRows;
                for (const WizardNodeRef &ref : selection.unused)
                {
                    const int index = window->findNodeIndex(current, ref);
                    if (index >= 0)
                        unusedRows.append(index);
                }
                if (!unusedRows.isEmpty())
                {
                    updateApplyProgress(25, QStringLiteral("Deleting unused data objects"), QStringLiteral("Rewriting verified archive XML"));
                    QString workspaceBackup;
                    QStringList unusedChangedFiles;
                    int removed = 0;
                    int skipped = 0;
                    if (!window->m_analyzer.applySelectedChanges(current, unusedRows, workspace.path(), window->m_whitelistIds,
                                                         &workspaceBackup, &error, &unusedChangedFiles, &removed, &skipped))
                    {
                        failure = error;
                    }
                    else
                    {
                        removedUnused += removed;
                        changedFiles.append(unusedChangedFiles);
                        changedFiles.removeDuplicates();
                        if (skipped > 0)
                        {
                            staleUnusedRecommendations += skipped;
                            window->logLine(QStringLiteral("Unused Data Objects: %1 selected recommendation(s) became stale after earlier changes.").arg(skipped));
                        }
                        if (!reloadWorkingAnalysis(workspace.path(), &current, &error))
                            failure = error;
                    }
                }
            }
            QHash<QString, QPair<WizardNodeRef, QVector<WizardNodeRef>>> mergeGroups;
            for (const WizardMergeSelection &item : selection.duplicates)
            {
                auto &group = mergeGroups[groupKey(item.keep)];
                group.first = item.keep;
                group.second.append(item.remove);
            }
            QVector<MergeRequest> mergeRequests;
            for (auto it = mergeGroups.cbegin(); it != mergeGroups.cend(); ++it)
            {
                const int keepIndex = window->findNodeIndex(current, it.value().first);
                if (keepIndex < 0)
                {
                    ++staleDuplicateRecommendations;
                    window->logLine(QStringLiteral("Duplicate Merge recommendation became stale after earlier changes: keep object %1 is no longer present.")
                                .arg(it.value().first.id));
                    continue;
                }
                MergeRequest request;
                request.keepNodeIndex = keepIndex;
                for (const WizardNodeRef &remove : it.value().second)
                {
                    const int removeIndex = window->findNodeIndex(current, remove);
                    if (removeIndex >= 0)
                        request.removeNodeIndices.append(removeIndex);
                }
                if (request.removeNodeIndices.isEmpty())
                    continue;
                mergeRequests.append(request);
            }
            if (failure.isEmpty() && !mergeRequests.isEmpty())
            {
                updateApplyProgress(35, QStringLiteral("Applying duplicate merges"), QStringLiteral("Redirecting references and removing duplicate objects"));
                const MergeApplyResult result = window->m_mergeService.applyBatch(
                    current, mergeRequests, workspace.path(), window->m_whitelistIds,
                    [&](int fileIndex, int totalFiles, const QString &file) {
                        const int percent = 35 + ((fileIndex * 15) / qMax(1, totalFiles));
                        updateApplyProgress(percent, QStringLiteral("Applying duplicate merges"),
                                            QStringLiteral("Scanning file %1 of %2: %3")
                                                .arg(qMin(fileIndex + 1, totalFiles))
                                                .arg(totalFiles)
                                                .arg(QFileInfo(file).fileName()));
                    });
                if (!result.success)
                {
                    failure = result.error;
                }
                else
                {
                    removedDuplicates += result.nodesDeleted;
                    redirectedReferences += result.referencesRedirected;
                    changedFiles.append(result.changedFiles);
                    changedFiles.removeDuplicates();
                    if (result.skippedMerges > 0)
                        staleDuplicateRecommendations += result.skippedMerges;
                    recordServiceMessages(result.warnings);
                    if (!reloadWorkingAnalysis(workspace.path(), &current, &error))
                        failure = error;
                }
            }

            if (failure.isEmpty() && !selection.rename.isEmpty())
            {
                updateApplyProgress(55, QStringLiteral("Applying rename changes"), QStringLiteral("Building one safe batch rename plan"));
                QStringList renamePlanNotes;
                const RenamePlan combinedRenamePlan = buildCombinedRenamePlan(current, selection.rename, &renamePlanNotes);
                recordRenamePlanNotes(renamePlanNotes);
                if (combinedRenamePlan.valid)
                {
                    updateApplyProgress(56, QStringLiteral("Applying rename changes"),
                                        QStringLiteral("Updating %1 real XML IDs and references in one pass")
                                            .arg(combinedRenamePlan.items.size()));
                    const RenameApplyResult result = window->m_referenceRenamer.apply(
                        current, combinedRenamePlan, workspace.path(), window->m_whitelistIds,
                        makeRenameProgress(56, 8));
                    if (!result.success)
                    {
                        failure = result.error;
                    }
                    else
                    {
                        renamedIds += result.identitiesRenamed;
                        recordServiceMessages(result.warnings);
                        changedFiles.append(result.changedFiles);
                        changedFiles.removeDuplicates();
                        sc2dh::ArchiveReferenceRewriteReport archiveRewrite;
                        QStringList skippedArchiveRenames;
                        const QHash<QString, QString> archiveRenames =
                            sc2dh::unambiguousArchiveReferenceRenames(current, result.appliedRenames, &skippedArchiveRenames);
                        if (!sc2dh::rewriteArchiveReferenceFiles(workspace.path(),
                                                                 archiveReferenceFilesForWorkspace(current, workspace.path()),
                                                                 archiveRenames,
                                                                 &archiveRewrite,
                                                                 &error))
                        {
                            failure = error;
                        }
                        else
                        {
                            changedFiles.append(archiveRewrite.changedFiles);
                            changedFiles.removeDuplicates();
                            if (archiveRewrite.replacements > 0)
                                notes << QStringLiteral("Rename To Standard: %1 archive placement/trigger/script reference(s) were updated.")
                                             .arg(archiveRewrite.replacements);
                        }
                        if (failure.isEmpty() && !reloadWorkingAnalysis(workspace.path(), &current, &error))
                            failure = error;
                        else if (failure.isEmpty())
                            window->applyArchiveReferenceSafety(&current);
                    }
                }
            }

            QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(current, configuredDataCollectionMode());
            QHash<QString, UnitFamily> familyByRoot;
            for (const UnitFamily &family : families)
                familyByRoot.insert(family.rootId, family);
            bool collectionChanged = false;
            const int collectionCount = selection.collection.size();
            for (int collectionIndex = 0; collectionIndex < collectionCount; ++collectionIndex)
            {
                const WizardCollectionSelection &selectedCollection = selection.collection[collectionIndex];
                if (failure.isEmpty())
                {
                    const int percent = 65 + ((collectionIndex * 15) / qMax(1, collectionCount));
                    updateApplyProgress(percent, QStringLiteral("Applying Data Collection"),
                                        QStringLiteral("Family %1 of %2 (%3%): %4")
                                            .arg(collectionIndex + 1)
                                            .arg(collectionCount)
                                            .arg(((collectionIndex + 1) * 100) / qMax(1, collectionCount))
                                            .arg(selectedCollection.familyRootId));
                    const auto match = familyByRoot.constFind(selectedCollection.familyRootId);
                    if (match == familyByRoot.cend())
                    {
                        ++dataCollectionUnavailable;
                        window->logLine(QStringLiteral("Data Collection note: %1 is no longer present after earlier optimization steps.")
                                    .arg(selectedCollection.familyRootId));
                        continue;
                    }
                    DataCollectionBuildRequest request;
                    request.family = match.value();
                    request.requestedUnitId = request.family.rootId;
                    request.confirmNonStandard = true;
                    for (const WizardNodeRef &ref : selectedCollection.nodes)
                    {
                        const int index = window->findNodeIndex(current, ref);
                        if (index >= 0)
                            request.includedNodeIndices.insert(index);
                    }
                    const DataCollectionPreviewReport preview = window->m_dataCollectionBuilder.preview(current, request, &families);
                    if (!preview.valid)
                    {
                        ++dataCollectionNotApplicable;
                        window->logLine(QStringLiteral("Data Collection note: %1 is not in Data Collection / not eligible for automatic Data Collection after refresh: %2")
                                    .arg(selectedCollection.familyRootId, collectionSkipReason(preview)));
                        continue;
                    }
                    const DataCollectionApplyResult result = window->m_dataCollectionBuilder.apply(
                        current, request, workspace.path(), window->m_whitelistIds, false, &families, true);
                    if (!result.success)
                    {
                        failure = result.error;
                        break;
                    }
                    collectionAdded += result.recordsAdded;
                    collectionReorganized += result.recordsRemoved;
                    changedFiles.append(result.changedFiles);
                    changedFiles.removeDuplicates();
                    collectionChanged = true;
                }
            }

            if (failure.isEmpty() && collectionChanged && !reloadWorkingAnalysis(workspace.path(), &current, &error))
                failure = error;

            if (failure.isEmpty())
            {
                const QVector<int> followUpRows = automaticFollowUpDeepCleanupRows(current);
                if (!followUpRows.isEmpty())
                {
                    updateApplyProgress(82, QStringLiteral("Applying follow-up deep cleanup"),
                                        QStringLiteral("Cleaning safe stale data created by earlier steps"));
                    const DeepCleanupApplyResult result = DeepCleanupService().apply(current, followUpRows, workspace.path(), false);
                    if (!result.success)
                    {
                        failure = result.error;
                    }
                    else
                    {
                        const int changed = deepCleanupChangeCount(result);
                        deepCleanupChanged += changed;
                        automaticFollowUpCleanupChanges += changed;
                        reviewOnlyCleanupSkipped += result.reportOnlySkipped;
                        changedFiles.append(result.changedFiles);
                        removedFiles.append(result.removedFiles);
                        changedFiles.removeDuplicates();
                        removedFiles.removeDuplicates();
                        if (!reloadWorkingAnalysis(workspace.path(), &current, &error))
                        {
                            failure = error;
                        }
                        else
                        {
                            window->applyArchiveReferenceSafety(&current);
                        }
                    }
                }
            }

            if (failure.isEmpty() && (!changedFiles.isEmpty() || !removedFiles.isEmpty()))
            {
                updateApplyProgress(85, QStringLiteral("Saving archive"), QStringLiteral("Writing verified XML back to the SC2 archive"));
                if (!window->commitArchiveChanges(workspace.path(), changedFiles, &archiveBackup, &error, removedFiles))
                {
                    failure = error;
                }
                else
                {
                    window->normalizeArchiveAnalysis(&current, workspace.path(), window->m_currentSourcePath);
                    window->m_result = std::move(current);
                    archiveAnalysisReady = true;
                }
            }
        }
    }
    else
    {
        AnalysisResult current = window->m_result;
        QString error;

        if (failure.isEmpty() && !selection.importCleanup.isEmpty())
        {
            updateApplyProgress(16, QStringLiteral("Applying import cleanup"), QStringLiteral("Removing unused imported assets"));
            const DeepCleanupApplyResult result = DeepCleanupService().apply(current, selection.importCleanup, window->m_rootFolder, true);
            if (!result.success)
            {
                failure = result.error;
            }
            else
            {
                importCleanupChanged += result.filesDeleted;
                reviewOnlyCleanupSkipped += result.reportOnlySkipped;
                if (!reloadWorkingAnalysis(window->m_rootFolder, &current, &error))
                    failure = error;
            }
        }

        if (failure.isEmpty() && !selection.deepCleanup.isEmpty())
        {
            updateApplyProgress(18, QStringLiteral("Applying deep cleanup"), QStringLiteral("Removing stale localization, redundant XML and broken actor events"));
            const DeepCleanupApplyResult result = DeepCleanupService().apply(current, selection.deepCleanup, window->m_rootFolder, true);
            if (!result.success)
            {
                failure = result.error;
            }
            else
            {
                deepCleanupChanged += result.filesDeleted + result.textLinesRemoved + result.xmlNodesRemoved + result.xmlAttributesRemoved;
                reviewOnlyCleanupSkipped += result.reportOnlySkipped;
                if (!reloadWorkingAnalysis(window->m_rootFolder, &current, &error))
                    failure = error;
            }
        }

        QVector<int> unusedRows;
        for (const WizardNodeRef &ref : selection.unused)
        {
            const int index = window->findNodeIndex(current, ref);
            if (index >= 0)
                unusedRows.append(index);
        }
        if (failure.isEmpty() && !unusedRows.isEmpty())
        {
            updateApplyProgress(20, QStringLiteral("Deleting unused data objects"), QStringLiteral("Removing selected safe unused data objects"));
            QString backupFolder;
            QStringList changedFiles;
            int removed = 0;
            int skipped = 0;
            if (!window->m_analyzer.applySelectedChanges(current, unusedRows, window->m_rootFolder, window->m_whitelistIds,
                                                 &backupFolder, &error, &changedFiles, &removed, &skipped))
            {
                failure = error;
            }
            else
            {
                removedUnused += removed;
                if (skipped > 0)
                {
                    staleUnusedRecommendations += skipped;
                    window->logLine(QStringLiteral("Unused Data Objects: %1 selected recommendation(s) became stale after earlier changes.").arg(skipped));
                }
                if (!reloadWorkingAnalysis(window->m_rootFolder, &current, &error))
                    failure = error;
            }
        }

        QHash<QString, QPair<WizardNodeRef, QVector<WizardNodeRef>>> mergeGroups;
        for (const WizardMergeSelection &item : selection.duplicates)
        {
            auto &group = mergeGroups[groupKey(item.keep)];
            group.first = item.keep;
            group.second.append(item.remove);
        }
        QVector<MergeRequest> mergeRequests;
        for (auto it = mergeGroups.cbegin(); it != mergeGroups.cend(); ++it)
        {
            const int keepIndex = window->findNodeIndex(current, it.value().first);
            if (keepIndex < 0)
            {
                ++staleDuplicateRecommendations;
                window->logLine(QStringLiteral("Duplicate Merge recommendation became stale after earlier changes: keep object %1 is no longer present.")
                            .arg(it.value().first.id));
                continue;
            }
            MergeRequest request;
            request.keepNodeIndex = keepIndex;
            for (const WizardNodeRef &remove : it.value().second)
            {
                const int removeIndex = window->findNodeIndex(current, remove);
                if (removeIndex >= 0)
                    request.removeNodeIndices.append(removeIndex);
            }
            if (request.removeNodeIndices.isEmpty())
                continue;
            mergeRequests.append(request);
        }
        if (failure.isEmpty() && !mergeRequests.isEmpty())
        {
            updateApplyProgress(45, QStringLiteral("Applying duplicate merges"), QStringLiteral("Redirecting references and removing duplicate objects"));
            const MergeApplyResult result = window->m_mergeService.applyBatch(
                current, mergeRequests, window->m_rootFolder, window->m_whitelistIds,
                [&](int fileIndex, int totalFiles, const QString &file) {
                    const int percent = 45 + ((fileIndex * 15) / qMax(1, totalFiles));
                    updateApplyProgress(percent, QStringLiteral("Applying duplicate merges"),
                                        QStringLiteral("Scanning file %1 of %2: %3")
                                            .arg(qMin(fileIndex + 1, totalFiles))
                                            .arg(totalFiles)
                                            .arg(QFileInfo(file).fileName()));
                });
            if (!result.success)
            {
                failure = result.error;
            }
            else
            {
                removedDuplicates += result.nodesDeleted;
                redirectedReferences += result.referencesRedirected;
                if (result.skippedMerges > 0)
                    staleDuplicateRecommendations += result.skippedMerges;
                recordServiceMessages(result.warnings);
                if (!reloadWorkingAnalysis(window->m_rootFolder, &current, &error))
                    failure = error;
            }
        }

        if (failure.isEmpty() && !selection.rename.isEmpty())
        {
            updateApplyProgress(60, QStringLiteral("Applying rename changes"), QStringLiteral("Building one safe batch rename plan"));
            QStringList renamePlanNotes;
            const RenamePlan combinedRenamePlan = buildCombinedRenamePlan(current, selection.rename, &renamePlanNotes);
            recordRenamePlanNotes(renamePlanNotes);
            if (combinedRenamePlan.valid)
            {
                updateApplyProgress(61, QStringLiteral("Applying rename changes"),
                                    QStringLiteral("Updating %1 IDs and references in one pass")
                                        .arg(combinedRenamePlan.items.size()));
                const RenameApplyResult result = window->m_referenceRenamer.apply(
                    current, combinedRenamePlan, window->m_rootFolder, window->m_whitelistIds,
                    makeRenameProgress(61, 17));
                if (!result.success)
                {
                    failure = result.error;
                }
                else
                {
                    renamedIds += result.identitiesRenamed;
                    recordServiceMessages(result.warnings);
                    if (!reloadWorkingAnalysis(window->m_rootFolder, &current, &error))
                        failure = error;
                }
            }
        }

        const QVector<UnitFamily> collectionFamilies = UnitFamilyDetector().detectCollectionFamilies(current, configuredDataCollectionMode());
        QHash<QString, UnitFamily> collectionFamilyByRoot;
        for (const UnitFamily &family : collectionFamilies)
            collectionFamilyByRoot.insert(family.rootId, family);
        const int collectionCount = selection.collection.size();
        for (int collectionIndex = 0; collectionIndex < collectionCount; ++collectionIndex)
        {
            const WizardCollectionSelection &selectedCollection = selection.collection[collectionIndex];
            if (!failure.isEmpty())
                break;
            const int percent = 80 + ((collectionIndex * 10) / qMax(1, collectionCount));
            updateApplyProgress(percent, QStringLiteral("Applying Data Collection"),
                                QStringLiteral("Family %1 of %2 (%3%): %4")
                                    .arg(collectionIndex + 1)
                                    .arg(collectionCount)
                                    .arg(((collectionIndex + 1) * 100) / qMax(1, collectionCount))
                                    .arg(selectedCollection.familyRootId));
            const auto familyIt = collectionFamilyByRoot.constFind(selectedCollection.familyRootId);
            if (familyIt == collectionFamilyByRoot.cend())
            {
                ++dataCollectionUnavailable;
                window->logLine(QStringLiteral("Data Collection note: %1 is no longer present after earlier optimization steps.")
                            .arg(selectedCollection.familyRootId));
                continue;
            }
            DataCollectionBuildRequest request;
            request.family = familyIt.value();
            request.requestedUnitId = request.family.rootId;
            request.confirmNonStandard = true;
            for (const WizardNodeRef &ref : selectedCollection.nodes)
            {
                const int index = window->findNodeIndex(current, ref);
                if (index >= 0)
                    request.includedNodeIndices.insert(index);
            }
            const DataCollectionPreviewReport preview = window->m_dataCollectionBuilder.preview(current, request, &collectionFamilies);
            if (!preview.valid)
            {
                ++dataCollectionNotApplicable;
                window->logLine(QStringLiteral("Data Collection note: %1 is not in Data Collection / not eligible for automatic Data Collection after refresh: %2")
                            .arg(selectedCollection.familyRootId, collectionSkipReason(preview)));
                continue;
            }
            const DataCollectionApplyResult result = window->m_dataCollectionBuilder.apply(
                current, request, window->m_rootFolder, window->m_whitelistIds, false, &collectionFamilies);
            if (!result.success)
            {
                failure = result.error;
                break;
            }
            collectionAdded += result.recordsAdded;
            collectionReorganized += result.recordsRemoved;
        }

        if (failure.isEmpty())
        {
            const QVector<int> followUpRows = automaticFollowUpDeepCleanupRows(current);
            if (!followUpRows.isEmpty())
            {
                updateApplyProgress(91, QStringLiteral("Applying follow-up deep cleanup"),
                                    QStringLiteral("Cleaning safe stale data created by earlier steps"));
                const DeepCleanupApplyResult result = DeepCleanupService().apply(current, followUpRows, window->m_rootFolder, true);
                if (!result.success)
                {
                    failure = result.error;
                }
                else
                {
                    const int changed = deepCleanupChangeCount(result);
                    deepCleanupChanged += changed;
                    automaticFollowUpCleanupChanges += changed;
                    reviewOnlyCleanupSkipped += result.reportOnlySkipped;
                    if (!reloadWorkingAnalysis(window->m_rootFolder, &current, &error))
                        failure = error;
                }
            }
        }
    }

    window->m_dryRunPage->setApplyingState(false);

    if (!failure.isEmpty())
    {
        applyProgress.close();
        window->loadPathAndAnalyze(window->m_currentSourcePath);
        if (window->m_wizardApplyAutomation)
        {
            window->finishWizardApplyAutomation(false, failure);
            return;
        }
        showSc2MessageDialog(window,
                             QMessageBox::Critical,
                             QStringLiteral("Optimization Apply Failed"),
                             QStringLiteral("The optimization batch stopped:\n%1").arg(failure),
                             QMessageBox::Ok,
                             700);
        return;
    }

    if (archiveAnalysisReady)
    {
        updateApplyProgress(92, QStringLiteral("Refreshing analysis"), QStringLiteral("Updating object tables"));
        window->m_rootFolder = QFileInfo(window->m_currentSourcePath).absolutePath();
        window->m_analysisPage->setFolderPath(window->m_currentSourcePath);
        window->m_analysisPage->setModeLabel(modeLabelFor(static_cast<int>(window->m_sourceKind)));
        window->m_analysisPage->setAnalysisResult(window->m_result);
        updateApplyProgress(95, QStringLiteral("Refreshing analysis"), QStringLiteral("Updating pages and recommendations"));
        window->refreshPages();
        updateApplyProgress(98, QStringLiteral("Writing report"), QStringLiteral("Saving latest analysis summary"));
        window->writeAnalysisReportFile();
        window->m_dryRunAction->setEnabled(true);
        window->m_applyAction->setEnabled(false);
        window->setCurrentSourcePath(window->m_currentSourcePath);
    }
    else if (!window->loadPathAndAnalyze(window->m_currentSourcePath))
    {
        if (window->m_wizardApplyAutomation)
        {
            window->finishWizardApplyAutomation(false, QStringLiteral("Changes were saved, but automatic re-analysis failed."));
            return;
        }
        showSc2MessageDialog(window,
                             QMessageBox::Warning,
                             QStringLiteral("Optimization Applied"),
                             QStringLiteral("Changes were saved, but automatic re-analysis failed. Re-open Analyze to refresh the wizard view."),
                             QMessageBox::Ok,
                             760);
        return;
    }
    updateApplyProgress(100, QStringLiteral("Apply complete"), QStringLiteral("Optimization changes were saved successfully"));
    applyProgress.close();

    if (removedUnused > 0)
        window->m_dryRunPage->recordUnusedResult(removedUnused);
    if (removedDuplicates > 0 || redirectedReferences > 0)
        window->m_dryRunPage->recordMergeResult(removedDuplicates, redirectedReferences);
    if (importCleanupChanged > 0)
        window->m_dryRunPage->recordImportCleanupResult(importCleanupChanged);
    if (deepCleanupChanged > 0)
        window->m_dryRunPage->recordDeepCleanupResult(deepCleanupChanged);
    if (renamedIds > 0)
        window->m_dryRunPage->recordRenameResult(renamedIds);
    if (collectionAdded > 0 || collectionReorganized > 0)
        window->m_dryRunPage->recordCollectionResult(collectionAdded, collectionReorganized);
    window->m_dryRunPage->rebuildAfterApply();

    if (automaticFollowUpCleanupChanges > 0)
        notes << QStringLiteral("Follow-up Deep Cleanup applied %1 safe cleanup change(s) after earlier optimization steps.")
                     .arg(automaticFollowUpCleanupChanges);
    if (staleUnusedRecommendations > 0)
        notes << QStringLiteral("Unused Data Objects: %1 selected recommendation(s) were already resolved or became unsafe after earlier steps.")
                     .arg(staleUnusedRecommendations);
    if (staleDuplicateRecommendations > 0)
        notes << QStringLiteral("Duplicate Merge: %1 selected recommendation(s) became stale after earlier steps.")
                     .arg(staleDuplicateRecommendations);
    if (staleRenameRecommendations > 0)
        notes << QStringLiteral("Rename To Standard: %1 recommendation(s) became stale after earlier steps.")
                     .arg(staleRenameRecommendations);
    if (renameConflictRecommendations > 0)
        notes << QStringLiteral("Rename To Standard: %1 conflict recommendation(s) were left for manual review.")
                     .arg(renameConflictRecommendations);
    if (dataCollectionUnavailable > 0)
        notes << QStringLiteral("Data Collection: %1 family recommendation(s) were not applied because the source objects were already removed or renamed.")
                     .arg(dataCollectionUnavailable);
    if (dataCollectionNotApplicable > 0)
        notes << QStringLiteral("Data Collection: %1 family recommendation(s) are not in Data Collection or are not eligible after refresh.")
                     .arg(dataCollectionNotApplicable);
    if (reviewOnlyCleanupSkipped > 0)
        notes << QStringLiteral("Review-only cleanup: %1 item(s) were left for manual review.")
                     .arg(reviewOnlyCleanupSkipped);
    if (serviceSkippedRecommendations > 0)
        notes << QStringLiteral("Optimization refresh: %1 stale low-level recommendation(s) were ignored after earlier steps.")
                     .arg(serviceSkippedRecommendations);
    notes.removeDuplicates();
    warnings.removeDuplicates();
    for (const QString &note : notes)
        window->logLine(QStringLiteral("Optimization note: %1").arg(note));
    for (const QString &warning : warnings)
        window->logLine(QStringLiteral("Optimization warning: %1").arg(warning));

    const auto sectionText = [](const QString &title, const QStringList &items, int maxItems)
    {
        if (items.isEmpty())
            return QString();
        QStringList visible = items.mid(0, qMax(1, maxItems));
        if (items.size() > visible.size())
            visible << QStringLiteral("%1 more item(s) are available in Logs.").arg(items.size() - visible.size());
        return QStringLiteral("\n\n%1:\n- %2").arg(title, visible.join(QStringLiteral("\n- ")));
    };

    QString message = QStringLiteral("Selected optimization steps were applied and saved.\n\nUnused data objects deleted: %1\nImported files deleted: %2\nDuplicates deleted: %3\nReferences redirected: %4\nDeep cleanup changes: %5\nIDs renamed: %6\nCollection records added: %7\nCollection records reorganized: %8")
                          .arg(removedUnused)
                          .arg(importCleanupChanged)
                          .arg(removedDuplicates)
                          .arg(redirectedReferences)
                          .arg(deepCleanupChanged)
                          .arg(renamedIds)
                          .arg(collectionAdded)
                          .arg(collectionReorganized);
    if (!archiveBackup.isEmpty())
        message += QStringLiteral("\nArchive backup: %1").arg(archiveBackup);
    message += sectionText(QStringLiteral("Notes"), notes, 6);
    if (!warnings.isEmpty())
        message += sectionText(QStringLiteral("Warnings"), warnings, 4);
    if (window->m_wizardApplyAutomation)
    {
        window->finishWizardApplyAutomation(true, message);
        return;
    }
    showSc2MessageDialog(window,
                         QMessageBox::Information,
                         QStringLiteral("Optimization Applied"),
                         message,
                         QMessageBox::Ok,
                         1040);
}
}

