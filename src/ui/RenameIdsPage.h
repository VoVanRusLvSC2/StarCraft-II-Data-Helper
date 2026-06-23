#pragma once

#include "core/ReferenceRenamer.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTextEdit;

class RenameIdsPage : public QWidget
{
    Q_OBJECT

public:
    explicit RenameIdsPage(QWidget *parent = nullptr);
    void setAnalysisResult(const AnalysisResult &result);
    RenamePlan currentPlan() const;
    void setPreviewReport(const RenamePreviewReport &report);
    void setApplyAvailable(bool available);

signals:
    void previewRequested(const RenamePlan &plan);
    void applyRequested(const RenamePlan &plan);
    void exportRequested(const QString &reportText);

private slots:
    void detectFamilies();
    void rebuildFamilyTables();

private:
    QTableWidget *makeTable(const QStringList &headers) const;
    QSet<int> includedRows() const;

    AnalysisResult m_result;
    QVector<UnitFamily> m_families;
    RenamePreviewReport m_previewReport;
    QLabel *m_summary = nullptr;
    QLabel *m_rootLabel = nullptr;
    QComboBox *m_familySelector = nullptr;
    QLineEdit *m_targetRoot = nullptr;
    QTabWidget *m_tabs = nullptr;
    QTableWidget *m_detected = nullptr;
    QTableWidget *m_nonStandard = nullptr;
    QTableWidget *m_manual = nullptr;
    QTableWidget *m_preview = nullptr;
    QTextEdit *m_details = nullptr;
    QPushButton *m_apply = nullptr;
};
