#include "ui/OverviewPage.h"

#include <QAbstractItemView>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QFrame>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QPainter>
#include <QSet>
#include <QSplitter>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QStyle>
#include <QTableView>
#include <QTreeView>
#include <QVBoxLayout>
#include <QVariant>

namespace {

class NoFocusItemDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem cleanOption(option);
        cleanOption.state &= ~QStyle::State_HasFocus;
        QStyledItemDelegate::paint(painter, cleanOption, index);
    }
};

QString buildSummaryText(const AnalysisResult &result)
{
    int mergeCandidates = 0;
    int allowedBodies = 0;
    for (const DuplicateContentGroup &group : result.duplicateContentGroups)
        group.mergeCandidate ? ++mergeCandidates : ++allowedBodies;
    return QStringLiteral("%1 files scanned | %2 XML files | %3 objects | %4 merge candidates | %5 allowed body matches | %6 cleanup candidates")
        .arg(result.totalFilesScanned())
        .arg(result.totalXmlFiles())
        .arg(result.totalDataNodes())
        .arg(mergeCandidates)
        .arg(allowedBodies)
        .arg(result.possibleUnusedNodeIndices.size());
}

QString archiveFileLabel(const QString &path)
{
    if (path.isEmpty()) {
        return QStringLiteral("Unknown file");
    }
    return QFileInfo(path).fileName();
}

}

OverviewPage::OverviewPage(QWidget *parent)
    : QWidget(parent)
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(8);

    auto *toolbar = new QFrame(this);
    toolbar->setObjectName(QStringLiteral("pageToolbar"));
    auto *toolbarLayout = new QVBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(12, 10, 12, 10);
    toolbarLayout->setSpacing(8);

    auto *topRow = new QHBoxLayout();
    topRow->setSpacing(8);

    m_folderEdit = new QLineEdit(toolbar);
    m_folderEdit->setPlaceholderText(QStringLiteral("Paste path here, then use the top toolbar to analyze"));
    topRow->addWidget(m_folderEdit, 1);

    m_showAllFilesButton = new QPushButton(QStringLiteral("Show All XML Files"), toolbar);
    topRow->addWidget(m_showAllFilesButton, 0);

    toolbarLayout->addLayout(topRow);

    auto *metaRow = new QHBoxLayout();
    metaRow->setSpacing(8);
    m_modeLabel = new QLabel(QStringLiteral("Mode: waiting for analysis"), toolbar);
    m_modeLabel->setObjectName(QStringLiteral("modeBadge"));
    m_summaryLabel = new QLabel(QStringLiteral("No analysis yet"), toolbar);
    m_summaryLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    metaRow->addWidget(m_modeLabel, 0);
    metaRow->addWidget(m_summaryLabel, 1);
    toolbarLayout->addLayout(metaRow);
    rootLayout->addWidget(toolbar);

    auto *mainSplitter = new QSplitter(Qt::Horizontal, this);
    mainSplitter->setObjectName(QStringLiteral("analysisSplitter"));

    m_fileTree = new QTreeView(mainSplitter);
    m_fileTree->setObjectName(QStringLiteral("fileTree"));
    m_fileTree->header()->hide();
    m_fileTree->setAlternatingRowColors(false);
    m_fileTree->setUniformRowHeights(true);
    m_fileTree->setTextElideMode(Qt::ElideNone);
    m_fileModel = new QStandardItemModel(this);
    m_fileTree->setModel(m_fileModel);
    auto *noFocusDelegate = new NoFocusItemDelegate(this);
    m_fileTree->setItemDelegate(noFocusDelegate);

    auto *centerSplitter = new QSplitter(Qt::Horizontal, mainSplitter);
    centerSplitter->setObjectName(QStringLiteral("centerSplitter"));

    auto *tablePane = new QFrame(centerSplitter);
    tablePane->setObjectName(QStringLiteral("tablePane"));
    auto *tableLayout = new QVBoxLayout(tablePane);
    tableLayout->setContentsMargins(0, 0, 0, 0);
    tableLayout->setSpacing(8);

    m_filterEdit = new QLineEdit(tablePane);
    m_filterEdit->setPlaceholderText(QStringLiteral("Filter rows"));

    m_fileSummaryTable = new QTableView(tablePane);
    m_fileSummaryTable->setObjectName(QStringLiteral("archiveFileTable"));
    m_fileSummaryTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_fileSummaryTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_fileSummaryTable->setSortingEnabled(false);
    m_fileSummaryTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_fileSummaryTable->horizontalHeader()->setStretchLastSection(true);
    m_fileSummaryTable->horizontalHeader()->setSectionsMovable(true);
    m_fileSummaryTable->horizontalHeader()->setMinimumSectionSize(60);
    m_fileSummaryTable->verticalHeader()->setVisible(false);
    m_fileSummaryTable->verticalHeader()->setDefaultSectionSize(32);
    m_fileSummaryTable->setAlternatingRowColors(false);
    m_fileSummaryTable->setMinimumHeight(230);
    m_fileSummaryModel = new QStandardItemModel(this);
    m_fileSummaryTable->setModel(m_fileSummaryModel);
    m_fileSummaryTable->setItemDelegate(noFocusDelegate);
    tableLayout->addWidget(m_fileSummaryTable, 0);
    tableLayout->addWidget(m_filterEdit);

    m_model = new ObjectTableModel(this);
    m_proxy = new ObjectFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);

    m_objectTable = new QTableView(tablePane);
    m_objectTable->setObjectName(QStringLiteral("objectTable"));
    m_objectTable->setModel(m_proxy);
    m_objectTable->setItemDelegate(noFocusDelegate);
    m_objectTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_objectTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_objectTable->setSortingEnabled(false);
    m_objectTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_objectTable->horizontalHeader()->setStretchLastSection(false);
    m_objectTable->horizontalHeader()->setSectionsMovable(true);
    m_objectTable->horizontalHeader()->setMinimumSectionSize(60);
    m_objectTable->verticalHeader()->setVisible(false);
    m_objectTable->verticalHeader()->setDefaultSectionSize(32);
    m_objectTable->setAlternatingRowColors(true);
    m_objectTable->setTextElideMode(Qt::ElideNone);
    tableLayout->addWidget(m_objectTable, 1);

    m_inspector = new ObjectInspectorWidget(centerSplitter);
    m_inspector->setObjectName(QStringLiteral("inspectorPane"));

    centerSplitter->addWidget(tablePane);
    centerSplitter->addWidget(m_inspector);
    centerSplitter->setStretchFactor(0, 3);
    centerSplitter->setStretchFactor(1, 2);
    centerSplitter->setSizes({860, 560});

    mainSplitter->addWidget(m_fileTree);
    mainSplitter->addWidget(centerSplitter);
    mainSplitter->setStretchFactor(0, 1);
    mainSplitter->setStretchFactor(1, 4);
    mainSplitter->setSizes({360, 1320});
    m_fileTree->setMinimumWidth(320);
    rootLayout->addWidget(mainSplitter, 1);

    m_reportView = new QPlainTextEdit(this);
    m_reportView->setReadOnly(true);
    m_reportView->setObjectName(QStringLiteral("reportView"));
    m_reportView->setPlaceholderText(QStringLiteral("Analysis output and warnings appear here."));
    rootLayout->addWidget(m_reportView, 0);

    connect(m_fileTree->selectionModel(), &QItemSelectionModel::currentChanged,
            this, &OverviewPage::handleFileTreeSelection);
    connect(m_fileSummaryTable->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex &current, const QModelIndex &) {
                const QString filePath = current.data(Qt::UserRole + 1).toString();
                if (!filePath.isEmpty()) {
                    setSelectedFileFilter(filePath);
                }
            });
    connect(m_objectTable->selectionModel(), &QItemSelectionModel::currentChanged,
            this, &OverviewPage::handleTableSelection);
    connect(m_filterEdit, &QLineEdit::textChanged, this, &OverviewPage::applyFilter);
    connect(m_showAllFilesButton, &QPushButton::clicked, this, &OverviewPage::showAllFileRows);
    connect(m_objectTable, &QTableView::doubleClicked, this, [this](const QModelIndex &proxyIndex) {
        const QModelIndex sourceIndex = m_proxy->mapToSource(proxyIndex);
        if (sourceIndex.isValid()) {
            emit objectDoubleClicked(sourceIndex.row());
        }
    });

    setModeLabel(QStringLiteral("Mode: waiting for analysis"));
    setOutputText(QStringLiteral("Load a folder, file or archive to see the analysis report."));
    m_inspector->clearSelection();
}

void OverviewPage::setFolderPath(const QString &folderPath)
{
    m_folderPath = folderPath;
    m_folderEdit->setText(folderPath);
    emit folderPathChanged(folderPath);
}

QString OverviewPage::folderPath() const
{
    return m_folderEdit->text().trimmed();
}

void OverviewPage::setModeLabel(const QString &modeText)
{
    if (m_modeLabel) {
        m_modeLabel->setText(modeText);
    }
}

void OverviewPage::setOutputText(const QString &text)
{
    if (m_reportView) {
        m_reportView->setPlainText(text);
    }
}

void OverviewPage::setAnalysisResult(const AnalysisResult &result)
{
    m_result = result;
    m_model->setNodes(m_result.nodes);
    m_fileSummaryModel->clear();
    m_fileSummaryModel->setHorizontalHeaderLabels({QStringLiteral("XML File"),
                                                   QStringLiteral("Objects"),
                                                   QStringLiteral("Status")});

    QSet<int> duplicateRows;
    for (const DuplicateIdGroup &group : m_result.duplicateIdGroups) {
        for (int index : group.nodeIndices) {
            duplicateRows.insert(index);
        }
    }
    m_model->setDuplicateIndices(duplicateRows);

    rebuildFileTree();
    rebuildFileSummary();
    setOutputText(m_result.analysisReportText);
    m_summaryLabel->setText(buildSummaryText(m_result));
    m_inspector->setAnalysisResult(m_result);
    selectFirstFileItem();
    m_objectTable->resizeColumnsToContents();
    if (m_fileSummaryTable) {
        m_fileSummaryTable->resizeColumnsToContents();
    }
}

QVector<int> OverviewPage::selectedRows() const
{
    QVector<int> rows;
    const QItemSelectionModel *selection = m_objectTable->selectionModel();
    if (!selection) {
        return rows;
    }
    const QModelIndexList selected = selection->selectedRows();
    rows.reserve(selected.size());
    for (const QModelIndex &proxyIndex : selected) {
        const QModelIndex sourceIndex = m_proxy->mapToSource(proxyIndex);
        if (sourceIndex.isValid()) {
            rows.append(sourceIndex.row());
        }
    }
    return rows;
}

QVector<DataNode> OverviewPage::selectedNodes() const
{
    QVector<DataNode> nodes;
    const QVector<int> rows = selectedRows();
    nodes.reserve(rows.size());
    for (int row : rows) {
        if (row >= 0 && row < m_result.nodes.size()) {
            nodes.append(m_result.nodes[row]);
        }
    }
    return nodes;
}

QString OverviewPage::analysisReport() const
{
    return m_reportView->toPlainText();
}

void OverviewPage::applyFilter()
{
    m_proxy->setFilterText(m_filterEdit->text().trimmed());
    selectFirstVisibleObjectRow();
}

void OverviewPage::handleFileTreeSelection(const QModelIndex &current, const QModelIndex &previous)
{
    Q_UNUSED(previous);
    const QString filePath = current.data(Qt::UserRole + 1).toString();
    if (!filePath.isEmpty()) {
        setSelectedFileFilter(filePath);
    }
}

void OverviewPage::handleTableSelection(const QModelIndex &current, const QModelIndex &previous)
{
    Q_UNUSED(previous);
    const QModelIndex sourceIndex = m_proxy->mapToSource(current);
    if (!sourceIndex.isValid()) {
        m_currentRow = -1;
        m_inspector->clearSelection();
        emit currentRowChanged(-1);
        return;
    }

    m_currentRow = sourceIndex.row();
    m_inspector->setCurrentRow(m_currentRow);
    emit currentRowChanged(m_currentRow);
}

void OverviewPage::showAllFileRows()
{
    m_selectedFilePath.clear();
    if (m_fileTree && m_fileModel && m_fileModel->rowCount() > 0) {
        m_fileTree->clearSelection();
    }
    m_proxy->setSourceFileFilter(QString());
    selectFirstVisibleObjectRow();
}

void OverviewPage::setSelectedFileFilter(const QString &filePath)
{
    m_selectedFilePath = filePath.trimmed();
    m_proxy->setSourceFileFilter(m_selectedFilePath);
    selectFirstVisibleObjectRow();
}

void OverviewPage::rebuildFileSummary()
{
    if (!m_fileSummaryModel) {
        return;
    }

    m_fileSummaryModel->removeRows(0, m_fileSummaryModel->rowCount());

    QHash<QString, int> objectCounts;
    QHash<QString, int> warningCounts;
    QHash<QString, QStringList> warningDetails;
    for (const DataNode &node : m_result.nodes) {
        ++objectCounts[node.sourceFile];
        if (node.duplicateId) { ++warningCounts[node.sourceFile]; warningDetails[node.sourceFile] << QStringLiteral("duplicate ID: %1").arg(node.id); }
        if (node.duplicateContent) { ++warningCounts[node.sourceFile]; warningDetails[node.sourceFile] << QStringLiteral("related identical body: %1").arg(node.id); }
        if (node.candidateUnused) { ++warningCounts[node.sourceFile]; warningDetails[node.sourceFile] << QStringLiteral("unused candidate: %1").arg(node.id); }
    }
    for (const ParseErrorInfo &error : m_result.parseErrors) {
        ++warningCounts[error.filePath]; warningDetails[error.filePath] << QStringLiteral("parse error: %1").arg(error.message);
    }

    QSet<QString> seenFiles;
    for (const ScannedFileInfo &file : m_result.scannedFiles) {
        if (!file.isXml || seenFiles.contains(file.filePath)) {
            continue;
        }
        seenFiles.insert(file.filePath);

        QList<QStandardItem *> rowItems;
        auto *fileItem = new QStandardItem(archiveFileLabel(file.filePath));
        fileItem->setEditable(false);
        fileItem->setData(file.filePath, Qt::UserRole + 1);
        auto *countItem = new QStandardItem(QString::number(objectCounts.value(file.filePath, 0)));
        countItem->setEditable(false);
        const int warnings = warningCounts.value(file.filePath);
        auto *statusItem = new QStandardItem(warnings > 0
                                                 ? QStringLiteral("Warnings: %1").arg(warnings)
                                                 : QStringLiteral("PASS (0 warnings)"));
        statusItem->setEditable(false);
        statusItem->setToolTip(warningDetails.value(file.filePath).join(QStringLiteral("\n")));
        rowItems << fileItem << countItem << statusItem;
        m_fileSummaryModel->appendRow(rowItems);
    }

    if (m_fileSummaryModel->rowCount() == 0) {
        auto *fileItem = new QStandardItem(QStringLiteral("No XML files found"));
        fileItem->setEditable(false);
        auto *countItem = new QStandardItem(QStringLiteral("0"));
        countItem->setEditable(false);
        auto *statusItem = new QStandardItem(QStringLiteral("Check archive support or folder scan"));
        statusItem->setEditable(false);
        m_fileSummaryModel->appendRow({fileItem, countItem, statusItem});
    }

    m_fileSummaryTable->resizeColumnsToContents();
}

void OverviewPage::rebuildFileTree()
{
    m_fileModel->clear();
    m_fileModel->setHorizontalHeaderLabels({QStringLiteral("Files")});
    QStandardItem *root = m_fileModel->invisibleRootItem();
    QStandardItem *folderItem = new QStandardItem(m_result.rootFolder.isEmpty() ? folderPath() : m_result.rootFolder);
    folderItem->setEditable(false);
    root->appendRow(folderItem);

    QStandardItem *scannedItem = new QStandardItem(QStringLiteral("Scanned files"));
    scannedItem->setEditable(false);
    folderItem->appendRow(scannedItem);

    QHash<QString, int> objectCounts;
    for (const DataNode &node : m_result.nodes) {
        ++objectCounts[node.sourceFile];
    }

    for (const ScannedFileInfo &file : m_result.scannedFiles) {
        const int count = objectCounts.value(file.filePath, 0);
        auto *item = new QStandardItem(QStringLiteral("%1  (%2)").arg(QFileInfo(file.filePath).fileName()).arg(count));
        item->setEditable(false);
        item->setData(file.filePath, Qt::UserRole + 1);
        scannedItem->appendRow(item);
    }

    m_fileTree->expandAll();
}

void OverviewPage::selectFirstFileItem()
{
    if (m_fileSummaryModel && m_fileSummaryModel->rowCount() > 0) {
        const QModelIndex index = m_fileSummaryModel->index(0, 0);
        if (index.isValid()) {
            m_fileSummaryTable->setCurrentIndex(index);
            setSelectedFileFilter(index.data(Qt::UserRole + 1).toString());
        }
    }

    if (!m_fileModel) {
        return;
    }

    QStandardItem *root = m_fileModel->invisibleRootItem();
    if (!root || root->rowCount() == 0) {
        setSelectedFileFilter(QString());
        return;
    }

    QStandardItem *folderItem = root->child(0);
    if (!folderItem || folderItem->rowCount() == 0) {
        setSelectedFileFilter(QString());
        return;
    }

    QStandardItem *scannedItem = folderItem->child(0);
    if (!scannedItem || scannedItem->rowCount() == 0) {
        setSelectedFileFilter(QString());
        return;
    }

    QStandardItem *firstFile = scannedItem->child(0);
    if (!firstFile) {
        setSelectedFileFilter(QString());
        return;
    }

    const QModelIndex index = firstFile->index();
    m_fileTree->setCurrentIndex(index);
    setSelectedFileFilter(firstFile->data(Qt::UserRole + 1).toString());
}

void OverviewPage::selectFirstVisibleObjectRow()
{
    if (m_proxy->rowCount() <= 0) {
        m_currentRow = -1;
        m_inspector->clearSelection();
        emit currentRowChanged(-1);
        return;
    }

    const QModelIndex firstIndex = m_proxy->index(0, 0);
    if (!firstIndex.isValid()) {
        return;
    }

    m_objectTable->setCurrentIndex(firstIndex);
    m_objectTable->selectRow(0);
    handleTableSelection(firstIndex, QModelIndex());
}
