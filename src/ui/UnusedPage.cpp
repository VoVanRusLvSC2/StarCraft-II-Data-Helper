#include "ui/UnusedPage.h"

#include <QAbstractItemView>
#include <QColor>
#include <QFrame>
#include <QHeaderView>
#include <QFileInfo>
#include <QLabel>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTableView>
#include <QVBoxLayout>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QTextEdit>
#include <QHBoxLayout>

namespace
{

    QString usageStateName(UsageState state)
    {
        switch (state)
        {
        case UsageState::Used:
            return QStringLiteral("Used");
        case UsageState::Disconnected:
            return QStringLiteral("Disconnected");
        case UsageState::UnusedSubgraph:
            return QStringLiteral("Unused subgraph");
        case UsageState::Risky:
            return QStringLiteral("Risky");
        case UsageState::Blocked:
            return QStringLiteral("Blocked");
        }
        return QStringLiteral("Blocked");
    }

    QList<QStandardItem *> buildRow(const QString &status,
                                    const QString &id,
                                    const QString &element,
                                    const QString &file,
                                    const QString &location,
                                    const QString &line,
                                    const QString &reason)
    {
        auto *selectItem = new QStandardItem;
        auto *statusItem = new QStandardItem(status);
        auto *idItem = new QStandardItem(id);
        auto *elementItem = new QStandardItem(element);
        auto *fileItem = new QStandardItem(file);
        auto *locationItem = new QStandardItem(location);
        auto *lineItem = new QStandardItem(line);
        auto *reasonItem = new QStandardItem(reason);
        for (QStandardItem *item : {selectItem, statusItem, idItem, elementItem, fileItem, locationItem, lineItem, reasonItem})
        {
            item->setEditable(false);
        }
        return {selectItem, statusItem, idItem, elementItem, fileItem, locationItem, lineItem, reasonItem};
    }

} // namespace

UnusedPage::UnusedPage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto *header = new QFrame(this);
    header->setObjectName(QStringLiteral("bucketCard"));
    auto *headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(12, 12, 12, 12);
    headerLayout->setSpacing(6);

    auto *title = new QLabel(QStringLiteral("Unused Data Objects"), header);
    title->setObjectName(QStringLiteral("panelTitle"));
    headerLayout->addWidget(title);

    m_summaryLabel = new QLabel(QStringLiteral("Candidates will appear here."), header);
    m_summaryLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    m_summaryLabel->setWordWrap(true);
    headerLayout->addWidget(m_summaryLabel);
    layout->addWidget(header);

    m_model = new QStandardItemModel(this);
    m_model->setHorizontalHeaderLabels({QStringLiteral("Select"), QStringLiteral("Status"),
                                        QStringLiteral("ID"),
                                        QStringLiteral("Element"),
                                        QStringLiteral("File"),
                                        QStringLiteral("XML Location"),
                                        QStringLiteral("Line"),
                                        QStringLiteral("Reason")});

    m_table = new QTableView(this);
    m_table->setObjectName(QStringLiteral("cleanupTable"));
    m_table->setModel(m_model);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setSortingEnabled(false);
    m_table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    m_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setMinimumSectionSize(70);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(32);
    m_table->setTextElideMode(Qt::ElideNone);
    m_table->setAutoScroll(true);
    layout->addWidget(m_table, 1);
    auto *buttons = new QHBoxLayout;
    m_previewButton = new QPushButton(QStringLiteral("Preview Deletion"), this);
    m_applyButton = new QPushButton(QStringLiteral("Delete Selected Unused Data Objects"), this);
    buttons->addWidget(m_previewButton);
    buttons->addWidget(m_applyButton);
    buttons->addStretch(1);
    layout->addLayout(buttons);
    m_preview = new QTextEdit(this);
    m_preview->setReadOnly(true);
    m_preview->setMaximumHeight(180);
    m_preview->setPlaceholderText(QStringLiteral("Preview selected unused data objects before deletion."));
    layout->addWidget(m_preview);
    connect(m_previewButton, &QPushButton::clicked, this, [this]
            { emit previewDeletionRequested(selectedSafeRows()); });
    connect(m_applyButton, &QPushButton::clicked, this, [this]
            { emit applyDeletionRequested(selectedSafeRows()); });
    connect(m_model, &QStandardItemModel::itemChanged, this, [this]
            { updateActionState(); });
    updateActionState();
}

void UnusedPage::setAnalysisResult(const AnalysisResult &result)
{
    const QSignalBlocker blocker(m_model);
    m_result = result;
    m_model->removeRows(0, m_model->rowCount());

    int count = 0;
    for (const UnusedCandidateInfo &candidate : m_result.unusedCandidates)
        if (candidate.state == CandidateState::Safe
            && (candidate.usageState == UsageState::Disconnected
                || candidate.usageState == UsageState::UnusedSubgraph))
            ++count;
    if (count == 0)
    {
        m_summaryLabel->setText(QStringLiteral("No cleanup candidates were detected."));
        m_model->appendRow(buildRow(QStringLiteral("PASS"),
                                    QStringLiteral("-"),
                                    QStringLiteral("-"),
                                    QStringLiteral("-"),
                                    QStringLiteral("-"),
                                    QStringLiteral("-"),
                                    QStringLiteral("No unused candidates")));
    }
    else
    {
        m_summaryLabel->setText(QStringLiteral("%1 safe unreachable data object(s) can be removed after manual confirmation.").arg(count));
        for (const UnusedCandidateInfo &candidate : m_result.unusedCandidates)
        {
            if (candidate.state != CandidateState::Safe
                || (candidate.usageState != UsageState::Disconnected
                    && candidate.usageState != UsageState::UnusedSubgraph))
                continue;
            const int index = candidate.nodeIndex;
            if (index < 0 || index >= m_result.nodes.size())
            {
                continue;
            }
            const DataNode &node = m_result.nodes[index];
            const QString status = usageStateName(candidate.usageState);
            const QString reason = QStringLiteral("%1 | path: %2 | incoming XML: %3 | outgoing XML: %4 | external: %5 | collections: %6 | whitelist/protected: %7/%8 | risk: %9")
                                       .arg(candidate.reason)
                                       .arg(candidate.usagePath.join(QStringLiteral(" -> ")))
                                       .arg(candidate.incomingXmlSources.join(QStringLiteral(", ")))
                                       .arg(candidate.outgoingXmlTargets.join(QStringLiteral(", ")))
                                       .arg(candidate.externalReferenceSources.join(QStringLiteral(", ")))
                                       .arg(candidate.dataCollectionMemberships.join(QStringLiteral(", ")))
                                       .arg(candidate.whitelisted ? QStringLiteral("yes") : QStringLiteral("no"))
                                       .arg(candidate.protectedObject ? QStringLiteral("yes") : QStringLiteral("no"))
                                       .arg(candidate.riskLevel);
            QList<QStandardItem *> row = buildRow(status,
                                                  node.id,
                                                  node.elementName,
                                                  QFileInfo(node.sourceFile).fileName(),
                                                  node.originalLocation,
                                                  node.lineNumber > 0 ? QString::number(node.lineNumber) : QStringLiteral("N/A"),
                                                  reason);
            row.first()->setData(index, Qt::UserRole + 1);
            row.first()->setCheckable(true);
            for (QStandardItem *item : row)
            {
                item->setBackground(QColor(QStringLiteral("#4a241f")));
                item->setForeground(QColor(QStringLiteral("#ffe9dc")));
            }
            m_model->appendRow(row);
        }
    }

    m_table->resizeColumnsToContents();
    m_preview->clear();
    updateActionState();
}

QVector<int> UnusedPage::selectedSafeRows() const
{
    QVector<int> rows;
    for (int row = 0; row < m_model->rowCount(); ++row)
    {
        QStandardItem *item = m_model->item(row, 0);
        if (item && item->isCheckable() && item->checkState() == Qt::Checked)
            rows << item->data(Qt::UserRole + 1).toInt();
    }
    return rows;
}

void UnusedPage::selectRows(const QVector<int> &rows)
{
    const QSignalBlocker blocker(m_model);
    const QSet<int> selected(rows.cbegin(), rows.cend());
    for (int row = 0; row < m_model->rowCount(); ++row)
    {
        QStandardItem *item = m_model->item(row, 0);
        if (item && item->isCheckable())
            item->setCheckState(selected.contains(item->data(Qt::UserRole + 1).toInt()) ? Qt::Checked : Qt::Unchecked);
    }
    updateActionState();
}

void UnusedPage::setPreviewText(const QString &text) { m_preview->setPlainText(text); }

void UnusedPage::updateActionState()
{
    const int count = selectedSafeRows().size();
    const bool available = count > 0;
    m_previewButton->setEnabled(available);
    m_applyButton->setEnabled(available);
    const QString hint = available
                             ? QStringLiteral("%1 safe data object(s) selected").arg(count)
                             : QStringLiteral("Select at least one Safe candidate first");
    m_previewButton->setToolTip(hint);
    m_applyButton->setToolTip(hint);
}
