#include "ui/FormatterPage.h"
#include "core/DataCollectionUnitBuilder.h"
#include "core/DeepCleanupService.h"
#include "core/StandardNamePlanner.h"
#include "core/UnitFamilyDetector.h"
#include <QFont>
#include <QAbstractItemView>
#include <QHash>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSettings>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabBar>
#include <QTabWidget>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <QVBoxLayout>

#include <algorithm>
namespace {
constexpr int kUnusedStep = 0;
constexpr int kDuplicateStep = 1;
constexpr int kImportCleanupStep = 2;
constexpr int kDeepCleanupStep = 3;
constexpr int kRenameStep = 4;
constexpr int kCollectionStep = 5;
constexpr int kSummaryStep = 6;
constexpr int kAuditStep = 7;

QTableWidget *makeTable(const QStringList &headers) {
    auto *value = new QTableWidget; value->setColumnCount(headers.size()); value->setHorizontalHeaderLabels(headers);
    value->setProperty("optimizationTable", true);
    value->setSelectionBehavior(QAbstractItemView::SelectRows); value->horizontalHeader()->setStretchLastSection(true);
    value->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn); value->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    value->setVerticalScrollMode(QAbstractItemView::ScrollPerItem); value->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    value->setMouseTracking(true);
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

bool isSelectableRow(const QTableWidget *table, int row)
{
    const QTableWidgetItem *item = table ? table->item(row, 0) : nullptr;
    return item && (item->flags() & Qt::ItemIsUserCheckable);
}

int selectableRowCount(const QTableWidget *table)
{
    int count = 0;
    if (!table) return count;
    for (int row = 0; row < table->rowCount(); ++row)
        if (isSelectableRow(table, row)) ++count;
    return count;
}

int leadingInt(const QString &text)
{
    static const QRegularExpression expression(QStringLiteral("^\\s*(\\d+)"));
    const QRegularExpressionMatch match = expression.match(text);
    return match.hasMatch() ? match.captured(1).toInt() : text.toInt();
}

int tableInt(const QTableWidget *table, int row, int column)
{
    const QTableWidgetItem *item = table ? table->item(row, column) : nullptr;
    return item ? leadingInt(item->text()) : 0;
}

QVector<int> wizardSteps(bool duplicateMergeEnabled, bool includePostApplyAudit)
{
    QVector<int> steps = duplicateMergeEnabled
        ? QVector<int>{kUnusedStep, kDuplicateStep, kImportCleanupStep, kDeepCleanupStep, kRenameStep, kCollectionStep, kSummaryStep}
        : QVector<int>{kUnusedStep, kImportCleanupStep, kDeepCleanupStep, kRenameStep, kCollectionStep, kSummaryStep};
    if (includePostApplyAudit)
        steps.append(kAuditStep);
    return steps;
}

class XmlTextHighlighter final : public QSyntaxHighlighter
{
public:
    explicit XmlTextHighlighter(QTextDocument *document)
        : QSyntaxHighlighter(document)
    {
    }

protected:
    void highlightBlock(const QString &text) override
    {
        apply(text, QRegularExpression(QStringLiteral("</?[^>\\s/]+")), QStringLiteral("#8bc5ff"), true);
        apply(text, QRegularExpression(QStringLiteral("\\b[A-Za-z_:-]+(?=\\=)")), QStringLiteral("#ffd47a"), false);
        apply(text, QRegularExpression(QStringLiteral("\"[^\"]*\"")), QStringLiteral("#9ef7b6"), false);
        apply(text, QRegularExpression(QStringLiteral("<!--.*-->")), QStringLiteral("#8292a6"), false);
    }

private:
    void apply(const QString &text, const QRegularExpression &regex, const QString &color, bool bold)
    {
        QTextCharFormat format;
        format.setForeground(QColor(color));
        format.setFontWeight(bold ? QFont::Bold : QFont::DemiBold);
        auto matches = regex.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            setFormat(match.capturedStart(), match.capturedLength(), format);
        }
    }
};

DataCollectionMode configuredDataCollectionMode() {
    QSettings settings;
    return settings.value(QStringLiteral("dataCollection/mode"), QStringLiteral("UnitAbilWeapon")).toString()
                   .compare(QStringLiteral("UnitAbilWeapon"), Qt::CaseInsensitive) == 0
        ? DataCollectionMode::UnitAbilWeapon : DataCollectionMode::Unit;
}

OptimizationPlanData calculatePlan(const AnalysisResult &result, bool duplicateMergeEnabled, DataCollectionMode collectionMode) {
    OptimizationPlanData plan;
    for (const UnusedCandidateInfo &candidate : result.unusedCandidates) {
        if (candidate.state != CandidateState::Safe
            || (candidate.usageState != UsageState::Disconnected
                && candidate.usageState != UsageState::UnusedSubgraph))
            continue;
        const DataNode &node = result.nodes[candidate.nodeIndex];
        const QString status = candidate.usageState == UsageState::Used ? QStringLiteral("Used")
            : candidate.usageState == UsageState::Disconnected ? QStringLiteral("Disconnected")
            : candidate.usageState == UsageState::UnusedSubgraph ? QStringLiteral("Unused subgraph")
            : candidate.usageState == UsageState::Risky ? QStringLiteral("Risky") : QStringLiteral("Blocked");
        const QString detail = candidate.reason + (candidate.usagePath.isEmpty() ? QString()
            : QStringLiteral(" | path: %1").arg(candidate.usagePath.join(QStringLiteral(" -> "))));
        plan.unused.append({{status, node.id, node.elementName, detail, candidate.riskLevel},
                            true, true, candidate.nodeIndex, -1});
    }
    QHash<QString, int> incomingReferenceCount;
    if (duplicateMergeEnabled)
        for (const DataNode &source : result.nodes)
            for (const QString &reference : source.referencedIds)
                incomingReferenceCount[reference] += 1;
    if (duplicateMergeEnabled) for (const DuplicateContentGroup &group : result.duplicateContentGroups) if (group.nodeIndices.size() > 1) {
        const int keep = group.nodeIndices.front();
        for (int position = 1; position < group.nodeIndices.size(); ++position) {
            const int remove = group.nodeIndices[position];
            const int references = incomingReferenceCount.value(result.nodes[remove].id);
            plan.duplicates.append({{group.mergeCandidate ? QString() : QStringLiteral("Compare"),
                                     group.mergeCandidate ? group.commonIdMask : QStringLiteral("Unrelated IDs - allowed"),
                                     result.nodes[keep].id, result.nodes[remove].id, QString::number(references)},
                                    group.mergeCandidate, group.mergeCandidate, keep, remove});
        }
    }
    for (const DeepCleanupCandidate &candidate : result.deepCleanupCandidates) {
        const bool safe = candidate.state == CandidateState::Safe && candidate.action != DeepCleanupAction::ReportOnly;
        const QString state = candidate.state == CandidateState::Safe ? QStringLiteral("Safe")
            : candidate.state == CandidateState::Risky ? QStringLiteral("Review")
                                                       : QStringLiteral("Blocked");
        if (candidate.kind == DeepCleanupKind::UnusedAsset) {
            plan.importCleanup.append({{state,
                                        candidate.label,
                                        deepCleanupActionName(candidate.action),
                                        candidate.reason,
                                        candidate.bytes > 0 ? QString::number(candidate.bytes) : QString()},
                                       safe && candidate.recommended,
                                       safe,
                                       candidate.index,
                                       -1});
        } else {
            plan.deepCleanup.append({{state,
                                      deepCleanupKindName(candidate.kind),
                                      candidate.label,
                                      deepCleanupActionName(candidate.action),
                                      candidate.reason,
                                      candidate.bytes > 0 ? QString::number(candidate.bytes) : QString()},
                                     safe && candidate.recommended,
                                     safe,
                                     candidate.index,
                                     -1});
        }
    }
    const QVector<UnitFamily> renameFamilies = UnitFamilyDetector().detect(result);
    for (const UnitFamily &family : renameFamilies) {
        if (family.rootId.isEmpty())
            continue;
        const RenamePlan renamePlan = StandardNamePlanner().plan(result, family, family.rootId);
        for (const RenamePlanItem &item : renamePlan.items) {
            const bool usable = !item.blocked;
            const QString atChange = item.newId.contains(QLatin1Char('@'))
                ? QStringLiteral("Data Collection child ID: %1").arg(item.newId.mid(item.newId.indexOf(QLatin1Char('@'))))
                : QStringLiteral("Root ID");
            plan.rename.append({{usable ? QString() : QStringLiteral("Blocked"),
                                 family.rootId,
                                 unitFamilyRoleName(item.role),
                                 item.oldId,
                                 item.newId,
                                 atChange,
                                 item.blocked ? item.conflict : item.reason},
                                usable, usable, family.rootNodeIndex, item.nodeIndex});
        }
    }
    const QVector<UnitFamily> collectionFamilies = UnitFamilyDetector().detectCollectionFamilies(result, collectionMode);
    DataCollectionUnitBuilder collectionBuilder;
    for (int familyIndex = 0; familyIndex < collectionFamilies.size(); ++familyIndex) {
        const UnitFamily &family = collectionFamilies[familyIndex];
        DataCollectionBuildRequest request;
        request.family = family;
        request.summaryOnly = true;
        request.confirmNonStandard = true;
        const DataCollectionPreviewReport collection = collectionBuilder.preview(result, request, &collectionFamilies);
        plan.collection.append({{QString(), QStringLiteral("%1 [%2]").arg(family.rootId, dataCollectionEntityTypeName(family.entityType)),
                                 QString::number(collection.existingRecordsPreserved.size()),
                                 QString::number(collection.recordsToAdd.size()),
                                 QStringLiteral("%1 remove / %2 copy").arg(collection.recordsToRemove.size()).arg(collection.recordsToMove.size()),
                                 collection.warnings.join(QStringLiteral("; "))},
                                collection.valid, collection.valid, familyIndex, -1});
    }
    return plan;
}
}
FormatterPage::FormatterPage(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("optimizationWizardPage"));
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(18, 18, 18, 18); layout->setSpacing(12);
    auto *title = new QLabel(QStringLiteral("Optimization Wizard"), this); title->setObjectName(QStringLiteral("panelTitle"));
    layout->addWidget(title);
    auto *hint = new QLabel(QStringLiteral("Build once, then review and adjust every enabled optimization step inside this window. Use the separate main tabs only for individual manual operations."), this);
    hint->setObjectName(QStringLiteral("inspectorSubtitle")); hint->setWordWrap(true); layout->addWidget(hint);
    m_steps = new QTabWidget(this);
    m_steps->setObjectName(QStringLiteral("optimizationSteps"));
    m_unused = makeTable({QStringLiteral("Use"), QStringLiteral("Data object ID"), QStringLiteral("Catalog type"), QStringLiteral("Reason"), QStringLiteral("Risk")});
    m_duplicates = makeTable({QStringLiteral("Use"), QStringLiteral("ID mask"), QStringLiteral("Keep"), QStringLiteral("Remove"), QStringLiteral("References redirected")});
    m_importCleanup = makeTable({QStringLiteral("Use"), QStringLiteral("Import file"), QStringLiteral("Action"), QStringLiteral("Reason"), QStringLiteral("Bytes")});
    m_deepCleanup = makeTable({QStringLiteral("Use"), QStringLiteral("Kind"), QStringLiteral("Target"), QStringLiteral("Action"), QStringLiteral("Reason"), QStringLiteral("Bytes")});
    m_rename = makeTable({QStringLiteral("Use"), QStringLiteral("Family"), QStringLiteral("Role"),
                          QStringLiteral("Current ID"), QStringLiteral("Proposed ID"),
                          QStringLiteral("@ change"), QStringLiteral("Risk / Conflict")});
    m_collection = makeTable({QStringLiteral("Use"), QStringLiteral("Family"), QStringLiteral("Existing records"), QStringLiteral("Can add"), QStringLiteral("Move out"), QStringLiteral("Warnings")});
    m_unused->setObjectName(QStringLiteral("optimizationUnusedTable"));
    m_duplicates->setObjectName(QStringLiteral("optimizationDuplicateTable"));
    m_importCleanup->setObjectName(QStringLiteral("optimizationImportCleanupTable"));
    m_deepCleanup->setObjectName(QStringLiteral("optimizationDeepCleanupTable"));
    m_rename->setObjectName(QStringLiteral("optimizationRenameTable"));
    m_collection->setObjectName(QStringLiteral("optimizationCollectionTable"));
    m_summary = new QPlainTextEdit; m_summary->setReadOnly(true);
    m_summary->setObjectName(QStringLiteral("optimizationSummary"));
    m_summary->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_summary->document()->setDefaultFont(QFont(QStringLiteral("Consolas"), 10));
    new XmlTextHighlighter(m_summary->document());
    m_audit = new QPlainTextEdit; m_audit->setReadOnly(true);
    m_audit->setObjectName(QStringLiteral("optimizationAudit"));
    m_audit->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_audit->document()->setDefaultFont(QFont(QStringLiteral("Consolas"), 10));
    m_steps->addTab(m_unused, QStringLiteral("Unused Data Objects")); m_steps->addTab(m_duplicates, QStringLiteral("Duplicate Merge"));
    m_steps->addTab(m_importCleanup, QStringLiteral("Import Cleanup"));
    m_steps->addTab(m_deepCleanup, QStringLiteral("Deep Cleanup"));
    m_steps->addTab(m_rename, QStringLiteral("Rename")); m_steps->addTab(m_collection, QStringLiteral("Data Collection")); m_steps->addTab(m_summary, QStringLiteral("Summary"));
    m_steps->addTab(m_audit, QStringLiteral("Post-Apply Audit"));
    m_steps->tabBar()->hide();
    m_details = new QPlainTextEdit(this);
    m_details->setObjectName(QStringLiteral("optimizationDetails"));
    m_details->setReadOnly(true);
    m_details->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_details->document()->setDefaultFont(QFont(QStringLiteral("Consolas"), 10));
    new XmlTextHighlighter(m_details->document());
    m_details->setMinimumWidth(420);
    m_details->setPlaceholderText(QStringLiteral("Select an item to inspect its XML or compare Keep / Remove objects."));
    auto *content = new QSplitter(Qt::Horizontal, this);
    content->setObjectName(QStringLiteral("optimizationContent"));
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
    m_stepActionButton = new QPushButton(QStringLiteral("Open Unused Data Objects Preview / Apply"), this);
    m_nextButton = new QPushButton(QStringLiteral("Next"), this);
    navigation->addWidget(m_stepLabel); navigation->addStretch(1); navigation->addWidget(m_selectRecommendedButton); navigation->addWidget(m_clearSelectionButton); navigation->addWidget(m_backButton); navigation->addWidget(m_buildButton); navigation->addWidget(m_applyPlanButton); navigation->addWidget(m_nextButton);
    m_stepActionButton->hide();
    layout->addLayout(navigation);
    connect(m_backButton, &QPushButton::clicked, this, [this] {
        const QVector<int> steps = wizardSteps(m_duplicateMergeEnabled, m_hasAppliedChanges);
        const int position = steps.indexOf(m_steps->currentIndex());
        if (position > 0) m_steps->setCurrentIndex(steps[position - 1]);
        updateNavigation();
    });
    connect(m_nextButton, &QPushButton::clicked, this, [this] {
        const QVector<int> steps = wizardSteps(m_duplicateMergeEnabled, m_hasAppliedChanges);
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
    for (QTableWidget *value : {m_unused, m_duplicates, m_importCleanup, m_deepCleanup, m_rename, m_collection})
        connect(value, &QTableWidget::itemSelectionChanged, this, &FormatterPage::updateDetails);
    for (QTableWidget *value : {m_unused, m_duplicates, m_importCleanup, m_deepCleanup, m_rename, m_collection}) connect(value, &QTableWidget::itemChanged, this, [this] {
        m_planConfirmed = false;
        m_hasAppliedChanges = false;
        updateSummary();
        updateNavigation();
    });
}
void FormatterPage::setAnalysisResult(const AnalysisResult &result) {
    m_result = result; for (QTableWidget *value : {m_unused, m_duplicates, m_importCleanup, m_deepCleanup, m_rename, m_collection}) value->setRowCount(0);
    m_planBuilt = false;
    m_planConfirmed = false;
    m_applying = false;
    m_hasAppliedChanges = false;
    m_summary->setPlainText(QStringLiteral("Open Optimization, then press Build Optimization Preview when you are ready to calculate the plan."));
    m_audit->setPlainText(QStringLiteral("Post-Apply Audit\n\nApply an optimization plan to generate the refreshed audit."));
    m_details->clear();
    if (m_steps && m_steps->currentIndex() == kAuditStep)
        m_steps->setCurrentIndex(kUnusedStep);
    updateNavigation();
}
void FormatterPage::startWizard(bool autoBuild) {
    m_planBuilt = false;
    m_planConfirmed = false;
    m_applying = false;
    m_hasAppliedChanges = false;
    m_actualUnused = 0;
    m_actualDuplicates = 0;
    m_actualRedirected = 0;
    m_actualRenamed = 0;
    m_actualCollectionAdded = 0;
    m_actualCollectionReorganized = 0;
    m_actualImportCleanup = 0;
    m_actualDeepCleanup = 0;
    for (QTableWidget *value : {m_unused, m_duplicates, m_importCleanup, m_deepCleanup, m_rename, m_collection}) value->setRowCount(0);
    m_summary->setPlainText(m_duplicateMergeEnabled
        ? QStringLiteral("Optimization is open.\n\nNo heavy scan was started yet.\nPress Build Optimization Preview to calculate unused data objects, duplicate merges, import cleanup, deep cleanup, rename items, Data Collection additions, and the final summary.")
        : QStringLiteral("Optimization is open.\n\nDuplicate Merge is disabled in Settings and will be skipped.\nPress Build Optimization Preview to calculate unused data objects, import cleanup, deep cleanup, rename items, Data Collection additions, and the final summary."));
    m_audit->setPlainText(QStringLiteral("Post-Apply Audit\n\nApply an optimization plan to generate the refreshed audit."));
    m_details->setPlainText(QStringLiteral("1. Press Build Optimization Preview.\n2. Review every item and compare XML here.\n3. Keep recommendations checked or clear individual items.\n4. Apply on the Summary step, then review Post-Apply Audit."));
    m_steps->setCurrentIndex(kUnusedStep);
    updateNavigation();
    if (autoBuild)
        QTimer::singleShot(0, this, &FormatterPage::buildPreview);
}
void FormatterPage::buildPreview() {
    if (m_building) return;
    m_planBuilt = false;
    m_building = true;
    m_planConfirmed = false;
    for (QTableWidget *value : {m_unused, m_duplicates, m_importCleanup, m_deepCleanup, m_rename, m_collection}) value->setRowCount(0);
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
        const QSignalBlocker importCleanupBlocker(m_importCleanup);
        const QSignalBlocker deepCleanupBlocker(m_deepCleanup);
        const QSignalBlocker renameBlocker(m_rename);
        const QSignalBlocker collectionBlocker(m_collection);
        for (QTableWidget *table : {m_unused, m_duplicates, m_importCleanup, m_deepCleanup, m_rename, m_collection}) table->setUpdatesEnabled(false);
        for (const OptimizationPlanRow &row : plan.unused) addRow(m_unused, row);
        for (const OptimizationPlanRow &row : plan.duplicates) addRow(m_duplicates, row);
        for (const OptimizationPlanRow &row : plan.importCleanup) addRow(m_importCleanup, row);
        for (const OptimizationPlanRow &row : plan.deepCleanup) addRow(m_deepCleanup, row);
        for (const OptimizationPlanRow &row : plan.rename) addRow(m_rename, row);
        for (const OptimizationPlanRow &row : plan.collection) addRow(m_collection, row);
        const auto addEmptyState = [](QTableWidget *table, const QString &text) {
            if (table->rowCount() == 0) addRow(table, {{QStringLiteral("Info"), text}, false, false, -1, -1});
        };
        addEmptyState(m_unused, QStringLiteral("No unused data objects were detected"));
        addEmptyState(m_duplicates, QStringLiteral("No duplicate bodies were detected"));
        addEmptyState(m_importCleanup, QStringLiteral("No unused imported files were detected"));
        addEmptyState(m_deepCleanup, QStringLiteral("No deep cleanup candidates were detected"));
        addEmptyState(m_rename, QStringLiteral("No rename recommendations were detected"));
        addEmptyState(m_collection, QStringLiteral("No Data Collection recommendations were detected"));
        for (QTableWidget *table : {m_unused, m_duplicates, m_importCleanup, m_deepCleanup, m_rename, m_collection}) {
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
    const DataCollectionMode collectionMode = configuredDataCollectionMode();
    watcher->setFuture(QtConcurrent::run([result = m_result, duplicates = m_duplicateMergeEnabled, collectionMode] {
        return calculatePlan(result, duplicates, collectionMode);
    }));
}
void FormatterPage::updateNavigation() {
    const int step = m_steps->currentIndex();
    const QVector<int> steps = wizardSteps(m_duplicateMergeEnabled, m_hasAppliedChanges);
    const int position = qMax(0, steps.indexOf(step));
    m_stepLabel->setText(QStringLiteral("Step %1 of %2 - %3").arg(position + 1).arg(steps.size()).arg(m_steps->tabText(step)));
    m_backButton->setEnabled(position > 0 && !m_building && !m_applying);
    const bool summary = step == kSummaryStep;
    const bool audit = step == kAuditStep;
    m_nextButton->setEnabled(!m_building && !m_applying
                             && (audit || (!summary ? m_planBuilt : (m_hasAppliedChanges || (m_planBuilt && m_planConfirmed)))));
    m_nextButton->setText(audit ? QStringLiteral("Close Optimization")
                                : !summary ? QStringLiteral("Next")
                                           : m_hasAppliedChanges ? QStringLiteral("Post-Apply Audit")
                                                                 : QStringLiteral("Apply Complete"));
    m_buildButton->setEnabled(!m_planBuilt && !m_building && !m_applying);
    m_buildButton->setText(m_applying ? QStringLiteral("Applying Plan...")
                                      : m_building ? QStringLiteral("Building Preview...")
                                      : m_planBuilt ? QStringLiteral("Preview Ready")
                                                    : QStringLiteral("Build Optimization Preview"));
    m_selectRecommendedButton->setEnabled(m_planBuilt && step < kSummaryStep && !m_building && !m_applying);
    m_clearSelectionButton->setEnabled(m_planBuilt && step < kSummaryStep && !m_building && !m_applying);
    m_applyPlanButton->setVisible(step == kSummaryStep);
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

void FormatterPage::selectRecommendedItems()
{
    for (QTableWidget *table : {m_unused, m_duplicates, m_importCleanup, m_deepCleanup, m_rename, m_collection}) {
        const QSignalBlocker blocker(table);
        for (int row = 0; row < table->rowCount(); ++row) {
            QTableWidgetItem *item = table->item(row, 0);
            if (item && (item->flags() & Qt::ItemIsUserCheckable))
                item->setCheckState(Qt::Checked);
        }
    }
    m_planConfirmed = false;
    updateSummary();
    updateDetails();
}

void FormatterPage::updateDetails()
{
    if (!m_details) return;
    const int step = m_steps->currentIndex();
    if (step == kSummaryStep) {
        m_details->setPlainText(buildIdChangePreview());
        return;
    }
    if (step == kAuditStep) {
        m_details->setPlainText(m_audit ? m_audit->toPlainText() : QStringLiteral("Post-apply audit is not available."));
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
    if (step == kUnusedStep) {
        m_details->setPlainText(QStringLiteral("UNUSED DATA OBJECT REVIEW\n\n%1").arg(xmlFor(primary)));
    } else if (step == kDuplicateStep) {
        m_details->setPlainText(QStringLiteral("KEEP OBJECT\n============\n%1\n\nREMOVE / COMPARE OBJECT\n=======================\n%2")
                                    .arg(xmlFor(primary), xmlFor(secondary)));
    } else if (step == kImportCleanupStep) {
        QStringList fields;
        for (int column = 1; column < table->columnCount(); ++column)
            fields << QStringLiteral("%1: %2").arg(table->horizontalHeaderItem(column)->text(), table->item(row, column)->text());
        if (primary >= 0 && primary < m_result.deepCleanupCandidates.size()) {
            const DeepCleanupCandidate &candidate = m_result.deepCleanupCandidates[primary];
            fields << QStringLiteral("File: %1").arg(candidate.filePath);
            if (candidate.bytes > 0)
                fields << QStringLiteral("Bytes: %1").arg(candidate.bytes);
        }
        m_details->setPlainText(QStringLiteral("IMPORT CLEANUP REVIEW\n\n%1").arg(fields.join(QLatin1Char('\n'))));
    } else if (step == kDeepCleanupStep) {
        QStringList fields;
        for (int column = 1; column < table->columnCount(); ++column)
            fields << QStringLiteral("%1: %2").arg(table->horizontalHeaderItem(column)->text(), table->item(row, column)->text());
        if (primary >= 0 && primary < m_result.deepCleanupCandidates.size()) {
            const DeepCleanupCandidate &candidate = m_result.deepCleanupCandidates[primary];
            fields << QStringLiteral("File: %1").arg(candidate.filePath);
            if (!candidate.xmlLocation.isEmpty())
                fields << QStringLiteral("XML location: %1").arg(candidate.xmlLocation);
            if (candidate.lineNumber >= 0)
                fields << QStringLiteral("Line: %1").arg(candidate.lineNumber + 1);
            if (!candidate.detail.isEmpty())
                fields << QStringLiteral("\n%1").arg(candidate.detail);
        }
        m_details->setPlainText(QStringLiteral("DEEP CLEANUP REVIEW\n\n%1").arg(fields.join(QLatin1Char('\n'))));
    } else if (step == kRenameStep) {
        const QString role = table->item(row, 2) ? table->item(row, 2)->text() : QString();
        const QString oldId = table->item(row, 3) ? table->item(row, 3)->text() : QString();
        const QString newId = table->item(row, 4) ? table->item(row, 4)->text() : QString();
        const QString atChange = table->item(row, 5) ? table->item(row, 5)->text() : QString();
        m_details->setPlainText(QStringLiteral("RENAME PREVIEW\n\nRole: %1\n%2 -> %3\n%4\n\n%5")
                                    .arg(role, oldId, newId, atChange, xmlFor(secondary)));
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
            lines << QStringLiteral("%1  ->  %2  [rename: %3]")
                         .arg(m_rename->item(row, 3)->text(),
                              m_rename->item(row, 4)->text(),
                              m_rename->item(row, 5)->text());
    }
    for (int row = 0; row < m_unused->rowCount(); ++row) {
        const QTableWidgetItem *use = m_unused->item(row, 0);
        if (use && use->checkState() == Qt::Checked)
            lines << QStringLiteral("%1  ->  <deleted>  [unused data object]").arg(m_unused->item(row, 1)->text());
    }
    for (int row = 0; row < m_importCleanup->rowCount(); ++row) {
        const QTableWidgetItem *use = m_importCleanup->item(row, 0);
        if (use && use->checkState() == Qt::Checked)
            lines << QStringLiteral("%1  ->  <deleted>  [import cleanup]")
                         .arg(m_importCleanup->item(row, 1)->text());
    }
    for (int row = 0; row < m_deepCleanup->rowCount(); ++row) {
        const QTableWidgetItem *use = m_deepCleanup->item(row, 0);
        if (use && use->checkState() == Qt::Checked)
            lines << QStringLiteral("%1  ->  %2  [deep cleanup: %3]")
                         .arg(m_deepCleanup->item(row, 2)->text(),
                              m_deepCleanup->item(row, 3)->text(),
                              m_deepCleanup->item(row, 1)->text());
    }
    if (lines.size() == 2) lines << QStringLiteral("No ID changes selected.");
    lines << QStringLiteral("\nDATA COLLECTION OUTPUT") << QStringLiteral("======================");
    for (int row = 0; row < m_collection->rowCount(); ++row) {
        const QTableWidgetItem *use = m_collection->item(row, 0);
        if (use && use->checkState() == Qt::Checked)
            lines << QStringLiteral("%1 -> typed Data Collection + %2 record(s), reorganize %3")
                         .arg(m_collection->item(row, 1)->text(), m_collection->item(row, 3)->text(), m_collection->item(row, 4)->text());
    }
    return lines.join(QLatin1Char('\n'));
}

void FormatterPage::openCurrentStep() {
    if (!m_planBuilt) return;
    switch (m_steps->currentIndex()) { case kUnusedStep: emit openUnusedRequested(selectedUnusedRows()); break; case kDuplicateStep: emit openDuplicateRequested(selectedMergeRequest()); break;
    case kRenameStep: emit openRenameRequested(); break; case kCollectionStep: emit openCollectionRequested(); break; default: break; }
}
QVector<int> FormatterPage::selectedUnusedRows() const { QVector<int> result; for (int index = 0; index < m_unused->rowCount(); ++index)
    if (m_unused->item(index, 0)->checkState() == Qt::Checked) result << m_unused->item(index, 0)->data(Qt::UserRole).toInt(); return result; }
MergeRequest FormatterPage::selectedMergeRequest() const { MergeRequest result; for (int index = 0; index < m_duplicates->rowCount(); ++index) {
    QTableWidgetItem *item = m_duplicates->item(index, 0); if (item->checkState() != Qt::Checked) continue; const int keep = item->data(Qt::UserRole).toInt();
    if (result.keepNodeIndex < 0) result.keepNodeIndex = keep; if (keep == result.keepNodeIndex) result.removeNodeIndices << item->data(Qt::UserRole + 1).toInt(); } return result; }
void FormatterPage::updateSummary() { int redirects = 0, renameCount = 0, collectionAdds = 0, collectionMoves = 0, importCleanupCount = 0, deepCleanupCount = 0;
    int duplicateDeletes = 0;
    for (int index = 0; index < m_duplicates->rowCount(); ++index) {
        QTableWidgetItem *item = m_duplicates->item(index, 0);
        if (item && (item->flags() & Qt::ItemIsUserCheckable) && item->checkState() == Qt::Checked) {
            redirects += tableInt(m_duplicates, index, 4);
            ++duplicateDeletes;
        }
    }
    for (int index = 0; index < m_rename->rowCount(); ++index) {
        QTableWidgetItem *item = m_rename->item(index, 0);
        if (item && (item->flags() & Qt::ItemIsUserCheckable) && item->checkState() == Qt::Checked) ++renameCount;
    }
    for (int index = 0; index < m_deepCleanup->rowCount(); ++index) {
        QTableWidgetItem *item = m_deepCleanup->item(index, 0);
        if (item && (item->flags() & Qt::ItemIsUserCheckable) && item->checkState() == Qt::Checked) ++deepCleanupCount;
    }
    for (int index = 0; index < m_importCleanup->rowCount(); ++index) {
        QTableWidgetItem *item = m_importCleanup->item(index, 0);
        if (item && (item->flags() & Qt::ItemIsUserCheckable) && item->checkState() == Qt::Checked) ++importCleanupCount;
    }
    for (int index = 0; index < m_collection->rowCount(); ++index) {
        QTableWidgetItem *item = m_collection->item(index, 0);
        if (item && (item->flags() & Qt::ItemIsUserCheckable) && item->checkState() == Qt::Checked)
            collectionAdds += tableInt(m_collection, index, 3), collectionMoves += tableInt(m_collection, index, 4);
    }
    QString summary = QStringLiteral("Optimization Summary\n\nProjected:\n- unused data objects to delete: %1\n- imported files to delete: %2\n- duplicate data objects to delete: %3\n- references moved to kept duplicates: %4\n- deep cleanup changes: %5\n- IDs to rename: %6\n- Data Collection records to add: %7\n- Data Collection records to reorganize: %8\n\nActually completed:\n- unused data objects deleted: %9\n- imported files deleted: %10\n- duplicates deleted: %11\n- references redirected: %12\n- deep cleanup changes: %13\n- IDs renamed: %14\n- collection records added: %15\n- collection records reorganized: %16\n")
        .arg(selectedUnusedRows().size()).arg(importCleanupCount).arg(duplicateDeletes).arg(redirects).arg(deepCleanupCount).arg(renameCount).arg(collectionAdds).arg(collectionMoves)
        .arg(m_actualUnused).arg(m_actualImportCleanup).arg(m_actualDuplicates).arg(m_actualRedirected).arg(m_actualDeepCleanup).arg(m_actualRenamed).arg(m_actualCollectionAdded).arg(m_actualCollectionReorganized);
    if (m_hasAppliedChanges && m_planBuilt) {
        QStringList nextSteps;
        if (selectedUnusedRows().size() > 0)
            nextSteps << QStringLiteral("Run another unused data-object pass: refreshed analysis still has %1 safe candidate(s).").arg(selectedUnusedRows().size());
        if (duplicateDeletes > 0)
            nextSteps << QStringLiteral("Review duplicate merge again: %1 merge candidate(s) remain after refresh.").arg(duplicateDeletes);
        if (importCleanupCount > 0)
            nextSteps << QStringLiteral("Review Import Cleanup again: %1 imported file(s) remain after refresh.").arg(importCleanupCount);
        if (deepCleanupCount > 0)
            nextSteps << QStringLiteral("Review deep cleanup again: %1 cleanup candidate(s) remain after the automatic follow-up pass.").arg(deepCleanupCount);
        if (renameCount > 0)
            nextSteps << QStringLiteral("Review Rename To Standard again: %1 ID change(s) are still available.").arg(renameCount);
        if (collectionAdds > 0 || collectionMoves > 0)
            nextSteps << QStringLiteral("Update Data Collection again: %1 record(s) can be added, %2 record(s) can be reorganized.").arg(collectionAdds).arg(collectionMoves);
        if (nextSteps.isEmpty())
            nextSteps << QStringLiteral("No remaining wizard recommendations were detected after the refreshed analysis.");
        summary += QStringLiteral("\nNext optimization pass\n- %1\n").arg(nextSteps.join(QStringLiteral("\n- ")));
    }
    m_summary->setPlainText(summary);
    updatePostApplyAudit();
    if (m_steps->currentIndex() == kSummaryStep || m_steps->currentIndex() == kAuditStep) updateDetails();
}

void FormatterPage::updatePostApplyAudit()
{
    if (!m_audit) return;
    if (!m_hasAppliedChanges) {
        m_audit->setPlainText(QStringLiteral("Post-Apply Audit\n\nApply an optimization plan to generate the refreshed audit."));
        return;
    }
    if (m_building || !m_planBuilt) {
        m_audit->setPlainText(QStringLiteral("Post-Apply Audit\n\nRefreshing analysis after apply..."));
        return;
    }

    const int remainingUnused = selectableRowCount(m_unused);
    const int remainingDuplicates = m_duplicateMergeEnabled ? selectableRowCount(m_duplicates) : 0;
    const int remainingImportCleanup = selectableRowCount(m_importCleanup);
    const int remainingDeepCleanup = selectableRowCount(m_deepCleanup);
    const int remainingRename = selectableRowCount(m_rename);
    const int remainingCollections = selectableRowCount(m_collection);
    int remainingRedirects = 0;
    int remainingCollectionAdds = 0;
    int remainingCollectionMoves = 0;
    if (m_duplicateMergeEnabled) {
        for (int row = 0; row < m_duplicates->rowCount(); ++row)
            if (isSelectableRow(m_duplicates, row)) remainingRedirects += tableInt(m_duplicates, row, 4);
    }
    for (int row = 0; row < m_collection->rowCount(); ++row) {
        if (!isSelectableRow(m_collection, row)) continue;
        remainingCollectionAdds += tableInt(m_collection, row, 3);
        remainingCollectionMoves += tableInt(m_collection, row, 4);
    }
    const int remainingTotal = remainingUnused + remainingDuplicates + remainingImportCleanup + remainingDeepCleanup + remainingRename + remainingCollections;

    QStringList lines;
    lines << QStringLiteral("Post-Apply Audit")
          << QStringLiteral("================")
          << QString()
          << (remainingTotal == 0
                  ? QStringLiteral("Status: PASS - refreshed analysis has no remaining wizard recommendations.")
                  : QStringLiteral("Status: FOLLOW-UP - refreshed analysis still has optimization candidates."))
          << QString()
          << QStringLiteral("Completed in this optimization session")
          << QStringLiteral("- unused data objects deleted: %1").arg(m_actualUnused)
          << QStringLiteral("- imported files deleted: %1").arg(m_actualImportCleanup)
          << QStringLiteral("- duplicate objects deleted: %1").arg(m_actualDuplicates)
          << QStringLiteral("- references redirected: %1").arg(m_actualRedirected)
          << QStringLiteral("- deep cleanup changes: %1").arg(m_actualDeepCleanup)
          << QStringLiteral("- IDs renamed: %1").arg(m_actualRenamed)
          << QStringLiteral("- collection records added: %1").arg(m_actualCollectionAdded)
          << QStringLiteral("- collection records reorganized: %1").arg(m_actualCollectionReorganized)
          << QString()
          << QStringLiteral("Remaining recommendations after refresh")
          << QStringLiteral("- safe unused data objects: %1").arg(remainingUnused)
          << QStringLiteral("- unused imported files: %1").arg(remainingImportCleanup);
    lines << (m_duplicateMergeEnabled
                  ? QStringLiteral("- duplicate merge candidates: %1, estimated redirects: %2").arg(remainingDuplicates).arg(remainingRedirects)
                  : QStringLiteral("- duplicate merge candidates: skipped because Duplicate Merge is disabled in Settings"));
    lines << QStringLiteral("- deep cleanup candidates after automatic follow-up: %1").arg(remainingDeepCleanup)
          << QStringLiteral("- rename candidates: %1").arg(remainingRename)
          << QStringLiteral("- Data Collection families: %1, records to add: %2, records to reorganize: %3")
                 .arg(remainingCollections)
                 .arg(remainingCollectionAdds)
                 .arg(remainingCollectionMoves)
          << QString();

    if (remainingTotal == 0) {
        lines << QStringLiteral("Recommended next action")
              << QStringLiteral("- Save the optimized copy and test it in StarCraft II Editor.");
    } else {
        lines << QStringLiteral("Recommended next action")
              << QStringLiteral("- Inspect the remaining rows; the wizard already applied safe recommended follow-up cleanup once.");
    }
    m_audit->setPlainText(lines.join(QLatin1Char('\n')));
}
void FormatterPage::setPreview(const QString &text) { m_summary->setPlainText(text); m_steps->setCurrentWidget(m_summary); updateNavigation(); }
void FormatterPage::recordUnusedResult(int value) { m_actualUnused += value; updateSummary(); }
void FormatterPage::recordMergeResult(int removed, int redirected) { m_actualDuplicates += removed; m_actualRedirected += redirected; updateSummary(); }
void FormatterPage::recordImportCleanupResult(int value) { m_actualImportCleanup += value; updateSummary(); }
void FormatterPage::recordDeepCleanupResult(int value) { m_actualDeepCleanup += value; updateSummary(); }
void FormatterPage::recordRenameResult(int value) { m_actualRenamed += value; updateSummary(); }
void FormatterPage::recordCollectionResult(int value, int reorganized) { m_actualCollectionAdded += value; m_actualCollectionReorganized += reorganized; updateSummary(); }
void FormatterPage::setApplyingState(bool applying, const QString &message)
{
    m_applying = applying;
    if (!message.isEmpty()) {
        m_summary->setPlainText(message);
        if (m_steps->currentIndex() != kSummaryStep) m_steps->setCurrentIndex(kSummaryStep);
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
    for (QTableWidget *value : {m_unused, m_duplicates, m_importCleanup, m_deepCleanup, m_rename, m_collection}) value->setRowCount(0);
    m_summary->setPlainText(QStringLiteral("Optimization was applied. Rebuilding preview from updated files..."));
    if (m_steps->currentIndex() != kSummaryStep) m_steps->setCurrentIndex(kSummaryStep);
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
        if (!item || !(item->flags() & Qt::ItemIsUserCheckable) || item->checkState() != Qt::Checked) continue;
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
    const QVector<UnitFamily> collectionFamilies = UnitFamilyDetector().detectCollectionFamilies(m_result, configuredDataCollectionMode());
    for (int row = 0; row < m_unused->rowCount(); ++row) {
        const QTableWidgetItem *item = m_unused->item(row, 0);
        if (!item || !(item->flags() & Qt::ItemIsUserCheckable) || item->checkState() != Qt::Checked) continue;
        result.unused.append(nodeRefFromIndices(item->data(Qt::UserRole).toInt()));
    }
    if (m_duplicateMergeEnabled) for (int row = 0; row < m_duplicates->rowCount(); ++row) {
        const QTableWidgetItem *item = m_duplicates->item(row, 0);
        if (!item || !(item->flags() & Qt::ItemIsUserCheckable) || item->checkState() != Qt::Checked) continue;
        WizardMergeSelection selection;
        selection.keep = nodeRefFromIndices(item->data(Qt::UserRole).toInt());
        selection.remove = nodeRefFromIndices(item->data(Qt::UserRole + 1).toInt());
        result.duplicates.append(selection);
    }
    for (int row = 0; row < m_importCleanup->rowCount(); ++row) {
        const QTableWidgetItem *item = m_importCleanup->item(row, 0);
        if (!item || !(item->flags() & Qt::ItemIsUserCheckable) || item->checkState() != Qt::Checked) continue;
        result.importCleanup.append(item->data(Qt::UserRole).toInt());
    }
    for (int row = 0; row < m_deepCleanup->rowCount(); ++row) {
        const QTableWidgetItem *item = m_deepCleanup->item(row, 0);
        if (!item || !(item->flags() & Qt::ItemIsUserCheckable) || item->checkState() != Qt::Checked) continue;
        result.deepCleanup.append(item->data(Qt::UserRole).toInt());
    }
    for (int row = 0; row < m_rename->rowCount(); ++row) {
        const QTableWidgetItem *item = m_rename->item(row, 0);
        if (!item || !(item->flags() & Qt::ItemIsUserCheckable) || item->checkState() != Qt::Checked) continue;
        WizardRenameSelection selection;
        selection.familyRootId = m_rename->item(row, 1) ? m_rename->item(row, 1)->text() : QString();
        selection.node = nodeRefFromIndices(item->data(Qt::UserRole + 1).toInt());
        result.rename.append(selection);
    }
    for (int row = 0; row < m_collection->rowCount(); ++row) {
        const QTableWidgetItem *item = m_collection->item(row, 0);
        if (!item || !(item->flags() & Qt::ItemIsUserCheckable) || item->checkState() != Qt::Checked) continue;
        const int familyIndex = item->data(Qt::UserRole).toInt();
        if (familyIndex < 0 || familyIndex >= collectionFamilies.size()) continue;
        const UnitFamily &family = collectionFamilies[familyIndex];
        WizardCollectionSelection selection;
        selection.familyRootId = family.rootId;
        for (const UnitFamilyObject &object : family.objects)
            selection.nodes.append(nodeRefFromIndices(object.nodeIndex));
        result.collection.append(selection);
    }
    return result;
}

void FormatterPage::setDuplicateMergeEnabled(bool enabled)
{
    m_duplicateMergeEnabled = enabled;
    if (!enabled && m_steps && m_steps->currentIndex() == kDuplicateStep) m_steps->setCurrentIndex(kImportCleanupStep);
    updateNavigation();
}
