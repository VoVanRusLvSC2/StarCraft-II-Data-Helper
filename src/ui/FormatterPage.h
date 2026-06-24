#pragma once
#include "core/AnalysisModels.h"
#include "core/MergeService.h"
#include "core/StandardNamePlanner.h"
#include <QFutureWatcher>
#include <QStringList>
#include <QWidget>
class QLabel; class QPlainTextEdit; class QPushButton; class QTableWidget; class QTabWidget;

struct OptimizationPlanRow
{
    QStringList values;
    bool checked = false;
    bool selectable = true;
    int primary = -1;
    int secondary = -1;
};

struct OptimizationPlanData
{
    QVector<OptimizationPlanRow> unused;
    QVector<OptimizationPlanRow> duplicates;
    QVector<OptimizationPlanRow> rename;
    QVector<OptimizationPlanRow> collection;
};

struct WizardNodeRef
{
    QString id;
    QString elementName;
    QString sourceFile;
    QString originalLocation;
};

struct WizardMergeSelection
{
    WizardNodeRef keep;
    WizardNodeRef remove;
};

struct WizardRenameSelection
{
    QString familyRootId;
    WizardNodeRef node;
};

struct WizardCollectionSelection
{
    QString familyRootId;
    QVector<WizardNodeRef> nodes;
};

struct OptimizationWizardSelection
{
    QVector<WizardNodeRef> unused;
    QVector<WizardMergeSelection> duplicates;
    QVector<WizardRenameSelection> rename;
    QVector<WizardCollectionSelection> collection;
};

class FormatterPage : public QWidget
{
    Q_OBJECT
public:
    explicit FormatterPage(QWidget *parent = nullptr);
    void setAnalysisResult(const AnalysisResult &result);
    void setPreview(const QString &text);
    void startWizard();
    void recordUnusedResult(int removed);
    void recordMergeResult(int removed, int redirected);
    void recordRenameResult(int renamed);
    void recordCollectionResult(int added);
    void setApplyingState(bool applying, const QString &message = {});
    void rebuildAfterApply();
    void setDuplicateMergeEnabled(bool enabled);
    OptimizationWizardSelection currentSelection() const;
signals:
    void previewBuilt();
    void openUnusedRequested(const QVector<int> &rows);
    void openDuplicateRequested(const MergeRequest &request);
    void openRenameRequested();
    void openCollectionRequested();
    void applyWizardRequested();
    void wizardFinished();
private:
    void buildPreview();
    void updateSummary();
    void updateNavigation();
    void updateDetails();
    void setRecommendedSelection(bool selected);
    QString buildIdChangePreview() const;
    void openCurrentStep();
    WizardNodeRef nodeRefFromIndices(int primary, int secondary = -1) const;
    QVector<int> selectedUnusedRows() const;
    MergeRequest selectedMergeRequest() const;
    QVector<MergeRequest> selectedMergeRequests() const;
    AnalysisResult m_result;
    QTabWidget *m_steps = nullptr;
    QTableWidget *m_unused = nullptr;
    QTableWidget *m_duplicates = nullptr;
    QTableWidget *m_rename = nullptr;
    QTableWidget *m_collection = nullptr;
    QPlainTextEdit *m_summary = nullptr;
    QPlainTextEdit *m_details = nullptr;
    QLabel *m_stepLabel = nullptr;
    QPushButton *m_backButton = nullptr;
    QPushButton *m_buildButton = nullptr;
    QPushButton *m_nextButton = nullptr;
    QPushButton *m_stepActionButton = nullptr;
    QPushButton *m_selectRecommendedButton = nullptr;
    QPushButton *m_clearSelectionButton = nullptr;
    QPushButton *m_applyPlanButton = nullptr;
    QFutureWatcher<OptimizationPlanData> *m_planWatcher = nullptr;
    bool m_planBuilt = false;
    bool m_building = false;
    bool m_applying = false;
    bool m_planConfirmed = false;
    bool m_hasAppliedChanges = false;
    bool m_duplicateMergeEnabled = false;
    int m_actualUnused = 0, m_actualDuplicates = 0, m_actualRedirected = 0, m_actualRenamed = 0, m_actualCollectionAdded = 0;
};
