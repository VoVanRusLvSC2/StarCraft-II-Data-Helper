#include "app/MainWindowAnalysisController.h"

#include "app/MainWindow.h"

#include "ui/AnalysisProgressDialog.h"
#include "ui/OverviewPage.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QLineEdit>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>

namespace
{
bool isSupportedSc2ArchiveForAnalysis(const QFileInfo &info)
{
    const QString suffix = info.suffix();
    return suffix.compare(QStringLiteral("SC2Map"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("SC2Mod"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("SC2Components"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("SC2Campaign"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("SC2Archive"), Qt::CaseInsensitive) == 0;
}

bool folderContainsSupportedArchivesForAnalysis(const QString &folderPath)
{
    QDirIterator it(folderPath, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString filePath = it.next();
        const QString relative = QDir(folderPath).relativeFilePath(filePath).replace('\\', '/');
        const QString relativeLower = relative.toLower();
        const QString fileNameLower = QFileInfo(filePath).fileName().toLower();
        if (relative.startsWith(QStringLiteral("backup_"), Qt::CaseInsensitive)
            || relative.contains(QStringLiteral("/backup_"), Qt::CaseInsensitive)
            || relativeLower.startsWith(QStringLiteral("orig/"))
            || relativeLower.contains(QStringLiteral("/orig/"))
            || fileNameLower.contains(QStringLiteral(".bak-"))
            || fileNameLower.contains(QStringLiteral(".bak.")))
            continue;
        if (isSupportedSc2ArchiveForAnalysis(QFileInfo(filePath)))
            return true;
    }
    return false;
}

QString modeLabelForSource(int kind)
{
    switch (kind)
    {
    case 0:
        return QStringLiteral("Mode: folder analysis");
    case 1:
        return QStringLiteral("Mode: archive folder analysis (read-only)");
    case 2:
        return QStringLiteral("Mode: XML file analysis");
    case 3:
        return QStringLiteral("Mode: archive analysis (read-only)");
    default:
        return QStringLiteral("Mode: waiting for analysis");
    }
}
}

namespace sc2dh::app
{
MainWindowAnalysisController::MainWindowAnalysisController(MainWindow &window)
    : m_window(window)
{
}

void MainWindowAnalysisController::analyzeCurrentSource()
{
    const QString path = m_window.m_pathEdit ? m_window.m_pathEdit->text().trimmed() : QString();
    if (path.isEmpty() && m_window.m_currentSourcePath.isEmpty())
    {
        QMessageBox::warning(&m_window, QStringLiteral("Analyze"), QStringLiteral("Select a file or folder first."));
        return;
    }

    const QString effectivePath = path.isEmpty() ? m_window.m_currentSourcePath : path;
    loadPathAndAnalyze(effectivePath);
}

bool MainWindowAnalysisController::loadPathAndAnalyze(const QString &path)
{
    QFileInfo info(path);
    const bool previousOptimizationEnabled = m_window.m_dryRunAction && m_window.m_dryRunAction->isEnabled();
    const bool previousReviewEnabled = m_window.m_applyAction && m_window.m_applyAction->isEnabled();
    m_window.m_dryRunAction->setEnabled(false);
    m_window.m_applyAction->setEnabled(false);
    if (!info.exists())
    {
        QMessageBox::warning(&m_window, QStringLiteral("Load"), QStringLiteral("Path does not exist: %1").arg(path));
        m_window.logLine(QStringLiteral("Path does not exist: %1").arg(path));
        return false;
    }

    const AnalysisResult previousResult = m_window.m_result;
    QString errorMessage;
    bool ok = false;
    AnalysisProgressDialog progress(&m_window);
    m_window.m_activeProgressDialog = &progress;
    progress.setProgress(8,
                         QStringLiteral("Preparing analysis"),
                         QFileInfo(path).fileName());
    progress.show();
    QApplication::processEvents();
    progress.setProgress(22,
                         info.isDir() ? QStringLiteral("Scanning folder") : QStringLiteral("Opening data source"),
                         path);
    QApplication::processEvents();
    if (info.isDir())
    {
        const bool hasArchives = folderContainsSupportedArchivesForAnalysis(path);
        m_window.m_sourceKind = hasArchives ? MainWindow::SourceKind::ArchiveFolder : MainWindow::SourceKind::Folder;
        ok = hasArchives ? m_window.analyzeArchiveFolderPath(path, &errorMessage)
                         : m_window.analyzeFolderPath(path, &errorMessage);
    }
    else if (info.suffix().compare(QStringLiteral("xml"), Qt::CaseInsensitive) == 0)
    {
        m_window.m_sourceKind = MainWindow::SourceKind::XmlFile;
        ok = m_window.analyzeXmlFile(path, &errorMessage);
    }
    else if (isSupportedSc2ArchiveForAnalysis(info))
    {
        m_window.m_sourceKind = MainWindow::SourceKind::ArchiveFile;
        ok = m_window.analyzeArchiveFile(path, &errorMessage);
    }
    else
    {
        m_window.m_sourceKind = MainWindow::SourceKind::Unknown;
        errorMessage = QStringLiteral("Unsupported path type: %1").arg(path);
        ok = false;
    }
    if (progress.isCancelled())
    {
        ok = false;
        errorMessage = QStringLiteral("Analysis canceled.");
    }
    progress.setProgress(ok ? 88 : 100,
                         ok ? QStringLiteral("Building object registry") : QStringLiteral("Analysis failed"),
                         ok ? QStringLiteral("Preparing tables, references and reports") : errorMessage);
    QApplication::processEvents();

    if (!ok)
    {
        m_window.m_result = previousResult;
        m_window.m_dryRunAction->setEnabled(previousOptimizationEnabled);
        m_window.m_applyAction->setEnabled(previousReviewEnabled);
        m_window.m_activeProgressDialog = nullptr;
        progress.close();
        if (errorMessage == QStringLiteral("Analysis canceled."))
        {
            m_window.statusBar()->showMessage(QStringLiteral("Analysis canceled. No partial result was applied."), 8000);
            m_window.logLine(QStringLiteral("Analysis canceled by user."));
        }
        else
        {
            QMessageBox::critical(&m_window, QStringLiteral("Analysis failed"), errorMessage);
            m_window.logLine(QStringLiteral("Analysis failed: %1").arg(errorMessage));
        }
        return false;
    }

    m_window.m_currentSourcePath = path;
    m_window.m_rootFolder = info.isDir() ? path : info.absolutePath();
    QSettings settings;
    settings.setValue(QStringLiteral("paths/lastSourcePath"), path);
    progress.setProgress(90,
                         QStringLiteral("Refreshing analysis"),
                         QStringLiteral("Updating object tables"));
    QApplication::processEvents();
    m_window.m_analysisPage->setFolderPath(path);
    m_window.m_analysisPage->setModeLabel(modeLabelForSource(static_cast<int>(m_window.m_sourceKind)));
    m_window.m_analysisPage->setAnalysisResult(m_window.m_result);
    progress.setProgress(94,
                         QStringLiteral("Refreshing analysis"),
                         QStringLiteral("Updating pages and recommendations"));
    QApplication::processEvents();
    m_window.refreshPages();
    progress.setProgress(98,
                         QStringLiteral("Writing report"),
                         QStringLiteral("Saving latest analysis summary"));
    QApplication::processEvents();
    m_window.writeAnalysisReportFile();
    progress.setProgress(100,
                         QStringLiteral("Analysis complete"),
                         QStringLiteral("%1 XML files | %2 objects")
                             .arg(m_window.m_result.totalXmlFiles())
                             .arg(m_window.m_result.totalDataNodes()));
    QApplication::processEvents();
    progress.close();
    m_window.m_activeProgressDialog = nullptr;
    m_window.showAnalysisTab();
    m_window.m_dryRunAction->setEnabled(true);
    m_window.m_applyAction->setEnabled(false);
    m_window.setCurrentSourcePath(path);
    m_window.logLine(QStringLiteral("Scanned files: %1").arg(m_window.m_result.totalFilesScanned()));
    m_window.logLine(QStringLiteral("XML files: %1").arg(m_window.m_result.totalXmlFiles()));
    m_window.logLine(QStringLiteral("Data nodes: %1").arg(m_window.m_result.totalDataNodes()));
    m_window.logLine(QStringLiteral("Duplicate IDs: %1").arg(m_window.m_result.duplicateIdGroups.size()));
    m_window.logLine(QStringLiteral("Duplicate content groups: %1").arg(m_window.m_result.duplicateContentGroups.size()));
    m_window.logLine(QStringLiteral("Parse errors: %1").arg(m_window.m_result.parseErrors.size()));
    for (const ParseErrorInfo &error : m_window.m_result.parseErrors)
        m_window.logLine(QStringLiteral("Parse error: %1 -> %2").arg(error.filePath, error.message));
    for (const DuplicateIdGroup &group : m_window.m_result.duplicateIdGroups)
        m_window.logLine(QStringLiteral("Duplicate ID group: %1 (%2 nodes)").arg(group.id).arg(group.nodeIndices.size()));
    for (const DuplicateContentGroup &group : m_window.m_result.duplicateContentGroups)
        m_window.logLine(QStringLiteral("Duplicate content group: %1 (%2 nodes)").arg(group.contentHash.left(12)).arg(group.nodeIndices.size()));
    if (!m_window.m_optimizationDialog && !m_window.m_wizardApplyAutomation)
        m_window.showDryRunTab(true);
    return true;
}
}
