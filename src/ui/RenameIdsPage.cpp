#include "ui/RenameIdsPage.h"

#include "core/StandardNamePlanner.h"
#include "core/UnitFamilyDetector.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QFileInfo>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTextEdit>
#include <QVBoxLayout>

namespace {

void addRow(QTableWidget *table, const QStringList &values, int nodeIndex = -1, bool checkable = false)
{
    const int row = table->rowCount();
    table->insertRow(row);
    for (int column = 0; column < table->columnCount(); ++column) {
        auto *item = new QTableWidgetItem(column < values.size() ? values[column] : QString());
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        if (column == 0 && checkable) {
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Checked);
            item->setData(Qt::UserRole, nodeIndex);
        }
        table->setItem(row, column, item);
    }
}

} // namespace

RenameIdsPage::RenameIdsPage(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    auto *header = new QFrame(this);
    header->setObjectName(QStringLiteral("bucketCard"));
    auto *headerLayout = new QVBoxLayout(header);
    auto *title = new QLabel(QStringLiteral("Rename To Standard"), header);
    title->setObjectName(QStringLiteral("panelTitle"));
    headerLayout->addWidget(title);
    m_summary = new QLabel(QStringLiteral("Detect a unit family, review the plan, then preview before apply."), header);
    m_summary->setWordWrap(true);
    m_summary->setObjectName(QStringLiteral("inspectorSubtitle"));
    headerLayout->addWidget(m_summary);
    auto *selectorRow = new QHBoxLayout;
    selectorRow->addWidget(new QLabel(QStringLiteral("Unit family:"), header));
    m_familySelector = new QComboBox(header);
    selectorRow->addWidget(m_familySelector, 1);
    m_rootLabel = new QLabel(QStringLiteral("Detected root: -"), header);
    selectorRow->addWidget(m_rootLabel);
    selectorRow->addWidget(new QLabel(QStringLiteral("Target root ID:"), header));
    m_targetRoot = new QLineEdit(header);
    m_targetRoot->setMinimumWidth(180);
    selectorRow->addWidget(m_targetRoot);
    headerLayout->addLayout(selectorRow);
    layout->addWidget(header);

    m_tabs = new QTabWidget(this);
    const QStringList columns{QStringLiteral("Include"), QStringLiteral("Type"), QStringLiteral("Role"),
        QStringLiteral("Current ID"), QStringLiteral("Proposed ID"), QStringLiteral("Confidence"),
        QStringLiteral("Risk"), QStringLiteral("Reason / Conflict")};
    m_detected = makeTable(columns);
    m_nonStandard = makeTable(columns);
    m_manual = makeTable(columns);
    m_preview = makeTable(columns);
    m_tabs->addTab(m_detected, QStringLiteral("Detected Family Objects"));
    m_tabs->addTab(m_nonStandard, QStringLiteral("Non-standard Objects"));
    m_tabs->addTab(m_manual, QStringLiteral("Manual Review"));
    m_tabs->addTab(m_preview, QStringLiteral("Rename Preview"));
    layout->addWidget(m_tabs, 1);
    m_details = new QTextEdit(this);
    m_details->setReadOnly(true);
    m_details->setMaximumHeight(190);
    m_details->setPlaceholderText(QStringLiteral("Reference update preview, warnings, and conflicts appear here."));
    layout->addWidget(m_details);
    auto *buttons = new QHBoxLayout;
    auto *detect = new QPushButton(QStringLiteral("Detect Unit Families"), this);
    auto *preview = new QPushButton(QStringLiteral("Preview Rename"), this);
    m_apply = new QPushButton(QStringLiteral("Apply Rename"), this);
    auto *exportButton = new QPushButton(QStringLiteral("Export Rename Report"), this);
    m_apply->setEnabled(false);
    buttons->addWidget(detect); buttons->addWidget(preview); buttons->addWidget(m_apply); buttons->addWidget(exportButton); buttons->addStretch(1);
    layout->addLayout(buttons);
    connect(detect, &QPushButton::clicked, this, &RenameIdsPage::detectFamilies);
    connect(m_familySelector, &QComboBox::currentIndexChanged, this, &RenameIdsPage::rebuildFamilyTables);
    connect(m_targetRoot, &QLineEdit::textChanged, this, [this] { m_apply->setEnabled(false); rebuildFamilyTables(); });
    connect(m_detected, &QTableWidget::itemChanged, this, [this] { m_apply->setEnabled(false); });
    connect(preview, &QPushButton::clicked, this, [this] { emit previewRequested(currentPlan()); });
    connect(m_apply, &QPushButton::clicked, this, [this] { emit applyRequested(currentPlan()); });
    connect(exportButton, &QPushButton::clicked, this, [this] { emit exportRequested(m_previewReport.reportText); });
}

QTableWidget *RenameIdsPage::makeTable(const QStringList &headers) const
{
    auto *table = new QTableWidget;
    table->setColumnCount(headers.size());
    table->setHorizontalHeaderLabels(headers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setTextElideMode(Qt::ElideNone);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setVisible(false);
    return table;
}

void RenameIdsPage::setAnalysisResult(const AnalysisResult &result)
{
    m_result = result;
    m_families.clear();
    m_familySelector->clear();
    for (QTableWidget *table : {m_detected, m_nonStandard, m_manual, m_preview}) table->setRowCount(0);
    m_details->clear();
    m_apply->setEnabled(false);
    m_summary->setText(result.nodes.isEmpty() ? QStringLiteral("No analysis loaded.")
                                               : QStringLiteral("%1 objects loaded. Click Detect Unit Families.").arg(result.nodes.size()));
}

void RenameIdsPage::detectFamilies()
{
    UnitFamilyDetector detector;
    m_families = detector.detect(m_result);
    const QSignalBlocker blocker(m_familySelector);
    m_familySelector->clear();
    for (const UnitFamily &family : m_families)
        m_familySelector->addItem(QStringLiteral("%1 (%2 objects)").arg(family.rootId).arg(family.objects.size()));
    if (!m_families.isEmpty()) m_familySelector->setCurrentIndex(0);
    rebuildFamilyTables();
    m_summary->setText(QStringLiteral("%1 unit families detected. No files were modified.").arg(m_families.size()));
}

QSet<int> RenameIdsPage::includedRows() const
{
    QSet<int> included;
    for (int row = 0; row < m_detected->rowCount(); ++row) {
        QTableWidgetItem *item = m_detected->item(row, 0);
        if (item && item->checkState() == Qt::Checked) included.insert(item->data(Qt::UserRole).toInt());
    }
    if (included.isEmpty() && m_detected->rowCount() > 0) included.insert(-1); // explicit "include none"
    return included;
}

RenamePlan RenameIdsPage::currentPlan() const
{
    if (m_familySelector->currentIndex() < 0 || m_familySelector->currentIndex() >= m_families.size()) return {};
    StandardNamePlanner planner;
    return planner.plan(m_result, m_families[m_familySelector->currentIndex()], m_targetRoot->text(), includedRows());
}

void RenameIdsPage::rebuildFamilyTables()
{
    for (QTableWidget *table : {m_detected, m_nonStandard, m_manual}) table->setRowCount(0);
    const int selected = m_familySelector->currentIndex();
    if (selected < 0 || selected >= m_families.size()) { m_rootLabel->setText(QStringLiteral("Detected root: -")); return; }
    const UnitFamily &family = m_families[selected];
    m_rootLabel->setText(QStringLiteral("Detected root: %1").arg(family.rootId));
    if (!m_targetRoot->hasFocus() || m_targetRoot->text().isEmpty()) {
        const QSignalBlocker blocker(m_targetRoot);
        m_targetRoot->setText(family.rootId);
    }
    StandardNamePlanner planner;
    const RenamePlan plan = planner.plan(m_result, family, m_targetRoot->text());
    QHash<int, RenamePlanItem> planned;
    for (const RenamePlanItem &item : plan.items) planned.insert(item.nodeIndex, item);
    for (const UnitFamilyObject &object : family.objects) {
        const DataNode &node = m_result.nodes[object.nodeIndex];
        const RenamePlanItem item = planned.value(object.nodeIndex);
        const QStringList values{QString(), node.elementName, unitFamilyRoleName(object.role), node.id, item.newId,
            object.confidence, item.riskLevel, item.blocked ? item.conflict : object.reason};
        addRow(m_detected, values, object.nodeIndex, true);
        if (object.role == UnitFamilyRole::ManualReview || object.role == UnitFamilyRole::Other)
            addRow(m_manual, values, object.nodeIndex, true);
        else if (!item.newId.isEmpty()) addRow(m_nonStandard, values, object.nodeIndex, false);
    }
    m_tabs->setTabText(0, QStringLiteral("Detected Family Objects (%1)").arg(m_detected->rowCount()));
    m_tabs->setTabText(1, QStringLiteral("Non-standard Objects (%1)").arg(m_nonStandard->rowCount()));
    m_tabs->setTabText(2, QStringLiteral("Manual Review (%1)").arg(m_manual->rowCount()));
    for (QTableWidget *table : {m_detected, m_nonStandard, m_manual}) table->resizeColumnsToContents();
}

void RenameIdsPage::setPreviewReport(const RenamePreviewReport &report)
{
    m_previewReport = report;
    m_preview->setRowCount(0);
    for (const RenamePlanItem &item : report.plan.items) {
        const DataNode &node = m_result.nodes[item.nodeIndex];
        addRow(m_preview, {QString(), node.elementName, unitFamilyRoleName(item.role), item.oldId, item.newId,
                           item.confidence, item.riskLevel, item.blocked ? item.conflict : item.reason});
    }
    m_preview->resizeColumnsToContents();
    m_details->setPlainText(report.reportText);
    m_apply->setEnabled(report.valid);
    m_tabs->setCurrentWidget(m_preview);
}

void RenameIdsPage::setApplyAvailable(bool available)
{
    if (!available) m_apply->setEnabled(false);
}
