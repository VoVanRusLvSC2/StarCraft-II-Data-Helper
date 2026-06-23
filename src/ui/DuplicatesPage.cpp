#include "ui/DuplicatesPage.h"

#include <QAbstractItemView>
#include <QColor>
#include <QFrame>
#include <QHeaderView>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>

namespace {

QString duplicateContentLabel(const DuplicateContentGroup &group)
{
    return group.mergeCandidate
        ? QStringLiteral("Same %1 body and related ID mask %2").arg(group.elementName, group.commonIdMask)
        : QStringLiteral("Same %1 body, but IDs are unrelated: allowed, no merge suggested").arg(group.elementName);
}

QList<QStandardItem *> buildRow(const QString &kind,
                                const QString &key,
                                const QString &id,
                                const QString &element,
                                const QString &file,
                                const QString &location,
                                const QString &line,
                                const QString &notes,
                                bool selectable = false)
{
    auto *kindItem = new QStandardItem(kind);
    auto *keyItem = new QStandardItem(key);
    auto *idItem = new QStandardItem(id);
    auto *elementItem = new QStandardItem(element);
    auto *fileItem = new QStandardItem(file);
    auto *locationItem = new QStandardItem(location);
    auto *lineItem = new QStandardItem(line);
    auto *notesItem = new QStandardItem(notes);
    for (QStandardItem *item : {kindItem, keyItem, idItem, elementItem, fileItem, locationItem, lineItem, notesItem}) {
        item->setEditable(false);
    }
    if (selectable) {
        kindItem->setCheckable(true);
        keyItem->setCheckable(true);
        kindItem->setToolTip(QStringLiteral("Check to keep this object"));
        keyItem->setToolTip(QStringLiteral("Check to remove this duplicate"));
    }
    return {kindItem, keyItem, idItem, elementItem, fileItem, locationItem, lineItem, notesItem};
}

} // namespace

DuplicatesPage::DuplicatesPage(QWidget *parent)
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

    auto *title = new QLabel(QStringLiteral("Duplicates"), header);
    title->setObjectName(QStringLiteral("panelTitle"));
    headerLayout->addWidget(title);

    m_summaryLabel = new QLabel(QStringLiteral("Duplicate groups will appear here."), header);
    m_summaryLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    m_summaryLabel->setWordWrap(true);
    headerLayout->addWidget(m_summaryLabel);
    layout->addWidget(header);

    m_model = new QStandardItemModel(this);
    m_model->setHorizontalHeaderLabels({QStringLiteral("Keep"),
                                        QStringLiteral("Remove / ID Mask"),
                                        QStringLiteral("ID"),
                                        QStringLiteral("Element"),
                                        QStringLiteral("File"),
                                        QStringLiteral("XML Location"),
                                        QStringLiteral("Line"),
                                        QStringLiteral("Notes")});

    m_tree = new QTreeView(this);
    m_tree->setObjectName(QStringLiteral("duplicatesTree"));
    m_tree->setModel(m_model);
    m_tree->setAlternatingRowColors(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setItemsExpandable(true);
    m_tree->setExpandsOnDoubleClick(true);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    m_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_tree->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    m_tree->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_tree->setAutoScroll(true);
    m_tree->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setMinimumSectionSize(70);
    m_tree->setTextElideMode(Qt::ElideNone);
    m_tree->setUniformRowHeights(false);
    layout->addWidget(m_tree, 1);

    auto *buttons = new QHBoxLayout;
    m_previewButton = new QPushButton(QStringLiteral("Preview Merge"), this);
    m_applyButton = new QPushButton(QStringLiteral("Apply Merge"), this);
    m_applyButton->setEnabled(false);
    buttons->addWidget(m_previewButton);
    buttons->addWidget(m_applyButton);
    buttons->addStretch(1);
    layout->addLayout(buttons);
    m_preview = new QTextEdit(this);
    m_preview->setReadOnly(true);
    m_preview->setMaximumHeight(190);
    m_preview->setPlaceholderText(QStringLiteral("Preview is mandatory before apply."));
    layout->addWidget(m_preview);
    connect(m_previewButton, &QPushButton::clicked, this, &DuplicatesPage::previewSelectedMerge);
    connect(m_applyButton, &QPushButton::clicked, this, &DuplicatesPage::applySelectedMerge);
    connect(m_tree, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        const QModelIndex firstColumn = index.siblingAtColumn(0);
        const QVariant value = firstColumn.data(Qt::UserRole + 2);
        if (value.isValid()) emit sourceRequested(value.toInt());
    });
    connect(m_model, &QStandardItemModel::itemChanged, this, [this](QStandardItem *item) {
        m_applyButton->setEnabled(false);
        if (item && item->checkState() == Qt::Checked && (item->column() == 0 || item->column() == 1)) {
            QStandardItem *group = item->parent();
            if (group) {
                const QSignalBlocker blocker(m_model);
                if (item->column() == 0) {
                    for (int row = 0; row < group->rowCount(); ++row) {
                        QStandardItem *otherKeep = group->child(row, 0);
                        if (otherKeep != item) otherKeep->setCheckState(Qt::Unchecked);
                    }
                    group->child(item->row(), 1)->setCheckState(Qt::Unchecked);
                } else {
                    group->child(item->row(), 0)->setCheckState(Qt::Unchecked);
                }
            }
        }
        const MergeRequest request = selectedRequest();
        m_previewButton->setEnabled(request.keepNodeIndex >= 0 && !request.removeNodeIndices.isEmpty());
    });
}

void DuplicatesPage::setAnalysisResult(const AnalysisResult &result)
{
    m_result = result;
    m_model->removeRows(0, m_model->rowCount());

    int groupCount = 0;
    int mergeCandidateCount = 0;
    int allowedCount = 0;
    int nodeCount = 0;

    for (const DuplicateContentGroup &group : m_result.duplicateContentGroups) {
        ++groupCount;
        if (group.mergeCandidate) ++mergeCandidateCount; else ++allowedCount;
        const QString kind = group.mergeCandidate ? QStringLiteral("Merge candidate") : QStringLiteral("Allowed identical body");
        const QString key = group.commonIdMask;
        QList<QStandardItem *> groupRows = buildRow(kind,
                                                    key,
                                                    group.elementName,
                                                    QStringLiteral(""),
                                                    QStringLiteral(""),
                                                    QStringLiteral(""),
                                                    QString::number(group.nodeIndices.size()),
                                                    duplicateContentLabel(group));
        groupRows.first()->setData(true, Qt::UserRole + 1);
        for (QStandardItem *item : groupRows) {
            item->setBackground(QColor(QStringLiteral("#3b3420")));
            item->setForeground(QColor(QStringLiteral("#fff6d8")));
        }

        for (int index : group.nodeIndices) {
            if (index < 0 || index >= m_result.nodes.size()) {
                continue;
            }
            const DataNode &node = m_result.nodes[index];
            ++nodeCount;
            int incoming = 0;
            for (const DataNode &source : m_result.nodes)
                if (source.referencedIds.contains(node.id)) ++incoming;
            QList<QStandardItem *> childRows = buildRow(QStringLiteral("Node"),
                                                        key,
                                                        node.id,
                                                        node.elementName,
                                                        QFileInfo(node.sourceFile).fileName(),
                                                        node.originalLocation,
                                                        node.lineNumber > 0 ? QString::number(node.lineNumber) : QStringLiteral("N/A"),
                                                        group.mergeCandidate
                                                            ? QStringLiteral("References to %1: %2 | redirectable: yes | risk: %3")
                                                                  .arg(node.id).arg(incoming)
                                                                  .arg(incoming > 100 ? QStringLiteral("medium") : QStringLiteral("low"))
                                                            : QStringLiteral("No warning: body reuse with unrelated ID is allowed"),
                                                        group.mergeCandidate);
            childRows.first()->setData(index, Qt::UserRole + 2);
            childRows.first()->setData(groupCount - 1, Qt::UserRole + 3);
            childRows.at(1)->setData(index, Qt::UserRole + 2);
            childRows.at(1)->setData(groupCount - 1, Qt::UserRole + 3);
            if (group.mergeCandidate) {
                if (groupRows.first()->rowCount() == 0) childRows.first()->setCheckState(Qt::Checked);
                else childRows.at(1)->setCheckState(Qt::Checked);
            }
            groupRows.first()->appendRow(childRows);
        }
        m_model->appendRow(groupRows);
    }

    if (groupCount == 0) {
        m_summaryLabel->setText(QStringLiteral("No duplicates detected."));
        m_model->appendRow(buildRow(QStringLiteral("PASS"),
                                    QStringLiteral("No duplicates"),
                                    QStringLiteral(""),
                                    QStringLiteral(""),
                                    QStringLiteral(""),
                                    QStringLiteral(""),
                                    QStringLiteral("0"),
                                    QStringLiteral("Everything is unique so far.")));
    } else {
        m_summaryLabel->setText(QStringLiteral("%1 merge candidates | %2 allowed identical-body groups | %3 nodes | Double-click an object to open full XML.")
                                    .arg(mergeCandidateCount).arg(allowedCount).arg(nodeCount));
    }

    m_tree->expandAll();
    m_tree->resizeColumnToContents(0);
    m_tree->resizeColumnToContents(1);
    m_tree->resizeColumnToContents(2);
    m_tree->resizeColumnToContents(3);
    m_tree->resizeColumnToContents(4);
    m_tree->resizeColumnToContents(5);
    m_tree->resizeColumnToContents(6);
    m_tree->resizeColumnToContents(7);
    m_applyButton->setEnabled(false);
    const MergeRequest request = selectedRequest();
    m_previewButton->setEnabled(request.keepNodeIndex >= 0 && !request.removeNodeIndices.isEmpty());
    m_preview->clear();
}

MergeRequest DuplicatesPage::selectedRequest() const
{
    MergeRequest request;
    for (int groupRow = 0; groupRow < m_model->rowCount(); ++groupRow) {
        QStandardItem *group = m_model->item(groupRow, 0);
        if (!group) continue;
        MergeRequest candidate;
        for (int row = 0; row < group->rowCount(); ++row) {
            QStandardItem *keep = group->child(row, 0);
            QStandardItem *remove = group->child(row, 1);
            if (keep && keep->checkState() == Qt::Checked) candidate.keepNodeIndex = keep->data(Qt::UserRole + 2).toInt();
            if (remove && remove->checkState() == Qt::Checked) candidate.removeNodeIndices << remove->data(Qt::UserRole + 2).toInt();
        }
        if (candidate.keepNodeIndex >= 0 && !candidate.removeNodeIndices.isEmpty()) return candidate;
    }
    return request;
}

void DuplicatesPage::previewSelectedMerge() { emit previewMergeRequested(selectedRequest()); }
void DuplicatesPage::applySelectedMerge() { emit applyMergeRequested(selectedRequest()); }
void DuplicatesPage::setPreviewText(const QString &text, bool canApply)
{
    m_preview->setPlainText(text);
    m_applyButton->setEnabled(canApply);
}

void DuplicatesPage::selectRequest(const MergeRequest &request)
{
    const QSignalBlocker blocker(m_model);
    for (int groupRow = 0; groupRow < m_model->rowCount(); ++groupRow) {
        QStandardItem *group = m_model->item(groupRow, 0);
        if (!group) continue;
        for (int row = 0; row < group->rowCount(); ++row) {
            QStandardItem *keep = group->child(row, 0); QStandardItem *remove = group->child(row, 1);
            const int nodeIndex = keep ? keep->data(Qt::UserRole + 2).toInt() : -1;
            if (keep && keep->isCheckable()) keep->setCheckState(nodeIndex == request.keepNodeIndex ? Qt::Checked : Qt::Unchecked);
            if (remove && remove->isCheckable()) remove->setCheckState(request.removeNodeIndices.contains(nodeIndex) ? Qt::Checked : Qt::Unchecked);
        }
    }
    m_applyButton->setEnabled(false);
    const MergeRequest selected = selectedRequest();
    m_previewButton->setEnabled(selected.keepNodeIndex >= 0 && !selected.removeNodeIndices.isEmpty());
}
