#include "app/MainWindowAnalysisController.h"

#include "app/MainWindow.h"

#include "ui/AnalysisProgressDialog.h"
#include "ui/OverviewPage.h"

#include "core/ExternalConsumerSafetyPolicy.h"
#include "core/Sc2Archive.h"
#include "core/XmlLoader.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFile>
#include <QFutureWatcher>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QSettings>
#include <QStatusBar>

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>

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

enum class BackgroundArchiveReferenceStrength
{
    None,
    Weak,
    Strong
};

struct BackgroundAnalysisResult
{
    bool success = false;
    AnalysisResult analysis;
    QSet<QString> archiveReferencedIds;
    QHash<QString, QStringList> archiveStrongReferenceSources;
    QHash<QString, QStringList> archiveWeakReferenceSources;
    bool archiveReferenceScanComplete = false;
    QStringList logLines;
    QString error;
};

using BackgroundProgress = std::function<void(int, const QString &, const QString &)>;
using BackgroundCancelled = std::function<bool()>;

BackgroundArchiveReferenceStrength backgroundArchiveReferenceStrength(const QString &entry)
{
    const QString normalized = QDir::cleanPath(entry).replace('\\', '/').toLower();
    const QString name = normalized.section('/', -1);
    if (name == QStringLiteral("objects") || name == QStringLiteral("units")
        || name == QStringLiteral("triggers") || normalized.contains(QStringLiteral("trigger")))
        return BackgroundArchiveReferenceStrength::Strong;
    const QString suffix = QFileInfo(name).suffix().toLower();
    if (suffix == QStringLiteral("galaxy"))
        return BackgroundArchiveReferenceStrength::Strong;
    static const QSet<QString> weakNames = {
        QStringLiteral("regions"), QStringLiteral("mapinfo"), QStringLiteral("documentinfo"),
        QStringLiteral("preload.xml"), QStringLiteral("componentlist.sc2components")};
    if (weakNames.contains(name))
        return BackgroundArchiveReferenceStrength::Weak;
    static const QSet<QString> weakExtensions = {
        QStringLiteral("txt"), QStringLiteral("ini"), QStringLiteral("json"),
        QStringLiteral("yaml"), QStringLiteral("yml"), QStringLiteral("version"),
        QStringLiteral("sc2components"), QStringLiteral("layout"), QStringLiteral("sc2layout"),
        QStringLiteral("fxa"), QStringLiteral("fxs"), QStringLiteral("fxh")};
    return weakExtensions.contains(suffix) ? BackgroundArchiveReferenceStrength::Weak
                                           : BackgroundArchiveReferenceStrength::None;
}

bool backgroundArchiveEntryShouldMaterialize(const QString &entry)
{
    const QString normalized = QDir::cleanPath(entry).replace('\\', '/').toLower();
    const QString suffix = QFileInfo(normalized).suffix().toLower();
    const QString fileName = QFileInfo(normalized).fileName();
    static const QSet<QString> assetExtensions = {
        QStringLiteral("dds"), QStringLiteral("tga"), QStringLiteral("png"), QStringLiteral("jpg"),
        QStringLiteral("jpeg"), QStringLiteral("bmp"), QStringLiteral("m3"), QStringLiteral("ogg"),
        QStringLiteral("wav"), QStringLiteral("mp3"), QStringLiteral("webm"), QStringLiteral("mp4"),
        QStringLiteral("fxa"), QStringLiteral("fxs"), QStringLiteral("fxh"), QStringLiteral("layout"),
        QStringLiteral("sc2layout"), QStringLiteral("txt")};
    const bool helperTrash = normalized.contains(QStringLiteral("/backup_"))
        || fileName.startsWith(QStringLiteral("backup_"))
        || fileName.contains(QStringLiteral(".bak-"))
        || fileName.endsWith(QStringLiteral(".bak"))
        || fileName.endsWith(QStringLiteral(".tmp"))
        || fileName.endsWith(QStringLiteral(".old"))
        || fileName.endsWith(QStringLiteral(".orig"))
        || fileName.endsWith(QStringLiteral(".log"))
        || fileName.endsWith(QStringLiteral(".sc2dh.pending"));
    return assetExtensions.contains(suffix) || helperTrash
        || backgroundArchiveReferenceStrength(entry) != BackgroundArchiveReferenceStrength::None;
}

void collectBackgroundKnownIdTokens(const QByteArray &bytes,
                                    const QSet<QString> &knownIds,
                                    const QString &source,
                                    QHash<QString, QStringList> *found)
{
    if (!found)
        return;
    QSet<QString> ids;
    const auto isIdChar = [](uchar value) {
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')
            || (value >= '0' && value <= '9') || value == '_' || value == '@';
    };
    for (qsizetype start = 0; start < bytes.size();) {
        while (start < bytes.size() && !isIdChar(uchar(bytes[start])))
            ++start;
        qsizetype end = start;
        while (end < bytes.size() && isIdChar(uchar(bytes[end])))
            ++end;
        if (end > start) {
            const QString token = QString::fromLatin1(bytes.constData() + start, end - start);
            if (knownIds.contains(token))
                ids.insert(token);
        }
        start = qMax(end, start + 1);
    }
    for (qsizetype start = 0; start + 1 < bytes.size();) {
        while (start + 1 < bytes.size()
               && (!isIdChar(uchar(bytes[start])) || bytes[start + 1] != '\0'))
            ++start;
        qsizetype end = start;
        QByteArray tokenBytes;
        while (end + 1 < bytes.size()
               && isIdChar(uchar(bytes[end])) && bytes[end + 1] == '\0') {
            tokenBytes.append(bytes[end]);
            end += 2;
        }
        if (!tokenBytes.isEmpty()) {
            const QString token = QString::fromLatin1(tokenBytes);
            if (knownIds.contains(token))
                ids.insert(token);
        }
        start = qMax(end, start + 1);
    }
    for (const QString &id : ids)
        (*found)[id].append(source);
}

QStringList collectBackgroundArchives(const QString &folderPath)
{
    QStringList archives;
    QDirIterator it(folderPath, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
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
            archives.append(filePath);
    }
    std::sort(archives.begin(), archives.end(), [](const QString &left, const QString &right) {
        return QString::compare(left, right, Qt::CaseInsensitive) < 0;
    });
    return archives;
}

BackgroundAnalysisResult analyzeBackgroundFolder(const QString &path,
                                                 const QSet<QString> &whitelistIds,
                                                 const BackgroundProgress &progress,
                                                 const BackgroundCancelled &cancelled)
{
    BackgroundAnalysisResult output;
    FolderAnalyzer analyzer;
    output.success = analyzer.analyzeFolder(
        path, whitelistIds, &output.analysis, &output.error,
        [&](int current, int total, const QString &file) {
            const int percent = total > 0 ? 22 + (current * 62 / total) : 22;
            progress(percent, QStringLiteral("Scanning XML and data files"),
                     file.isEmpty() ? QStringLiteral("Finalizing scan") : QDir::toNativeSeparators(file));
        }, cancelled);
    return output;
}

BackgroundAnalysisResult analyzeBackgroundXml(const QString &path,
                                              const QSet<QString> &whitelistIds,
                                              const BackgroundProgress &progress,
                                              const BackgroundCancelled &cancelled)
{
    Q_UNUSED(whitelistIds);
    BackgroundAnalysisResult output;
    if (cancelled()) {
        output.error = QStringLiteral("Analysis canceled.");
        return output;
    }
    progress(28, QStringLiteral("Reading XML file"), path);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        output.error = QStringLiteral("Unable to open XML file: %1").arg(path);
        return output;
    }
    const QByteArray xmlBytes = file.readAll();
    output.analysis.rootFolder = path;
    ScannedFileInfo scanned;
    scanned.filePath = path;
    scanned.isXml = true;
    scanned.isSc2DataLike = true;
    scanned.size = QFileInfo(path).size();
    output.analysis.scannedFiles.append(scanned);
    output.analysis.sourceXmlByFile.insert(path, QString::fromUtf8(xmlBytes));

    XmlLoader loader;
    QString parseError;
    if (!loader.extractNodes(path, xmlBytes, &output.analysis.nodes, &parseError)) {
        output.analysis.parseErrors.append({path, parseError});
        output.error = parseError;
        return output;
    }
    QString revisionError;
    const SourceRevision revision = captureSourceRevision(path, &revisionError);
    if (!revisionError.isEmpty())
        output.analysis.unreadableSources.append(revisionError);
    else
        output.analysis.sourceRevisions.append(revision);
    output.analysis.sourceDiscoveryComplete = true;
    FolderAnalyzer analyzer;
    output.analysis.referenceExtractionComplete = analyzer.populateReferenceIds(&output.analysis, {}, cancelled);
    output.analysis.dependencyGraphComplete = false;
    output.analysis.incompleteSources.append(
        QStringLiteral("Standalone XML analysis cannot prove references from the containing map or mod."));
    updateAnalysisCompleteness(&output.analysis);
    output.analysis.analysisReportText = analyzer.buildAnalysisReport(output.analysis);
    output.analysis.plannedChangesReportText = analyzer.buildDryRunReport(output.analysis, {});
    output.success = !cancelled();
    if (!output.success)
        output.error = QStringLiteral("Analysis canceled.");
    return output;
}

BackgroundAnalysisResult analyzeBackgroundArchives(const QString &path,
                                                   bool folderMode,
                                                   const QSet<QString> &whitelistIds,
                                                   const BackgroundProgress &progress,
                                                   const BackgroundCancelled &cancelled)
{
    BackgroundAnalysisResult output;
    const QStringList archives = folderMode ? collectBackgroundArchives(path) : QStringList{path};
    if (archives.isEmpty()) {
        output.error = QStringLiteral("No SC2 map/mod archives found: %1").arg(path);
        return output;
    }

    output.analysis.rootFolder = path;
    XmlLoader loader;
    QHash<QString, QStringList> entriesByArchive;
    int xmlEntriesFound = 0;
    int xmlEntriesLoaded = 0;
    for (int archiveIndex = 0; archiveIndex < archives.size(); ++archiveIndex) {
        if (cancelled()) {
            output.error = QStringLiteral("Analysis canceled.");
            return output;
        }
        const QString archivePath = archives.at(archiveIndex);
        const QString archiveLabel = folderMode
            ? QDir(path).relativeFilePath(archivePath).replace('\\', '/') : QString();
        progress(22 + archiveIndex * 25 / qMax(1, archives.size()),
                 QStringLiteral("Opening SC2 archives"),
                 folderMode ? archiveLabel : QFileInfo(archivePath).fileName());

        Sc2Archive archive;
        QString archiveError;
        if (!archive.load(archivePath, &archiveError)) {
            output.error = QStringLiteral("%1: %2").arg(archivePath, archiveError);
            return output;
        }
        QString revisionError;
        const SourceRevision revision = captureSourceRevision(archivePath, &revisionError);
        if (!revisionError.isEmpty())
            output.analysis.unreadableSources.append(revisionError);
        else
            output.analysis.sourceRevisions.append(revision);

        const QStringList entries = archive.allEntries();
        entriesByArchive.insert(archivePath, entries);
        output.logLines << QStringLiteral("Archive entry count: %1 -> %2")
                               .arg(folderMode ? archiveLabel : QFileInfo(archivePath).fileName())
                               .arg(entries.size());
        QStringList xmlEntries;
        for (const QString &entry : entries) {
            if (entry.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive))
                xmlEntries.append(entry);
        }
        xmlEntriesFound += xmlEntries.size();

        for (int entryIndex = 0; entryIndex < xmlEntries.size(); ++entryIndex) {
            if (cancelled()) {
                output.error = QStringLiteral("Analysis canceled.");
                return output;
            }
            const QString entryName = xmlEntries.at(entryIndex);
            const QString sourceName = folderMode
                ? QStringLiteral("%1::%2").arg(archiveLabel, entryName) : entryName;
            progress(30 + archiveIndex * 30 / qMax(1, archives.size()),
                     QStringLiteral("Extracting archive XML"), sourceName);
            QByteArray xmlBytes;
            QString readError;
            if (!archive.readEntry(entryName, &xmlBytes, &readError)) {
                output.analysis.parseErrors.append({sourceName, readError});
                continue;
            }
            ScannedFileInfo scanned;
            scanned.filePath = sourceName;
            scanned.isXml = true;
            scanned.isSc2DataLike = true;
            scanned.size = xmlBytes.size();
            output.analysis.scannedFiles.append(scanned);
            output.analysis.sourceXmlByFile.insert(sourceName, QString::fromUtf8(xmlBytes));
            QVector<DataNode> nodes;
            QString parseError;
            if (!loader.extractNodes(sourceName, xmlBytes, &nodes, &parseError)) {
                output.analysis.parseErrors.append({sourceName, parseError});
                continue;
            }
            output.analysis.nodes += nodes;
            ++xmlEntriesLoaded;
        }

        if (!folderMode) {
            for (const QString &entryName : entries) {
                if (entryName.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)
                    || !backgroundArchiveEntryShouldMaterialize(entryName))
                    continue;
                ScannedFileInfo scanned;
                scanned.filePath = entryName;
                scanned.isXml = false;
                scanned.isSc2DataLike = backgroundArchiveReferenceStrength(entryName)
                    != BackgroundArchiveReferenceStrength::None;
                output.analysis.scannedFiles.append(scanned);
            }
        }
    }
    output.logLines << QStringLiteral("Archive scan: archives=%1, XML entries=%2, XML loaded=%3")
                           .arg(archives.size()).arg(xmlEntriesFound).arg(xmlEntriesLoaded);
    if (output.analysis.sourceXmlByFile.isEmpty()) {
        output.error = QStringLiteral("No XML files found inside SC2 archive source: %1").arg(path);
        return output;
    }

    output.analysis.sourceDiscoveryComplete = true;
    FolderAnalyzer analyzer;
    if (!analyzer.finalizeAnalysisResult(
            &output.analysis, whitelistIds, &output.error,
            [&] { progress(82, QStringLiteral("Analyzing extracted XML"),
                           QStringLiteral("Building references, duplicate groups and candidates")); },
            cancelled)) {
        return output;
    }

    QSet<QString> knownIds;
    for (const DataNode &node : output.analysis.nodes) {
        if (!node.id.isEmpty())
            knownIds.insert(node.id);
    }
    output.archiveReferenceScanComplete = true;
    int scannedReferenceEntries = 0;
    int strongReferenceEntries = 0;
    int weakReferenceEntries = 0;
    for (const QString &archivePath : archives) {
        if (cancelled()) {
            output.error = QStringLiteral("Analysis canceled.");
            return output;
        }
        const QString archiveLabel = folderMode
            ? QDir(path).relativeFilePath(archivePath).replace('\\', '/') : QString();
        Sc2Archive archive;
        QString archiveError;
        if (!archive.load(archivePath, &archiveError)) {
            output.archiveReferenceScanComplete = false;
            output.logLines << QStringLiteral("Archive reference scan failed to reopen %1: %2")
                                   .arg(archivePath, archiveError);
            continue;
        }
        for (const QString &entry : entriesByArchive.value(archivePath, archive.allEntries())) {
            const BackgroundArchiveReferenceStrength strength = backgroundArchiveReferenceStrength(entry);
            if (entry.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)
                || strength == BackgroundArchiveReferenceStrength::None)
                continue;
            if (cancelled()) {
                output.error = QStringLiteral("Analysis canceled.");
                return output;
            }
            const QString sourceName = folderMode
                ? QStringLiteral("%1::%2").arg(archiveLabel, entry) : entry;
            progress(86, QStringLiteral("Checking archive references"), sourceName);
            QByteArray bytes;
            QString readError;
            if (!archive.readEntry(entry, &bytes, &readError)) {
                output.archiveReferenceScanComplete = false;
                output.analysis.incompleteSources.append(QStringLiteral("%1: %2").arg(sourceName, readError));
                continue;
            }
            const QString sourceLabel = QStringLiteral("%1 [%2]")
                                            .arg(sourceName,
                                                 strength == BackgroundArchiveReferenceStrength::Strong
                                                     ? QStringLiteral("map/trigger/script")
                                                     : QStringLiteral("metadata/text"));
            if (strength == BackgroundArchiveReferenceStrength::Strong) {
                collectBackgroundKnownIdTokens(bytes, knownIds, sourceLabel,
                                               &output.archiveStrongReferenceSources);
                ++strongReferenceEntries;
            } else {
                collectBackgroundKnownIdTokens(bytes, knownIds, sourceLabel,
                                               &output.archiveWeakReferenceSources);
                ++weakReferenceEntries;
            }
            ++scannedReferenceEntries;
        }
    }
    for (auto it = output.archiveStrongReferenceSources.begin();
         it != output.archiveStrongReferenceSources.end(); ++it) {
        it.value().removeDuplicates();
        output.archiveReferencedIds.insert(it.key());
    }
    for (auto it = output.archiveWeakReferenceSources.begin();
         it != output.archiveWeakReferenceSources.end(); ++it)
        it.value().removeDuplicates();
    output.logLines << QStringLiteral("Archive references: scanned=%1, strong entries=%2, weak entries=%3, strong IDs=%4, weak IDs=%5")
                           .arg(scannedReferenceEntries).arg(strongReferenceEntries).arg(weakReferenceEntries)
                           .arg(output.archiveStrongReferenceSources.size())
                           .arg(output.archiveWeakReferenceSources.size());
    output.success = true;
    return output;
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
    startPathAnalysis(effectivePath);
}

bool MainWindowAnalysisController::startPathAnalysis(const QString &path)
{
    if (m_window.m_analysisInProgress) {
        m_window.statusBar()->showMessage(QStringLiteral("Analysis is already running."), 4000);
        m_window.logLine(QStringLiteral("Ignored duplicate analysis request while another analysis is running."));
        return false;
    }

    const QFileInfo info(path);
    if (!info.exists()) {
        QMessageBox::warning(&m_window, QStringLiteral("Load"),
                             QStringLiteral("Path does not exist: %1").arg(path));
        return false;
    }

    MainWindow::SourceKind sourceKind = MainWindow::SourceKind::Unknown;
    if (info.isDir()) {
        sourceKind = folderContainsSupportedArchivesForAnalysis(path)
            ? MainWindow::SourceKind::ArchiveFolder : MainWindow::SourceKind::Folder;
    } else if (info.suffix().compare(QStringLiteral("xml"), Qt::CaseInsensitive) == 0) {
        sourceKind = MainWindow::SourceKind::XmlFile;
    } else if (isSupportedSc2ArchiveForAnalysis(info)) {
        sourceKind = MainWindow::SourceKind::ArchiveFile;
    } else {
        QMessageBox::critical(&m_window, QStringLiteral("Analysis failed"),
                              QStringLiteral("Unsupported path type: %1").arg(path));
        return false;
    }

    m_window.m_analysisInProgress = true;
    const bool previousAnalyzeEnabled = m_window.m_analyzeAction && m_window.m_analyzeAction->isEnabled();
    const bool previousOptimizationEnabled = m_window.m_dryRunAction && m_window.m_dryRunAction->isEnabled();
    const bool previousReviewEnabled = m_window.m_applyAction && m_window.m_applyAction->isEnabled();
    const AnalysisResult previousResult = m_window.m_result;
    const MainWindow::SourceKind previousSourceKind = m_window.m_sourceKind;
    if (m_window.m_analyzeAction)
        m_window.m_analyzeAction->setEnabled(false);
    if (m_window.m_dryRunAction)
        m_window.m_dryRunAction->setEnabled(false);
    if (m_window.m_applyAction)
        m_window.m_applyAction->setEnabled(false);

    auto *progress = new AnalysisProgressDialog(&m_window);
    progress->setAttribute(Qt::WA_DeleteOnClose, false);
    progress->setProgress(8, QStringLiteral("Preparing background analysis"), QFileInfo(path).fileName());
    progress->show();
    m_window.m_activeProgressDialog = progress;

    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    QObject::connect(progress, &AnalysisProgressDialog::cancellationRequested,
                     progress, [cancelled] { cancelled->store(true, std::memory_order_relaxed); });
    const QPointer<AnalysisProgressDialog> guardedProgress(progress);
    const BackgroundProgress reportProgress = [guardedProgress](int percent,
                                                                const QString &primary,
                                                                const QString &secondary) {
        if (!guardedProgress)
            return;
        QMetaObject::invokeMethod(guardedProgress, [guardedProgress, percent, primary, secondary] {
            if (guardedProgress)
                guardedProgress->setProgress(percent, primary, secondary);
        }, Qt::QueuedConnection);
    };
    const BackgroundCancelled isCancelled = [cancelled] {
        return cancelled->load(std::memory_order_relaxed);
    };

    auto *watcher = new QFutureWatcher<BackgroundAnalysisResult>(&m_window);
    const QPointer<MainWindow> guardedWindow(&m_window);
    QObject::connect(watcher, &QFutureWatcher<BackgroundAnalysisResult>::finished,
                     &m_window,
                     [guardedWindow, guardedProgress, watcher, path, info, sourceKind,
                      previousAnalyzeEnabled, previousOptimizationEnabled, previousReviewEnabled,
                      previousResult, previousSourceKind]() mutable {
        const BackgroundAnalysisResult background = watcher->result();
        watcher->deleteLater();
        if (!guardedWindow)
            return;
        MainWindow &window = *guardedWindow;
        QString errorMessage = background.error;
        const bool cancelledResult = errorMessage == QStringLiteral("Analysis canceled.");
        if (!background.success) {
            window.m_result = previousResult;
            window.m_sourceKind = previousSourceKind;
            if (window.m_dryRunAction)
                window.m_dryRunAction->setEnabled(previousOptimizationEnabled);
            if (window.m_applyAction)
                window.m_applyAction->setEnabled(previousReviewEnabled);
            if (window.m_analyzeAction)
                window.m_analyzeAction->setEnabled(previousAnalyzeEnabled);
            window.m_analysisInProgress = false;
            window.m_activeProgressDialog = nullptr;
            if (guardedProgress) {
                guardedProgress->close();
                guardedProgress->deleteLater();
            }
            if (cancelledResult) {
                window.statusBar()->showMessage(
                    QStringLiteral("Analysis canceled. No partial result was applied."), 8000);
                window.logLine(QStringLiteral("Background analysis canceled by user."));
            } else {
                QMessageBox::critical(&window, QStringLiteral("Analysis failed"), errorMessage);
                window.logLine(QStringLiteral("Background analysis failed: %1").arg(errorMessage));
            }
            return;
        }

        window.m_result = background.analysis;
        window.m_sourceKind = sourceKind;
        window.m_archiveReferencedIds = background.archiveReferencedIds;
        window.m_archiveStrongReferenceSources = background.archiveStrongReferenceSources;
        window.m_archiveWeakReferenceSources = background.archiveWeakReferenceSources;
        window.m_archiveReferenceScanComplete = background.archiveReferenceScanComplete;
        for (const QString &line : background.logLines)
            window.logLine(line);

        if (sourceKind == MainWindow::SourceKind::ArchiveFile
            || sourceKind == MainWindow::SourceKind::ArchiveFolder) {
            window.normalizeArchiveAnalysis(&window.m_result, QString(), path);
        } else if (sourceKind == MainWindow::SourceKind::Folder) {
            const bool closedProjectMode = QSettings().value(
                QStringLiteral("optimization/closedProjectMode"), false).toBool();
            if (sourceMayHaveExternalConsumers(path) && !closedProjectMode) {
                window.m_result.externalConsumersUnknown = true;
                applyExternalConsumerSafety(&window.m_result);
                window.m_result.analysisReportText = window.m_analyzer.buildAnalysisReport(window.m_result);
                window.m_result.plannedChangesReportText = window.m_analyzer.buildDryRunReport(window.m_result, {});
            }
        }

        if (guardedProgress) {
            guardedProgress->setProgress(90, QStringLiteral("Refreshing analysis"),
                                         QStringLiteral("Updating object tables"));
        }
        window.m_currentSourcePath = path;
        window.m_rootFolder = info.isDir() ? path : info.absolutePath();
        QSettings().setValue(QStringLiteral("paths/lastSourcePath"), path);
        window.m_analysisPage->setFolderPath(path);
        window.m_analysisPage->setModeLabel(modeLabelForSource(static_cast<int>(sourceKind)));
        window.m_analysisPage->setAnalysisResult(window.m_result);
        if (guardedProgress) {
            guardedProgress->setProgress(94, QStringLiteral("Refreshing analysis"),
                                         QStringLiteral("Updating pages and recommendations"));
        }
        window.refreshPages();
        if (guardedProgress)
            guardedProgress->setProgress(98, QStringLiteral("Writing report"),
                                         QStringLiteral("Saving latest analysis summary"));
        window.writeAnalysisReportFile();
        if (guardedProgress) {
            guardedProgress->setProgress(100, QStringLiteral("Analysis complete"),
                                         QStringLiteral("%1 XML files | %2 objects")
                                             .arg(window.m_result.totalXmlFiles())
                                             .arg(window.m_result.totalDataNodes()));
            guardedProgress->close();
            guardedProgress->deleteLater();
        }
        window.m_activeProgressDialog = nullptr;
        window.showAnalysisTab();
        if (window.m_dryRunAction)
            window.m_dryRunAction->setEnabled(true);
        if (window.m_applyAction)
            window.m_applyAction->setEnabled(false);
        if (window.m_analyzeAction)
            window.m_analyzeAction->setEnabled(previousAnalyzeEnabled);
        window.m_analysisInProgress = false;
        window.setCurrentSourcePath(path);
        window.logLine(QStringLiteral("Background analysis complete; UI thread remained responsive."));
        window.logLine(QStringLiteral("Scanned files: %1").arg(window.m_result.totalFilesScanned()));
        window.logLine(QStringLiteral("XML files: %1").arg(window.m_result.totalXmlFiles()));
        window.logLine(QStringLiteral("Data nodes: %1").arg(window.m_result.totalDataNodes()));
        window.logLine(QStringLiteral("Parse errors: %1").arg(window.m_result.parseErrors.size()));
        if (!window.m_optimizationDialog && !window.m_wizardApplyAutomation)
            window.showDryRunTab(true);
    });

    watcher->setFuture(QtConcurrent::run([path, sourceKind, whitelistIds = m_window.m_whitelistIds,
                                          reportProgress, isCancelled] {
        switch (sourceKind) {
        case MainWindow::SourceKind::Folder:
            return analyzeBackgroundFolder(path, whitelistIds, reportProgress, isCancelled);
        case MainWindow::SourceKind::ArchiveFolder:
            return analyzeBackgroundArchives(path, true, whitelistIds, reportProgress, isCancelled);
        case MainWindow::SourceKind::XmlFile:
            return analyzeBackgroundXml(path, whitelistIds, reportProgress, isCancelled);
        case MainWindow::SourceKind::ArchiveFile:
            return analyzeBackgroundArchives(path, false, whitelistIds, reportProgress, isCancelled);
        case MainWindow::SourceKind::Unknown:
            break;
        }
        BackgroundAnalysisResult output;
        output.error = QStringLiteral("Unsupported source kind.");
        return output;
    }));
    return true;
}

bool MainWindowAnalysisController::loadPathAndAnalyze(const QString &path)
{
    if (m_window.m_analysisInProgress)
    {
        m_window.statusBar()->showMessage(QStringLiteral("Analysis is already running."), 4000);
        m_window.logLine(QStringLiteral("Ignored duplicate analysis request while another analysis is running."));
        return false;
    }

    m_window.m_analysisInProgress = true;
    QFileInfo info(path);
    const bool previousAnalyzeEnabled = m_window.m_analyzeAction && m_window.m_analyzeAction->isEnabled();
    const bool previousOptimizationEnabled = m_window.m_dryRunAction && m_window.m_dryRunAction->isEnabled();
    const bool previousReviewEnabled = m_window.m_applyAction && m_window.m_applyAction->isEnabled();
    if (m_window.m_analyzeAction)
        m_window.m_analyzeAction->setEnabled(false);
    m_window.m_dryRunAction->setEnabled(false);
    m_window.m_applyAction->setEnabled(false);
    if (!info.exists())
    {
        QMessageBox::warning(&m_window, QStringLiteral("Load"), QStringLiteral("Path does not exist: %1").arg(path));
        m_window.logLine(QStringLiteral("Path does not exist: %1").arg(path));
        m_window.m_dryRunAction->setEnabled(previousOptimizationEnabled);
        m_window.m_applyAction->setEnabled(previousReviewEnabled);
        if (m_window.m_analyzeAction)
            m_window.m_analyzeAction->setEnabled(previousAnalyzeEnabled);
        m_window.m_analysisInProgress = false;
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
        if (m_window.m_analyzeAction)
            m_window.m_analyzeAction->setEnabled(previousAnalyzeEnabled);
        m_window.m_analysisInProgress = false;
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
    if (m_window.m_analyzeAction)
        m_window.m_analyzeAction->setEnabled(previousAnalyzeEnabled);
    m_window.m_analysisInProgress = false;
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
