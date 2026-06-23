#pragma once

#include "core/AnalysisModels.h"
#include "ui/ObjectInspectorWidget.h"
#include "ui/ObjectFilterProxyModel.h"
#include "ui/ObjectTableModel.h"

#include <QStandardItemModel>
#include <QWidget>

class QLineEdit;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableView;
class QTreeView;

class OverviewPage : public QWidget
{
    Q_OBJECT

public:
    explicit OverviewPage(QWidget *parent = nullptr);

    void setFolderPath(const QString &folderPath);
    QString folderPath() const;
    void setAnalysisResult(const AnalysisResult &result);
    const AnalysisResult &analysisResult() const { return m_result; }
    QVector<int> selectedRows() const;
    QVector<DataNode> selectedNodes() const;
    QString analysisReport() const;
    int currentRow() const { return m_currentRow; }
    void setModeLabel(const QString &modeText);
    void setOutputText(const QString &text);

signals:
    void folderPathChanged(const QString &folderPath);
    void currentRowChanged(int sourceRow);
    void objectDoubleClicked(int sourceRow);

private slots:
    void applyFilter();
    void handleFileTreeSelection(const QModelIndex &current, const QModelIndex &previous);
    void handleTableSelection(const QModelIndex &current, const QModelIndex &previous);
    void showAllFileRows();

private:
    void rebuildFileTree();
    void rebuildFileSummary();
    void selectFirstFileItem();
    void selectFirstVisibleObjectRow();
    void setSelectedFileFilter(const QString &filePath);

    AnalysisResult m_result;
    QString m_folderPath;
    int m_currentRow = -1;
    QLineEdit *m_folderEdit = nullptr;
    QLineEdit *m_filterEdit = nullptr;
    QLabel *m_modeLabel = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QPlainTextEdit *m_reportView = nullptr;
    QTreeView *m_fileTree = nullptr;
    QTableView *m_fileSummaryTable = nullptr;
    QTableView *m_objectTable = nullptr;
    QPushButton *m_showAllFilesButton = nullptr;
    ObjectInspectorWidget *m_inspector = nullptr;
    ObjectTableModel *m_model = nullptr;
    ObjectFilterProxyModel *m_proxy = nullptr;
    QStandardItemModel *m_fileModel = nullptr;
    QStandardItemModel *m_fileSummaryModel = nullptr;
    QString m_selectedFilePath;
};
