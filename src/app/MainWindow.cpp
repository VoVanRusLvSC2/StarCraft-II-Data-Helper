#include "app/MainWindow.h"
#include "app/MainWindowAnalysisController.h"
#include "app/MainWindowSettings.h"
#include "app/MainWindowStartup.h"
#include "app/MainWindowUiBuilder.h"
#include "app/Sc2FileDialogs.h"
#include "app/Sc2MessageDialog.h"
#include "app/SourceSelectionController.h"
#include "app/ThemeManager.h"

#include "ui/DataCollectionPage.h"
#include "ui/AnalysisProgressDialog.h"
#include "ui/DuplicatesPage.h"
#include "ui/DependenciesPage.h"
#include "ui/FormatterPage.h"
#include "ui/GraphPage.h"
#include "ui/LogPanel.h"
#include "ui/OverviewPage.h"
#include "ui/PropertiesPage.h"
#include "ui/RenameIdsPage.h"
#include "ui/UnusedPage.h"
#include "ui/XmlSourcePage.h"
#include "core/ArchiveReferenceRewriter.h"
#include "core/CatalogEnumRepair.h"
#include "core/Sc2Archive.h"
#include "core/DataCollectionPreservation.h"
#include "core/DeepCleanupService.h"
#include "core/StandardNamePlanner.h"
#include "core/UnitFamilyDetector.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QColor>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTabBar>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <optional>
#include <algorithm>
#include <cmath>
#include <vector>

using sc2dh::app::MainWindowAnalysisController;
using sc2dh::app::MainWindowSettings;
using sc2dh::app::MainWindowStartup;
using sc2dh::app::MainWindowUiBuilder;
using sc2dh::app::saveTextFileStyled;
using sc2dh::app::showSc2MessageDialog;
using sc2dh::app::SourceSelectionController;

namespace
{

    QStringList archiveReferenceFilesForWorkspace(const AnalysisResult &analysis, const QString &rootFolder)
    {
        QStringList files;
        const QDir root(rootFolder);
        for (const ScannedFileInfo &file : analysis.scannedFiles)
        {
            if (file.isXml || !file.isSc2DataLike)
                continue;
            QString relative = root.relativeFilePath(file.filePath);
            relative = QDir::cleanPath(relative).replace('\\', '/');
            if (!relative.startsWith(QStringLiteral("../")) && !QDir::isAbsolutePath(relative))
                files << relative;
        }
        files.removeDuplicates();
        return files;
    }

    QString firstExistingFile(const QStringList &candidates)
    {
        for (const QString &candidate : candidates)
        {
            const QString cleaned = QDir::cleanPath(candidate);
            if (QFileInfo::exists(cleaned) && QFileInfo(cleaned).isFile())
                return cleaned;
        }
        return {};
    }

    QString schemaValidatorScriptPath()
    {
        const QString appDir = QCoreApplication::applicationDirPath();
        return firstExistingFile({
            QDir(appDir).absoluteFilePath(QStringLiteral("scripts/validate_sc2_catalogs.py")),
            QDir(appDir).absoluteFilePath(QStringLiteral("../scripts/validate_sc2_catalogs.py")),
            QDir(appDir).absoluteFilePath(QStringLiteral("../../scripts/validate_sc2_catalogs.py")),
            QDir::current().absoluteFilePath(QStringLiteral("scripts/validate_sc2_catalogs.py"))
        });
    }

    QString catalogXsdPath()
    {
        const QString appDir = QCoreApplication::applicationDirPath();
        return firstExistingFile({
            QDir(appDir).absoluteFilePath(QStringLiteral("resources/catalogsData.xsd")),
            QDir(appDir).absoluteFilePath(QStringLiteral("../resources/catalogsData.xsd")),
            QDir(appDir).absoluteFilePath(QStringLiteral("../../resources/catalogsData.xsd")),
            QDir::current().absoluteFilePath(QStringLiteral("resources/catalogsData.xsd"))
        });
    }

    QString compactProcessOutput(QString text, int maxChars = 2400)
    {
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        text = text.trimmed();
        if (text.size() > maxChars)
            text = text.left(maxChars) + QStringLiteral("\n...");
        return text;
    }

    QString normalizedArchiveName(QString name)
    {
        return QDir::cleanPath(name).replace('\\', '/').toCaseFolded();
    }

    QString replacementKeyForArchiveEntry(const QHash<QString, QByteArray> &replacements, const QString &archiveEntry)
    {
        const QString wanted = normalizedArchiveName(archiveEntry);
        for (auto it = replacements.cbegin(); it != replacements.cend(); ++it) {
            if (normalizedArchiveName(it.key()) == wanted)
                return it.key();
        }
        return {};
    }

    bool isRemovedArchiveEntry(const QStringList &removedEntries, const QString &archiveEntry)
    {
        const QString wanted = normalizedArchiveName(archiveEntry);
        for (const QString &removed : removedEntries) {
            if (normalizedArchiveName(removed) == wanted)
                return true;
        }
        return false;
    }

    bool addCatalogEnumRepairs(const Sc2Archive &archive,
                               QHash<QString, QByteArray> *replacements,
                               const QStringList &removedEntries,
                               int *repairCount,
                               QString *errorMessage)
    {
        if (repairCount)
            *repairCount = 0;
        if (!replacements)
            return true;

        int total = 0;
        for (const QString &entry : archive.gameDataXmlEntries()) {
            if (isRemovedArchiveEntry(removedEntries, entry))
                continue;

            const QString existingKey = replacementKeyForArchiveEntry(*replacements, entry);
            QByteArray bytes;
            QString replacementKey = existingKey.isEmpty() ? entry : existingKey;
            if (existingKey.isEmpty()) {
                if (!archive.readEntry(entry, &bytes, errorMessage))
                    return false;
            } else {
                bytes = replacements->value(existingKey);
            }

            int changes = 0;
            if (!sc2dh::repairKnownCatalogEnumDamage(&bytes, &changes, errorMessage))
                return false;
            if (changes <= 0)
                continue;
            replacements->insert(replacementKey, bytes);
            total += changes;
        }

        if (repairCount)
            *repairCount = total;
        return true;
    }


    DataCollectionMode configuredDataCollectionMode()
    {
        QSettings settings;
        return settings.value(QStringLiteral("dataCollection/mode"), QStringLiteral("UnitAbilWeapon")).toString().compare(QStringLiteral("UnitAbilWeapon"), Qt::CaseInsensitive) == 0
                   ? DataCollectionMode::UnitAbilWeapon
                   : DataCollectionMode::Unit;
    }

    enum class ArchiveReferenceStrength
    {
        None,
        Weak,
        Strong
    };

    ArchiveReferenceStrength archiveReferenceStrength(const QString &entry)
    {
        const QString normalized = QDir::cleanPath(entry).replace('\\', '/').toLower();
        const QString name = normalized.section('/', -1);
        if (name == QStringLiteral("objects") || name == QStringLiteral("units")
            || name == QStringLiteral("triggers") || normalized.contains(QStringLiteral("trigger")))
            return ArchiveReferenceStrength::Strong;
        const QString suffix = QFileInfo(name).suffix().toLower();
        if (suffix == QStringLiteral("galaxy"))
            return ArchiveReferenceStrength::Strong;
        static const QSet<QString> weakNames = {
            QStringLiteral("regions"), QStringLiteral("mapinfo"), QStringLiteral("documentinfo"),
            QStringLiteral("preload.xml"), QStringLiteral("componentlist.sc2components")};
        if (weakNames.contains(name))
            return ArchiveReferenceStrength::Weak;
        static const QSet<QString> weakExtensions = {
            QStringLiteral("txt"), QStringLiteral("ini"), QStringLiteral("json"),
            QStringLiteral("yaml"), QStringLiteral("yml"), QStringLiteral("version"),
            QStringLiteral("sc2components"), QStringLiteral("layout"), QStringLiteral("sc2layout"),
            QStringLiteral("fxa"), QStringLiteral("fxs"), QStringLiteral("fxh")};
        return weakExtensions.contains(suffix) ? ArchiveReferenceStrength::Weak : ArchiveReferenceStrength::None;
    }

    bool archiveEntryLooksLikeImportedAsset(const QString &entry)
    {
        const QString suffix = QFileInfo(QDir::cleanPath(entry).replace('\\', '/')).suffix().toLower();
        static const QSet<QString> extensions = {
            QStringLiteral("dds"), QStringLiteral("tga"), QStringLiteral("png"), QStringLiteral("jpg"),
            QStringLiteral("jpeg"), QStringLiteral("bmp"), QStringLiteral("m3"), QStringLiteral("ogg"),
            QStringLiteral("wav"), QStringLiteral("mp3"), QStringLiteral("webm"), QStringLiteral("mp4"),
            QStringLiteral("fxa"), QStringLiteral("fxs"), QStringLiteral("fxh"), QStringLiteral("layout"),
            QStringLiteral("sc2layout"), QStringLiteral("txt")
        };
        return extensions.contains(suffix);
    }

    bool archiveEntryLooksLikeHelperTrash(const QString &entry)
    {
        const QString normalized = QDir::cleanPath(entry).replace('\\', '/').toLower();
        const QString fileName = QFileInfo(normalized).fileName();
        return normalized.contains(QStringLiteral("/backup_"))
            || fileName.startsWith(QStringLiteral("backup_"))
            || fileName.contains(QStringLiteral(".bak-"))
            || fileName.endsWith(QStringLiteral(".bak"))
            || fileName.endsWith(QStringLiteral(".tmp"))
            || fileName.endsWith(QStringLiteral(".old"))
            || fileName.endsWith(QStringLiteral(".orig"))
            || fileName.endsWith(QStringLiteral(".log"))
            || fileName.endsWith(QStringLiteral(".sc2dh.pending"));
    }

    bool archiveEntryShouldMaterializeForCleanup(const QString &entry)
    {
        return archiveEntryLooksLikeImportedAsset(entry)
            || archiveEntryLooksLikeHelperTrash(entry)
            || archiveReferenceStrength(entry) != ArchiveReferenceStrength::None;
    }

    void collectKnownIdTokens(const QByteArray &bytes, const QSet<QString> &knownIds, QSet<QString> *found)
    {
        const auto isIdChar = [](uchar value)
        {
            return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_' || value == '@';
        };
        for (qsizetype start = 0; start < bytes.size();)
        {
            while (start < bytes.size() && !isIdChar(uchar(bytes[start])))
                ++start;
            qsizetype end = start;
            while (end < bytes.size() && isIdChar(uchar(bytes[end])))
                ++end;
            if (end > start)
            {
                const QString token = QString::fromLatin1(bytes.constData() + start, end - start);
                if (knownIds.contains(token))
                    found->insert(token);
            }
            start = qMax(end, start + 1);
        }
        for (qsizetype start = 0; start + 1 < bytes.size();)
        {
            while (start + 1 < bytes.size() && (!isIdChar(uchar(bytes[start])) || bytes[start + 1] != '\0'))
                ++start;
            qsizetype end = start;
            QByteArray tokenBytes;
            while (end + 1 < bytes.size() && isIdChar(uchar(bytes[end])) && bytes[end + 1] == '\0')
            {
                tokenBytes.append(bytes[end]);
                end += 2;
            }
            if (!tokenBytes.isEmpty())
            {
                const QString token = QString::fromLatin1(tokenBytes);
                if (knownIds.contains(token))
                    found->insert(token);
            }
            start = qMax(end, start + 1);
        }
    }

    void collectKnownIdTokenSources(const QByteArray &bytes, const QSet<QString> &knownIds,
                                    const QString &source, QHash<QString, QStringList> *found)
    {
        QSet<QString> ids;
        collectKnownIdTokens(bytes, knownIds, &ids);
        if (!found)
            return;
        for (const QString &id : ids)
            (*found)[id].append(source);
    }

    bool isSupportedSc2Archive(const QFileInfo &info)
    {
        const QString suffix = info.suffix();
        return suffix.compare(QStringLiteral("SC2Map"), Qt::CaseInsensitive) == 0
            || suffix.compare(QStringLiteral("SC2Mod"), Qt::CaseInsensitive) == 0
            || suffix.compare(QStringLiteral("SC2Components"), Qt::CaseInsensitive) == 0
            || suffix.compare(QStringLiteral("SC2Campaign"), Qt::CaseInsensitive) == 0
            || suffix.compare(QStringLiteral("SC2Archive"), Qt::CaseInsensitive) == 0;
    }

    QStringList collectArchiveFiles(const QString &folderPath)
    {
        QStringList archives;
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
            if (isSupportedSc2Archive(QFileInfo(filePath)))
                archives.append(filePath);
        }
        std::sort(archives.begin(), archives.end(), [](const QString &left, const QString &right)
        {
            return QString::compare(left, right, Qt::CaseInsensitive) < 0;
        });
        return archives;
    }

    bool persistentBackupsEnabledForUi()
    {
        return QSettings().value(QStringLiteral("backup/enabled"), true).toBool();
    }

    QString backupPrompt(const QString &withBackup, const QString &withoutBackup)
    {
        return persistentBackupsEnabledForUi() ? withBackup : withoutBackup;
    }

    QString archiveFolderReadOnlyMessage()
    {
        return QStringLiteral("Archive folder mode analyzes multiple maps/mods together and is read-only. Open a single SC2Map/SC2Mod archive or an extracted folder to apply changes.");
    }


    QString defaultTestFolder()
    {
        return QStringLiteral("C:/Users/Vladimir/Downloads/Regenerate_trigger/TriggerCustom/comp");
    }

    QString levelToString(spdlog::level::level_enum level)
    {
        switch (level)
        {
        case spdlog::level::trace:
            return QStringLiteral("TRACE");
        case spdlog::level::debug:
            return QStringLiteral("DEBUG");
        case spdlog::level::info:
            return QStringLiteral("INFO");
        case spdlog::level::warn:
            return QStringLiteral("WARN");
        case spdlog::level::err:
            return QStringLiteral("ERROR");
        case spdlog::level::critical:
            return QStringLiteral("CRITICAL");
        default:
            return QStringLiteral("LOG");
        }
    }

    QString modeLabelFor(int kind)
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

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    MainWindowStartup(*this).initialize();
}

void MainWindow::setupUi()
{
    MainWindowUiBuilder(*this).build();
}

void MainWindow::undoFocusedEditor()
{
    if (QWidget *focused = QApplication::focusWidget())
    {
        QMetaObject::invokeMethod(focused, "undo", Qt::DirectConnection);
    }
}

void MainWindow::redoFocusedEditor()
{
    if (QWidget *focused = QApplication::focusWidget())
    {
        QMetaObject::invokeMethod(focused, "redo", Qt::DirectConnection);
    }
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event && event->type() == QEvent::WindowStateChange)
    {
        updateFullscreenActionText();
    }
}

void MainWindow::toggleFullscreen()
{
    if (isFullScreen())
    {
        showMaximized();
    }
    else
    {
        showFullScreen();
    }
    updateFullscreenActionText();
}

void MainWindow::updateFullscreenActionText()
{
    if (!m_fullscreenAction)
    {
        return;
    }
    m_fullscreenAction->setText(isFullScreen() ? QStringLiteral("Windowed")
                                               : QStringLiteral("Fullscreen"));
    m_fullscreenAction->setToolTip(QStringLiteral("Toggle fullscreen mode (F11)"));
}

void MainWindow::showSettingsDialog()
{
    MainWindowSettings(*this).show();
}

void MainWindow::setupLogging()
{
    const QString logFile = runtimePath(QStringLiteral("logs/sc2_data_helper.log"));
    QDir().mkpath(QFileInfo(logFile).absolutePath());

    auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logFile.toStdString(), 1024 * 1024 * 5, 3);
    auto uiSink = std::make_shared<spdlog::sinks::callback_sink_mt>(
        [this](const spdlog::details::log_msg &msg)
        {
            const QString payload = QString::fromUtf8(msg.payload.data(), static_cast<int>(msg.payload.size()));
            const QString line = QStringLiteral("[%1] %2").arg(levelToString(msg.level), payload);
            QMetaObject::invokeMethod(m_logPanel, [this, line]()
                                      { m_logPanel->appendMessage(line); }, Qt::QueuedConnection);
        });

    std::vector<spdlog::sink_ptr> sinks{fileSink, uiSink};
    m_logger = std::make_shared<spdlog::logger>("sc2dh", sinks.begin(), sinks.end());
    m_logger->set_level(spdlog::level::info);
    m_logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(m_logger);
    logLine(QStringLiteral("Logging to %1").arg(logFile));
}

void MainWindow::setupTheme()
{
    QString loadedFrom;
    QString errorMessage;
    if (ThemeManager::applyDarkTheme(qApp, &loadedFrom, &errorMessage))
    {
        logLine(QStringLiteral("Theme loaded from %1").arg(loadedFrom));
    }
    else
    {
        logLine(QStringLiteral("Theme warning: %1").arg(errorMessage));
        statusBar()->showMessage(QStringLiteral("Theme warning: %1").arg(errorMessage), 10000);
    }
}

void MainWindow::loadDefaultFolder()
{
    QSettings settings;
    const QString lastSource = settings.value(QStringLiteral("paths/lastSourcePath")).toString();
    const QFileInfo lastInfo(lastSource);
    const QString folder = !lastSource.isEmpty() && lastInfo.exists() ? lastSource : defaultTestFolder();
    const QFileInfo info(folder);
    m_rootFolder = info.isDir() ? folder : info.absolutePath();
    if (info.isDir())
        m_sourceKind = collectArchiveFiles(folder).isEmpty() ? SourceKind::Folder : SourceKind::ArchiveFolder;
    else if (info.suffix().compare(QStringLiteral("xml"), Qt::CaseInsensitive) == 0)
        m_sourceKind = SourceKind::XmlFile;
    else if (isSupportedSc2Archive(info))
        m_sourceKind = SourceKind::ArchiveFile;
    else
        m_sourceKind = SourceKind::Folder;
    m_pathEdit->setText(folder);
    m_analysisPage->setFolderPath(folder);
    m_analysisPage->setModeLabel(QStringLiteral("Mode: ready to analyze"));
    m_analysisPage->setOutputText(QStringLiteral("Loaded last path. Press Analyze to scan it."));
    setCurrentSourcePath(folder);
    if (QFileInfo::exists(folder))
    {
        logLine(QStringLiteral("Initial path set: %1").arg(folder));
    }
    else
    {
        logLine(QStringLiteral("Initial path does not exist yet: %1").arg(folder));
    }
}

QString MainWindow::runtimePath(const QString &relativePath) const
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/") + relativePath;
}

bool MainWindow::validateArchiveCatalogSchema(const QString &archivePath, QString *errorMessage) const
{
    const QString scriptPath = schemaValidatorScriptPath();
    if (scriptPath.isEmpty())
    {
        logLine(QStringLiteral("XSD validation skipped: scripts/validate_sc2_catalogs.py was not found."));
        return true;
    }

    const QString xsdPath = catalogXsdPath();
    if (xsdPath.isEmpty())
    {
        logLine(QStringLiteral("XSD validation skipped: resources/catalogsData.xsd was not found."));
        return true;
    }

    QProcess process;
    process.setProgram(QStringLiteral("python"));
    process.setArguments({
        scriptPath,
        archivePath,
        QStringLiteral("--xsd"),
        xsdPath,
        QStringLiteral("--max-errors"),
        QStringLiteral("16")
    });
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    process.setProcessEnvironment(env);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();

    if (!process.waitForStarted(5000))
    {
        logLine(QStringLiteral("XSD validation skipped: Python could not be started."));
        return true;
    }

    if (!process.waitForFinished(180000))
    {
        process.kill();
        process.waitForFinished(5000);
        logLine(QStringLiteral("XSD validation warning: timed out before archive save; saving anyway."));
        return true;
    }

    const QString output = compactProcessOutput(QString::fromUtf8(process.readAll()));
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    {
        Q_UNUSED(errorMessage);
        logLine(QStringLiteral("XSD validation warning: failed before archive save; saving anyway.\n%1").arg(output));
        return true;
    }

    logLine(output.isEmpty()
                ? QStringLiteral("XSD catalog validation passed.")
                : QStringLiteral("XSD catalog validation passed: %1").arg(output.section(QLatin1Char('\n'), -1)));
    return true;
}

void MainWindow::logLine(const QString &line) const
{
    if (m_logger)
    {
        m_logger->info("{}", line.toStdString());
    }
    else if (m_logPanel)
    {
        m_logPanel->appendMessage(line);
    }
}

void MainWindow::setCurrentSourcePath(const QString &path)
{
    m_currentSourcePath = path;
    if (m_pathEdit)
    {
        m_pathEdit->setText(path);
        m_pathEdit->setCursorPosition(0);
    }
    if (!path.isEmpty())
    {
        statusBar()->showMessage(QStringLiteral("Loaded path: %1").arg(path));
    }
}

void MainWindow::openSc2File()
{
    SourceSelectionController(*this).openSc2File();
}

void MainWindow::openSourceFolder()
{
    SourceSelectionController(*this).openSourceFolder();
}

void MainWindow::analyzeFolder()
{
    MainWindowAnalysisController(*this).analyzeCurrentSource();
}

bool MainWindow::loadPathAndAnalyze(const QString &path)
{
    return MainWindowAnalysisController(*this).loadPathAndAnalyze(path);
}

bool MainWindow::analyzeFolderPath(const QString &folderPath, QString *errorMessage)
{
    m_archiveReferencedIds.clear();
    m_archiveStrongReferenceSources.clear();
    m_archiveWeakReferenceSources.clear();
    m_archiveReferenceScanComplete = false;
    return m_analyzer.analyzeFolder(folderPath, m_whitelistIds, &m_result, errorMessage, [this](int current, int total, const QString &file)
                                    {
            if (!m_activeProgressDialog) return;
            const int percent = total > 0 ? 22 + (current * 62 / total) : 22;
            m_activeProgressDialog->setProgress(percent, QStringLiteral("Scanning XML and data files"),
                                                file.isEmpty() ? QStringLiteral("Finalizing scan") : QDir::toNativeSeparators(file));
            QApplication::processEvents(); }, [this]
                                    { return m_activeProgressDialog && m_activeProgressDialog->isCancelled(); });
}

bool MainWindow::analyzeArchiveFolderPath(const QString &folderPath, QString *errorMessage)
{
    m_archiveReferencedIds.clear();
    m_archiveStrongReferenceSources.clear();
    m_archiveWeakReferenceSources.clear();
    m_archiveReferenceScanComplete = false;

    const QStringList archives = collectArchiveFiles(folderPath);
    if (archives.isEmpty())
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("No SC2 map/mod archives found in folder: %1").arg(folderPath);
        return false;
    }

    m_result = AnalysisResult{};
    m_result.rootFolder = folderPath;
    XmlLoader loader;
    QHash<QString, QStringList> entriesByArchive;
    int xmlEntriesFound = 0;
    int xmlEntriesLoaded = 0;

    for (int archiveIndex = 0; archiveIndex < archives.size(); ++archiveIndex)
    {
        const QString &archivePath = archives[archiveIndex];
        const QString archiveRelative = QDir(folderPath).relativeFilePath(archivePath).replace('\\', '/');
        if (m_activeProgressDialog)
        {
            if (m_activeProgressDialog->isCancelled())
            {
                if (errorMessage)
                    *errorMessage = QStringLiteral("Analysis canceled.");
                return false;
            }
            m_activeProgressDialog->setProgress(22 + (archiveIndex * 35 / qMax(1, archives.size())),
                                                QStringLiteral("Opening SC2 archives"), archiveRelative);
            QApplication::processEvents();
        }

        Sc2Archive archive;
        QString archiveError;
        if (!archive.load(archivePath, &archiveError))
        {
            if (errorMessage)
                *errorMessage = QStringLiteral("%1: %2").arg(archivePath, archiveError);
            return false;
        }

        const QStringList entries = archive.allEntries();
        entriesByArchive.insert(archivePath, entries);
        logLine(QStringLiteral("Archive folder entry count: %1 -> %2").arg(archiveRelative).arg(entries.size()));

        QStringList xmlEntries;
        for (const QString &entry : entries)
            if (entry.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive))
                xmlEntries.append(entry);
        xmlEntriesFound += xmlEntries.size();

        for (int entryIndex = 0; entryIndex < xmlEntries.size(); ++entryIndex)
        {
            const QString &entryName = xmlEntries[entryIndex];
            if (m_activeProgressDialog)
            {
                if (m_activeProgressDialog->isCancelled())
                {
                    if (errorMessage)
                        *errorMessage = QStringLiteral("Analysis canceled.");
                    return false;
                }
                m_activeProgressDialog->setProgress(30 + (archiveIndex * 40 / qMax(1, archives.size())),
                                                    QStringLiteral("Extracting archive XML"),
                                                    QStringLiteral("%1::%2").arg(archiveRelative, entryName));
                QApplication::processEvents();
            }

            QByteArray xmlBytes;
            QString readError;
            const QString sourceName = QStringLiteral("%1::%2").arg(archiveRelative, entryName);
            if (!archive.readEntry(entryName, &xmlBytes, &readError))
            {
                ParseErrorInfo error;
                error.filePath = sourceName;
                error.message = readError;
                m_result.parseErrors.append(error);
                continue;
            }

            ScannedFileInfo scanned;
            scanned.filePath = sourceName;
            scanned.isXml = true;
            scanned.isSc2DataLike = true;
            scanned.size = xmlBytes.size();
            m_result.scannedFiles.append(scanned);
            m_result.sourceXmlByFile.insert(sourceName, QString::fromUtf8(xmlBytes));

            QVector<DataNode> fileNodes;
            QString parseError;
            if (!loader.extractNodes(sourceName, xmlBytes, &fileNodes, &parseError))
            {
                ParseErrorInfo error;
                error.filePath = sourceName;
                error.message = parseError;
                m_result.parseErrors.append(error);
                continue;
            }
            m_result.nodes += fileNodes;
            ++xmlEntriesLoaded;
        }
    }

    logLine(QStringLiteral("Archive folder scan: archives=%1, XML entries=%2, XML loaded=%3")
                .arg(archives.size())
                .arg(xmlEntriesFound)
                .arg(xmlEntriesLoaded));
    if (m_result.sourceXmlByFile.isEmpty())
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("No XML files found inside SC2 archives in folder: %1").arg(folderPath);
        return false;
    }

    if (!m_analyzer.finalizeAnalysisResult(&m_result, m_whitelistIds, errorMessage, [this]
                                           {
            if (!m_activeProgressDialog)
                return;
            m_activeProgressDialog->setProgress(82,
                                                QStringLiteral("Analyzing extracted XML"),
                                                QStringLiteral("Building references, duplicate groups and candidates"));
            QApplication::processEvents(); }, [this]
                                           { return m_activeProgressDialog && m_activeProgressDialog->isCancelled(); }))
    {
        return false;
    }

    QSet<QString> knownIds;
    for (const DataNode &node : m_result.nodes)
        if (!node.id.isEmpty())
            knownIds.insert(node.id);

    m_archiveReferenceScanComplete = true;
    int scannedReferenceEntries = 0;
    int strongReferenceEntries = 0;
    int weakReferenceEntries = 0;
    for (int archiveIndex = 0; archiveIndex < archives.size(); ++archiveIndex)
    {
        const QString &archivePath = archives[archiveIndex];
        const QString archiveRelative = QDir(folderPath).relativeFilePath(archivePath).replace('\\', '/');
        Sc2Archive archive;
        QString archiveError;
        if (!archive.load(archivePath, &archiveError))
        {
            m_archiveReferenceScanComplete = false;
            logLine(QStringLiteral("Archive reference scan failed to reopen %1: %2").arg(archiveRelative, archiveError));
            continue;
        }

        const QStringList entries = entriesByArchive.value(archivePath, archive.allEntries());
        for (const QString &entry : entries)
        {
            const ArchiveReferenceStrength strength = archiveReferenceStrength(entry);
            if (entry.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive) || strength == ArchiveReferenceStrength::None)
                continue;
            if (m_activeProgressDialog)
            {
                if (m_activeProgressDialog->isCancelled())
                {
                    if (errorMessage)
                        *errorMessage = QStringLiteral("Analysis canceled.");
                    return false;
                }
                m_activeProgressDialog->setProgress(86,
                                                    QStringLiteral("Checking archive references"),
                                                    QStringLiteral("%1::%2").arg(archiveRelative, entry));
                QApplication::processEvents();
            }

            QByteArray bytes;
            QString readError;
            if (!archive.readEntry(entry, &bytes, &readError))
            {
                if (strength == ArchiveReferenceStrength::Strong)
                    m_archiveReferenceScanComplete = false;
                logLine(QStringLiteral("Archive reference scan failed for %1::%2: %3").arg(archiveRelative, entry, readError));
                continue;
            }
            const QString sourceLabel = QStringLiteral("%1::%2 [%3]")
                                            .arg(archiveRelative,
                                                 entry,
                                                 strength == ArchiveReferenceStrength::Strong
                                                     ? QStringLiteral("map/trigger/script")
                                                     : QStringLiteral("metadata/text"));
            if (strength == ArchiveReferenceStrength::Strong)
            {
                collectKnownIdTokenSources(bytes, knownIds, sourceLabel, &m_archiveStrongReferenceSources);
                ++strongReferenceEntries;
            }
            else
            {
                collectKnownIdTokenSources(bytes, knownIds, sourceLabel, &m_archiveWeakReferenceSources);
                ++weakReferenceEntries;
            }
            ++scannedReferenceEntries;
        }
    }

    for (auto it = m_archiveStrongReferenceSources.begin(); it != m_archiveStrongReferenceSources.end(); ++it)
    {
        it.value().removeDuplicates();
        m_archiveReferencedIds.insert(it.key());
    }
    for (auto it = m_archiveWeakReferenceSources.begin(); it != m_archiveWeakReferenceSources.end(); ++it)
        it.value().removeDuplicates();
    logLine(QStringLiteral("Archive folder reference-bearing entries scanned: %1; strong entries: %2; weak entries: %3; strong IDs: %4; weak IDs: %5")
                .arg(scannedReferenceEntries)
                .arg(strongReferenceEntries)
                .arg(weakReferenceEntries)
                .arg(m_archiveStrongReferenceSources.size())
                .arg(m_archiveWeakReferenceSources.size()));

    normalizeArchiveAnalysis(&m_result, QString(), folderPath);
    return true;
}

bool MainWindow::analyzeXmlFile(const QString &filePath, QString *errorMessage)
{
    m_archiveReferencedIds.clear();
    m_archiveStrongReferenceSources.clear();
    m_archiveWeakReferenceSources.clear();
    m_archiveReferenceScanComplete = false;
    if (m_activeProgressDialog && m_activeProgressDialog->isCancelled())
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("Analysis canceled.");
        return false;
    }
    m_result = AnalysisResult{};
    m_result.rootFolder = filePath;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Unable to open XML file: %1").arg(filePath);
        }
        return false;
    }

    const QByteArray xmlBytes = file.readAll();
    file.close();

    ScannedFileInfo scanned;
    scanned.filePath = filePath;
    scanned.isXml = true;
    scanned.isSc2DataLike = true;
    scanned.size = QFileInfo(filePath).size();
    m_result.scannedFiles.append(scanned);
    m_result.sourceXmlByFile.insert(filePath, QString::fromUtf8(xmlBytes));

    XmlLoader loader;
    QVector<DataNode> nodes;
    QString parseError;
    if (!loader.extractNodes(filePath, xmlBytes, &nodes, &parseError))
    {
        ParseErrorInfo error;
        error.filePath = filePath;
        error.message = parseError;
        m_result.parseErrors.append(error);
        if (errorMessage)
        {
            *errorMessage = parseError;
        }
        return false;
    }

    m_result.nodes = nodes;
    m_analyzer.populateReferenceIds(&m_result);
    m_result.analysisReportText = m_analyzer.buildAnalysisReport(m_result);
    m_result.plannedChangesReportText = m_analyzer.buildDryRunReport(m_result, QVector<int>{});
    return true;
}

bool MainWindow::analyzeArchiveFile(const QString &filePath, QString *errorMessage)
{
    m_archiveReferencedIds.clear();
    m_archiveStrongReferenceSources.clear();
    m_archiveWeakReferenceSources.clear();
    m_archiveReferenceScanComplete = false;
    Sc2Archive archive;
    QString archiveError;
    if (!archive.load(filePath, &archiveError))
    {
        if (errorMessage)
        {
            *errorMessage = archiveError.isEmpty()
                                ? QStringLiteral("Archive open failed. Check libzip/MPQ support.")
                                : archiveError;
        }
        return false;
    }

    QStringList xmlEntries;
    for (const QString &entry : archive.allEntries())
        if (entry.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive))
            xmlEntries.append(entry);
    logLine(QStringLiteral("Archive entries count: %1").arg(archive.totalEntriesCount()));
    logLine(QStringLiteral("Matched XML count: %1").arg(xmlEntries.size()));
    if (xmlEntries.isEmpty())
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("No XML files found inside archive");
        }
        return false;
    }

    m_result = AnalysisResult{};
    m_result.rootFolder = filePath;
    XmlLoader loader;

    for (int entryIndex = 0; entryIndex < xmlEntries.size(); ++entryIndex)
    {
        const QString &entryName = xmlEntries[entryIndex];
        if (m_activeProgressDialog)
        {
            if (m_activeProgressDialog->isCancelled())
            {
                if (errorMessage)
                    *errorMessage = QStringLiteral("Analysis canceled.");
                return false;
            }
            m_activeProgressDialog->setProgress(22 + (entryIndex * 35 / qMax(1, xmlEntries.size())),
                                                QStringLiteral("Extracting archive XML"), entryName);
            QApplication::processEvents();
        }
        QByteArray xmlBytes;
        QString readError;
        if (!archive.readEntry(entryName, &xmlBytes, &readError))
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("%1: %2").arg(entryName, readError);
            }
            return false;
        }

        ScannedFileInfo scanned;
        scanned.filePath = entryName;
        scanned.isXml = true;
        scanned.isSc2DataLike = true;
        scanned.size = xmlBytes.size();
        m_result.scannedFiles.append(scanned);
        m_result.sourceXmlByFile.insert(entryName, QString::fromUtf8(xmlBytes));

        QVector<DataNode> fileNodes;
        QString parseError;
        if (!loader.extractNodes(entryName, xmlBytes, &fileNodes, &parseError))
        {
            ParseErrorInfo error;
            error.filePath = entryName;
            error.message = parseError;
            m_result.parseErrors.append(error);
            continue;
        }
        m_result.nodes += fileNodes;
    }

    for (const QString &entryName : archive.allEntries())
    {
        if (entryName.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)
            || !archiveEntryShouldMaterializeForCleanup(entryName))
            continue;
        ScannedFileInfo scanned;
        scanned.filePath = entryName;
        scanned.isXml = false;
        scanned.isSc2DataLike = archiveReferenceStrength(entryName) != ArchiveReferenceStrength::None;
        scanned.size = 0;
        m_result.scannedFiles.append(scanned);
    }

    if (!m_analyzer.finalizeAnalysisResult(&m_result, m_whitelistIds, errorMessage, [this]
                                           {
            if (!m_activeProgressDialog)
                return;
            m_activeProgressDialog->setProgress(85,
                                                QStringLiteral("Analyzing extracted XML"),
                                                QStringLiteral("Building references, duplicate groups and candidates"));
            QApplication::processEvents(); }, [this]
                                           { return m_activeProgressDialog && m_activeProgressDialog->isCancelled(); }))
    {
        return false;
    }

    QSet<QString> knownIds;
    for (const DataNode &node : m_result.nodes)
        if (!node.id.isEmpty())
            knownIds.insert(node.id);
    m_archiveReferenceScanComplete = true;
    const QStringList archiveEntries = archive.allEntries();
    int scannedReferenceEntries = 0;
    int strongReferenceEntries = 0;
    int weakReferenceEntries = 0;
    for (int index = 0; index < archiveEntries.size(); ++index)
    {
        const QString &entry = archiveEntries[index];
        const ArchiveReferenceStrength strength = archiveReferenceStrength(entry);
        if (entry.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive) || strength == ArchiveReferenceStrength::None)
            continue;
        if (m_activeProgressDialog)
        {
            if (m_activeProgressDialog->isCancelled())
            {
                if (errorMessage)
                    *errorMessage = QStringLiteral("Analysis canceled.");
                return false;
            }
            m_activeProgressDialog->setProgress(86, QStringLiteral("Checking archive references"), entry);
            QApplication::processEvents();
        }
        QByteArray bytes;
        QString readError;
        if (!archive.readEntry(entry, &bytes, &readError))
        {
            if (strength == ArchiveReferenceStrength::Strong)
                m_archiveReferenceScanComplete = false;
            logLine(QStringLiteral("Archive reference scan failed for %1: %2").arg(entry, readError));
            continue;
        }
        const QString sourceLabel = QStringLiteral("%1 [%2]")
                                        .arg(entry,
                                             strength == ArchiveReferenceStrength::Strong
                                                 ? QStringLiteral("map/trigger/script")
                                                 : QStringLiteral("metadata/text"));
        if (strength == ArchiveReferenceStrength::Strong)
        {
            collectKnownIdTokenSources(bytes, knownIds, sourceLabel, &m_archiveStrongReferenceSources);
            ++strongReferenceEntries;
        }
        else
        {
            collectKnownIdTokenSources(bytes, knownIds, sourceLabel, &m_archiveWeakReferenceSources);
            ++weakReferenceEntries;
        }
        ++scannedReferenceEntries;
    }
    for (auto it = m_archiveStrongReferenceSources.begin(); it != m_archiveStrongReferenceSources.end(); ++it)
    {
        it.value().removeDuplicates();
        m_archiveReferencedIds.insert(it.key());
    }
    for (auto it = m_archiveWeakReferenceSources.begin(); it != m_archiveWeakReferenceSources.end(); ++it)
        it.value().removeDuplicates();
    logLine(QStringLiteral("Archive reference-bearing entries scanned: %1; strong entries: %2; weak entries: %3; strong IDs: %4; weak IDs: %5")
                .arg(scannedReferenceEntries)
                .arg(strongReferenceEntries)
                .arg(weakReferenceEntries)
                .arg(m_archiveStrongReferenceSources.size())
                .arg(m_archiveWeakReferenceSources.size()));

    normalizeArchiveAnalysis(&m_result, QString(), filePath);
    return true;
}

void MainWindow::normalizeArchiveAnalysis(AnalysisResult *analysis, const QString &tempRoot,
                                          const QString &archivePath) const
{
    if (!analysis)
        return;
    if (!tempRoot.isEmpty())
    {
        QHash<QString, QString> relativeXml;
        for (auto it = analysis->sourceXmlByFile.cbegin(); it != analysis->sourceXmlByFile.cend(); ++it)
            relativeXml.insert(QDir(tempRoot).relativeFilePath(it.key()), it.value());
        analysis->sourceXmlByFile = relativeXml;
        for (ScannedFileInfo &file : analysis->scannedFiles)
            file.filePath = QDir(tempRoot).relativeFilePath(file.filePath);
        for (DataNode &node : analysis->nodes)
            node.sourceFile = QDir(tempRoot).relativeFilePath(node.sourceFile);
    }
    analysis->rootFolder = archivePath;
    applyArchiveReferenceSafety(analysis);
    analysis->analysisReportText = m_analyzer.buildAnalysisReport(*analysis);
    analysis->plannedChangesReportText = m_analyzer.buildDryRunReport(*analysis, QVector<int>{});
}

void MainWindow::applyArchiveReferenceSafety(AnalysisResult *analysis) const
{
    if (!analysis)
        return;
    analysis->possibleUnusedNodeIndices.clear();
    for (UnusedCandidateInfo &candidate : analysis->unusedCandidates)
    {
        if (candidate.state != CandidateState::Safe)
            continue;
        QString id;
        if (candidate.nodeIndex >= 0 && candidate.nodeIndex < analysis->nodes.size())
            id = analysis->nodes[candidate.nodeIndex].id;
        const QStringList strongSources = m_archiveStrongReferenceSources.value(id);
        const QStringList weakSources = m_archiveWeakReferenceSources.value(id);
        if (!strongSources.isEmpty())
            candidate.externalReferenceSources.append(strongSources);
        if (!weakSources.isEmpty())
            candidate.externalReferenceSources.append(weakSources);
        candidate.externalReferenceSources.removeDuplicates();

        if (!m_archiveReferenceScanComplete || !strongSources.isEmpty())
        {
            candidate.state = CandidateState::Blocked;
            candidate.usageState = UsageState::Blocked;
            candidate.protectedObject = true;
            candidate.reason = !m_archiveReferenceScanComplete
                                   ? QStringLiteral("Archive reference scan was incomplete")
                                   : QStringLiteral("Referenced by archive placement, trigger, or script data: %1")
                                         .arg(strongSources.mid(0, 6).join(QStringLiteral(", ")));
            candidate.riskLevel = QStringLiteral("high");
            if (candidate.nodeIndex >= 0 && candidate.nodeIndex < analysis->nodes.size())
                analysis->nodes[candidate.nodeIndex].candidateUnused = false;
        }
        else
        {
            if (!weakSources.isEmpty())
            {
                candidate.reason += QStringLiteral("; weak archive metadata/text token: %1")
                                        .arg(weakSources.mid(0, 6).join(QStringLiteral(", ")));
                if (candidate.riskLevel == QStringLiteral("low"))
                    candidate.riskLevel = QStringLiteral("medium");
            }
            analysis->possibleUnusedNodeIndices.append(candidate.nodeIndex);
        }
    }
}

bool MainWindow::materializeArchiveAnalysis(const QString &tempRoot, AnalysisResult *analysis, QString *errorMessage) const
{
    if (!analysis || m_sourceKind != SourceKind::ArchiveFile)
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("Archive analysis is not available.");
        return false;
    }
    *analysis = m_result;
    QHash<QString, QString> absoluteSources;
    for (auto it = m_result.sourceXmlByFile.cbegin(); it != m_result.sourceXmlByFile.cend(); ++it)
    {
        const QString relative = QDir::cleanPath(it.key()).replace('\\', '/');
        if (relative.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(relative))
        {
            if (errorMessage)
                *errorMessage = QStringLiteral("Unsafe archive entry path: %1").arg(it.key());
            return false;
        }
        const QString target = QDir(tempRoot).absoluteFilePath(relative);
        QDir().mkpath(QFileInfo(target).absolutePath());
        QSaveFile file(target);
        const QByteArray bytes = it.value().toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        {
            if (errorMessage)
                *errorMessage = QStringLiteral("Unable to materialize archive XML: %1").arg(relative);
            return false;
        }
        absoluteSources.insert(target, it.value());
    }
    Sc2Archive archive;
    QString archiveError;
    if (archive.load(m_currentSourcePath, &archiveError))
    {
        QByteArray listfileBytes;
        if (!archive.readEntry(QStringLiteral("(listfile)"), &listfileBytes, &archiveError))
        {
            QStringList entries;
            for (QString entry : archive.allEntries())
                entries << entry.replace('/', '\\');
            listfileBytes = entries.join(QStringLiteral("\r\n")).toUtf8() + QByteArrayLiteral("\r\n");
        }
        QSaveFile listfile(QDir(tempRoot).absoluteFilePath(QStringLiteral("(listfile)")));
        if (!listfile.open(QIODevice::WriteOnly) || listfile.write(listfileBytes) != listfileBytes.size() || !listfile.commit())
        {
            if (errorMessage)
                *errorMessage = QStringLiteral("Unable to materialize archive (listfile).");
            return false;
        }
        for (const QString &entry : archive.allEntries())
        {
            if (entry.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)
                || !archiveEntryShouldMaterializeForCleanup(entry))
                continue;
            const QString relative = QDir::cleanPath(entry).replace('\\', '/');
            if (relative.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(relative))
            {
                if (errorMessage)
                    *errorMessage = QStringLiteral("Unsafe archive entry path: %1").arg(entry);
                return false;
            }
            QByteArray bytes;
            QString readError;
            if (!archive.readEntry(entry, &bytes, &readError))
                continue;
            const QString target = QDir(tempRoot).absoluteFilePath(relative);
            QDir().mkpath(QFileInfo(target).absolutePath());
            QSaveFile file(target);
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            {
                if (errorMessage)
                    *errorMessage = QStringLiteral("Unable to materialize archive import: %1").arg(relative);
                return false;
            }
        }
    }
    for (ScannedFileInfo &file : analysis->scannedFiles)
        file.filePath = QDir(tempRoot).absoluteFilePath(QDir::cleanPath(file.filePath));
    for (DataNode &node : analysis->nodes)
        node.sourceFile = QDir(tempRoot).absoluteFilePath(QDir::cleanPath(node.sourceFile));
    for (DeepCleanupCandidate &candidate : analysis->deepCleanupCandidates)
        candidate.filePath = QDir(tempRoot).absoluteFilePath(QDir::cleanPath(candidate.filePath));
    analysis->sourceXmlByFile = absoluteSources;
    analysis->rootFolder = tempRoot;
    return true;
}

bool MainWindow::commitArchiveChanges(const QString &tempRoot, const QStringList &changedFiles,
                                      QString *backupPath, QString *errorMessage,
                                      const QStringList &removedFiles) const
{
    if (changedFiles.isEmpty() && removedFiles.isEmpty())
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("No archive entries changed.");
        return false;
    }
    Sc2Archive archive;
    if (!archive.load(m_currentSourcePath, errorMessage))
        return false;
    QHash<QString, QByteArray> replacements;
    for (const QString &relativeFile : changedFiles)
    {
        QString normalized = QDir::cleanPath(relativeFile).replace('\\', '/');
        QString archiveName;
        for (const QString &entry : archive.allEntries())
        {
            if (QDir::cleanPath(entry).replace('\\', '/').compare(normalized, Qt::CaseInsensitive) == 0)
            {
                archiveName = entry;
                break;
            }
        }
        if (archiveName.isEmpty())
            archiveName = normalized.replace('/', '\\');
        QFile file(QDir(tempRoot).absoluteFilePath(relativeFile));
        if (!file.open(QIODevice::ReadOnly))
        {
            if (errorMessage)
                *errorMessage = QStringLiteral("Unable to read changed XML: %1").arg(relativeFile);
            return false;
        }
        QByteArray replacementBytes = file.readAll();
        if (normalized.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive))
        {
            QByteArray originalBytes;
            QString readError;
            if (archive.readEntry(archiveName, &originalBytes, &readError))
            {
                DataCollectionPreservationReport preservationReport;
                if (!restoreMissingDataCollectionRecords(originalBytes, &replacementBytes, &preservationReport, errorMessage))
                    return false;
            }
        }
        replacements.insert(archiveName, replacementBytes);
    }
    QStringList removedEntries;
    for (const QString &relativeFile : removedFiles)
    {
        QString normalized = QDir::cleanPath(relativeFile).replace('\\', '/');
        QString archiveName;
        for (const QString &entry : archive.allEntries())
        {
            if (QDir::cleanPath(entry).replace('\\', '/').compare(normalized, Qt::CaseInsensitive) == 0)
            {
                archiveName = entry;
                break;
            }
        }
        if (archiveName.isEmpty())
            archiveName = normalized.replace('/', '\\');
        removedEntries.append(archiveName);
    }
    removedEntries.removeDuplicates();

    int enumRepairCount = 0;
    if (!addCatalogEnumRepairs(archive, &replacements, removedEntries, &enumRepairCount, errorMessage))
        return false;
    if (enumRepairCount > 0)
        logLine(QStringLiteral("Catalog enum self-repair fixed %1 legacy invalid value(s) before archive save.").arg(enumRepairCount));

    BackupManager backupManager;
    QString backup;
    if (!backupManager.createBackup(m_currentSourcePath, &backup, errorMessage))
        return false;
    const QString pending = m_currentSourcePath + QStringLiteral(".sc2dh.pending");
    QFile::remove(pending);
    if (!archive.saveCopy(pending, replacements, removedEntries, errorMessage))
        return false;
    if (!validateArchiveCatalogSchema(pending, errorMessage))
    {
        QFile::remove(pending);
        return false;
    }

    QFile pendingFile(pending);
    if (!pendingFile.open(QIODevice::ReadOnly))
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("Unable to read verified archive copy.");
        QFile::remove(pending);
        return false;
    }
    QSaveFile destination(m_currentSourcePath);
    const QByteArray archiveBytes = pendingFile.readAll();
    pendingFile.close();
    if (!destination.open(QIODevice::WriteOnly) || destination.write(archiveBytes) != archiveBytes.size() || !destination.commit())
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("Unable to atomically replace the archive; original file was preserved.");
        QFile::remove(pending);
        return false;
    }
    QFile::remove(pending);

    // saveCopy already verifies every rewritten entry before returning. The
    // final QSaveFile write is a byte-for-byte atomic copy of that verified
    // archive, so reopening and extracting every entry here was redundant.
    if (backupPath)
        *backupPath = backup;
    return true;
}

void MainWindow::runDryRun()
{
    if (m_result.nodes.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("Optimization"), QStringLiteral("Analyze a folder first."));
        return;
    }

    showDryRunTab();
}

void MainWindow::applySelectedChanges()
{
    if (m_result.nodes.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("Apply"), QStringLiteral("Analyze a folder first."));
        return;
    }

    if (m_sourceKind == SourceKind::ArchiveFile || m_sourceKind == SourceKind::ArchiveFolder)
    {
        QMessageBox::information(this, QStringLiteral("Apply"),
                                 m_sourceKind == SourceKind::ArchiveFolder
                                     ? archiveFolderReadOnlyMessage()
                                     : QStringLiteral("Archive apply is not available in this build. Use folder or XML input."));
        return;
    }

    const QVector<int> selectedRows = m_analysisPage->selectedRows();
    if (selectedRows.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("Apply"), QStringLiteral("No rows selected."));
        return;
    }

    const QString planned = m_analyzer.buildPlannedChangesReport(m_result, selectedRows);
    m_dryRunPage->setPreview(planned);
    showDryRunTab();

    const QString backupHint = backupPrompt(
        QStringLiteral("This will create a backup folder before editing. Continue?"),
        QStringLiteral("Backups are disabled in Settings. Apply selected changes without creating a persistent backup?"));
    if (QMessageBox::question(this, QStringLiteral("Apply Selected Changes"), backupHint) != QMessageBox::Yes)
    {
        logLine(QStringLiteral("Apply canceled by user."));
        return;
    }

    QString backupFolder;
    QString errorMessage;
    QStringList changedFiles;
    int removedNodes = 0;
    int skippedNodes = 0;
    if (!m_analyzer.applySelectedChanges(m_result,
                                         selectedRows,
                                         m_rootFolder,
                                         m_whitelistIds,
                                         &backupFolder,
                                         &errorMessage,
                                         &changedFiles,
                                         &removedNodes,
                                         &skippedNodes))
    {
        QMessageBox::critical(this, QStringLiteral("Apply failed"), errorMessage);
        logLine(QStringLiteral("Apply failed: %1").arg(errorMessage));
        return;
    }

    logLine(QStringLiteral("Backup folder: %1").arg(backupFolder));
    logLine(QStringLiteral("Changed files count: %1").arg(changedFiles.size()));
    logLine(QStringLiteral("Removed nodes count: %1").arg(removedNodes));
    logLine(QStringLiteral("Skipped nodes count: %1").arg(skippedNodes));
    for (int row : selectedRows)
    {
        if (row >= 0 && row < m_result.nodes.size())
        {
            const DataNode &node = m_result.nodes[row];
            if (m_whitelistIds.contains(node.id))
            {
                continue;
            }
            logLine(QStringLiteral("Removed node: %1 | %2 | %3 | %4")
                        .arg(node.sourceFile, node.elementName, node.id, node.originalLocation));
        }
    }
    for (const QString &file : changedFiles)
    {
        logLine(QStringLiteral("Changed file: %1").arg(file));
    }

    analyzeFolder();
    logLine(QStringLiteral("Save result: success"));
    QMessageBox::information(this, QStringLiteral("Apply complete"),
                             QStringLiteral("Backup: %1\nChanged files: %2\nRemoved nodes: %3\nSkipped nodes: %4")
                                 .arg(backupFolder)
                                 .arg(changedFiles.size())
                                 .arg(removedNodes)
                                 .arg(skippedNodes));
}

void MainWindow::previewMerge(const MergeRequest &request)
{
    MergePreview preview;
    if (m_sourceKind == SourceKind::ArchiveFolder)
    {
        m_previewedMerge = request;
        m_mergePreviewValid = false;
        m_duplicatesPage->setPreviewText(archiveFolderReadOnlyMessage(), false);
        return;
    }
    if (m_sourceKind == SourceKind::ArchiveFile)
    {
        QTemporaryDir workspace;
        AnalysisResult materialized;
        QString error;
        if (!workspace.isValid() || !materializeArchiveAnalysis(workspace.path(), &materialized, &error))
        {
            m_duplicatesPage->setPreviewText(QStringLiteral("Archive preview failed: %1").arg(error), false);
            return;
        }
        preview = m_mergeService.preview(materialized, request);
    }
    else
    {
        preview = m_mergeService.preview(m_result, request);
    }
    m_previewedMerge = request;
    m_mergePreviewValid = preview.valid;
    m_duplicatesPage->setPreviewText(preview.reportText.isEmpty() ? preview.warnings.join(QStringLiteral("\n")) : preview.reportText,
                                     preview.valid);
    if (!preview.reportText.isEmpty())
    {
        m_result.analysisReportText = m_analyzer.buildAnalysisReport(m_result) + QStringLiteral("\n") + preview.reportText;
        writeAnalysisReportFile();
    }
}

void MainWindow::applyMerge(const MergeRequest &request)
{
    if (m_sourceKind == SourceKind::ArchiveFolder)
    {
        QMessageBox::information(this, QStringLiteral("Apply Merge"), archiveFolderReadOnlyMessage());
        return;
    }
    const bool sameRequest = request.keepNodeIndex == m_previewedMerge.keepNodeIndex && request.removeNodeIndices == m_previewedMerge.removeNodeIndices;
    if (!m_mergePreviewValid || !sameRequest)
    {
        QMessageBox::warning(this, QStringLiteral("Apply Merge"),
                             QStringLiteral("Preview this exact merge selection before applying it."));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("Apply Merge"),
                              backupPrompt(
                                  QStringLiteral("Create a backup, redirect references, verify, and delete the selected duplicates?"),
                                  QStringLiteral("Backups are disabled in Settings. Redirect references, verify, and delete the selected duplicates without a persistent backup?"))) != QMessageBox::Yes)
        return;

    if (m_sourceKind == SourceKind::ArchiveFile)
    {
        QTemporaryDir workspace;
        AnalysisResult materialized;
        QString error;
        if (!workspace.isValid() || !materializeArchiveAnalysis(workspace.path(), &materialized, &error))
        {
            QMessageBox::critical(this, QStringLiteral("Merge failed"), error);
            return;
        }
        const MergeApplyResult result = m_mergeService.apply(materialized, request, workspace.path(), m_whitelistIds);
        if (!result.success)
        {
            QMessageBox::critical(this, QStringLiteral("Merge failed"), result.error + QStringLiteral("\nThe archive was not changed."));
            return;
        }
        QString archiveBackup;
        if (!commitArchiveChanges(workspace.path(), result.changedFiles, &archiveBackup, &error))
        {
            QMessageBox::critical(this, QStringLiteral("Merge failed"), error + QStringLiteral("\nNo partial archive change was retained."));
            return;
        }
        m_mergePreviewValid = false;
        if (!loadPathAndAnalyze(m_currentSourcePath))
        {
            QMessageBox::warning(this, QStringLiteral("Merge saved"), QStringLiteral("The archive was saved, but automatic re-analysis failed. Backup: %1").arg(archiveBackup));
            return;
        }
        m_dryRunPage->recordMergeResult(result.nodesDeleted, result.referencesRedirected);
        QMessageBox::information(this, QStringLiteral("Merge complete"),
                                 QStringLiteral("Archive backup: %1\nFiles changed: %2\nReferences redirected: %3\nNodes deleted: %4")
                                     .arg(archiveBackup)
                                     .arg(result.changedFiles.size())
                                     .arg(result.referencesRedirected)
                                     .arg(result.nodesDeleted));
        return;
    }
    const MergeApplyResult result = m_mergeService.apply(m_result, request, m_rootFolder, m_whitelistIds);
    if (!result.success)
    {
        QMessageBox::critical(this, QStringLiteral("Merge failed"),
                              result.error + QStringLiteral("\nNo partial merge was retained."));
        logLine(QStringLiteral("Merge failed: %1").arg(result.error));
        return;
    }
    m_mergePreviewValid = false;
    logLine(QStringLiteral("Merge backup: %1").arg(result.backupFolder));
    logLine(QStringLiteral("Merge redirected %1 references and deleted %2 nodes.")
                .arg(result.referencesRedirected)
                .arg(result.nodesDeleted));
    loadPathAndAnalyze(m_currentSourcePath);
    m_dryRunPage->recordMergeResult(result.nodesDeleted, result.referencesRedirected);
    QMessageBox::information(this, QStringLiteral("Merge complete"),
                             QStringLiteral("Backup: %1\nFiles changed: %2\nReferences redirected: %3\nNodes deleted: %4")
                                 .arg(result.backupFolder)
                                 .arg(result.changedFiles.size())
                                 .arg(result.referencesRedirected)
                                 .arg(result.nodesDeleted));
}

void MainWindow::previewUnusedDeletion(const QVector<int> &rows)
{
    m_previewedUnusedRows = rows;
    m_cleanupPage->setPreviewText(m_analyzer.buildDryRunReport(m_result, rows));
}

void MainWindow::applyUnusedDeletion(const QVector<int> &rows)
{
    if (m_sourceKind == SourceKind::ArchiveFolder)
    {
        QMessageBox::information(this, QStringLiteral("Delete Unused Data Objects"), archiveFolderReadOnlyMessage());
        return;
    }
    if (rows.isEmpty() || rows != m_previewedUnusedRows)
    {
        QMessageBox::warning(this, QStringLiteral("Delete Unused Data Objects"),
                             QStringLiteral("Preview this exact selection before deletion."));
        return;
    }
    if (m_sourceKind == SourceKind::ArchiveFile)
    {
        if (!m_archiveReferenceScanComplete)
        {
            QMessageBox::information(this, QStringLiteral("Delete Unused Data Objects"),
                                     QStringLiteral("Blocked: the archive reference scan was incomplete."));
            return;
        }
        if (QMessageBox::question(this, QStringLiteral("Delete Selected Unused Data Objects"),
                                  backupPrompt(
                                      QStringLiteral("Create an archive backup, delete the selected verified data objects, and atomically save the SC2 archive?"),
                                      QStringLiteral("Backups are disabled in Settings. Delete the selected verified data objects and atomically save the SC2 archive without a persistent backup?"))) != QMessageBox::Yes)
            return;
        QTemporaryDir workspace;
        AnalysisResult materialized;
        QString error;
        if (!workspace.isValid() || !materializeArchiveAnalysis(workspace.path(), &materialized, &error))
        {
            QMessageBox::critical(this, QStringLiteral("Deletion failed"), error);
            return;
        }
        QVector<WizardNodeRef> selectedRefs;
        for (int row : rows)
        {
            if (row < 0 || row >= m_result.nodes.size())
                continue;
            const DataNode &node = m_result.nodes[row];
            selectedRefs.append({node.id, node.elementName, node.sourceFile, node.originalLocation});
        }
        if (!m_analyzer.analyzeFolder(workspace.path(), m_whitelistIds, &materialized, &error))
        {
            QMessageBox::critical(this, QStringLiteral("Deletion failed"), error);
            return;
        }
        applyArchiveReferenceSafety(&materialized);
        QVector<int> refreshedRows;
        for (const WizardNodeRef &ref : selectedRefs)
        {
            const int index = findNodeIndex(materialized, ref);
            if (index >= 0)
                refreshedRows.append(index);
        }
        QString workspaceBackup;
        QStringList changedFiles;
        int removed = 0, skipped = 0;
        if (!m_analyzer.applySelectedChanges(materialized, refreshedRows, workspace.path(), m_whitelistIds,
                                             &workspaceBackup, &error, &changedFiles, &removed, &skipped))
        {
            QMessageBox::critical(this, QStringLiteral("Deletion failed"), error);
            return;
        }
        QString archiveBackup;
        if (!commitArchiveChanges(workspace.path(), changedFiles, &archiveBackup, &error))
        {
            QMessageBox::critical(this, QStringLiteral("Deletion failed"), error + QStringLiteral("\nThe original archive was preserved."));
            return;
        }
        m_previewedUnusedRows.clear();
        loadPathAndAnalyze(m_currentSourcePath);
        m_dryRunPage->recordUnusedResult(removed);
        QMessageBox::information(this, QStringLiteral("Deletion complete"),
                                 QStringLiteral("Archive backup: %1\nDeleted: %2\nSkipped: %3")
                                     .arg(archiveBackup)
                                     .arg(removed)
                                     .arg(skipped));
        return;
    }
    QString backupFolder, error;
    QStringList changedFiles;
    int removed = 0, skipped = 0;
    if (QMessageBox::question(this, QStringLiteral("Delete Selected Unused Data Objects"),
                              backupPrompt(
                                  QStringLiteral("A backup will be created before deleting the selected safe data objects. Continue?"),
                                  QStringLiteral("Backups are disabled in Settings. Delete the selected safe data objects without a persistent backup?"))) != QMessageBox::Yes)
        return;
    if (!m_analyzer.applySelectedChanges(m_result, rows, m_rootFolder, m_whitelistIds,
                                         &backupFolder, &error, &changedFiles, &removed, &skipped))
    {
        QMessageBox::critical(this, QStringLiteral("Deletion failed"), error);
        return;
    }
    m_previewedUnusedRows.clear();
    loadPathAndAnalyze(m_currentSourcePath);
    m_dryRunPage->recordUnusedResult(removed);
    QMessageBox::information(this, QStringLiteral("Deletion complete"),
                             QStringLiteral("Backup: %1\nDeleted: %2\nSkipped: %3")
                                 .arg(backupFolder)
                                 .arg(removed)
                                 .arg(skipped));
}

void MainWindow::previewStandardRename(const RenamePlan &plan)
{
    RenamePreviewReport report;
    if (m_sourceKind == SourceKind::ArchiveFile)
    {
        QTemporaryDir workspace;
        AnalysisResult materialized;
        QString error;
        if (!workspace.isValid() || !materializeArchiveAnalysis(workspace.path(), &materialized, &error))
        {
            report.valid = false;
            report.plan = plan;
            report.conflicts << error;
            report.reportText = QStringLiteral("Archive rename preview failed: %1").arg(error);
        }
        else
        {
            report = m_referenceRenamer.preview(materialized, plan);
        }
    }
    else
    {
        report = m_referenceRenamer.preview(m_result, plan);
    }
    m_previewedRenamePlan = plan;
    m_renamePreviewValid = report.valid;
    m_renameIdsPage->setPreviewReport(report);
    if (m_sourceKind == SourceKind::ArchiveFolder)
        m_renameIdsPage->setApplyAvailable(false);
    logLine(QStringLiteral("Rename-to-standard preview: %1 renames, %2 reference updates, valid=%3")
                .arg(report.identitiesRenamed)
                .arg(report.referencesUpdated)
                .arg(report.valid ? QStringLiteral("yes") : QStringLiteral("no")));
}

void MainWindow::applyStandardRename(const RenamePlan &plan)
{
    if (m_sourceKind == SourceKind::ArchiveFolder)
    {
        QMessageBox::information(this, QStringLiteral("Apply Rename"), archiveFolderReadOnlyMessage());
        return;
    }
    const auto signature = [](const RenamePlan &value)
    {
        QStringList parts;
        for (const RenamePlanItem &item : value.items)
            parts << item.oldId + QChar(0x1f) + item.newId;
        std::sort(parts.begin(), parts.end());
        return value.family.rootId + QChar(0x1e) + value.targetRootId + QChar(0x1e) + parts.join(QChar(0x1d));
    };
    if (!m_renamePreviewValid || signature(plan) != signature(m_previewedRenamePlan))
    {
        QMessageBox::warning(this, QStringLiteral("Apply Rename"),
                             QStringLiteral("Preview this exact family and rename selection before applying."));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("Apply Rename"),
                              backupPrompt(
                                  QStringLiteral("Create a backup, rename selected real XML IDs, update references, and verify?"),
                                  QStringLiteral("Backups are disabled in Settings. Rename selected real XML IDs, update references, and verify without a persistent backup?"))) != QMessageBox::Yes)
        return;
    if (m_sourceKind == SourceKind::ArchiveFile)
    {
        QTemporaryDir workspace;
        AnalysisResult materialized;
        QString error;
        if (!workspace.isValid() || !materializeArchiveAnalysis(workspace.path(), &materialized, &error))
        {
            QMessageBox::critical(this, QStringLiteral("Rename failed"), error);
            return;
        }
        const RenameApplyResult result = m_referenceRenamer.apply(materialized, plan, workspace.path(), m_whitelistIds);
        if (!result.success)
        {
            QMessageBox::critical(this, QStringLiteral("Rename failed"), result.error + QStringLiteral("\nThe archive was not changed."));
            return;
        }
        QStringList changedFiles = result.changedFiles;
        sc2dh::ArchiveReferenceRewriteReport archiveRewrite;
        QStringList skippedArchiveRenames;
        const QHash<QString, QString> archiveRenames =
            sc2dh::unambiguousArchiveReferenceRenames(materialized, result.appliedRenames, &skippedArchiveRenames);
        if (!sc2dh::rewriteArchiveReferenceFiles(workspace.path(),
                                                 archiveReferenceFilesForWorkspace(materialized, workspace.path()),
                                                 archiveRenames,
                                                 &archiveRewrite,
                                                 &error))
        {
            QMessageBox::critical(this, QStringLiteral("Rename failed"), error + QStringLiteral("\nThe archive was not changed."));
            return;
        }
        changedFiles.append(archiveRewrite.changedFiles);
        changedFiles.removeDuplicates();
        QString archiveBackup;
        if (!commitArchiveChanges(workspace.path(), changedFiles, &archiveBackup, &error))
        {
            QMessageBox::critical(this, QStringLiteral("Rename failed"), error + QStringLiteral("\nNo partial archive change was retained."));
            return;
        }
        m_renamePreviewValid = false;
        loadPathAndAnalyze(m_currentSourcePath);
        m_dryRunPage->recordRenameResult(result.identitiesRenamed);
        QMessageBox::information(this, QStringLiteral("Rename complete"),
                                 QStringLiteral("Archive backup: %1\nObjects renamed: %2\nReferences updated: %3")
                                     .arg(archiveBackup)
                                     .arg(result.identitiesRenamed)
                                     .arg(result.referencesUpdated + archiveRewrite.replacements));
        return;
    }
    const RenameApplyResult result = m_referenceRenamer.apply(m_result, plan, m_rootFolder, m_whitelistIds);
    if (!result.success)
    {
        QMessageBox::critical(this, QStringLiteral("Rename failed"), result.error + QStringLiteral("\nChanges were rolled back."));
        return;
    }
    m_renamePreviewValid = false;
    loadPathAndAnalyze(m_currentSourcePath);
    m_dryRunPage->recordRenameResult(result.identitiesRenamed);
    QMessageBox::information(this, QStringLiteral("Rename complete"),
                             QStringLiteral("Backup: %1\nObjects renamed: %2\nReferences updated: %3")
                                 .arg(result.backupFolder)
                                 .arg(result.identitiesRenamed)
                                 .arg(result.referencesUpdated));
}

void MainWindow::exportStandardRenameReport(const QString &reportText)
{
    if (reportText.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("Export Rename Report"), QStringLiteral("Preview a rename first."));
        return;
    }
    const QString selected = saveTextFileStyled(this, QStringLiteral("Export Rename Report"),
                                                QDir(m_rootFolder).absoluteFilePath(QStringLiteral("rename_to_standard_preview.txt")));
    if (selected.isEmpty())
        return;
    QSaveFile file(selected);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text) || file.write(reportText.toUtf8()) != reportText.toUtf8().size() || !file.commit())
        QMessageBox::critical(this, QStringLiteral("Export Rename Report"), QStringLiteral("Unable to write %1").arg(selected));
}

void MainWindow::previewDataCollection(const DataCollectionBuildRequest &request)
{
    DataCollectionPreviewReport report;
    if (m_sourceKind == SourceKind::ArchiveFolder)
    {
        report.valid = false;
        report.request = request;
        report.warnings << archiveFolderReadOnlyMessage();
        report.reportText = archiveFolderReadOnlyMessage();
    }
    else if (m_sourceKind == SourceKind::ArchiveFile)
    {
        QTemporaryDir workspace;
        AnalysisResult materialized;
        QString error;
        if (!workspace.isValid() || !materializeArchiveAnalysis(workspace.path(), &materialized, &error))
        {
            report.warnings << error;
            report.reportText = QStringLiteral("Archive Data Collection preview failed: %1").arg(error);
        }
        else
        {
            report = m_dataCollectionBuilder.preview(materialized, request);
        }
    }
    else
    {
        report = m_dataCollectionBuilder.preview(m_result, request);
    }
    m_previewedCollectionRequest = request;
    m_collectionPreviewValid = report.valid;
    m_dataCollectionPage->setPreviewReport(report);
    logLine(QStringLiteral("Data Collection preview: %1 records to add, valid=%2")
                .arg(report.recordsToAdd.size())
                .arg(report.valid ? QStringLiteral("yes") : QStringLiteral("no")));
}

void MainWindow::applyDataCollection(const DataCollectionBuildRequest &request)
{
    if (m_sourceKind == SourceKind::ArchiveFolder)
    {
        QMessageBox::information(this, QStringLiteral("Apply Collection"), archiveFolderReadOnlyMessage());
        return;
    }
    const auto signature = [](const DataCollectionBuildRequest &value)
    {
        QList<int> indices = value.includedNodeIndices.values();
        std::sort(indices.begin(), indices.end());
        QStringList indexText;
        for (int index : indices)
            indexText << QString::number(index);
        return value.family.rootId + QChar(0x1f) + value.requestedUnitId + QChar(0x1f) + value.parent + QChar(0x1f) + value.editorCategories + QChar(0x1f) + indexText.join(QLatin1Char(',')) + QChar(0x1f) + QString::number(value.confirmNonStandard);
    };
    if (!m_collectionPreviewValid || signature(request) != signature(m_previewedCollectionRequest))
    {
        QMessageBox::warning(this, QStringLiteral("Apply Collection"),
                             QStringLiteral("Preview this exact collection selection and field configuration before applying."));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("Apply Collection"),
                              backupPrompt(
                                  QStringLiteral("Create a backup and create or update the typed Data Collection?"),
                                  QStringLiteral("Backups are disabled in Settings. Create or update the typed Data Collection without a persistent backup?"))) != QMessageBox::Yes)
        return;
    if (m_sourceKind == SourceKind::ArchiveFile)
    {
        QTemporaryDir workspace;
        AnalysisResult materialized;
        QString error;
        if (!workspace.isValid() || !materializeArchiveAnalysis(workspace.path(), &materialized, &error))
        {
            QMessageBox::critical(this, QStringLiteral("Collection failed"), error);
            return;
        }
        const DataCollectionApplyResult result = m_dataCollectionBuilder.apply(materialized, request, workspace.path(), m_whitelistIds);
        if (!result.success)
        {
            QMessageBox::critical(this, QStringLiteral("Collection failed"), result.error + QStringLiteral("\nThe archive was not changed."));
            return;
        }
        QString archiveBackup;
        if (!commitArchiveChanges(workspace.path(), result.changedFiles, &archiveBackup, &error))
        {
            QMessageBox::critical(this, QStringLiteral("Collection failed"), error + QStringLiteral("\nNo partial archive change was retained."));
            return;
        }
        m_collectionPreviewValid = false;
        loadPathAndAnalyze(m_currentSourcePath);
        m_dryRunPage->recordCollectionResult(result.recordsAdded, result.recordsRemoved);
        QMessageBox::information(this, QStringLiteral("Collection complete"),
                                 QStringLiteral("Archive backup: %1\nDataCollectionData.xml and (listfile) saved.\nRecords added: %2")
                                     .arg(archiveBackup)
                                     .arg(result.recordsAdded));
        return;
    }
    const DataCollectionApplyResult result = m_dataCollectionBuilder.apply(m_result, request, m_rootFolder, m_whitelistIds);
    if (!result.success)
    {
        QMessageBox::critical(this, QStringLiteral("Collection failed"), result.error + QStringLiteral("\nChanges were rolled back."));
        return;
    }
    m_collectionPreviewValid = false;
    loadPathAndAnalyze(m_currentSourcePath);
    m_dryRunPage->recordCollectionResult(result.recordsAdded, result.recordsRemoved);
    QMessageBox::information(this, QStringLiteral("Collection complete"),
                             QStringLiteral("Backup: %1\nRecords added: %2\nDuplicate records skipped: %3")
                                 .arg(result.backupFolder)
                                 .arg(result.recordsAdded)
                                 .arg(result.duplicatesSkipped));
}

void MainWindow::exportDataCollectionReport(const QString &reportText)
{
    if (reportText.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("Export Collection Report"), QStringLiteral("Preview a collection first."));
        return;
    }
    const QString selected = saveTextFileStyled(this, QStringLiteral("Export Collection Report"),
                                                QDir(m_rootFolder).absoluteFilePath(QStringLiteral("data_collection_preview.txt")));
    if (selected.isEmpty())
        return;
    const QByteArray bytes = reportText.toUtf8();
    QSaveFile file(selected);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text) || file.write(bytes) != bytes.size() || !file.commit())
        QMessageBox::critical(this, QStringLiteral("Export Collection Report"), QStringLiteral("Unable to write %1").arg(selected));
}

void MainWindow::showAnalysisTab()
{
    m_tabs->setCurrentWidget(m_analysisPage);
}

void MainWindow::showDataCollectionTab()
{
    m_tabs->setCurrentWidget(m_dataCollectionPage);
}

void MainWindow::showDuplicatesTab()
{
    m_tabs->setCurrentWidget(m_duplicatesPage);
}

void MainWindow::showCleanupTab()
{
    m_tabs->setCurrentWidget(m_cleanupPage);
}

void MainWindow::showDryRunTab(bool autoBuild)
{
    if (m_result.nodes.isEmpty())
        return;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("optimizationWizardDialog"));
    m_optimizationDialog = &dialog;
    dialog.setWindowTitle(QStringLiteral("SC2 Data Optimization Wizard"));
    dialog.setWindowFlags(dialog.windowFlags() | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
    dialog.setMinimumSize(1200, 760);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    m_dryRunPage->setParent(&dialog);
    m_dryRunPage->show();
    layout->addWidget(m_dryRunPage);
    m_dryRunPage->startWizard(autoBuild);
    dialog.resize(1500, 900);
    dialog.showNormal();
    dialog.exec();
    layout->removeWidget(m_dryRunPage);
    m_dryRunPage->setParent(this);
    m_dryRunPage->hide();
    m_optimizationDialog = nullptr;
}

void MainWindow::setDuplicateMergeEnabled(bool enabled)
{
    if (m_dryRunPage)
        m_dryRunPage->setDuplicateMergeEnabled(enabled);
    if (!m_tabs || !m_duplicatesPage)
        return;
    const int index = m_tabs->indexOf(m_duplicatesPage);
    if (index < 0)
        return;
    if (!enabled && m_tabs->currentWidget() == m_duplicatesPage)
        m_tabs->setCurrentWidget(m_analysisPage);
    m_tabs->setTabVisible(index, enabled);
    m_tabs->setTabEnabled(index, enabled);
    m_tabs->setTabToolTip(index, enabled ? QStringLiteral("Duplicate Merge")
                                         : QStringLiteral("Enable Duplicate Merge in Settings"));
}

int MainWindow::findNodeIndex(const AnalysisResult &analysis, const WizardNodeRef &ref) const
{
    for (int index = 0; index < analysis.nodes.size(); ++index)
    {
        const DataNode &node = analysis.nodes[index];
        if (node.id == ref.id && node.elementName == ref.elementName && node.sourceFile == ref.sourceFile && node.originalLocation == ref.originalLocation)
        {
            return index;
        }
    }
    for (int index = 0; index < analysis.nodes.size(); ++index)
    {
        const DataNode &node = analysis.nodes[index];
        if (node.id == ref.id && node.elementName == ref.elementName)
            return index;
    }
    for (int index = 0; index < analysis.nodes.size(); ++index)
        if (analysis.nodes[index].id == ref.id)
            return index;
    return -1;
}

void MainWindow::applyOptimizationWizardPlan()
{
    if (!m_dryRunPage)
        return;

    const OptimizationWizardSelection selection = m_dryRunPage->currentSelection();
    if (selection.unused.isEmpty() && selection.duplicates.isEmpty() && selection.importCleanup.isEmpty() && selection.deepCleanup.isEmpty()
        && selection.rename.isEmpty() && selection.collection.isEmpty())
    {
        if (m_wizardApplyAutomation)
        {
            finishWizardApplyAutomation(false, QStringLiteral("No optimization items were selected."));
            return;
        }
        showSc2MessageDialog(this,
                             QMessageBox::Information,
                             QStringLiteral("Optimization Wizard"),
                             QStringLiteral("Select at least one item before applying the optimization plan."),
                             QMessageBox::Ok,
                             660);
        return;
    }
    if (m_sourceKind == SourceKind::ArchiveFolder)
    {
        if (m_wizardApplyAutomation)
        {
            finishWizardApplyAutomation(false, archiveFolderReadOnlyMessage());
            return;
        }
        showSc2MessageDialog(this,
                             QMessageBox::Information,
                             QStringLiteral("Optimization Wizard"),
                             archiveFolderReadOnlyMessage(),
                             QMessageBox::Ok,
                             760);
        return;
    }

    if (!m_wizardApplyAutomation
        && showSc2MessageDialog(this,
                                QMessageBox::Question,
                                QStringLiteral("Apply Optimization Plan"),
                                QStringLiteral("Apply the selected optimization steps to files now, then rebuild the preview from the updated data?"),
                                QMessageBox::Yes | QMessageBox::No,
                                660) != QMessageBox::Yes)
    {
        return;
    }

    m_dryRunPage->setApplyingState(true, QStringLiteral("Applying selected optimization steps and saving files...\n\nThe wizard will rebuild the preview from updated files when the batch finishes."));
    AnalysisProgressDialog applyProgress(this);
    applyProgress.setTitleText(QStringLiteral("SC2 DATA APPLY"));
    applyProgress.setCancelVisible(false);
    applyProgress.setProgress(5, QStringLiteral("Preparing apply"), QStringLiteral("Building the selected optimization batch"));
    applyProgress.show();
    QApplication::processEvents();
    const auto updateApplyProgress = [&](int percent, const QString &primary, const QString &secondary = QString())
    {
        if (m_wizardApplyAutomation)
            appendWizardApplyAutomationLog(QStringLiteral("progress %1% | %2 | %3").arg(percent).arg(primary, secondary));
        applyProgress.setProgress(percent, primary, secondary);
        QApplication::processEvents();
    };

    int removedUnused = 0;
    int removedDuplicates = 0;
    int redirectedReferences = 0;
    int importCleanupChanged = 0;
    int deepCleanupChanged = 0;
    int renamedIds = 0;
    int collectionAdded = 0;
    int collectionReorganized = 0;
    QStringList warnings;
    QStringList notes;
    QString failure;
    QString archiveBackup;
    bool archiveAnalysisReady = false;
    int staleRenameRecommendations = 0;
    int renameConflictRecommendations = 0;
    int staleDuplicateRecommendations = 0;
    int staleUnusedRecommendations = 0;
    int dataCollectionUnavailable = 0;
    int dataCollectionNotApplicable = 0;
    int reviewOnlyCleanupSkipped = 0;
    int automaticFollowUpCleanupChanges = 0;
    int serviceSkippedRecommendations = 0;

    const auto reloadWorkingAnalysis = [this](const QString &rootFolder, AnalysisResult *analysis, QString *errorMessage)
    {
        return m_analyzer.analyzeFolder(rootFolder, m_whitelistIds, analysis, errorMessage);
    };
    const auto groupKey = [](const WizardNodeRef &ref)
    {
        return ref.sourceFile + QChar(0x1f) + ref.originalLocation + QChar(0x1f) + ref.elementName + QChar(0x1f) + ref.id;
    };
    const auto makeRenameProgress = [&](int basePercent, int spanPercent) -> ReferenceRenamer::ProgressCallback
    {
        return [&, basePercent, spanPercent](const QString &stage, int index, int total, const QString &file)
        {
            int percent = basePercent;
            if (total > 0)
                percent += (qBound(0, index, total) * spanPercent) / total;
            QString detail;
            if (stage == QStringLiteral("locate"))
                detail = QStringLiteral("Locating XML identities");
            else if (stage == QStringLiteral("rewrite"))
                detail = QStringLiteral("Rewriting XML IDs and references");
            else if (stage == QStringLiteral("backup"))
                detail = QStringLiteral("Creating rename backup");
            else if (stage == QStringLiteral("write"))
                detail = QStringLiteral("Saving renamed XML");
            else if (stage == QStringLiteral("verify"))
                detail = QStringLiteral("Verifying renamed IDs");
            else
                detail = QStringLiteral("Applying rename changes");
            if (!file.isEmpty())
                detail += QStringLiteral(": %1").arg(QFileInfo(file).fileName());
            updateApplyProgress(qBound(basePercent, percent, basePercent + spanPercent),
                                QStringLiteral("Applying rename changes"), detail);
        };
    };
    const auto archiveReferenceFilesForWorkspace = [](const AnalysisResult &analysis, const QString &rootFolder)
    {
        QStringList files;
        const QDir root(rootFolder);
        for (const ScannedFileInfo &file : analysis.scannedFiles)
        {
            if (file.isXml || !file.isSc2DataLike)
                continue;
            QString relative = root.relativeFilePath(file.filePath);
            relative = QDir::cleanPath(relative).replace('\\', '/');
            if (!relative.startsWith(QStringLiteral("../")) && !QDir::isAbsolutePath(relative))
                files << relative;
        }
        files.removeDuplicates();
        return files;
    };
    const auto buildCombinedRenamePlan = [this](const AnalysisResult &analysis,
                                                const QVector<WizardRenameSelection> &renameSelection,
                                                QStringList *planWarnings)
    {
        RenamePlan combined;
        combined.targetRootId = QStringLiteral("Batch");
        if (renameSelection.isEmpty())
            return combined;

        QHash<QString, QVector<WizardNodeRef>> renameByFamily;
        for (const WizardRenameSelection &item : renameSelection)
            renameByFamily[item.familyRootId].append(item.node);
        if (renameByFamily.isEmpty())
            return combined;

        const QVector<UnitFamily> families = UnitFamilyDetector().detect(analysis);
        QHash<QString, UnitFamily> familyByRoot;
        for (const UnitFamily &family : families)
            familyByRoot.insert(family.rootId, family);

        QSet<QString> existingIds;
        for (const DataNode &node : analysis.nodes)
            existingIds.insert(node.id);

        StandardNamePlanner planner;
        QVector<RenamePlanItem> candidates;
        QSet<int> selectedNodes;
        QHash<QString, int> proposedNewCounts;

        for (auto it = renameByFamily.cbegin(); it != renameByFamily.cend(); ++it)
        {
            const auto familyIt = familyByRoot.constFind(it.key());
            if (familyIt == familyByRoot.cend())
            {
                if (planWarnings)
                    *planWarnings << QStringLiteral("Rename recommendation became stale after earlier changes: %1").arg(it.key());
                continue;
            }
            QSet<int> includedNodeIndices;
            for (const WizardNodeRef &ref : it.value())
            {
                const int index = findNodeIndex(analysis, ref);
                if (index >= 0)
                    includedNodeIndices.insert(index);
            }
            if (includedNodeIndices.isEmpty())
                continue;

            const RenamePlan plan = planner.plan(analysis, familyIt.value(), familyIt.value().rootId, includedNodeIndices);
            if (!plan.valid)
            {
                if (planWarnings)
                    *planWarnings << QStringLiteral("Rename recommendation is no longer valid after refresh: %1").arg(it.key());
                continue;
            }
            if (combined.family.rootId.isEmpty())
                combined.family = plan.family;
            combined.warnings.append(plan.warnings);

            for (const RenamePlanItem &item : plan.items)
            {
                if (!item.selected || item.blocked || item.oldId == item.newId)
                    continue;
                if (selectedNodes.contains(item.nodeIndex))
                {
                    if (planWarnings)
                        *planWarnings << QStringLiteral("Duplicate rename recommendation ignored after refresh: %1").arg(item.oldId);
                    continue;
                }
                selectedNodes.insert(item.nodeIndex);
                candidates.append(item);
                ++proposedNewCounts[item.newId.toLower()];
            }
        }

        QVector<RenamePlanItem> filtered;
        filtered.reserve(candidates.size());
        for (const RenamePlanItem &item : candidates)
        {
            if (proposedNewCounts.value(item.newId.toLower()) > 1)
            {
                if (planWarnings)
                    *planWarnings << QStringLiteral("Rename conflict after refresh: %1 -> %2 uses a duplicate target ID.")
                                      .arg(item.oldId, item.newId);
                continue;
            }
            filtered.append(item);
        }

        bool changed = true;
        while (changed)
        {
            changed = false;
            QSet<QString> movingOldIds;
            for (const RenamePlanItem &item : filtered)
                movingOldIds.insert(item.oldId);

            QVector<RenamePlanItem> next;
            next.reserve(filtered.size());
            for (const RenamePlanItem &item : filtered)
            {
                if (existingIds.contains(item.newId) && !movingOldIds.contains(item.newId))
                {
                    if (planWarnings)
                        *planWarnings << QStringLiteral("Rename conflict after refresh: %1 -> %2 target ID is still occupied.")
                                          .arg(item.oldId, item.newId);
                    changed = true;
                    continue;
                }
                next.append(item);
            }
            filtered = next;
        }

        combined.items = filtered;
        combined.valid = !combined.items.isEmpty();
        if (!combined.valid)
            combined.conflicts << QStringLiteral("No safe rename items remained after batch validation.");
        return combined;
    };
    const auto collectionSkipReason = [](const DataCollectionPreviewReport &preview)
    {
        QStringList details = preview.warnings + preview.idConflicts;
        details.removeDuplicates();
        if (details.isEmpty())
            return QStringLiteral("preview is not valid for automatic apply");
        if (details.size() > 4)
            details = details.mid(0, 4) << QStringLiteral("...");
        return details.join(QStringLiteral("; "));
    };
    const auto automaticFollowUpDeepCleanupRows = [](const AnalysisResult &analysis)
    {
        QVector<int> rows;
        for (const DeepCleanupCandidate &candidate : analysis.deepCleanupCandidates)
        {
            if (candidate.kind == DeepCleanupKind::UnusedAsset)
                continue;
            if (candidate.state == CandidateState::Safe
                && candidate.recommended
                && candidate.action != DeepCleanupAction::ReportOnly)
            {
                rows.append(candidate.index);
            }
        }
        return rows;
    };
    const auto deepCleanupChangeCount = [](const DeepCleanupApplyResult &result)
    {
        return result.filesDeleted + result.textLinesRemoved + result.xmlNodesRemoved + result.xmlAttributesRemoved;
    };
    const auto recordRenamePlanNotes = [&](const QStringList &messages)
    {
        for (const QString &message : messages)
        {
            logLine(message);
            if (message.startsWith(QStringLiteral("Rename conflict after refresh:")))
                ++renameConflictRecommendations;
            else
                ++staleRenameRecommendations;
        }
    };
    const auto recordServiceMessages = [&](const QStringList &messages)
    {
        for (const QString &message : messages)
        {
            logLine(QStringLiteral("Optimization service message: %1").arg(message));
            if (message.contains(QStringLiteral("residual old ID token"), Qt::CaseInsensitive)
                || message.startsWith(QStringLiteral("Post-merge verification still sees"), Qt::CaseInsensitive)
                || message.startsWith(QStringLiteral("Post-rename verification reported non-fatal"), Qt::CaseInsensitive)
                || message.contains(QStringLiteral("saved anyway for manual review"), Qt::CaseInsensitive))
            {
                notes << message;
                continue;
            }
            if (message.startsWith(QStringLiteral("Skipped "), Qt::CaseInsensitive))
            {
                ++serviceSkippedRecommendations;
                continue;
            }
            warnings << message;
        }
    };

    if (m_sourceKind == SourceKind::ArchiveFile)
    {
        updateApplyProgress(15, QStringLiteral("Preparing archive workspace"), QStringLiteral("Materializing XML and listfile"));
        QTemporaryDir workspace;
        AnalysisResult current;
        QString error;
        if (!workspace.isValid() || !materializeArchiveAnalysis(workspace.path(), &current, &error))
        {
            failure = error;
        }
        else
        {
            QStringList changedFiles;
            QStringList removedFiles;

            if (!reloadWorkingAnalysis(workspace.path(), &current, &error))
            {
                failure = error;
            }
            else
            {
                applyArchiveReferenceSafety(&current);
            }

            if (failure.isEmpty() && !selection.importCleanup.isEmpty())
            {
                updateApplyProgress(20, QStringLiteral("Applying import cleanup"), QStringLiteral("Removing unused imported assets from the archive workspace"));
                const DeepCleanupApplyResult result = DeepCleanupService().apply(current, selection.importCleanup, workspace.path(), false);
                if (!result.success)
                {
                    failure = result.error;
                }
                else
                {
                    importCleanupChanged += result.filesDeleted;
                    reviewOnlyCleanupSkipped += result.reportOnlySkipped;
                    changedFiles.append(result.changedFiles);
                    removedFiles.append(result.removedFiles);
                    changedFiles.removeDuplicates();
                    removedFiles.removeDuplicates();
                    if (!reloadWorkingAnalysis(workspace.path(), &current, &error))
                    {
                        failure = error;
                    }
                    else
                    {
                        applyArchiveReferenceSafety(&current);
                    }
                }
            }

            if (failure.isEmpty() && !selection.deepCleanup.isEmpty())
            {
                updateApplyProgress(22, QStringLiteral("Applying deep cleanup"), QStringLiteral("Removing stale localization, redundant XML and broken actor events"));
                const DeepCleanupApplyResult result = DeepCleanupService().apply(current, selection.deepCleanup, workspace.path(), false);
                if (!result.success)
                {
                    failure = result.error;
                }
                else
                {
                    deepCleanupChanged += result.filesDeleted + result.textLinesRemoved + result.xmlNodesRemoved + result.xmlAttributesRemoved;
                    reviewOnlyCleanupSkipped += result.reportOnlySkipped;
                    changedFiles.append(result.changedFiles);
                    removedFiles.append(result.removedFiles);
                    changedFiles.removeDuplicates();
                    removedFiles.removeDuplicates();
                    if (!reloadWorkingAnalysis(workspace.path(), &current, &error))
                    {
                        failure = error;
                    }
                    else
                    {
                        applyArchiveReferenceSafety(&current);
                    }
                }
            }

            if (failure.isEmpty() && !selection.unused.isEmpty())
            {
                QVector<int> unusedRows;
                for (const WizardNodeRef &ref : selection.unused)
                {
                    const int index = findNodeIndex(current, ref);
                    if (index >= 0)
                        unusedRows.append(index);
                }
                if (!unusedRows.isEmpty())
                {
                    updateApplyProgress(25, QStringLiteral("Deleting unused data objects"), QStringLiteral("Rewriting verified archive XML"));
                    QString workspaceBackup;
                    QStringList unusedChangedFiles;
                    int removed = 0;
                    int skipped = 0;
                    if (!m_analyzer.applySelectedChanges(current, unusedRows, workspace.path(), m_whitelistIds,
                                                         &workspaceBackup, &error, &unusedChangedFiles, &removed, &skipped))
                    {
                        failure = error;
                    }
                    else
                    {
                        removedUnused += removed;
                        changedFiles.append(unusedChangedFiles);
                        changedFiles.removeDuplicates();
                        if (skipped > 0)
                        {
                            staleUnusedRecommendations += skipped;
                            logLine(QStringLiteral("Unused Data Objects: %1 selected recommendation(s) became stale after earlier changes.").arg(skipped));
                        }
                        if (!reloadWorkingAnalysis(workspace.path(), &current, &error))
                            failure = error;
                    }
                }
            }
            QHash<QString, QPair<WizardNodeRef, QVector<WizardNodeRef>>> mergeGroups;
            for (const WizardMergeSelection &item : selection.duplicates)
            {
                auto &group = mergeGroups[groupKey(item.keep)];
                group.first = item.keep;
                group.second.append(item.remove);
            }
            QVector<MergeRequest> mergeRequests;
            for (auto it = mergeGroups.cbegin(); it != mergeGroups.cend(); ++it)
            {
                const int keepIndex = findNodeIndex(current, it.value().first);
                if (keepIndex < 0)
                {
                    ++staleDuplicateRecommendations;
                    logLine(QStringLiteral("Duplicate Merge recommendation became stale after earlier changes: keep object %1 is no longer present.")
                                .arg(it.value().first.id));
                    continue;
                }
                MergeRequest request;
                request.keepNodeIndex = keepIndex;
                for (const WizardNodeRef &remove : it.value().second)
                {
                    const int removeIndex = findNodeIndex(current, remove);
                    if (removeIndex >= 0)
                        request.removeNodeIndices.append(removeIndex);
                }
                if (request.removeNodeIndices.isEmpty())
                    continue;
                mergeRequests.append(request);
            }
            if (failure.isEmpty() && !mergeRequests.isEmpty())
            {
                updateApplyProgress(35, QStringLiteral("Applying duplicate merges"), QStringLiteral("Redirecting references and removing duplicate objects"));
                const MergeApplyResult result = m_mergeService.applyBatch(
                    current, mergeRequests, workspace.path(), m_whitelistIds,
                    [&](int fileIndex, int totalFiles, const QString &file) {
                        const int percent = 35 + ((fileIndex * 15) / qMax(1, totalFiles));
                        updateApplyProgress(percent, QStringLiteral("Applying duplicate merges"),
                                            QStringLiteral("Scanning file %1 of %2: %3")
                                                .arg(qMin(fileIndex + 1, totalFiles))
                                                .arg(totalFiles)
                                                .arg(QFileInfo(file).fileName()));
                    });
                if (!result.success)
                {
                    failure = result.error;
                }
                else
                {
                    removedDuplicates += result.nodesDeleted;
                    redirectedReferences += result.referencesRedirected;
                    changedFiles.append(result.changedFiles);
                    changedFiles.removeDuplicates();
                    if (result.skippedMerges > 0)
                        staleDuplicateRecommendations += result.skippedMerges;
                    recordServiceMessages(result.warnings);
                    if (!reloadWorkingAnalysis(workspace.path(), &current, &error))
                        failure = error;
                }
            }

            if (failure.isEmpty() && !selection.rename.isEmpty())
            {
                updateApplyProgress(55, QStringLiteral("Applying rename changes"), QStringLiteral("Building one safe batch rename plan"));
                QStringList renamePlanNotes;
                const RenamePlan combinedRenamePlan = buildCombinedRenamePlan(current, selection.rename, &renamePlanNotes);
                recordRenamePlanNotes(renamePlanNotes);
                if (combinedRenamePlan.valid)
                {
                    updateApplyProgress(56, QStringLiteral("Applying rename changes"),
                                        QStringLiteral("Updating %1 real XML IDs and references in one pass")
                                            .arg(combinedRenamePlan.items.size()));
                    const RenameApplyResult result = m_referenceRenamer.apply(
                        current, combinedRenamePlan, workspace.path(), m_whitelistIds,
                        makeRenameProgress(56, 8));
                    if (!result.success)
                    {
                        failure = result.error;
                    }
                    else
                    {
                        renamedIds += result.identitiesRenamed;
                        recordServiceMessages(result.warnings);
                        changedFiles.append(result.changedFiles);
                        changedFiles.removeDuplicates();
                        sc2dh::ArchiveReferenceRewriteReport archiveRewrite;
                        QStringList skippedArchiveRenames;
                        const QHash<QString, QString> archiveRenames =
                            sc2dh::unambiguousArchiveReferenceRenames(current, result.appliedRenames, &skippedArchiveRenames);
                        if (!sc2dh::rewriteArchiveReferenceFiles(workspace.path(),
                                                                 archiveReferenceFilesForWorkspace(current, workspace.path()),
                                                                 archiveRenames,
                                                                 &archiveRewrite,
                                                                 &error))
                        {
                            failure = error;
                        }
                        else
                        {
                            changedFiles.append(archiveRewrite.changedFiles);
                            changedFiles.removeDuplicates();
                            if (archiveRewrite.replacements > 0)
                                notes << QStringLiteral("Rename To Standard: %1 archive placement/trigger/script reference(s) were updated.")
                                             .arg(archiveRewrite.replacements);
                        }
                        if (failure.isEmpty() && !reloadWorkingAnalysis(workspace.path(), &current, &error))
                            failure = error;
                        else if (failure.isEmpty())
                            applyArchiveReferenceSafety(&current);
                    }
                }
            }

            QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(current, configuredDataCollectionMode());
            QHash<QString, UnitFamily> familyByRoot;
            for (const UnitFamily &family : families)
                familyByRoot.insert(family.rootId, family);
            bool collectionChanged = false;
            const int collectionCount = selection.collection.size();
            for (int collectionIndex = 0; collectionIndex < collectionCount; ++collectionIndex)
            {
                const WizardCollectionSelection &selectedCollection = selection.collection[collectionIndex];
                if (failure.isEmpty())
                {
                    const int percent = 65 + ((collectionIndex * 15) / qMax(1, collectionCount));
                    updateApplyProgress(percent, QStringLiteral("Applying Data Collection"),
                                        QStringLiteral("Family %1 of %2 (%3%): %4")
                                            .arg(collectionIndex + 1)
                                            .arg(collectionCount)
                                            .arg(((collectionIndex + 1) * 100) / qMax(1, collectionCount))
                                            .arg(selectedCollection.familyRootId));
                    const auto match = familyByRoot.constFind(selectedCollection.familyRootId);
                    if (match == familyByRoot.cend())
                    {
                        ++dataCollectionUnavailable;
                        logLine(QStringLiteral("Data Collection note: %1 is no longer present after earlier optimization steps.")
                                    .arg(selectedCollection.familyRootId));
                        continue;
                    }
                    DataCollectionBuildRequest request;
                    request.family = match.value();
                    request.requestedUnitId = request.family.rootId;
                    request.confirmNonStandard = true;
                    for (const WizardNodeRef &ref : selectedCollection.nodes)
                    {
                        const int index = findNodeIndex(current, ref);
                        if (index >= 0)
                            request.includedNodeIndices.insert(index);
                    }
                    const DataCollectionPreviewReport preview = m_dataCollectionBuilder.preview(current, request, &families);
                    if (!preview.valid)
                    {
                        ++dataCollectionNotApplicable;
                        logLine(QStringLiteral("Data Collection note: %1 is not in Data Collection / not eligible for automatic Data Collection after refresh: %2")
                                    .arg(selectedCollection.familyRootId, collectionSkipReason(preview)));
                        continue;
                    }
                    const DataCollectionApplyResult result = m_dataCollectionBuilder.apply(
                        current, request, workspace.path(), m_whitelistIds, false, &families, true);
                    if (!result.success)
                    {
                        failure = result.error;
                        break;
                    }
                    collectionAdded += result.recordsAdded;
                    collectionReorganized += result.recordsRemoved;
                    changedFiles.append(result.changedFiles);
                    changedFiles.removeDuplicates();
                    collectionChanged = true;
                }
            }

            if (failure.isEmpty() && collectionChanged && !reloadWorkingAnalysis(workspace.path(), &current, &error))
                failure = error;

            if (failure.isEmpty())
            {
                const QVector<int> followUpRows = automaticFollowUpDeepCleanupRows(current);
                if (!followUpRows.isEmpty())
                {
                    updateApplyProgress(82, QStringLiteral("Applying follow-up deep cleanup"),
                                        QStringLiteral("Cleaning safe stale data created by earlier steps"));
                    const DeepCleanupApplyResult result = DeepCleanupService().apply(current, followUpRows, workspace.path(), false);
                    if (!result.success)
                    {
                        failure = result.error;
                    }
                    else
                    {
                        const int changed = deepCleanupChangeCount(result);
                        deepCleanupChanged += changed;
                        automaticFollowUpCleanupChanges += changed;
                        reviewOnlyCleanupSkipped += result.reportOnlySkipped;
                        changedFiles.append(result.changedFiles);
                        removedFiles.append(result.removedFiles);
                        changedFiles.removeDuplicates();
                        removedFiles.removeDuplicates();
                        if (!reloadWorkingAnalysis(workspace.path(), &current, &error))
                        {
                            failure = error;
                        }
                        else
                        {
                            applyArchiveReferenceSafety(&current);
                        }
                    }
                }
            }

            if (failure.isEmpty() && (!changedFiles.isEmpty() || !removedFiles.isEmpty()))
            {
                updateApplyProgress(85, QStringLiteral("Saving archive"), QStringLiteral("Writing verified XML back to the SC2 archive"));
                if (!commitArchiveChanges(workspace.path(), changedFiles, &archiveBackup, &error, removedFiles))
                {
                    failure = error;
                }
                else
                {
                    normalizeArchiveAnalysis(&current, workspace.path(), m_currentSourcePath);
                    m_result = std::move(current);
                    archiveAnalysisReady = true;
                }
            }
        }
    }
    else
    {
        AnalysisResult current = m_result;
        QString error;

        if (failure.isEmpty() && !selection.importCleanup.isEmpty())
        {
            updateApplyProgress(16, QStringLiteral("Applying import cleanup"), QStringLiteral("Removing unused imported assets"));
            const DeepCleanupApplyResult result = DeepCleanupService().apply(current, selection.importCleanup, m_rootFolder, true);
            if (!result.success)
            {
                failure = result.error;
            }
            else
            {
                importCleanupChanged += result.filesDeleted;
                reviewOnlyCleanupSkipped += result.reportOnlySkipped;
                if (!reloadWorkingAnalysis(m_rootFolder, &current, &error))
                    failure = error;
            }
        }

        if (failure.isEmpty() && !selection.deepCleanup.isEmpty())
        {
            updateApplyProgress(18, QStringLiteral("Applying deep cleanup"), QStringLiteral("Removing stale localization, redundant XML and broken actor events"));
            const DeepCleanupApplyResult result = DeepCleanupService().apply(current, selection.deepCleanup, m_rootFolder, true);
            if (!result.success)
            {
                failure = result.error;
            }
            else
            {
                deepCleanupChanged += result.filesDeleted + result.textLinesRemoved + result.xmlNodesRemoved + result.xmlAttributesRemoved;
                reviewOnlyCleanupSkipped += result.reportOnlySkipped;
                if (!reloadWorkingAnalysis(m_rootFolder, &current, &error))
                    failure = error;
            }
        }

        QVector<int> unusedRows;
        for (const WizardNodeRef &ref : selection.unused)
        {
            const int index = findNodeIndex(current, ref);
            if (index >= 0)
                unusedRows.append(index);
        }
        if (failure.isEmpty() && !unusedRows.isEmpty())
        {
            updateApplyProgress(20, QStringLiteral("Deleting unused data objects"), QStringLiteral("Removing selected safe unused data objects"));
            QString backupFolder;
            QStringList changedFiles;
            int removed = 0;
            int skipped = 0;
            if (!m_analyzer.applySelectedChanges(current, unusedRows, m_rootFolder, m_whitelistIds,
                                                 &backupFolder, &error, &changedFiles, &removed, &skipped))
            {
                failure = error;
            }
            else
            {
                removedUnused += removed;
                if (skipped > 0)
                {
                    staleUnusedRecommendations += skipped;
                    logLine(QStringLiteral("Unused Data Objects: %1 selected recommendation(s) became stale after earlier changes.").arg(skipped));
                }
                if (!reloadWorkingAnalysis(m_rootFolder, &current, &error))
                    failure = error;
            }
        }

        QHash<QString, QPair<WizardNodeRef, QVector<WizardNodeRef>>> mergeGroups;
        for (const WizardMergeSelection &item : selection.duplicates)
        {
            auto &group = mergeGroups[groupKey(item.keep)];
            group.first = item.keep;
            group.second.append(item.remove);
        }
        QVector<MergeRequest> mergeRequests;
        for (auto it = mergeGroups.cbegin(); it != mergeGroups.cend(); ++it)
        {
            const int keepIndex = findNodeIndex(current, it.value().first);
            if (keepIndex < 0)
            {
                ++staleDuplicateRecommendations;
                logLine(QStringLiteral("Duplicate Merge recommendation became stale after earlier changes: keep object %1 is no longer present.")
                            .arg(it.value().first.id));
                continue;
            }
            MergeRequest request;
            request.keepNodeIndex = keepIndex;
            for (const WizardNodeRef &remove : it.value().second)
            {
                const int removeIndex = findNodeIndex(current, remove);
                if (removeIndex >= 0)
                    request.removeNodeIndices.append(removeIndex);
            }
            if (request.removeNodeIndices.isEmpty())
                continue;
            mergeRequests.append(request);
        }
        if (failure.isEmpty() && !mergeRequests.isEmpty())
        {
            updateApplyProgress(45, QStringLiteral("Applying duplicate merges"), QStringLiteral("Redirecting references and removing duplicate objects"));
            const MergeApplyResult result = m_mergeService.applyBatch(
                current, mergeRequests, m_rootFolder, m_whitelistIds,
                [&](int fileIndex, int totalFiles, const QString &file) {
                    const int percent = 45 + ((fileIndex * 15) / qMax(1, totalFiles));
                    updateApplyProgress(percent, QStringLiteral("Applying duplicate merges"),
                                        QStringLiteral("Scanning file %1 of %2: %3")
                                            .arg(qMin(fileIndex + 1, totalFiles))
                                            .arg(totalFiles)
                                            .arg(QFileInfo(file).fileName()));
                });
            if (!result.success)
            {
                failure = result.error;
            }
            else
            {
                removedDuplicates += result.nodesDeleted;
                redirectedReferences += result.referencesRedirected;
                if (result.skippedMerges > 0)
                    staleDuplicateRecommendations += result.skippedMerges;
                recordServiceMessages(result.warnings);
                if (!reloadWorkingAnalysis(m_rootFolder, &current, &error))
                    failure = error;
            }
        }

        if (failure.isEmpty() && !selection.rename.isEmpty())
        {
            updateApplyProgress(60, QStringLiteral("Applying rename changes"), QStringLiteral("Building one safe batch rename plan"));
            QStringList renamePlanNotes;
            const RenamePlan combinedRenamePlan = buildCombinedRenamePlan(current, selection.rename, &renamePlanNotes);
            recordRenamePlanNotes(renamePlanNotes);
            if (combinedRenamePlan.valid)
            {
                updateApplyProgress(61, QStringLiteral("Applying rename changes"),
                                    QStringLiteral("Updating %1 IDs and references in one pass")
                                        .arg(combinedRenamePlan.items.size()));
                const RenameApplyResult result = m_referenceRenamer.apply(
                    current, combinedRenamePlan, m_rootFolder, m_whitelistIds,
                    makeRenameProgress(61, 17));
                if (!result.success)
                {
                    failure = result.error;
                }
                else
                {
                    renamedIds += result.identitiesRenamed;
                    recordServiceMessages(result.warnings);
                    if (!reloadWorkingAnalysis(m_rootFolder, &current, &error))
                        failure = error;
                }
            }
        }

        const QVector<UnitFamily> collectionFamilies = UnitFamilyDetector().detectCollectionFamilies(current, configuredDataCollectionMode());
        QHash<QString, UnitFamily> collectionFamilyByRoot;
        for (const UnitFamily &family : collectionFamilies)
            collectionFamilyByRoot.insert(family.rootId, family);
        const int collectionCount = selection.collection.size();
        for (int collectionIndex = 0; collectionIndex < collectionCount; ++collectionIndex)
        {
            const WizardCollectionSelection &selectedCollection = selection.collection[collectionIndex];
            if (!failure.isEmpty())
                break;
            const int percent = 80 + ((collectionIndex * 10) / qMax(1, collectionCount));
            updateApplyProgress(percent, QStringLiteral("Applying Data Collection"),
                                QStringLiteral("Family %1 of %2 (%3%): %4")
                                    .arg(collectionIndex + 1)
                                    .arg(collectionCount)
                                    .arg(((collectionIndex + 1) * 100) / qMax(1, collectionCount))
                                    .arg(selectedCollection.familyRootId));
            const auto familyIt = collectionFamilyByRoot.constFind(selectedCollection.familyRootId);
            if (familyIt == collectionFamilyByRoot.cend())
            {
                ++dataCollectionUnavailable;
                logLine(QStringLiteral("Data Collection note: %1 is no longer present after earlier optimization steps.")
                            .arg(selectedCollection.familyRootId));
                continue;
            }
            DataCollectionBuildRequest request;
            request.family = familyIt.value();
            request.requestedUnitId = request.family.rootId;
            request.confirmNonStandard = true;
            for (const WizardNodeRef &ref : selectedCollection.nodes)
            {
                const int index = findNodeIndex(current, ref);
                if (index >= 0)
                    request.includedNodeIndices.insert(index);
            }
            const DataCollectionPreviewReport preview = m_dataCollectionBuilder.preview(current, request, &collectionFamilies);
            if (!preview.valid)
            {
                ++dataCollectionNotApplicable;
                logLine(QStringLiteral("Data Collection note: %1 is not in Data Collection / not eligible for automatic Data Collection after refresh: %2")
                            .arg(selectedCollection.familyRootId, collectionSkipReason(preview)));
                continue;
            }
            const DataCollectionApplyResult result = m_dataCollectionBuilder.apply(
                current, request, m_rootFolder, m_whitelistIds, false, &collectionFamilies);
            if (!result.success)
            {
                failure = result.error;
                break;
            }
            collectionAdded += result.recordsAdded;
            collectionReorganized += result.recordsRemoved;
        }

        if (failure.isEmpty())
        {
            const QVector<int> followUpRows = automaticFollowUpDeepCleanupRows(current);
            if (!followUpRows.isEmpty())
            {
                updateApplyProgress(91, QStringLiteral("Applying follow-up deep cleanup"),
                                    QStringLiteral("Cleaning safe stale data created by earlier steps"));
                const DeepCleanupApplyResult result = DeepCleanupService().apply(current, followUpRows, m_rootFolder, true);
                if (!result.success)
                {
                    failure = result.error;
                }
                else
                {
                    const int changed = deepCleanupChangeCount(result);
                    deepCleanupChanged += changed;
                    automaticFollowUpCleanupChanges += changed;
                    reviewOnlyCleanupSkipped += result.reportOnlySkipped;
                    if (!reloadWorkingAnalysis(m_rootFolder, &current, &error))
                        failure = error;
                }
            }
        }
    }

    m_dryRunPage->setApplyingState(false);

    if (!failure.isEmpty())
    {
        applyProgress.close();
        loadPathAndAnalyze(m_currentSourcePath);
        if (m_wizardApplyAutomation)
        {
            finishWizardApplyAutomation(false, failure);
            return;
        }
        showSc2MessageDialog(this,
                             QMessageBox::Critical,
                             QStringLiteral("Optimization Apply Failed"),
                             QStringLiteral("The optimization batch stopped:\n%1").arg(failure),
                             QMessageBox::Ok,
                             700);
        return;
    }

    if (archiveAnalysisReady)
    {
        updateApplyProgress(92, QStringLiteral("Refreshing analysis"), QStringLiteral("Updating object tables"));
        m_rootFolder = QFileInfo(m_currentSourcePath).absolutePath();
        m_analysisPage->setFolderPath(m_currentSourcePath);
        m_analysisPage->setModeLabel(modeLabelFor(static_cast<int>(m_sourceKind)));
        m_analysisPage->setAnalysisResult(m_result);
        updateApplyProgress(95, QStringLiteral("Refreshing analysis"), QStringLiteral("Updating pages and recommendations"));
        refreshPages();
        updateApplyProgress(98, QStringLiteral("Writing report"), QStringLiteral("Saving latest analysis summary"));
        writeAnalysisReportFile();
        m_dryRunAction->setEnabled(true);
        m_applyAction->setEnabled(false);
        setCurrentSourcePath(m_currentSourcePath);
    }
    else if (!loadPathAndAnalyze(m_currentSourcePath))
    {
        if (m_wizardApplyAutomation)
        {
            finishWizardApplyAutomation(false, QStringLiteral("Changes were saved, but automatic re-analysis failed."));
            return;
        }
        showSc2MessageDialog(this,
                             QMessageBox::Warning,
                             QStringLiteral("Optimization Applied"),
                             QStringLiteral("Changes were saved, but automatic re-analysis failed. Re-open Analyze to refresh the wizard view."),
                             QMessageBox::Ok,
                             760);
        return;
    }
    updateApplyProgress(100, QStringLiteral("Apply complete"), QStringLiteral("Optimization changes were saved successfully"));
    applyProgress.close();

    if (removedUnused > 0)
        m_dryRunPage->recordUnusedResult(removedUnused);
    if (removedDuplicates > 0 || redirectedReferences > 0)
        m_dryRunPage->recordMergeResult(removedDuplicates, redirectedReferences);
    if (importCleanupChanged > 0)
        m_dryRunPage->recordImportCleanupResult(importCleanupChanged);
    if (deepCleanupChanged > 0)
        m_dryRunPage->recordDeepCleanupResult(deepCleanupChanged);
    if (renamedIds > 0)
        m_dryRunPage->recordRenameResult(renamedIds);
    if (collectionAdded > 0 || collectionReorganized > 0)
        m_dryRunPage->recordCollectionResult(collectionAdded, collectionReorganized);
    m_dryRunPage->rebuildAfterApply();

    if (automaticFollowUpCleanupChanges > 0)
        notes << QStringLiteral("Follow-up Deep Cleanup applied %1 safe cleanup change(s) after earlier optimization steps.")
                     .arg(automaticFollowUpCleanupChanges);
    if (staleUnusedRecommendations > 0)
        notes << QStringLiteral("Unused Data Objects: %1 selected recommendation(s) were already resolved or became unsafe after earlier steps.")
                     .arg(staleUnusedRecommendations);
    if (staleDuplicateRecommendations > 0)
        notes << QStringLiteral("Duplicate Merge: %1 selected recommendation(s) became stale after earlier steps.")
                     .arg(staleDuplicateRecommendations);
    if (staleRenameRecommendations > 0)
        notes << QStringLiteral("Rename To Standard: %1 recommendation(s) became stale after earlier steps.")
                     .arg(staleRenameRecommendations);
    if (renameConflictRecommendations > 0)
        notes << QStringLiteral("Rename To Standard: %1 conflict recommendation(s) were left for manual review.")
                     .arg(renameConflictRecommendations);
    if (dataCollectionUnavailable > 0)
        notes << QStringLiteral("Data Collection: %1 family recommendation(s) were not applied because the source objects were already removed or renamed.")
                     .arg(dataCollectionUnavailable);
    if (dataCollectionNotApplicable > 0)
        notes << QStringLiteral("Data Collection: %1 family recommendation(s) are not in Data Collection or are not eligible after refresh.")
                     .arg(dataCollectionNotApplicable);
    if (reviewOnlyCleanupSkipped > 0)
        notes << QStringLiteral("Review-only cleanup: %1 item(s) were left for manual review.")
                     .arg(reviewOnlyCleanupSkipped);
    if (serviceSkippedRecommendations > 0)
        notes << QStringLiteral("Optimization refresh: %1 stale low-level recommendation(s) were ignored after earlier steps.")
                     .arg(serviceSkippedRecommendations);
    notes.removeDuplicates();
    warnings.removeDuplicates();
    for (const QString &note : notes)
        logLine(QStringLiteral("Optimization note: %1").arg(note));
    for (const QString &warning : warnings)
        logLine(QStringLiteral("Optimization warning: %1").arg(warning));

    const auto sectionText = [](const QString &title, const QStringList &items, int maxItems)
    {
        if (items.isEmpty())
            return QString();
        QStringList visible = items.mid(0, qMax(1, maxItems));
        if (items.size() > visible.size())
            visible << QStringLiteral("%1 more item(s) are available in Logs.").arg(items.size() - visible.size());
        return QStringLiteral("\n\n%1:\n- %2").arg(title, visible.join(QStringLiteral("\n- ")));
    };

    QString message = QStringLiteral("Selected optimization steps were applied and saved.\n\nUnused data objects deleted: %1\nImported files deleted: %2\nDuplicates deleted: %3\nReferences redirected: %4\nDeep cleanup changes: %5\nIDs renamed: %6\nCollection records added: %7\nCollection records reorganized: %8")
                          .arg(removedUnused)
                          .arg(importCleanupChanged)
                          .arg(removedDuplicates)
                          .arg(redirectedReferences)
                          .arg(deepCleanupChanged)
                          .arg(renamedIds)
                          .arg(collectionAdded)
                          .arg(collectionReorganized);
    if (!archiveBackup.isEmpty())
        message += QStringLiteral("\nArchive backup: %1").arg(archiveBackup);
    message += sectionText(QStringLiteral("Notes"), notes, 6);
    if (!warnings.isEmpty())
        message += sectionText(QStringLiteral("Warnings"), warnings, 4);
    if (m_wizardApplyAutomation)
    {
        finishWizardApplyAutomation(true, message);
        return;
    }
    showSc2MessageDialog(this,
                         QMessageBox::Information,
                         QStringLiteral("Optimization Applied"),
                         message,
                         QMessageBox::Ok,
                         1040);
}

void MainWindow::runWizardApplyAutomation(const QString &path, const QString &logPath, int timeoutMs)
{
    m_wizardApplyAutomation = true;
    m_wizardApplyAutomationLogPath = logPath;
    appendWizardApplyAutomationLog(QStringLiteral("start path=%1 timeoutMs=%2").arg(path).arg(timeoutMs));
    if (timeoutMs > 0)
    {
        QTimer::singleShot(timeoutMs, this, [this]
        {
            if (m_wizardApplyAutomation)
                finishWizardApplyAutomation(false, QStringLiteral("Wizard apply automation timed out."));
        });
    }
    QTimer::singleShot(0, this, [this, path]
    {
        appendWizardApplyAutomationLog(QStringLiteral("load begin"));
        if (!loadPathAndAnalyze(path))
        {
            finishWizardApplyAutomation(false, QStringLiteral("Failed to analyze %1").arg(path));
            return;
        }
        appendWizardApplyAutomationLog(QStringLiteral("load ok nodes=%1 files=%2").arg(m_result.nodes.size()).arg(m_result.scannedFiles.size()));
        connect(m_dryRunPage, &FormatterPage::previewBuilt, this, [this]
        {
            appendWizardApplyAutomationLog(QStringLiteral("preview built; selecting recommended"));
            m_dryRunPage->selectRecommendedItems();
            QTimer::singleShot(0, this, [this]
            {
                appendWizardApplyAutomationLog(QStringLiteral("apply begin"));
                applyOptimizationWizardPlan();
            });
        }, Qt::SingleShotConnection);
        appendWizardApplyAutomationLog(QStringLiteral("preview start"));
        m_dryRunPage->startWizard(true);
    });
}

void MainWindow::appendWizardApplyAutomationLog(const QString &line) const
{
    if (m_wizardApplyAutomationLogPath.isEmpty())
        return;
    QFile file(m_wizardApplyAutomationLogPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    QTextStream stream(&file);
    stream << QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
           << ' ' << line << '\n';
}

void MainWindow::finishWizardApplyAutomation(bool success, const QString &message)
{
    appendWizardApplyAutomationLog(QStringLiteral("finish status=%1 message=%2")
                                       .arg(success ? QStringLiteral("ok") : QStringLiteral("failed"), message));
    m_wizardApplyAutomation = false;
    qInfo().noquote() << (success ? QStringLiteral("wizard_apply_status=ok")
                                  : QStringLiteral("wizard_apply_status=failed"));
    qInfo().noquote() << QStringLiteral("wizard_apply_message=%1").arg(message);
    QCoreApplication::exit(success ? 0 : 1);
}

void MainWindow::showLogsTab()
{
    m_tabs->setCurrentWidget(m_logPanel);
}

void MainWindow::showGraphForRow(int row)
{
    if (row < 0 || row >= m_result.nodes.size())
    {
        return;
    }

    m_tabs->setCurrentWidget(m_graphPage);
    m_graphPage->setCurrentRow(row);
    m_dependenciesPage->setCurrentRow(row);
    m_propertiesPage->setCurrentRow(row);
    QMetaObject::invokeMethod(m_graphPage, "fitGraph", Qt::QueuedConnection);
}

void MainWindow::writeAnalysisReportFile() const
{
    // Reports stay in the UI unless the user explicitly exports them.
}

void MainWindow::refreshPages()
{
    m_mergePreviewValid = false;
    m_previewedUnusedRows.clear();
    m_renamePreviewValid = false;
    m_collectionPreviewValid = false;
    m_duplicatesPage->setAnalysisResult(m_result);
    m_cleanupPage->setAnalysisResult(m_result);
    m_dataCollectionPage->setAnalysisResult(m_result);
    m_renameIdsPage->setAnalysisResult(m_result);
    m_analysisPage->setAnalysisResult(m_result);
    m_dependenciesPage->setAnalysisResult(m_result);
    m_graphPage->setAnalysisResult(m_result);
    m_propertiesPage->setAnalysisResult(m_result);
    m_xmlSourcePage->setAnalysisResult(m_result);
    m_dryRunPage->setAnalysisResult(m_result);
    m_applyAction->setEnabled(false);

    const int currentRow = m_analysisPage->currentRow();
    m_dependenciesPage->setCurrentRow(currentRow);
    m_graphPage->setCurrentRow(currentRow);
    m_propertiesPage->setCurrentRow(currentRow);
}
