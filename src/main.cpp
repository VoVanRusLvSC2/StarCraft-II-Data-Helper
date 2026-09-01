#include "app/MainWindow.h"
#include "app/AudioManager.h"
#include "app/AppSettings.h"
#include "app/TranslationManager.h"
#include "core/Sc2Archive.h"
#include "ui/MapPerformancePage.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QOpenGLWidget>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTimer>

#include <memory>
#include <functional>

namespace
{

struct LayoutProfile
{
    QString name;
    QSize logicalSize;
    QString language;
};

bool hasScrollableAncestor(QWidget *widget)
{
    for (QWidget *parent = widget ? widget->parentWidget() : nullptr; parent; parent = parent->parentWidget()) {
        auto *area = qobject_cast<QAbstractScrollArea *>(parent);
        if (!area)
            continue;
        if (area->verticalScrollBar()->maximum() > 0 || area->horizontalScrollBar()->maximum() > 0)
            return true;
    }
    return false;
}

QByteArray sourceSha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(4 * 1024 * 1024);
        if (chunk.isEmpty() && file.error() != QFile::NoError)
            return {};
        hash.addData(chunk);
    }
    return hash.result();
}

int runLayoutSmoke(QApplication &app, const QString &outputDirectory)
{
    const QVector<LayoutProfile> profiles{
        {QStringLiteral("1280x720-100-en"), QSize(1280, 720), QStringLiteral("enUS")},
        {QStringLiteral("1366x768-100-ru"), QSize(1366, 768), QStringLiteral("ruRU")},
        {QStringLiteral("1366x768-125-en"), QSize(1093, 614), QStringLiteral("enUS")},
        {QStringLiteral("1366x768-125-ru"), QSize(1093, 614), QStringLiteral("ruRU")},
        {QStringLiteral("1920x1080-100-en"), QSize(1920, 1080), QStringLiteral("enUS")},
        {QStringLiteral("1920x1080-150-ru"), QSize(1280, 720), QStringLiteral("ruRU")},
        {QStringLiteral("1920x1080-200-en"), QSize(960, 540), QStringLiteral("enUS")}
    };
    if (!QDir().mkpath(outputDirectory))
        return 20;

    auto page = std::make_shared<QPointer<MapPerformancePage>>(new MapPerformancePage);
    auto reports = std::make_shared<QJsonArray>();
    auto index = std::make_shared<int>(0);
    auto runner = std::make_shared<std::function<void()>>();
    *runner = [&, page, reports, index, runner, profiles, outputDirectory] {
        if (!*page) {
            app.exit(21);
            return;
        }
        if (*index >= profiles.size()) {
            QJsonObject root{
                {QStringLiteral("schema"), QStringLiteral("sc2dh.beta2.layout-smoke.v1")},
                {QStringLiteral("profiles"), *reports}
            };
            QSaveFile reportFile(QDir(outputDirectory).absoluteFilePath(QStringLiteral("layout-smoke.json")));
            bool saved = reportFile.open(QIODevice::WriteOnly);
            if (saved) {
                const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
                saved = reportFile.write(bytes) == bytes.size() && reportFile.commit();
            }
            (*page)->close();
            (*page)->deleteLater();
            app.exit(saved ? 0 : 22);
            return;
        }

        const LayoutProfile profile = profiles.at((*index)++);
        sc2dh::app::TranslationManager::instance().setLanguage(profile.language, false);
        (*page)->resize(profile.logicalSize);
        (*page)->show();
        QTimer::singleShot(180, &app, [&, page, reports, runner, profile, outputDirectory] {
            if (!*page) {
                app.exit(23);
                return;
            }
            QJsonArray failures;
            QList<QWidget *> controls;
            for (QAbstractButton *button : (*page)->findChildren<QAbstractButton *>())
                controls << button;
            for (QLineEdit *edit : (*page)->findChildren<QLineEdit *>())
                controls << edit;
            for (QAbstractItemView *view : (*page)->findChildren<QAbstractItemView *>())
                controls << view;

            const QRect pageRect(QPoint(0, 0), (*page)->size());
            for (QWidget *control : controls) {
                if (control->isHidden())
                    continue;
                const QRect geometry(control->mapTo(*page, QPoint(0, 0)), control->size());
                const bool directlyVisible = !control->visibleRegion().isEmpty() && pageRect.intersects(geometry);
                if (!directlyVisible && !hasScrollableAncestor(control)) {
                    failures.append(QStringLiteral("Control is outside the viewport without scrolling: %1")
                                        .arg(control->objectName().isEmpty()
                                                 ? QString::fromLatin1(control->metaObject()->className())
                                                 : control->objectName()));
                }
            }

            const QStringList actionNames{
                QStringLiteral("decorPreviewButton"),
                QStringLiteral("decorCreateCopyButton"),
                QStringLiteral("maximumCompatibleCompressionButton")
            };
            QVector<QRect> visibleActions;
            for (const QString &name : actionNames) {
                QWidget *button = (*page)->findChild<QWidget *>(name);
                if (!button) {
                    failures.append(QStringLiteral("Missing action button: %1").arg(name));
                    continue;
                }
                if (!button->visibleRegion().isEmpty())
                    visibleActions << QRect(button->mapTo(*page, QPoint(0, 0)), button->size());
                else if (!hasScrollableAncestor(button))
                    failures.append(QStringLiteral("Action button is inaccessible: %1").arg(name));
            }
            for (int first = 0; first < visibleActions.size(); ++first) {
                for (int second = first + 1; second < visibleActions.size(); ++second) {
                    if (visibleActions.at(first).intersects(visibleActions.at(second)))
                        failures.append(QStringLiteral("Visible action buttons overlap."));
                }
            }

            const QString screenshotName = profile.name + QStringLiteral(".png");
            const bool screenshotSaved = (*page)->grab().save(
                QDir(outputDirectory).absoluteFilePath(screenshotName), "PNG");
            if (!screenshotSaved)
                failures.append(QStringLiteral("Screenshot could not be saved."));
            QString actionsScreenshotName;
            if (auto *controlsScroll = (*page)->findChild<QScrollArea *>(
                    QStringLiteral("mapPerformanceControlsScroll"))) {
                const int previous = controlsScroll->verticalScrollBar()->value();
                controlsScroll->ensureWidgetVisible(
                    (*page)->findChild<QWidget *>(QStringLiteral("maximumCompatibleCompressionButton")), 40, 40);
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                actionsScreenshotName = profile.name + QStringLiteral("-actions.png");
                if (!(*page)->grab().save(QDir(outputDirectory).absoluteFilePath(actionsScreenshotName), "PNG"))
                    failures.append(QStringLiteral("Action-area screenshot could not be saved."));
                controlsScroll->verticalScrollBar()->setValue(previous);
            }
            auto *mapCanvas = (*page)->findChild<QOpenGLWidget *>(QStringLiteral("mapPerformanceHeatmap"));
            reports->append(QJsonObject{
                {QStringLiteral("profile"), profile.name},
                {QStringLiteral("logical_width"), profile.logicalSize.width()},
                {QStringLiteral("logical_height"), profile.logicalSize.height()},
                {QStringLiteral("language"), profile.language},
                {QStringLiteral("interactive_controls"), controls.size()},
                {QStringLiteral("opengl_canvas"), mapCanvas != nullptr},
                {QStringLiteral("opengl_context_valid"), mapCanvas && mapCanvas->isValid()},
                {QStringLiteral("qt_opengl_mode"), QString::fromLocal8Bit(qgetenv("QT_OPENGL"))},
                {QStringLiteral("result"), failures.isEmpty() ? QStringLiteral("PASS") : QStringLiteral("FAIL")},
                {QStringLiteral("failures"), failures},
                {QStringLiteral("screenshot"), screenshotName},
                {QStringLiteral("actions_screenshot"), actionsScreenshotName}
            });
            QTimer::singleShot(0, &app, *runner);
        });
    };
    QTimer::singleShot(0, &app, *runner);
    return app.exec();
}

int runMapPreviewSmoke(QApplication &app, const QString &mapPath, const QString &outputDirectory)
{
    const QString sourcePath = QFileInfo(mapPath).absoluteFilePath();
    if (!QFileInfo(sourcePath).isFile() || !QDir().mkpath(outputDirectory))
        return 30;
    const QByteArray hashBefore = sourceSha256(sourcePath);
    Sc2Archive archive;
    QString error;
    if (hashBefore.isEmpty() || !archive.load(sourcePath, &error))
        return 31;

    AnalysisResult analysis;
    analysis.rootFolder = sourcePath;
    for (const QString &entry : archive.allEntries()) {
        ScannedFileInfo scanned;
        scanned.filePath = entry;
        scanned.isXml = entry.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive);
        scanned.isSc2DataLike = scanned.isXml;
        analysis.scannedFiles.append(scanned);
    }

    auto page = std::make_shared<QPointer<MapPerformancePage>>(new MapPerformancePage);
    (*page)->resize(1280, 900);
    (*page)->show();
    const QElapsedTimer started = [] { QElapsedTimer timer; timer.start(); return timer; }();
    (*page)->setAnalysisResult(analysis);

    auto poll = std::make_shared<std::function<void()>>();
    auto readyObservedMs = std::make_shared<qint64>(-1);
    *poll = [&, page, poll, readyObservedMs, sourcePath, outputDirectory, hashBefore, started] {
        if (!*page) {
            app.exit(32);
            return;
        }
        const auto findRoleLabel = [page](const QString &role) -> QLabel * {
            for (QLabel *label : (*page)->findChildren<QLabel *>()) {
                if (label->property("sc2dh.mapPerformanceRole").toString() == role)
                    return label;
            }
            return nullptr;
        };
        QLabel *summary = findRoleLabel(QStringLiteral("summary"));
        const bool ready = summary && !summary->text().contains(QStringLiteral("worker thread"), Qt::CaseInsensitive);
        if (ready && *readyObservedMs < 0)
            *readyObservedMs = started.elapsed();
        const bool canvasSettled = ready && started.elapsed() - *readyObservedMs >= 800;
        if (!canvasSettled && started.elapsed() < 120000) {
            QTimer::singleShot(100, &app, *poll);
            return;
        }
        const QByteArray hashAfter = sourceSha256(sourcePath);
        auto *canvas = (*page)->findChild<QOpenGLWidget *>(QStringLiteral("mapPerformanceHeatmap"));
        QLabel *warning = findRoleLabel(QStringLiteral("warning"));
        const QString screenshotPath = QDir(outputDirectory).absoluteFilePath(QStringLiteral("real-map-preview.png"));
        QImage screenshot((*page)->size(), QImage::Format_ARGB32_Premultiplied);
        screenshot.fill(Qt::transparent);
        QPainter screenshotPainter(&screenshot);
        (*page)->render(&screenshotPainter);
        bool framebufferComposited = false;
        if (canvas && canvas->isValid()) {
            const QImage framebuffer = canvas->grabFramebuffer();
            if (!framebuffer.isNull()) {
                const QPoint canvasOrigin = canvas->mapTo(*page, QPoint(0, 0));
                screenshotPainter.drawImage(QRect(canvasOrigin, canvas->size()), framebuffer);
                framebufferComposited = true;
            }
        }
        screenshotPainter.end();
        const bool screenshotSaved = screenshot.save(screenshotPath, "PNG");
        QJsonObject report{
            {QStringLiteral("schema"), QStringLiteral("sc2dh.beta2.real-map-preview-smoke.v1")},
            {QStringLiteral("source_path"), sourcePath},
            {QStringLiteral("source_sha256_before"), QString::fromLatin1(hashBefore.toHex())},
            {QStringLiteral("source_sha256_after"), QString::fromLatin1(hashAfter.toHex())},
            {QStringLiteral("source_unchanged"), !hashBefore.isEmpty() && hashBefore == hashAfter},
            {QStringLiteral("archive_entries"), analysis.scannedFiles.size()},
            {QStringLiteral("ready"), canvasSettled},
            {QStringLiteral("first_paint_ready_ms"), double(*readyObservedMs)},
            {QStringLiteral("summary"), summary ? summary->text() : QString()},
            {QStringLiteral("warning"), warning ? warning->text() : QString()},
            {QStringLiteral("opengl_context_valid"), canvas && canvas->isValid()},
            {QStringLiteral("opengl_framebuffer_composited"), framebufferComposited},
            {QStringLiteral("screenshot_saved"), screenshotSaved},
            {QStringLiteral("screenshot"), screenshotPath}
        };
        QSaveFile reportFile(QDir(outputDirectory).absoluteFilePath(QStringLiteral("real-map-preview.json")));
        bool saved = reportFile.open(QIODevice::WriteOnly);
        if (saved) {
            const QByteArray bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
            saved = reportFile.write(bytes) == bytes.size() && reportFile.commit();
        }
        (*page)->close();
        (*page)->deleteLater();
        app.exit(canvasSettled && screenshotSaved && saved && hashBefore == hashAfter ? 0 : 33);
    };
    QTimer::singleShot(100, &app, *poll);
    return app.exec();
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication::setApplicationName(QStringLiteral("SC2 Data Helper"));
    QCoreApplication::setOrganizationName(QStringLiteral("SC2DataHelper"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("local"));
    sc2dh::app::AppSettings::configureRendererBeforeApplication();

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("SC2 Data Helper"));
    // Numeric by contract: update checks and QVersionNumber consumers can
    // compare this independently from the user-facing release label.
    QApplication::setApplicationVersion(QStringLiteral(SC2DH_VERSION_NUMBER));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/Icon.png")));

    // Build the persistent UI once in the source language so the generic
    // TranslationManager can retain stable source keys. The user's selected
    // language is installed immediately after construction, before the window
    // becomes visible.
    auto &translationManager = sc2dh::app::TranslationManager::instance();
    translationManager.setLanguage(QStringLiteral("enUS"), false);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [] {
        AudioManager::instance()->shutdown();
    });

    if (sc2dh::app::AppSettings::customCursor()) {
        const QPixmap cursorPixmap(QStringLiteral(":/cursors/cursor.png"));
        if (!cursorPixmap.isNull()) QApplication::setOverrideCursor(QCursor(cursorPixmap, 2, 2));
    }

    const QStringList args = app.arguments();
    const int layoutSmokeIndex = args.indexOf(QStringLiteral("--layout-smoke"));
    if (layoutSmokeIndex >= 0 && layoutSmokeIndex + 1 < args.size())
        return runLayoutSmoke(app, QFileInfo(args.at(layoutSmokeIndex + 1)).absoluteFilePath());
    const int previewSmokeIndex = args.indexOf(QStringLiteral("--map-preview-smoke"));
    if (previewSmokeIndex >= 0 && previewSmokeIndex + 2 < args.size()) {
        return runMapPreviewSmoke(app,
                                  args.at(previewSmokeIndex + 1),
                                  QFileInfo(args.at(previewSmokeIndex + 2)).absoluteFilePath());
    }

    MainWindow window;
    translationManager.setLanguage(sc2dh::app::AppSettings::selectedLanguage(), false);
    const int wizardApplyTestIndex = args.indexOf(QStringLiteral("--wizard-apply-test"));
    if (wizardApplyTestIndex >= 0 && wizardApplyTestIndex + 1 < args.size()) {
        QString logPath;
        int timeoutMs = 600000;
        const int logIndex = args.indexOf(QStringLiteral("--wizard-apply-test-log"));
        if (logIndex >= 0 && logIndex + 1 < args.size())
            logPath = args.at(logIndex + 1);
        const int timeoutIndex = args.indexOf(QStringLiteral("--wizard-apply-timeout-ms"));
        if (timeoutIndex >= 0 && timeoutIndex + 1 < args.size()) {
            bool ok = false;
            const int parsed = args.at(timeoutIndex + 1).toInt(&ok);
            if (ok && parsed > 0)
                timeoutMs = parsed;
        }
        window.showMaximized();
        window.runWizardApplyAutomation(args.at(wizardApplyTestIndex + 1), logPath, timeoutMs);
        return app.exec();
    }

    QSettings settings;
    if (settings.value(QStringLiteral("ui/startFullscreen"), true).toBool())
        window.showFullScreen();
    else
        window.showMaximized();

    return app.exec();
}
