#include "ui/FormatterPage.h"
#include "core/DataCollectionUnitBuilder.h"
#include "core/StandardNamePlanner.h"
#include "core/UnitFamilyDetector.h"
#include <QAbstractItemView>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabBar>
#include <QTabWidget>
#include <QtConcurrent/QtConcurrentRun>
#include <QVBoxLayout>

#include <algorithm>
namespace {
QTableWidget *makeTable(const QStringList &headers) {
    auto *value = new QTableWidget; value->setColumnCount(headers.size()); value->setHorizontalHeaderLabels(headers);
    value->setSelectionBehavior(QAbstractItemView::SelectRows); value->horizontalHeader()->setStretchLastSection(true);
    value->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn); value->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    value->setVerticalScrollMode(QAbstractItemView::ScrollPerItem); value->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    value->setTextElideMode(Qt::ElideNone); value->verticalHeader()->hide(); return value;
}
void addRow(QTableWidget *target, const OptimizationPlanRow &row) {
    const int index = target->rowCount(); target->insertRow(index);
    for (int column = 0; column < target->columnCount(); ++column) {
        auto *item = new QTableWidgetItem(column < row.values.size() ? row.values[column] : QString()); item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        if (column == 0) {
            if (row.selectable) {
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(row.checked ? Qt::Checked : Qt::Unchecked);
            }
            item->setData(Qt::UserRole, row.primary); item->setData(Qt::UserRole + 1, row.secondary);
            item->setData(Qt::UserRole + 2, row.selectable);
        }
        target->setItem(index, column, item);
    }
}

QVector<int> wizardSteps(bool duplicateMergeEnabled)
{
    return duplicateMergeEnabled ? QVector<int>{0, 1, 2, 3, 4} : QVector<int>{0, 2, 3, 4};
}

OptimizationPlanData calculatePlan(const AnalysisResult &result, bool duplicateMergeEnabled) {
    OptimizationPlanData plan;
    for (const UnusedCandidateInfo &candidate : result.unusedCandidates) {
        const DataNode &node = result.nodes[candidate.nodeIndex];
        const bool safe = candidate.state == CandidateState::Safe;
        plan.unused.append({{safe ? QString() : QStringLiteral("Blocked"), node.id, node.elementName,
                             candidate.reason, candidate.riskLevel}, safe, safe, candidate.nodeIndex, -1});
    }
    if (duplicateMergeEnabled) for (const DuplicateContentGroup &group : result.duplicateContentGroups) if (group.nodeIndices.size() > 1) {
        const int keep = group.nodeIndices.front();
        for (int position = 1; position < group.nodeIndices.size(); ++position) {
            const int remove = group.nodeIndices[position];
            int references = 0;
            for (const DataNode &source : result.nodes) if (source.referencedIds.contains(result.nodes[remove].id)) ++references;
            plan.duplicates.append({{group.mergeCandidate ? QString() : QStringLiteral("Compare"),
                                     group.mergeCandidate ? group.commonIdMask : QStringLiteral("Unrelated IDs - allowed"),
                                     result.nodes[keep].id, result.nodes[remove].id, QString::number(references)},
                                    group.mergeCandidate, group.mergeCandidate, keep, remove});
        }
    }
    const QVector<UnitFamily> collectionFamilies = UnitFamilyDetector().detectCollectionFamilies(result);
    for (int familyIndex = 0; familyIndex < collectionFamilies.size(); ++familyIndex) {
        const UnitFamily &family = collectionFamilies[familyIndex];
        DataCollectionBuildRequest request;
        request.family = family;
        const DataCollectionPreviewReport collection = DataCollectionUnitBuilder().preview(result, request);
        plan.collection.append({{QString(), family.rootId,
                                 QString::number(collection.existingRecordsPreserved.size()),
                                 QString::number(collection.recordsToAdd.size()),
                                 collection.warnings.join(QStringLiteral("; "))},
                                collection.valid, collection.valid, familyIndex, -1});
    }
    return plan;
}
}
FormatterPage::FormatterPage(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(18, 18, 18, 18); layout->setSpacing(12);
    auto *title = new QLabel(QStringLiteral("Optimization Wizard"), this); title->setObjectName(QStringLiteral("panelTitle"));
    layout->addWidget(title);
    auto *hint = new QLabel(QStringLiteral("Build once, then review and adjust every enabled optimization step inside this window. Use the separate main tabs only for individual manual operations."), this);
    hint->setObjectName(QStringLiteral("inspectorSubtitle")); hint->setWordWrap(true); layout->addWidget(hint);
    m_steps = new QTabWidget(this);
    m_steps->setObjectName(QStringLiteral("optimizationSteps"));
    m_unused = makeTable({QStringLiteral("Use"), QStringLiteral("Object ID"), QStringLiteral("Type"), QStringLiteral("Reason"), QStringLiteral("Risk")});
    m_duplicates = makeTable({QStringLiteral("Use"), QStringLiteral("ID mask"), QStringLiteral("Keep"), QStringLiteral("Remove"), QStringLiteral("References redirected")});
    m_rename = makeTable({QStringLiteral("Use"), QStringLiteral("Family"), QStringLiteral("Current ID"), QStringLiteral("Proposed ID"), QStringLiteral("Risk / Conflict")});
    m_collection = makeTable({QStringLiteral("Use"), QStringLiteral("Family"), QStringLiteral("Existing records"), QStringLiteral("Can add"), QStringLiteral("Warnings")});
    m_summary = new QPlainTextEdit; m_summary->setReadOnly(true);
    m_steps->addTab(m_unused, QStringLiteral("Unused Objects")); m_steps->addTab(m_duplicates, QStringLiteral("Duplicate Merge"));
    m_steps->addTab(m_rename, QStringLiteral("Rename")); m_steps->addTab(m_collection, QStringLiteral("Data Collection")); m_steps->addTab(m_summary, QStringLiteral("Summary"));
    m_steps->tabBar()->hide();
    m_details = new QPlainTextEdit(this);
    m_details->setReadOnly(true);
    m_details->setMinimumWidth(420);
    m_details->setPlaceholderText(QStringLiteral("Select an item to inspect its XML or compare Keep / Remove objects."));
    auto *content = new QSplitter(Qt::Horizontal, this);
    content->addWidget(m_steps);
    content->addWidget(m_details);
    content->setStretchFactor(0, 3);
    content->setStretchFactor(1, 2);
    content->setSizes({1250, 600});
    layout->addWidget(content, 1);
    auto *navigation = new QHBoxLayout;
    m_stepLabel = new QLabel(QStringLiteral("Step 1 of 5"), this); m_stepLabel->setObjectName(QStringLiteral("panelTitle"));
    m_backButton = new QPushButton(QStringLiteral("Back"), this);
    m_buildButton = new QPushButton(QStringLiteral("Build Optimization Preview"), this);
    m_selectRecommendedButton = new QPushButton(QStringLiteral("Select Recommended"), this);
    m_clearSelectionButton = new QPushButton(QStringLiteral("Clear Selection"), this);
    m_applyPlanButton = new QPushButton(QStringLiteral("Apply Optimization Plan"), this);
    m_stepActionButton = new QPushButton(QStringLiteral("Open Unused Objects Preview / Apply"), this);
    m_nextButton = new QPushButton(QStringLiteral("Next"), this);
    navigation->addWidget(m_stepLabel); navigation->addStretch(1); navigation->addWidget(m_selectRecommendedButton); navigation->addWidget(m_clearSelectionButton); navigation->addWidget(m_backButton); navigation->addWidget(m_buildButton); navigation->addWidget(m_applyPlanButton); navigation->addWidget(m_nextButton);
    m_stepActionButton->hide();
    layout->addLayout(navigation);
    connect(m_backButton, &QPushButton::clicked, this, [this] {
        const QVector<int> steps = wizardSteps(m_duplicateMergeEnabled);
        const int position = steps.indexOf(m_steps->currentIndex());
        if (position > 0) m_steps->setCurrentIndex(steps[position - 1]);
        updateNavigation();
    });
    connect(m_nextButton, &QPushButton::clicked, this, [this] {
        const QVector<int> steps = wizardSteps(m_duplicateMergeEnabled);
        const int position = steps.indexOf(m_steps->currentIndex());
        if (position < 0 || position + 1 >= steps.size()) emit wizardFinished();
        else { m_steps->setCurrentIndex(steps[position + 1]); updateNavigation(); }
    });
    connect(m_buildButton, &QPushButton::clicked, this, &FormatterPage::buildPreview);
    connect(m_selectRecommendedButton, &QPushButton::clicked, this, [this] { setRecommendedSelection(true); });
    connect(m_clearSelectionButton, &QPushButton::clicked, this, [this] { setRecommendedSelection(false); });
    connect(m_applyPlanButton, &QPushButton::clicked, this, [this] {
        if (!m_planBuilt || m_applying) return;
        emit applyWizardRequested();
    });
    connect(m_steps, &QTabWidget::currentChanged, this, [this] { updateNavigation(); updateDetails(); });
    for (QTableWidget *value : {m_unused, m_duplicates, m_rename, m_collection})
        connect(value, &QTableWidget::itemSelectionChanged, this, &FormatterPage::updateDetails);
    for (QTableWidget *value : {m_unused, m_duplicates, m_rename, m_collection}) connect(value, &QTableWidget::itemChanged, this, [this] {
        m_planConfirmed = false;
        m_hasAppliedChanges = false;
        updateSummary();
        updateNavigation();
    });
}
void FormatterPage::setAnalysisResult(const AnalysisResult &result) {
    m_result = result; for (QTableWidget *value : {m_unused, m_duplicates, m_rename, m_collection}) value->setRowCount(0);
    m_planBuilt = false;
    m_planConfirmed = false;
    m_applying = false;
    m_summary->setPlainText(QStringLiteral("Open Optimization, then press Build Optimization Preview when you are ready to calculate the five-step plan."));
    m_details->clear();
    updateNavigation();
}
void FormatterPage::startWizard() {
    m_planBuilt = false;
    m_planConfirmed = false;
    m_applying = false;
    m_hasAppliedChanges = false;
    m_actualUnused = 0;
    m_actualDuplicates = 0;
    m_actualRedirected = 0;
    m_actualRenamed = 0;
    m_actualCollectionAdded = 0;
    for (QTableWidget *value : {m_unused, m_duplicates, m_rename, m_collection}) value->setRowCount(0);
    m_summary->setPlainText(m_duplicateMergeEnabled
        ? QStringLiteral("Optimization is open.\n\nNo heavy scan was started yet.\nPress Build Optimization Preview to calculate unused objects, duplicate merges, rename items, Data Collection additions, and the final summary.")
        : QStringLiteral("Optimization is open.\n\nDuplicate Merge is disabled in Settings and will be skipped.\nPress Build Optimization Preview to calculate unused objects, rename items, Data Collection additions, and the final summary."));
    m_details->setPlainText(QStringLiteral("1. Press Build Optimization Preview.\n2. Review every item and compare XML here.\n3. Keep recommendations checked or clear individual items.\n4. Apply on step 4 and close when finished."));
    m_steps->setCurrentIndex(0);
    updateNavigation();
}
void FormatterPage::buildPreview() {
    if (m_building) return;
    m_planBuilt = false;
    m_building = true;
    m_planConfirmed = false;
    for (QTableWidget *value : {m_unused, m_duplicates, m_rename, m_collection}) value->setRowCount(0);
    m_summary->setPlainText(QStringLiteral("Building optimization preview in the background...\n\nThe window remains usable while the plan is calculated."));
    updateNavigation();
    auto *watcher = new QFutureWatcher<OptimizationPlanData>(this);
    m_planWatcher = watcher;
    connect(watcher, &QFutureWatcher<OptimizationPlanData>::finished, this, [this, watcher] {
        if (m_planWatcher != watcher) { watcher->deleteLater(); return; }
        const OptimizationPlanData plan = watcher->result();
        m_planWatcher = nullptr;
        const QSignalBlocker unusedBlocker(m_unused);
        const QSignalBlocker duplicateBlocker(m_duplicates);
        const QSignalBlocker renameBlocker(m_rename);
        const QSignalBlocker collectionBlocker(m_collection);
        for (QTableWidget *table : {m_unused, m_duplicates, m_rename, m_collection}) table->setUpdatesEnabled(false);
        for (const OptimizationPlanRow &row : plan.unused) addRow(m_unused, row);
        for (const OptimizationPlanRow &row : plan.duplicates) addRow(m_duplicates, row);
        for (const OptimizationPlanRow &row : plan.rename) addRow(m_rename, row);
        for (const OptimizationPlanRow &row : plan.collection) addRow(m_collection, row);
        const auto addEmptyState = [](QTableWidget *table, const QString &text) {
            if (table->rowCount() == 0) addRow(table, {{QStringLiteral("Info"), text}, false, false, -1, -1});
        };
        addEmptyState(m_unused, QStringLiteral("No unused objects were detected"));
        addEmptyState(m_duplicates, QStringLiteral("No duplicate bodies were detected"));
        addEmptyState(m_rename, QStringLiteral("No rename recommendations were detected"));
        addEmptyState(m_collection, QStringLiteral("No Data Collection recommendations were detected"));
        for (QTableWidget *table : {m_unused, m_duplicates, m_rename, m_collection}) {
            table->resizeColumnsToContents();
            table->setUpdatesEnabled(true);
        }
        m_building = false;
        m_planBuilt = true;
        updateSummary();
        updateNavigation();
        updateDetails();
        emit previewBuilt();
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([result = m_result, duplicates = m_duplicateMergeEnabled] { return calculatePlan(result, duplicates); }));
}
void FormatterPage::updateNavigation() {
    const int step = m_steps->currentIndex();
    const QVector<int> steps = wizardSteps(m_duplicateMergeEnabled);
    const int position = qMax(0, steps.indexOf(step));
    m_stepLabel->setText(QStringLiteral("Step %1 of %2 - %3").arg(position + 1).arg(steps.size()).arg(m_steps->tabText(step)));
    m_backButton->setEnabled(position > 0 && !m_building && !m_applying);
    const bool summary = step == 4;
    m_nextButton->setEnabled(!m_building && !m_applying && (!summary ? m_planBuilt : (m_hasAppliedChanges || (m_planBuilt && m_planConfirmed))));
    m_nextButton->setText(!summary ? QStringLiteral("Next")
                                   : m_hasAppliedChanges ? QStringLiteral("Close Optimization")
                                                         : QStringLiteral("Apply Complete"));
    m_buildButton->setEnabled(!m_planBuilt && !m_building && !m_applying);
    m_buildButton->setText(m_applying ? QStringLiteral("Applying Plan...")
                                      : m_building ? QStringLiteral("Building Preview...")
                                      : m_planBuilt ? QStringLiteral("Preview Ready")
                                                    : QStringLiteral("Build Optimization Preview"));
    m_selectRecommendedButton->setEnabled(m_planBuilt && step < 4 && !m_building && !m_applying);
    m_clearSelectionButton->setEnabled(m_planBuilt && step < 4 && !m_building && !m_applying);
    m_applyPlanButton->setVisible(step == 4);
    m_applyPlanButton->setEnabled(m_planBuilt && !m_planConfirmed && !m_building && !m_applying);
    m_applyPlanButton->setText(m_applying ? QStringLiteral("Applying Plan...")
                                          : m_planConfirmed ? QStringLiteral("Plan Applied")
                                                            : QStringLiteral("Apply Optimization Plan"));
}

void FormatterPage::setRecommendedSelection(bool selected)
{
    auto *table = qobject_cast<QTableWidget *>(m_steps->currentWidget());
    if (!table) return;
    const QSignalBlocker blocker(table);
    m_planConfirmed = false;
    for (int row = 0; row < table->rowCount(); ++row) {
        QTableWidgetItem *item = table->item(row, 0);
        if (item && (item->flags() & Qt::ItemIsUserCheckable))
            item->setCheckState(selected ? Qt::Checked : Qt::Unchecked);
    }
    updateSummary();
    updateDetails();
}

void FormatterPage::updateDetails()
{
    if (!m_details) return;
    const int step = m_steps->currentIndex();
    if (step == 4) {
        m_details->setPlainText(buildIdChangePreview());
        return;
    }
    auto *table = qobject_cast<QTableWidget *>(m_steps->currentWidget());
    if (!table || table->currentRow() < 0) {
        m_details->setPlainText(QStringLiteral("Select a row to inspect it. Checkboxes control which recommendations are included in the final plan."));
        return;
    }
    const int row = table->currentRow();
    QTableWidgetItem *key = table->item(row, 0);
    if (!key) return;
    const int primary = key->data(Qt::UserRole).toInt();
    const int secondary = key->data(Qt::UserRole + 1).toInt();
    const auto xmlFor = [this](int nodeIndex) {
        if (nodeIndex < 0 || nodeIndex >= m_result.nodes.size()) return QStringLiteral("XML is not available.");
        const DataNode &node = m_result.nodes[nodeIndex];
        return QStringLiteral("%1 %2\nFile: %3\nLocation: %4\n\n%5")
            .arg(node.elementName, node.id, node.sourceFile, node.originalLocation, node.serializedXml);
    };
    if (step == 0) {
        m_details->setPlainText(QStringLiteral("UNUSED OBJECT REVIEW\n\n%1").arg(xmlFor(primary)));
    } else if (step == 1) {
        m_details->setPlainText(QStringLiteral("KEEP OBJECT\n============\n%1\n\nREMOVE / COMPARE OBJECT\n=======================\n%2")
                                    .arg(xmlFor(primary), xmlFor(secondary)));
    } else if (step == 2) {
        const QString oldId = table->item(row, 2) ? table->item(row, 2)->text() : QString();
        const QString newId = table->item(row, 3) ? table->item(row, 3)->text() : QString();
        m_details->setPlainText(QStringLiteral("RENAME PREVIEW\n\n%1 -> %2\n\n%3").arg(oldId, newId, xmlFor(secondary)));
    } else {
        QStringList fields;
        for (int column = 1; column < table->columnCount(); ++column)
            fields << QStringLiteral("%1: %2").arg(table->horizontalHeaderItem(column)->text(), table->item(row, column)->text());
        m_details->setPlainText(QStringLiteral("DATA COLLECTION PREVIEW\n\n%1").arg(fields.join(QLatin1Char('\n'))));
    }
}

QString FormatterPage::buildIdChangePreview() const
{
    QStringList lines{QStringLiteral("CURRENT ID  ->  FUTURE ID"), QStringLiteral("========================")};
    for (int row = 0; row < m_duplicates->rowCount(); ++row) {
        const QTableWidgetItem *use = m_duplicates->item(row, 0);
        if (use && use->checkState() == Qt::Checked)
            lines << QStringLiteral("%1  ->  %2  [duplicate merge]").arg(m_duplicates->item(row, 3)->text(), m_duplicates->item(row, 2)->text());
    }
    for (int row = 0; row < m_rename->rowCount(); ++row) {
        const QTableWidgetItem *use = m_rename->item(row, 0);
        if (use && use->checkState() == Qt::Checked)
            lines << QStringLiteral("%1  ->  %2  [rename]").arg(m_rename->item(row, 2)->text(), m_rename->item(row, 3)->text());
    }
    for (int row = 0; row < m_unused->rowCount(); ++row) {
        const QTableWidgetItem *use = m_unused->item(row, 0);
        if (use && use->checkState() == Qt::Checked)
            lines << QStringLiteral("%1  ->  <deleted>  [unused]").arg(m_unused->item(row, 1)->text());
    }
    if (lines.size() == 2) lines << QStringLiteral("No ID changes selected.");
    lines << QStringLiteral("\nDATA COLLECTION OUTPUT") << QStringLiteral("======================");
    for (int row = 0; row < m_collection->rowCount(); ++row) {
        const QTableWidgetItem *use = m_collection->item(row, 0);
        if (use && use->checkState() == Qt::Checked)
            lines << QStringLiteral("%1 -> CDataCollectionUnit + %2 record(s)")
                         .arg(m_collection->item(row, 1)->text(), m_collection->item(row, 3)->text());
    }
    return lines.join(QLatin1Char('\n'));
}

void FormatterPage::openCurrentStep() {
    if (!m_planBuilt) return;
    switch (m_steps->currentIndex()) { case 0: emit openUnusedRequested(selectedUnusedRows()); break; case 1: emit openDuplicateRequested(selectedMergeRequest()); break;
    case 2: emit openRenameRequested(); break; case 3: emit openCollectionRequested(); break; default: break; }
}
QVector<int> FormatterPage::selectedUnusedRows() const { QVector<int> result; for (int index = 0; index < m_unused->rowCount(); ++index)
    if (m_unused->item(index, 0)->checkState() == Qt::Checked) result << m_unused->item(index, 0)->data(Qt::UserRole).toInt(); return result; }
MergeRequest FormatterPage::selectedMergeRequest() const { MergeRequest result; for (int index = 0; index < m_duplicates->rowCount(); ++index) {
    QTableWidgetItem *item = m_duplicates->item(index, 0); if (item->checkState() != Qt::Checked) continue; const int keep = item->data(Qt::UserRole).toInt();
    if (result.keepNodeIndex < 0) result.keepNodeIndex = keep; if (keep == result.keepNodeIndex) result.removeNodeIndices << item->data(Qt::UserRole + 1).toInt(); } return result; }
void FormatterPage::updateSummary() { int redirects = 0, renameCount = 0, collectionAdds = 0;
    int duplicateDeletes = 0;
    for (int index = 0; index < m_duplicates->rowCount(); ++index) {
        QTableWidgetItem *item = m_duplicates->item(index, 0);
        if (item && (item->flags() & Qt::ItemIsUserCheckable) && item->data(Qt::UserRole + 2).toBool() && item->checkState() == Qt::Checked) {
            redirects += m_duplicates->item(index, 4)->text().toInt();
            ++duplicateDeletes;
        }
    }
    for (int index = 0; index < m_rename->rowCount(); ++index) {
        QTableWidgetItem *item = m_rename->item(index, 0);
        if (item && (item->flags() & Qt::ItemIsUserCheckable) && item->data(Qt::UserRole + 2).toBool() && item->checkState() == Qt::Checked) ++renameCount;
    }
    for (int index = 0; index < m_collection->rowCount(); ++index) {
        QTableWidgetItem *item = m_collection->item(index, 0);
        if (item && (item->flags() & Qt::ItemIsUserCheckable) && item->data(Qt::UserRole + 2).toBool() && item->checkState() == Qt::Checked)
            collectionAdds += m_collection->item(index, 3)->text().toInt();
    }
    m_summary->setPlainText(QStringLiteral("Optimization Summary\n\nProjected:\n- unused objects to delete: %1\n- duplicate objects to delete: %2\n- references moved to kept duplicates: %3\n- IDs to rename: %4\n- Data Collection records to add: %5\n\nActually completed:\n- unused deleted: %6\n- duplicates deleted: %7\n- references redirected: %8\n- IDs renamed: %9\n- collection records added: %10\n")
        .arg(selectedUnusedRows().size()).arg(duplicateDeletes).arg(redirects).arg(renameCount).arg(collectionAdds)
        .arg(m_actualUnused).arg(m_actualDuplicates).arg(m_actualRedirected).arg(m_actualRenamed).arg(m_actualCollectionAdded));
    if (m_steps->currentIndex() == 4) updateDetails();
}
void FormatterPage::setPreview(const QString &text) { m_summary->setPlainText(text); m_steps->setCurrentWidget(m_summary); updateNavigation(); }
void FormatterPage::recordUnusedResult(int value) { m_actualUnused += value; updateSummary(); }
void FormatterPage::recordMergeResult(int removed, int redirected) { m_actualDuplicates += removed; m_actualRedirected += redirected; updateSummary(); }
void FormatterPage::recordRenameResult(int value) { m_actualRenamed += value; updateSummary(); }
void FormatterPage::recordCollectionResult(int value) { m_actualCollectionAdded += value; updateSummary(); }
void FormatterPage::setApplyingState(bool applying, const QString &message)
{
    m_applying = applying;
    if (!message.isEmpty()) {
        m_summary->setPlainText(message);
        if (m_steps->currentIndex() != 4) m_steps->setCurrentIndex(4);
    }
    updateNavigation();
}

void FormatterPage::rebuildAfterApply()
{
    m_planBuilt = false;
    m_planConfirmed = false;
    m_building = false;
    m_applying = false;
    m_hasAppliedChanges = true;
    for (QTableWidget *value : {m_unused, m_duplicates, m_rename, m_collection}) value->setRowCount(0);
    m_summary->setPlainText(QStringLiteral("Optimization was applied. Rebuilding preview from updated files..."));
    if (m_steps->currentIndex() != 4) m_steps->setCurrentIndex(4);
    updateNavigation();
    buildPreview();
}

WizardNodeRef FormatterPage::nodeRefFromIndices(int primary, int secondary) const
{
    const int nodeIndex = secondary >= 0 ? secondary : primary;
    if (nodeIndex < 0 || nodeIndex >= m_result.nodes.size()) return {};
    const DataNode &node = m_result.nodes[nodeIndex];
    return {node.id, node.elementName, node.sourceFile, node.originalLocation};
}

QVector<MergeRequest> FormatterPage::selectedMergeRequests() const
{
    QHash<int, QVector<int>> grouped;
    for (int index = 0; index < m_duplicates->rowCount(); ++index) {
        QTableWidgetItem *item = m_duplicates->item(index, 0);
        if (!item || !(item->flags() & Qt::ItemIsUserCheckable) || item->data(Qt::UserRole + 2).toBool() == false || item->checkState() != Qt::Checked) continue;
        grouped[item->data(Qt::UserRole).toInt()].append(item->data(Qt::UserRole + 1).toInt());
    }
    QVector<MergeRequest> result;
    for (auto it = grouped.cbegin(); it != grouped.cend(); ++it) {
        MergeRequest request;
        request.keepNodeIndex = it.key();
        request.removeNodeIndices = it.value();
        result.append(request);
    }
    std::sort(result.begin(), result.end(), [](const MergeRequest &left, const MergeRequest &right) {
        return left.keepNodeIndex < right.keepNodeIndex;
    });
    return result;
}

OptimizationWizardSelection FormatterPage::currentSelection() const
{
    OptimizationWizardSelection result;
    for (int row = 0; row < m_unused->rowCount(); ++row) {
        const QTableWidgetItem *item = m_unused->item(row, 0);
        if (!item || !(item->flags() & Qt::ItemIsUserCheckable) || item->data(Qt::UserRole + 2).toBool() == false || item->checkState() != Qt::Checked) continue;
        result.unused.append(nodeRefFromIndices(item->data(Qt::UserRole).toInt()));
    }
    if (m_duplicateMergeEnabled) for (int row = 0; row < m_duplicates->rowCount(); ++row) {
        const QTableWidgetItem *item = m_duplicates->item(row, 0);
        if (!item || !(item->flags() & Qt::ItemIsUserCheckable) || item->data(Qt::UserRole + 2).toBool() == false || item->checkState() != Qt::Checked) continue;
        WizardMergeSelection selection;
        selection.keep = nodeRefFromIndices(item->data(Qt::UserRole).toInt());
        selection.remove = nodeRefFromIndices(item->data(Qt::UserRole + 1).toInt());
        result.duplicates.append(selection);
    }
    for (int row = 0; row < m_rename->rowCount(); ++row) {
        const QTableWidgetItem *item = m_rename->item(row, 0);
        if (!item || !(item->flags() & Qt::ItemIsUserCheckable) || item->data(Qt::UserRole + 2).toBool() == false || item->checkState() != Qt::Checked) continue;
        WizardRenameSelection selection;
        selection.familyRootId = m_rename->item(row, 1) ? m_rename->item(row, 1)->text() : QString();
        selection.node = nodeRefFromIndices(item->data(Qt::UserRole + 1).toInt());
        result.rename.append(selection);
    }
    for (int row = 0; row < m_collection->rowCount(); ++row) {
        const QTableWidgetItem *item = m_collection->item(row, 0);
        if (!item || !(item->flags() & Qt::ItemIsUserCheckable) || item->data(Qt::UserRole + 2).toBool() == false || item->checkState() != Qt::Checked) continue;
        if (QTableWidgetItem *family = m_collection->item(row, 1))
            result.collectionFamilyRoots.append(family->text());
    }
    result.collectionFamilyRoots.removeDuplicates();
    return result;
}

void FormatterPage::setDuplicateMergeEnabled(bool enabled)
{
    m_duplicateMergeEnabled = enabled;
    if (!enabled && m_steps && m_steps->currentIndex() == 1) m_steps->setCurrentIndex(2);
    updateNavigation();
}
