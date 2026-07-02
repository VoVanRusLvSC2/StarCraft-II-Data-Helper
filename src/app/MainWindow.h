#pragma once

#include "core/AnalysisModels.h"
#include "core/BackupManager.h"
#include "core/ConfigManager.h"
#include "core/FolderAnalyzer.h"
#include "core/MergeService.h"
#include "core/ReferenceRenamer.h"
#include "core/DataCollectionUnitBuilder.h"

#include <QMainWindow>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <memory>

class QAction;
class AnalysisProgressDialog;
class QLineEdit;
class QDialog;
class QTemporaryDir;
class QTabWidget;
class OverviewPage;
class DependenciesPage;
class GraphPage;
class PropertiesPage;
class DataCollectionPage;
class RenameIdsPage;
class DuplicatesPage;
class UnusedPage;
class FormatterPage;
class LogPanel;
class XmlSourcePage;
struct WizardNodeRef;

namespace sc2dh::app {
class MainWindowAnalysisController;
class MainWindowSettings;
class MainWindowStartup;
class MainWindowUiBuilder;
class SourceSelectionController;
}

namespace spdlog {
class logger;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    void runWizardApplyAutomation(const QString &path, const QString &logPath = QString(), int timeoutMs = 600000);

private slots:
    void openSc2File();
    void openSourceFolder();
    void analyzeFolder();
    void showSettingsDialog();
    void runDryRun();
    void applySelectedChanges();
    void previewMerge(const MergeRequest &request);
    void applyMerge(const MergeRequest &request);
    void previewUnusedDeletion(const QVector<int> &rows);
    void applyUnusedDeletion(const QVector<int> &rows);
    void previewStandardRename(const RenamePlan &plan);
    void applyStandardRename(const RenamePlan &plan);
    void exportStandardRenameReport(const QString &reportText);
    void previewDataCollection(const DataCollectionBuildRequest &request);
    void applyDataCollection(const DataCollectionBuildRequest &request);
    void exportDataCollectionReport(const QString &reportText);
    void showAnalysisTab();
    void showDataCollectionTab();
    void showDuplicatesTab();
    void showCleanupTab();
    void showDryRunTab(bool autoBuild = false);
    void applyOptimizationWizardPlan();
    void showLogsTab();
    void undoFocusedEditor();
    void redoFocusedEditor();
    void toggleFullscreen();

protected:
    void changeEvent(QEvent *event) override;

private:
    friend class sc2dh::app::MainWindowAnalysisController;
    friend class sc2dh::app::MainWindowSettings;
    friend class sc2dh::app::MainWindowStartup;
    friend class sc2dh::app::MainWindowUiBuilder;
    friend class sc2dh::app::SourceSelectionController;

    void setupUi();
    void setupLogging();
    void setupTheme();
    void loadDefaultFolder();
    void writeAnalysisReportFile() const;
    QString runtimePath(const QString &relativePath) const;
    bool validateArchiveCatalogSchema(const QString &archivePath, QString *errorMessage) const;
    void setCurrentSourcePath(const QString &path);
    void logLine(const QString &line) const;
    void refreshPages();
    bool loadPathAndAnalyze(const QString &path);
    bool analyzeFolderPath(const QString &folderPath, QString *errorMessage);
    bool analyzeArchiveFolderPath(const QString &folderPath, QString *errorMessage);
    bool analyzeXmlFile(const QString &filePath, QString *errorMessage);
    bool analyzeArchiveFile(const QString &filePath, QString *errorMessage);
    bool materializeArchiveAnalysis(const QString &tempRoot, AnalysisResult *analysis, QString *errorMessage) const;
    void applyArchiveReferenceSafety(AnalysisResult *analysis) const;
    void normalizeArchiveAnalysis(AnalysisResult *analysis, const QString &tempRoot, const QString &archivePath) const;
    bool commitArchiveChanges(const QString &tempRoot, const QStringList &changedFiles,
                              QString *backupPath, QString *errorMessage,
                              const QStringList &removedFiles = {}) const;
    int findNodeIndex(const AnalysisResult &analysis, const WizardNodeRef &ref) const;
    void showGraphForRow(int row);
    void setDuplicateMergeEnabled(bool enabled);
    void updateFullscreenActionText();
    void appendWizardApplyAutomationLog(const QString &line) const;
    void finishWizardApplyAutomation(bool success, const QString &message);

    QString m_rootFolder;
    QString m_currentSourcePath;
    AnalysisResult m_result;
    FolderAnalyzer m_analyzer;
    ConfigManager m_configManager;
    QSet<QString> m_whitelistIds;
    QSet<QString> m_archiveReferencedIds;
    QHash<QString, QStringList> m_archiveStrongReferenceSources;
    QHash<QString, QStringList> m_archiveWeakReferenceSources;
    bool m_archiveReferenceScanComplete = false;
    MergeService m_mergeService;
    MergeRequest m_previewedMerge;
    QVector<int> m_previewedUnusedRows;
    bool m_mergePreviewValid = false;
    ReferenceRenamer m_referenceRenamer;
    RenamePlan m_previewedRenamePlan;
    bool m_renamePreviewValid = false;
    DataCollectionUnitBuilder m_dataCollectionBuilder;
    DataCollectionBuildRequest m_previewedCollectionRequest;
    bool m_collectionPreviewValid = false;
    std::shared_ptr<spdlog::logger> m_logger;
    enum class SourceKind {
        Folder,
        ArchiveFolder,
        XmlFile,
        ArchiveFile,
        Unknown
    };
    SourceKind m_sourceKind = SourceKind::Unknown;

    QTabWidget *m_tabs = nullptr;
    OverviewPage *m_analysisPage = nullptr;
    DependenciesPage *m_dependenciesPage = nullptr;
    GraphPage *m_graphPage = nullptr;
    PropertiesPage *m_propertiesPage = nullptr;
    DataCollectionPage *m_dataCollectionPage = nullptr;
    RenameIdsPage *m_renameIdsPage = nullptr;
    DuplicatesPage *m_duplicatesPage = nullptr;
    UnusedPage *m_cleanupPage = nullptr;
    FormatterPage *m_dryRunPage = nullptr;
    LogPanel *m_logPanel = nullptr;
    XmlSourcePage *m_xmlSourcePage = nullptr;
    QLineEdit *m_pathEdit = nullptr;

    QAction *m_openFileAction = nullptr;
    QAction *m_openFolderAction = nullptr;
    QAction *m_analyzeAction = nullptr;
    QAction *m_settingsAction = nullptr;
    QAction *m_fullscreenAction = nullptr;
    QAction *m_dryRunAction = nullptr;
    QAction *m_applyAction = nullptr;
    QAction *m_exitAction = nullptr;
    AnalysisProgressDialog *m_activeProgressDialog = nullptr;
    QDialog *m_optimizationDialog = nullptr;
    bool m_wizardApplyAutomation = false;
    QString m_wizardApplyAutomationLogPath;
};
