#include "app/SourceSelectionController.h"

#include "app/MainWindow.h"
#include "app/Sc2FileDialogs.h"

#include "ui/OverviewPage.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSettings>

namespace
{
bool isSupportedSc2ArchiveForSelection(const QFileInfo &info)
{
    const QString suffix = info.suffix();
    return suffix.compare(QStringLiteral("SC2Map"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("SC2Mod"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("SC2Components"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("SC2Campaign"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("SC2Archive"), Qt::CaseInsensitive) == 0;
}

bool folderContainsSupportedArchives(const QString &folderPath)
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
        if (isSupportedSc2ArchiveForSelection(QFileInfo(filePath)))
            return true;
    }
    return false;
}
}

namespace sc2dh::app
{
SourceSelectionController::SourceSelectionController(MainWindow &window)
    : m_window(window)
{
}

void SourceSelectionController::openSc2File()
{
    QSettings settings;
    const QString savedPath = settings.value(QStringLiteral("paths/lastSourcePath")).toString();
    QString startPath = !m_window.m_currentSourcePath.isEmpty() ? m_window.m_currentSourcePath : savedPath;
    if (!startPath.isEmpty() && !QFileInfo::exists(startPath))
        startPath = QFileInfo(startPath).absolutePath();

    const QString selected = openSc2FileStyled(&m_window, startPath);
    if (selected.isEmpty())
        return;

    const QFileInfo info(selected);
    if (info.suffix().compare(QStringLiteral("xml"), Qt::CaseInsensitive) == 0)
        m_window.m_sourceKind = MainWindow::SourceKind::XmlFile;
    else if (isSupportedSc2ArchiveForSelection(info))
        m_window.m_sourceKind = MainWindow::SourceKind::ArchiveFile;
    else
        m_window.m_sourceKind = MainWindow::SourceKind::Unknown;

    m_window.m_rootFolder = info.absolutePath();
    m_window.setCurrentSourcePath(selected);
    settings.setValue(QStringLiteral("paths/lastSourcePath"), selected);
    m_window.m_analysisPage->setFolderPath(selected);
    m_window.m_analysisPage->setModeLabel(QStringLiteral("Mode: starting automatic analysis"));
    m_window.m_analysisPage->setOutputText(QStringLiteral("File selected. Automatic analysis is starting..."));
    m_window.logLine(QStringLiteral("File selected; starting automatic analysis: %1").arg(selected));
    m_window.loadPathAndAnalyze(selected);
}

void SourceSelectionController::openSourceFolder()
{
    QSettings settings;
    const QString savedPath = settings.value(QStringLiteral("paths/lastSourcePath")).toString();
    QString startPath = !m_window.m_currentSourcePath.isEmpty() ? m_window.m_currentSourcePath : savedPath;
    if (!startPath.isEmpty() && !QFileInfo(startPath).isDir())
        startPath = QFileInfo(startPath).absolutePath();

    const QString selected = openFolderStyled(&m_window, startPath);
    if (selected.isEmpty())
        return;

    m_window.m_rootFolder = selected;
    m_window.m_sourceKind = folderContainsSupportedArchives(selected)
        ? MainWindow::SourceKind::ArchiveFolder
        : MainWindow::SourceKind::Folder;
    m_window.setCurrentSourcePath(selected);
    settings.setValue(QStringLiteral("paths/lastSourcePath"), selected);
    m_window.m_analysisPage->setFolderPath(selected);
    m_window.m_analysisPage->setModeLabel(QStringLiteral("Mode: starting automatic analysis"));
    m_window.m_analysisPage->setOutputText(QStringLiteral("Folder selected. Automatic analysis is starting..."));
    m_window.logLine(QStringLiteral("Folder selected; starting automatic analysis: %1").arg(selected));
    m_window.loadPathAndAnalyze(selected);
}
}
