#include "app/MainWindowSettings.h"

#include "app/AudioManager.h"
#include "app/AppSettings.h"
#include "app/MainWindow.h"
#include "app/ModalBackdrop.h"
#include "app/TranslationManager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEasingCurve>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLinearGradient>
#include <QListWidget>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QPushButton>
#include <QRegion>
#include <QResizeEvent>
#include <QSettings>
#include <QShowEvent>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
[[maybe_unused]] const char *const settingsTranslationSources[] = {
    QT_TRANSLATE_NOOP("Settings", "SC2 Data Helper Settings"),
    QT_TRANSLATE_NOOP("Settings", "Button sounds"),
    QT_TRANSLATE_NOOP("Settings", "Button animations"),
    QT_TRANSLATE_NOOP("Settings", "Blue background glow effects"),
    QT_TRANSLATE_NOOP("Settings", "Decorative interface textures"),
    QT_TRANSLATE_NOOP("Settings", "SC2 custom cursor"),
    QT_TRANSLATE_NOOP("Settings", "Background music"),
    QT_TRANSLATE_NOOP("Settings", "Start in full screen"),
    QT_TRANSLATE_NOOP("Settings", "Interface language"),
    QT_TRANSLATE_NOOP("Settings", "System language"),
    QT_TRANSLATE_NOOP("Settings", "Graphics profile"),
    QT_TRANSLATE_NOOP("Settings", "Full"),
    QT_TRANSLATE_NOOP("Settings", "Balanced"),
    QT_TRANSLATE_NOOP("Settings", "Minimal"),
    QT_TRANSLATE_NOOP("Settings", "Custom"),
    QT_TRANSLATE_NOOP("Settings", "Rendering mode (restart required)"),
    QT_TRANSLATE_NOOP("Settings", "Automatic (recommended)"),
    QT_TRANSLATE_NOOP("Settings", "Desktop OpenGL"),
    QT_TRANSLATE_NOOP("Settings", "Software rendering (compatibility)"),
    QT_TRANSLATE_NOOP("Settings", "Preview images and textures"),
    QT_TRANSLATE_NOOP("Settings", "Preview M3 models"),
    QT_TRANSLATE_NOOP("Settings", "M3 geometry antialiasing"),
    QT_TRANSLATE_NOOP("Settings", "Maximum preview file size (MiB)"),
    QT_TRANSLATE_NOOP("Settings", "Interface"),
    QT_TRANSLATE_NOOP("Settings", "Graphics & previews"),
    QT_TRANSLATE_NOOP("Settings", "Optimization"),
    QT_TRANSLATE_NOOP("Settings", "INTERFACE SETTINGS"),
    QT_TRANSLATE_NOOP("Settings", "GRAPHICS AND PREVIEW SETTINGS"),
    QT_TRANSLATE_NOOP("Settings", "OPTIMIZATION SETTINGS"),
    QT_TRANSLATE_NOOP("Settings", "Restart required"),
    QT_TRANSLATE_NOOP("Settings", "Rendering mode and cursor changes will be fully applied after restarting SC2 Data Helper."),
    QT_TRANSLATE_NOOP("Settings", "OPTIONS // SYSTEM CONFIGURATION"),
    QT_TRANSLATE_NOOP("Settings", "X"),
    QT_TRANSLATE_NOOP("Settings", "Shows animated soft blue background glows behind the main interface."),
    QT_TRANSLATE_NOOP("Settings", "Draws SC2 frames, scanlines and background texture layers. This never disables map texture analysis."),
    QT_TRANSLATE_NOOP("Settings", "Music volume: %1%"),
    QT_TRANSLATE_NOOP("Settings", "Enable Duplicate Merge in Optimization"),
    QT_TRANSLATE_NOOP("Settings", "Enabled by default. When enabled, Optimization adds the Duplicate Merge review step."),
    QT_TRANSLATE_NOOP("Settings", "Create backup files before applying changes"),
    QT_TRANSLATE_NOOP("Settings", "When disabled, SC2 archives and folders are edited without creating persistent .bak or backup_ copies."),
    QT_TRANSLATE_NOOP("Settings", "Closed Project / Aggressive standardization"),
    QT_TRANSLATE_NOOP("Settings", "Allows public SC2Mod/SC2Campaign IDs and imports to change. Enable only when every consuming map/mod is included in your project and will be updated together."),
    QT_TRANSLATE_NOOP("Settings", "WARNING: this mode can break external maps that depend on renamed catalog IDs or removed imports. A backup is strongly recommended."),
    QT_TRANSLATE_NOOP("Settings", " MiB"),
    QT_TRANSLATE_NOOP("Settings", "Enable Closed Project mode?"),
    QT_TRANSLATE_NOOP("Settings", "This allows public SC2Mod/SC2Campaign IDs and imports to change. External maps that are not updated together can break. Enable aggressive standardization?")
};

QString settingsText(const char *source)
{
    return QCoreApplication::translate("Settings", source);
}

void addTextGlow(QLabel *label, const QColor &color, qreal blurRadius)
{
    if (!label)
        return;
    auto *glow = new QGraphicsDropShadowEffect(label);
    glow->setOffset(0, 0);
    glow->setBlurRadius(blurRadius);
    glow->setColor(color);
    label->setGraphicsEffect(glow);
}

void drawHorizontalTexture(QPainter &painter, const QRect &target, const QPixmap &source, int cap)
{
    if (target.isEmpty() || source.isNull())
        return;

    const QPixmap texture = source.scaledToHeight(target.height(), Qt::SmoothTransformation);
    cap = qMin(cap, qMin(target.width() / 2, texture.width() / 2));
    if (cap <= 0 || target.width() <= cap * 2)
    {
        painter.drawPixmap(target, texture);
        return;
    }

    painter.drawPixmap(QRect(target.left(), target.top(), cap, target.height()),
                       texture, QRect(0, 0, cap, texture.height()));
    painter.drawTiledPixmap(QRect(target.left() + cap, target.top(), target.width() - cap * 2, target.height()),
                            texture.copy(QRect(cap, 0, texture.width() - cap * 2, texture.height())));
    painter.drawPixmap(QRect(target.right() - cap + 1, target.top(), cap, target.height()),
                       texture, QRect(texture.width() - cap, 0, cap, texture.height()));
}

void drawNinePatch(QPainter &painter, const QRect &target, const QPixmap &pixmap,
                   const QMargins &sourceMargins, const QMargins &targetMargins)
{
    if (target.isEmpty() || pixmap.isNull())
        return;

    const int sourceLeft = qBound(0, sourceMargins.left(), pixmap.width() / 2);
    const int sourceRight = qBound(0, sourceMargins.right(), pixmap.width() - sourceLeft);
    const int sourceTop = qBound(0, sourceMargins.top(), pixmap.height() / 2);
    const int sourceBottom = qBound(0, sourceMargins.bottom(), pixmap.height() - sourceTop);

    const int targetLeft = qBound(0, targetMargins.left(), target.width() / 2);
    const int targetRight = qBound(0, targetMargins.right(), target.width() - targetLeft);
    const int targetTop = qBound(0, targetMargins.top(), target.height() / 2);
    const int targetBottom = qBound(0, targetMargins.bottom(), target.height() - targetTop);

    const int sourceX[] = {0, sourceLeft, pixmap.width() - sourceRight, pixmap.width()};
    const int sourceY[] = {0, sourceTop, pixmap.height() - sourceBottom, pixmap.height()};
    const int targetX[] = {target.left(), target.left() + targetLeft,
                           target.right() + 1 - targetRight, target.right() + 1};
    const int targetY[] = {target.top(), target.top() + targetTop,
                           target.bottom() + 1 - targetBottom, target.bottom() + 1};

    for (int y = 0; y < 3; ++y)
    {
        for (int x = 0; x < 3; ++x)
        {
            const QRect sourceRect(sourceX[x], sourceY[y],
                                   sourceX[x + 1] - sourceX[x],
                                   sourceY[y + 1] - sourceY[y]);
            const QRect targetRect(targetX[x], targetY[y],
                                   targetX[x + 1] - targetX[x],
                                   targetY[y + 1] - targetY[y]);
            if (!sourceRect.isEmpty() && !targetRect.isEmpty())
                painter.drawPixmap(targetRect, pixmap, sourceRect);
        }
    }
}

class SettingsDialog final : public QDialog
{
public:
    explicit SettingsDialog(QWidget *parent)
        : QDialog(parent)
    {
        m_highlightTimer.setParent(this);
        m_highlightTimer.setTimerType(Qt::PreciseTimer);
        m_highlightTimer.setInterval(16);
        connect(&m_highlightTimer, &QTimer::timeout, this, [this]
        {
            if (m_highlightPhase + 1 >= kPeriodFrames)
            {
                m_highlightPhase = kPeriodFrames - 1;
                m_highlightTimer.stop();
            }
            else
            {
                ++m_highlightPhase;
            }
            update(frameRegion());
        });
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        QDialog::showEvent(event);
        m_highlightPhase = 0;
        update(frameRegion());
        m_highlightTimer.start();
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QDialog::resizeEvent(event);
        update(frameRegion());
    }

    void paintEvent(QPaintEvent *event) override
    {
        QDialog::paintEvent(event);

        if (sc2dh::app::AppSettings::decorativeTextures())
        {
            static const QPixmap grid(QStringLiteral(":/textures/ui_nova_storymode_bggrid_shimmer_sideways.png"));
            static const QPixmap scanlines(QStringLiteral(":/textures/ui_nova_archives_backgroundframe_scanlines.png"));
            static const QPixmap sideLights(QStringLiteral(":/textures/ui_nova_archives_backgroundframe_lights_side.png"));
            QPainter hudPainter(this);
            hudPainter.setClipRect(rect().adjusted(18, 58, -18, -18));
            if (!grid.isNull()) {
                hudPainter.setOpacity(0.045);
                hudPainter.drawTiledPixmap(rect(), grid);
            }
            if (!scanlines.isNull()) {
                hudPainter.setOpacity(0.035);
                hudPainter.drawTiledPixmap(rect(), scanlines);
            }
            hudPainter.setOpacity(0.72);
            if (!sideLights.isNull()) {
                const int sideWidth = 54;
                hudPainter.drawPixmap(QRect(3, 82, sideWidth, qMax(1, height() - 156)), sideLights, sideLights.rect());
                const QPixmap mirrored = sideLights.transformed(QTransform().scale(-1.0, 1.0));
                hudPainter.drawPixmap(QRect(width() - sideWidth - 3, 82, sideWidth, qMax(1, height() - 156)),
                                      mirrored, mirrored.rect());
            }
            hudPainter.setOpacity(0.9);
            hudPainter.setPen(QPen(QColor(42, 255, 203, 145), 1));
            const int left = 22;
            const int right = width() - 23;
            const int top = 64;
            const int bottom = height() - 23;
            constexpr int arm = 34;
            hudPainter.drawLine(left, top, left + arm, top);
            hudPainter.drawLine(left, top, left, top + arm);
            hudPainter.drawLine(right - arm, top, right, top);
            hudPainter.drawLine(right, top, right, top + arm);
            hudPainter.drawLine(left, bottom, left + arm, bottom);
            hudPainter.drawLine(left, bottom - arm, left, bottom);
            hudPainter.drawLine(right - arm, bottom, right, bottom);
            hudPainter.drawLine(right, bottom - arm, right, bottom);
        }

        if (m_highlightPhase <= 0)
            return;

        static const QPixmap highlight(QStringLiteral(":/textures/ui_nova_archives_backgroundframehighlight.png"));
        if (highlight.isNull())
            return;

        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setClipRegion(event ? event->region().intersected(frameRegion()) : frameRegion());
        // The highlight asset contains dark pixels intended for its original
        // background. Screen composition makes it a light-only overlay, so
        // the bright frame line never turns into a dark rectangular patch.
        painter.setCompositionMode(QPainter::CompositionMode_Screen);

        QPixmap frame(rect().size());
        frame.fill(Qt::transparent);
        {
            QPainter framePainter(&frame);
            framePainter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            drawNinePatch(framePainter, frame.rect(), highlight,
                          QMargins(kSourceSlice, kSourceSlice, kSourceSlice, kSourceSlice),
                          QMargins(kTargetBorder, kTargetBorder, kTargetBorder, kTargetBorder));
        }

        const qreal progress = qBound<qreal>(0.0, qreal(m_highlightPhase) / qreal(kPeriodFrames - 1), 1.0);
        const qreal eased = QEasingCurve(QEasingCurve::OutCubic).valueForProgress(progress);
        const int revealWidth = qBound(0, qRound((width() + kRevealTail) * eased), width());
        const int fadeStart = qMax(0, revealWidth - kRevealTail);
        const QRect revealRect(0, 0, revealWidth, height());
        if (!revealRect.isEmpty())
            painter.drawPixmap(revealRect, frame, revealRect);

        if (revealWidth > fadeStart && revealWidth < width())
        {
            QPixmap fadeLayer(QSize(revealWidth - fadeStart, height()));
            fadeLayer.fill(Qt::transparent);
            {
                QPainter fadePainter(&fadeLayer);
                fadePainter.drawPixmap(QRect(QPoint(0, 0), fadeLayer.size()), frame,
                                       QRect(fadeStart, 0, fadeLayer.width(), height()));
                fadePainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                QLinearGradient gradient(0, 0, fadeLayer.width(), 0);
                gradient.setColorAt(0.0, QColor(255, 255, 255, 190));
                gradient.setColorAt(1.0, QColor(255, 255, 255, 0));
                fadePainter.fillRect(fadeLayer.rect(), gradient);
            }
            painter.drawPixmap(QPoint(fadeStart, 0), fadeLayer);
        }
    }

private:
    QRegion frameRegion() const
    {
        const int horizontalBand = qMin(176, height());
        const int verticalBand = qMin(72, width());
        const int sideHeight = qMax(0, height() - horizontalBand * 2);
        QRegion region(0, 0, width(), horizontalBand);
        region += QRegion(0, qMax(0, height() - horizontalBand), width(), horizontalBand);
        region += QRegion(0, horizontalBand, verticalBand, sideHeight);
        region += QRegion(qMax(0, width() - verticalBand), horizontalBand, verticalBand, sideHeight);
        return region;
    }

    static constexpr int kPeriodFrames = 54;
    static constexpr int kSourceSlice = 24;
    static constexpr int kTargetBorder = 12;
    static constexpr int kRevealTail = 220;

    QTimer m_highlightTimer;
    int m_highlightPhase = 0;
};

class MusicVolumeSlider final : public QSlider
{
public:
    explicit MusicVolumeSlider(QWidget *parent)
        : QSlider(Qt::Horizontal, parent)
    {
        setMouseTracking(true);
        setAttribute(Qt::WA_OpaquePaintEvent, false);
        setMinimumHeight(32);
        setMaximumHeight(32);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);

        static const QPixmap frame(QStringLiteral(":/textures/ui_glue_sliderframe_terran_top.png"));
        static const QPixmap fill(QStringLiteral(":/textures/ui_glue_sliderfill_terran_top.png"));
        static const QPixmap handle(QStringLiteral(":/textures/ui_glue_sliderhandle_normal_terran_top.png"));
        static const QPixmap handleHover(QStringLiteral(":/textures/ui_glue_sliderhandle_over_terran_top.png"));
        static const QPixmap handlePressed(QStringLiteral(":/textures/ui_glue_sliderhandle_pressed_terran_bottom.png"));

        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

        const int handleWidth = 20;
        const int handleHeight = 28;
        const QRect trackRect(0, (height() - 18) / 2, width(), 18);
        const QRect fillArea = trackRect.adjusted(6, 6, -6, -6);
        const int available = qMax(0, width() - handleWidth);
        const int handleX = QStyle::sliderPositionFromValue(minimum(), maximum(), sliderPosition(),
                                                            available, invertedAppearance());
        const QRect handleRect(handleX, (height() - handleHeight) / 2, handleWidth, handleHeight);
        const int fillRight = qBound(fillArea.left(), handleRect.center().x(), fillArea.right());

        drawHorizontalTexture(painter, trackRect, frame, 10);
        if (fillRight >= fillArea.left())
            drawHorizontalTexture(painter, QRect(fillArea.left(), fillArea.top(),
                                                fillRight - fillArea.left() + 1, fillArea.height()),
                                  fill, 4);

        const QPixmap &handleTexture = isSliderDown() ? handlePressed
                                      : underMouse() ? handleHover
                                                     : handle;
        if (!handleTexture.isNull())
            painter.drawPixmap(handleRect, handleTexture);
    }
};

class DialogDragFilter final : public QObject
{
public:
    explicit DialogDragFilter(QDialog *dialog)
        : QObject(dialog)
        , m_dialog(dialog)
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        Q_UNUSED(watched);
        if (!m_dialog || !event)
            return false;

        if (event->type() == QEvent::MouseButtonPress)
        {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton)
            {
                m_dragPosition = mouse->globalPosition().toPoint() - m_dialog->frameGeometry().topLeft();
                m_dragging = true;
                return true;
            }
        }
        else if (event->type() == QEvent::MouseMove)
        {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (m_dragging && (mouse->buttons() & Qt::LeftButton))
            {
                m_dialog->move(mouse->globalPosition().toPoint() - m_dragPosition);
                return true;
            }
        }
        else if (event->type() == QEvent::MouseButtonRelease)
        {
            m_dragging = false;
        }
        return false;
    }

private:
    QDialog *m_dialog = nullptr;
    QPoint m_dragPosition;
    bool m_dragging = false;
};
}

namespace sc2dh::app
{
MainWindowSettings::MainWindowSettings(MainWindow &window)
    : m_window(window)
{
}

void MainWindowSettings::show()
{
    SettingsDialog dialog(&m_window);
    dialog.setObjectName(QStringLiteral("settingsDialog"));
    dialog.setWindowTitle(settingsText("SC2 Data Helper Settings"));
    dialog.setWindowIcon(QIcon(QStringLiteral(":/icons/Icon.png")));
    dialog.setWindowFlags((dialog.windowFlags() | Qt::FramelessWindowHint) & ~Qt::WindowContextHelpButtonHint);
    dialog.setAttribute(Qt::WA_TranslucentBackground, true);
    dialog.setMinimumSize(980, 720);
    dialog.resize(1040, 760);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(14, 12, 14, 14);
    layout->setSpacing(10);

    auto *titleBar = new QFrame(&dialog);
    titleBar->setObjectName(QStringLiteral("settingsTitleBar"));
    auto *titleBarLayout = new QHBoxLayout(titleBar);
    titleBarLayout->setContentsMargins(10, 4, 4, 4);
    titleBarLayout->setSpacing(8);
    auto *appIcon = new QLabel(titleBar);
    appIcon->setPixmap(QPixmap(QStringLiteral(":/icons/Icon.png")).scaled(30, 30, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    appIcon->setFixedSize(34, 34);
    appIcon->setAlignment(Qt::AlignCenter);
    auto *windowTitle = new QLabel(settingsText("SC2 Data Helper Settings"), titleBar);
    windowTitle->setObjectName(QStringLiteral("settingsWindowTitle"));
    addTextGlow(windowTitle, QColor(74, 255, 205, 220), 13.0);
    auto *titleTextLayout = new QVBoxLayout;
    titleTextLayout->setContentsMargins(0, 0, 0, 0);
    titleTextLayout->setSpacing(0);
    titleTextLayout->addWidget(windowTitle);
    auto *windowContext = new QLabel(settingsText("OPTIONS // SYSTEM CONFIGURATION"), titleBar);
    windowContext->setObjectName(QStringLiteral("settingsWindowContext"));
    addTextGlow(windowContext, QColor(22, 215, 255, 190), 9.0);
    titleTextLayout->addWidget(windowContext);
    auto *closeButton = new QPushButton(settingsText("X"), titleBar);
    closeButton->setObjectName(QStringLiteral("settingsCloseButton"));
    closeButton->setFixedSize(38, 32);
    closeButton->setFocusPolicy(Qt::NoFocus);
    titleBarLayout->addWidget(appIcon);
    titleBarLayout->addLayout(titleTextLayout, 1);
    titleBarLayout->addWidget(closeButton);
    layout->addWidget(titleBar);

    QSettings settings;
    const bool savedCustomCursor = AppSettings::customCursor();
    const auto checkBoxRow = [&dialog](const QString &text, bool checked, const QString &toolTip = QString())
    {
        auto *row = new QCheckBox(text, &dialog);
        row->setProperty("textureType", QStringLiteral("checkBoxRow"));
        row->setChecked(checked);
        row->setFocusPolicy(Qt::NoFocus);
        if (!toolTip.isEmpty())
            row->setToolTip(toolTip);
        return row;
    };
    auto *soundCheck = checkBoxRow(settingsText("Button sounds"),
                                   settings.value(QStringLiteral("ui/buttonSounds"), true).toBool());
    auto *animationCheck = checkBoxRow(settingsText("Button animations"),
                                       settings.value(QStringLiteral("ui/buttonAnimations"), true).toBool());
    auto *backgroundGlowCheck = checkBoxRow(
        settingsText("Blue background glow effects"),
        settings.value(QStringLiteral("ui/backgroundGlows"), true).toBool(),
        settingsText("Shows animated soft blue background glows behind the main interface."));
    auto *decorativeTexturesCheck = checkBoxRow(
        settingsText("Decorative interface textures"),
        settings.value(QStringLiteral("ui/decorativeTextures"), true).toBool(),
        settingsText("Draws SC2 frames, scanlines and background texture layers. This never disables map texture analysis."));
    auto *customCursorCheck = checkBoxRow(settingsText("SC2 custom cursor"),
                                          settings.value(QStringLiteral("ui/customCursor"), true).toBool());
    auto *musicCheck = checkBoxRow(settingsText("Background music"), AudioManager::isMusicEnabled());
    auto *musicValue = new QLabel(&dialog);
    musicValue->setObjectName(QStringLiteral("inspectorSubtitle"));
    auto *musicSlider = new MusicVolumeSlider(&dialog);
    musicSlider->setObjectName(QStringLiteral("backgroundMusicVolume"));
    musicSlider->setRange(0, 100);
    musicSlider->setValue(int(AudioManager::musicVolume() * 100.0));
    musicSlider->setFocusPolicy(Qt::NoFocus);
    QObject::connect(musicSlider, &QSlider::valueChanged, &dialog, [musicValue](int value)
                     { musicValue->setText(settingsText("Music volume: %1%").arg(value)); });
    musicValue->setText(settingsText("Music volume: %1%").arg(musicSlider->value()));
    auto *duplicatesCheck = checkBoxRow(
        settingsText("Enable Duplicate Merge in Optimization"),
        settings.value(QStringLiteral("optimization/duplicateMergeEnabled"), true).toBool(),
        settingsText("Enabled by default. When enabled, Optimization adds the Duplicate Merge review step."));
    auto *backupCheck = checkBoxRow(
        settingsText("Create backup files before applying changes"),
        settings.value(QStringLiteral("backup/enabled"), true).toBool(),
        settingsText("When disabled, SC2 archives and folders are edited without creating persistent .bak or backup_ copies."));
    const bool savedClosedProjectMode = settings.value(QStringLiteral("optimization/closedProjectMode"), false).toBool();
    auto *closedProjectCheck = checkBoxRow(
        settingsText("Closed Project / Aggressive standardization"),
        savedClosedProjectMode,
        settingsText("Allows public SC2Mod/SC2Campaign IDs and imports to change. Enable only when every consuming map/mod is included in your project and will be updated together."));
    auto *closedProjectWarning = new QLabel(
        settingsText("WARNING: this mode can break external maps that depend on renamed catalog IDs or removed imports. A backup is strongly recommended."),
        &dialog);
    closedProjectWarning->setObjectName(QStringLiteral("inspectorSubtitle"));
    closedProjectWarning->setWordWrap(true);
    auto *startFullscreenCheck = checkBoxRow(settingsText("Start in full screen"),
                                             settings.value(QStringLiteral("ui/startFullscreen"), true).toBool());

    auto *languageLabel = new QLabel(settingsText("Interface language"), &dialog);
    languageLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    auto *languageCombo = new QComboBox(&dialog);
    const QString selectedLanguage = AppSettings::selectedLanguage();
    for (const LanguageOption &language : AppSettings::supportedLanguages()) {
        languageCombo->addItem(language.code == QStringLiteral("system")
                                   ? settingsText("System language") : language.nativeName,
                               language.code);
        if (language.code == selectedLanguage)
            languageCombo->setCurrentIndex(languageCombo->count() - 1);
    }

    auto *presetLabel = new QLabel(settingsText("Graphics profile"), &dialog);
    presetLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    auto *presetCombo = new QComboBox(&dialog);
    presetCombo->addItem(settingsText("Full"), QStringLiteral("full"));
    presetCombo->addItem(settingsText("Balanced"), QStringLiteral("balanced"));
    presetCombo->addItem(settingsText("Minimal"), QStringLiteral("minimal"));
    presetCombo->addItem(settingsText("Custom"), QStringLiteral("custom"));
    presetCombo->setCurrentIndex(qMax(0, presetCombo->findData(AppSettings::graphicsPreset())));

    auto *rendererLabel = new QLabel(settingsText("Rendering mode (restart required)"), &dialog);
    rendererLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    auto *rendererCombo = new QComboBox(&dialog);
    rendererCombo->addItem(settingsText("Automatic (recommended)"), QStringLiteral("auto"));
    rendererCombo->addItem(settingsText("Desktop OpenGL"), QStringLiteral("desktop"));
    rendererCombo->addItem(settingsText("Software rendering (compatibility)"), QStringLiteral("software"));
    rendererCombo->setCurrentIndex(qMax(0, rendererCombo->findData(AppSettings::renderer())));

    auto *imagePreviewCheck = checkBoxRow(settingsText("Preview images and textures"), AppSettings::previewImages());
    auto *modelPreviewCheck = checkBoxRow(settingsText("Preview M3 models"), AppSettings::previewModels());
    auto *modelAntialiasingCheck = checkBoxRow(settingsText("M3 geometry antialiasing"), AppSettings::modelAntialiasing());
    auto *previewLimitLabel = new QLabel(settingsText("Maximum preview file size (MiB)"), &dialog);
    previewLimitLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    auto *previewLimitSpin = new QSpinBox(&dialog);
    previewLimitSpin->setRange(1, 512);
    previewLimitSpin->setValue(AppSettings::previewLimitMiB());
    previewLimitSpin->setSuffix(settingsText(" MiB"));
    auto *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(12);

    auto *navigation = new QListWidget(&dialog);
    navigation->setObjectName(QStringLiteral("settingsNavigation"));
    navigation->setFixedWidth(230);
    navigation->setFocusPolicy(Qt::NoFocus);
    navigation->addItem(settingsText("Interface"));
    navigation->addItem(settingsText("Graphics & previews"));
    navigation->addItem(settingsText("Optimization"));
    body->addWidget(navigation);

    auto *pages = new QStackedWidget(&dialog);
    pages->setObjectName(QStringLiteral("settingsPages"));
    body->addWidget(pages, 1);

    const auto makePage = [&dialog, pages](const QString &titleText)
    {
        auto *page = new QFrame(&dialog);
        page->setObjectName(QStringLiteral("settingsPage"));
        auto *pageLayout = new QVBoxLayout(page);
        pageLayout->setContentsMargins(14, 12, 14, 14);
        pageLayout->setSpacing(10);
        auto *title = new QLabel(titleText, page);
        title->setObjectName(QStringLiteral("panelTitle"));
        addTextGlow(title, QColor(80, 255, 210, 210), 14.0);
        pageLayout->addWidget(title);
        pages->addWidget(page);
        return pageLayout;
    };

    auto *interfacePage = makePage(settingsText("INTERFACE SETTINGS"));
    interfacePage->addWidget(languageLabel);
    interfacePage->addWidget(languageCombo);
    interfacePage->addWidget(soundCheck);
    interfacePage->addWidget(animationCheck);
    interfacePage->addWidget(backgroundGlowCheck);
    interfacePage->addWidget(decorativeTexturesCheck);
    interfacePage->addWidget(customCursorCheck);
    interfacePage->addWidget(musicCheck);
    interfacePage->addWidget(musicValue);
    interfacePage->addWidget(musicSlider);
    interfacePage->addWidget(startFullscreenCheck);
    interfacePage->addStretch(1);

    auto *graphicsPage = makePage(settingsText("GRAPHICS AND PREVIEW SETTINGS"));
    graphicsPage->addWidget(presetLabel);
    graphicsPage->addWidget(presetCombo);
    graphicsPage->addWidget(rendererLabel);
    graphicsPage->addWidget(rendererCombo);
    graphicsPage->addWidget(imagePreviewCheck);
    graphicsPage->addWidget(modelPreviewCheck);
    graphicsPage->addWidget(modelAntialiasingCheck);
    graphicsPage->addWidget(previewLimitLabel);
    graphicsPage->addWidget(previewLimitSpin);
    graphicsPage->addStretch(1);

    auto *optimizationPage = makePage(settingsText("OPTIMIZATION SETTINGS"));
    optimizationPage->addWidget(duplicatesCheck);
    optimizationPage->addWidget(backupCheck);
    optimizationPage->addWidget(closedProjectCheck);
    optimizationPage->addWidget(closedProjectWarning);
    optimizationPage->addStretch(1);

    navigation->setCurrentRow(0);

    bool applyingPreset = false;
    const auto applyPresetToControls = [=, &applyingPreset](const QString &preset) {
        if (preset == QStringLiteral("custom"))
            return;
        applyingPreset = true;
        const bool full = preset == QStringLiteral("full");
        const bool balanced = preset == QStringLiteral("balanced");
        animationCheck->setChecked(full || balanced);
        backgroundGlowCheck->setChecked(full);
        decorativeTexturesCheck->setChecked(full || balanced);
        customCursorCheck->setChecked(full || balanced);
        imagePreviewCheck->setChecked(true);
        modelPreviewCheck->setChecked(full || balanced);
        modelAntialiasingCheck->setChecked(full);
        applyingPreset = false;
    };
    QObject::connect(presetCombo, &QComboBox::currentIndexChanged, &dialog, [=](int) {
        applyPresetToControls(presetCombo->currentData().toString());
    });
    const auto markCustom = [=, &applyingPreset](bool) {
        if (applyingPreset)
            return;
        if (presetCombo->currentData().toString() != QStringLiteral("custom"))
            presetCombo->setCurrentIndex(presetCombo->findData(QStringLiteral("custom")));
    };
    for (QCheckBox *control : {animationCheck, backgroundGlowCheck, decorativeTexturesCheck, customCursorCheck,
                               imagePreviewCheck, modelPreviewCheck, modelAntialiasingCheck})
        QObject::connect(control, &QCheckBox::toggled, &dialog, markCustom);
    QObject::connect(navigation, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    layout->addLayout(body, 1);

    auto *buttons = new QDialogButtonBox(&dialog);
    auto *saveSettingsButton = buttons->addButton(QDialogButtonBox::Save);
    auto *cancelSettingsButton = buttons->addButton(QDialogButtonBox::Cancel);
    saveSettingsButton->setObjectName(QStringLiteral("settingsSaveButton"));
    cancelSettingsButton->setObjectName(QStringLiteral("settingsCancelButton"));
    bool reanalyzeAfterSave = false;
    bool restartRequired = false;
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]()
            {
        if (!savedClosedProjectMode && closedProjectCheck->isChecked()
            && QMessageBox::warning(&dialog,
                                     settingsText("Enable Closed Project mode?"),
                                     settingsText("This allows public SC2Mod/SC2Campaign IDs and imports to change. External maps that are not updated together can break. Enable aggressive standardization?"),
                                    QMessageBox::Yes | QMessageBox::No,
                                    QMessageBox::No) != QMessageBox::Yes)
            return;
        settings.setValue(QStringLiteral("ui/buttonSounds"), soundCheck->isChecked());
        settings.setValue(QStringLiteral("ui/buttonAnimations"), animationCheck->isChecked());
        settings.setValue(QStringLiteral("ui/backgroundGlows"), backgroundGlowCheck->isChecked());
        settings.setValue(QStringLiteral("ui/decorativeTextures"), decorativeTexturesCheck->isChecked());
        settings.setValue(QStringLiteral("ui/customCursor"), customCursorCheck->isChecked());
        settings.setValue(QStringLiteral("graphics/preset"), presetCombo->currentData().toString());
        settings.setValue(QStringLiteral("preview/images"), imagePreviewCheck->isChecked());
        settings.setValue(QStringLiteral("preview/models"), modelPreviewCheck->isChecked());
        settings.setValue(QStringLiteral("preview/modelAntialiasing"), modelAntialiasingCheck->isChecked());
        settings.setValue(QStringLiteral("preview/maxFileMiB"), previewLimitSpin->value());
        const QString newLanguage = languageCombo->currentData().toString();
        const QString newRenderer = rendererCombo->currentData().toString();
        restartRequired = newRenderer != AppSettings::renderer()
            || customCursorCheck->isChecked() != savedCustomCursor;
        settings.setValue(QStringLiteral("ui/language"), newLanguage);
        TranslationManager::instance().setLanguage(newLanguage, false);
        settings.setValue(QStringLiteral("graphics/renderer"), newRenderer);
        settings.setValue(QStringLiteral("optimization/duplicateMergeEnabled"), duplicatesCheck->isChecked());
        settings.setValue(QStringLiteral("optimization/closedProjectMode"), closedProjectCheck->isChecked());
        settings.setValue(QStringLiteral("backup/enabled"), backupCheck->isChecked());
        settings.setValue(QStringLiteral("ui/startFullscreen"), startFullscreenCheck->isChecked());
        AudioManager::setMusicSettings(musicCheck->isChecked(), musicSlider->value() / 100.0);
        m_window.setDuplicateMergeEnabled(duplicatesCheck->isChecked());
        if (auto *root = m_window.findChild<QWidget *>(QStringLiteral("workspaceRoot")))
            root->update();
        if (!m_window.m_result.nodes.isEmpty())
            m_window.refreshPages();
        reanalyzeAfterSave = savedClosedProjectMode != closedProjectCheck->isChecked()
            && !m_window.m_currentSourcePath.isEmpty();
        dialog.accept(); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 0, 26, 2);
    buttonRow->setSpacing(8);
    buttonRow->addStretch(1);
    buttonRow->addWidget(buttons, 0, Qt::AlignRight);
    layout->addLayout(buttonRow);
    titleBar->installEventFilter(new DialogDragFilter(&dialog));
    ScopedModalBackdrop backdrop(&m_window);
    animateModalOpen(&dialog);
    dialog.exec();
    if (restartRequired)
        QMessageBox::information(&m_window, settingsText("Restart required"),
                                 settingsText("Rendering mode and cursor changes will be fully applied after restarting SC2 Data Helper."));
    if (reanalyzeAfterSave)
        m_window.loadPathAndAnalyze(m_window.m_currentSourcePath);
}
}
