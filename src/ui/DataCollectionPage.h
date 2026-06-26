#pragma once

#include "core/DataCollectionUnitBuilder.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTextEdit;
class QTreeWidget;

class DataCollectionPage : public QWidget
{
    Q_OBJECT

public:
    explicit DataCollectionPage(QWidget *parent = nullptr);
    void setAnalysisResult(const AnalysisResult &result);
    DataCollectionBuildRequest currentRequest() const;
    void setPreviewReport(const DataCollectionPreviewReport &report);
    void setApplyAvailable(bool available);

signals:
    void previewRequested(const DataCollectionBuildRequest &request);
    void applyRequested(const DataCollectionBuildRequest &request);
    void exportRequested(const QString &reportText);

private:
    void rebuildFamily();
    void populateTables(const DataCollectionPreviewReport &report);
    QTableWidget *createEntryTable() const;
    AnalysisResult m_result;
    QVector<UnitFamily> m_families;
    DataCollectionPreviewReport m_previewReport;
    QString m_auditSummary;
    QLabel *m_summary = nullptr;
    QLabel *m_root = nullptr;
    QLabel *m_standard = nullptr;
    QLabel *m_existing = nullptr;
    QLabel *m_fileStatus = nullptr;
    QComboBox *m_selector = nullptr;
    QLineEdit *m_parent = nullptr;
    QLineEdit *m_categories = nullptr;
    QLineEdit *m_currentUnitName = nullptr;
    QLineEdit *m_newUnitName = nullptr;
    QCheckBox *m_confirmNonStandard = nullptr;
    QTabWidget *m_entryTabs = nullptr;
    QTreeWidget *m_familyTree = nullptr;
    QTableWidget *m_addableTable = nullptr;
    QTableWidget *m_existingTable = nullptr;
    QTableWidget *m_reviewTable = nullptr;
    QTextEdit *m_xml = nullptr;
    QTextEdit *m_warnings = nullptr;
    QPushButton *m_apply = nullptr;
};
