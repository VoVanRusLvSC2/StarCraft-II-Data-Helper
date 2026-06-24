#include "app/MainWindow.h"
#include "app/AudioManager.h"
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
#include "core/Sc2Archive.h"
#include "core/StandardNamePlanner.h"
#include "core/UnitFamilyDetector.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGraphicsOpacityEffect>
#include <QGraphicsDropShadowEffect>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QIODevice>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSaveFile>
#include <QStandardPaths>
#include <QAbstractAnimation>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QIcon>
#include <QKeySequence>
#include <QStatusBar>
#include <QStyle>
#include <QSlider>
#include <QTabWidget>
#include <QTabBar>
#include <QToolTip>
#include <QHelpEvent>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QTemporaryDir>
#include <QToolButton>
#include <QToolBar>
#include <QTimer>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmsystem.h>
#endif

#include <spdlog/spdlog.h>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <optional>
#include <algorithm>
#include <vector>

namespace
{
    bool isArchiveReferenceEntry(const QString &entry)
    {
        const QString normalized = QDir::cleanPath(entry).replace('\\', '/').toLower();
        const QString name = normalized.section('/', -1);
        static const QSet<QString> exactNames = {
            QStringLiteral("objects"), QStringLiteral("units"), QStringLiteral("regions"),
            QStringLiteral("triggers"), QStringLiteral("mapinfo"), QStringLiteral("documentinfo"),
            QStringLiteral("preload.xml"), QStringLiteral("componentlist.sc2components")};
        if (exactNames.contains(name)) return true;
        static const QSet<QString> extensions = {
            QStringLiteral("galaxy"), QStringLiteral("txt"), QStringLiteral("ini"),
            QStringLiteral("json"), QStringLiteral("yaml"), QStringLiteral("yml"),
            QStringLiteral("version"), QStringLiteral("sc2components")};
        return extensions.contains(QFileInfo(name).suffix().toLower());
    }

    void collectKnownIdTokens(const QByteArray &bytes, const QSet<QString> &knownIds, QSet<QString> *found)
    {
        const auto isIdChar = [](uchar value) {
            return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')
                || (value >= '0' && value <= '9') || value == '_' || value == '@';
        };
        for (qsizetype start = 0; start < bytes.size();) {
            while (start < bytes.size() && !isIdChar(uchar(bytes[start]))) ++start;
            qsizetype end = start;
            while (end < bytes.size() && isIdChar(uchar(bytes[end]))) ++end;
            if (end > start) {
                const QString token = QString::fromLatin1(bytes.constData() + start, end - start);
                if (knownIds.contains(token)) found->insert(token);
            }
            start = qMax(end, start + 1);
        }
        for (qsizetype start = 0; start + 1 < bytes.size();) {
            while (start + 1 < bytes.size()
                   && (!isIdChar(uchar(bytes[start])) || bytes[start + 1] != '\0')) ++start;
            qsizetype end = start;
            QByteArray tokenBytes;
            while (end + 1 < bytes.size() && isIdChar(uchar(bytes[end])) && bytes[end + 1] == '\0') {
                tokenBytes.append(bytes[end]);
                end += 2;
            }
            if (!tokenBytes.isEmpty()) {
                const QString token = QString::fromLatin1(tokenBytes);
                if (knownIds.contains(token)) found->insert(token);
            }
            start = qMax(end, start + 1);
        }
    }

    class PersistentTabToolTipFilter final : public QObject
    {
    public:
        using QObject::QObject;
    protected:
        bool eventFilter(QObject *watched, QEvent *event) override
        {
            auto *bar = qobject_cast<QTabBar *>(watched);
            if (!bar || (event->type() != QEvent::ToolTip && event->type() != QEvent::MouseMove))
                return QObject::eventFilter(watched, event);
            QPoint position;
            if (auto *help = dynamic_cast<QHelpEvent *>(event)) position = help->pos();
            else position = static_cast<QMouseEvent *>(event)->position().toPoint();
            const int index = bar->tabAt(position);
            if (index >= 0) {
                const QString text = bar->tabText(index).isEmpty() ? bar->tabToolTip(index) : bar->tabText(index);
                QToolTip::showText(bar->mapToGlobal(position), text, bar, bar->tabRect(index), 60000);
            }
            return event->type() == QEvent::ToolTip;
        }
    };
    void drawCoverPixmap(QPainter &painter, const QRect &target, const QPixmap &pixmap)
    {
        if (target.isEmpty() || pixmap.isNull()) return;
        const qreal targetRatio = qreal(target.width()) / qMax(1, target.height());
        const qreal sourceRatio = qreal(pixmap.width()) / qMax(1, pixmap.height());
        QRect source = pixmap.rect();
        if (sourceRatio > targetRatio) {
            const int width = qRound(pixmap.height() * targetRatio);
            source.setLeft((pixmap.width() - width) / 2);
            source.setWidth(width);
        } else {
            const int height = qRound(pixmap.width() / targetRatio);
            source.setTop((pixmap.height() - height) / 2);
            source.setHeight(height);
        }
        painter.drawPixmap(target, pixmap, source);
    }

    class Sc2BackgroundWidget final : public QWidget
    {
    public:
        explicit Sc2BackgroundWidget(QWidget *parent = nullptr)
            : QWidget(parent),
              m_grid(QStringLiteral(":/textures/ui_nova_storymode_bggrid.png")),
              m_points(QStringLiteral(":/textures/ui_nova_storymode_bgpointgrid.png")),
              m_lights(QStringLiteral(":/textures/ui_nova_login_backgroundlights.png")),
              m_frame(QStringLiteral(":/textures/ui_nova_archives_backgroundframe.png")),
              m_highlight(QStringLiteral(":/textures/ui_nova_archives_backgroundframehighlight.png")),
              m_scanlines(QStringLiteral(":/textures/ui_nova_archives_backgroundframe_scanlines.png"))
        {
            setAttribute(Qt::WA_StyledBackground, true);
        }

    protected:
        void paintEvent(QPaintEvent *event) override
        {
            QPainter painter(this);
            painter.fillRect(event->rect(), QColor(5, 11, 16));

            painter.setOpacity(0.12);
            painter.drawTiledPixmap(event->rect(), m_grid);
            painter.setOpacity(0.10);
            painter.drawTiledPixmap(event->rect(), m_points);

            painter.setOpacity(0.13);
            drawCoverPixmap(painter, rect(), m_lights);
            painter.setOpacity(0.20);
            drawCoverPixmap(painter, rect(), m_frame);
            painter.setOpacity(0.08);
            drawCoverPixmap(painter, rect(), m_highlight);
            painter.setOpacity(0.035);
            drawCoverPixmap(painter, rect(), m_scanlines);
        }

    private:
        QPixmap m_grid;
        QPixmap m_points;
        QPixmap m_lights;
        QPixmap m_frame;
        QPixmap m_highlight;
        QPixmap m_scanlines;
    };

    class ButtonEffects : public QObject
    {
    public:
        explicit ButtonEffects(QObject *parent = nullptr)
            : QObject(parent)
        {
            QFile soundFile(QStringLiteral(":/sounds/UI_NovaUT_ButtonClick1.wav"));
            if (soundFile.open(QIODevice::ReadOnly))
            {
                m_soundData = soundFile.readAll();
            }
        }

        void installOn(QWidget *widget)
        {
            if (!widget)
            {
                return;
            }
            widget->installEventFilter(this);
        }

    protected:
        bool eventFilter(QObject *watched, QEvent *event) override
        {
            auto *button = qobject_cast<QAbstractButton *>(watched);
            if (!button || !button->isEnabled())
            {
                return QObject::eventFilter(watched, event);
            }

            if (event->type() == QEvent::MouseButtonRelease)
            {
                auto *mouseEvent = static_cast<QMouseEvent *>(event);
                if (mouseEvent->button() == Qt::LeftButton)
                {
                    playClick(button);
                }
            }
            return QObject::eventFilter(watched, event);
        }

    private:
        void playClick(QAbstractButton *button)
        {
            QSettings settings;
#ifdef Q_OS_WIN
            if (settings.value(QStringLiteral("ui/buttonSounds"), true).toBool() && !m_soundData.isEmpty())
            {
                PlaySoundA(reinterpret_cast<LPCSTR>(m_soundData.constData()), nullptr,
                           SND_ASYNC | SND_MEMORY | SND_NODEFAULT | SND_NOSTOP);
            }
#else
            QApplication::beep();
#endif

            if (!settings.value(QStringLiteral("ui/buttonAnimations"), true).toBool())
            {
                return;
            }

            // Pulse the text only. A graphics effect on the whole button also
            // glows the texture border and produces unwanted extra colors.
            button->setProperty("textPulse", true);
            button->style()->unpolish(button);
            button->style()->polish(button);
            QTimer::singleShot(150, button, [button] {
                button->setProperty("textPulse", false);
                button->style()->unpolish(button);
                button->style()->polish(button);
            });
        }

        QByteArray m_soundData;
    };

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
            return QStringLiteral("Mode: XML file analysis");
        case 2:
            return QStringLiteral("Mode: archive analysis (read-only)");
        default:
            return QStringLiteral("Mode: waiting for analysis");
        }
    }

}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUi();
    setupLogging();
    setupTheme();
    AudioManager::instance()->initialize();
    QSettings settings;
    setDuplicateMergeEnabled(settings.value(QStringLiteral("optimization/duplicateMergeEnabled"), false).toBool());
    loadDefaultFolder();

    const QString rulesPath = runtimePath(QStringLiteral("config/rules.json"));
    const QString whitelistPath = runtimePath(QStringLiteral("config/whitelist.json"));
    QString errorMessage;
    if (m_configManager.load(rulesPath, whitelistPath, &errorMessage))
    {
        m_whitelistIds = m_configManager.whitelistIds();
        logLine(QStringLiteral("Loaded whitelist entries: %1").arg(m_whitelistIds.size()));
    }
    else
    {
        logLine(QStringLiteral("Config load skipped: %1").arg(errorMessage));
    }
}

void MainWindow::setupUi()
{
    setWindowTitle(QStringLiteral("SC2 Data Helper"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/Icon.png")));
    resize(1550, 980);

    auto *toolbar = addToolBar(QStringLiteral("Main"));
    toolbar->setObjectName(QStringLiteral("mainToolbar"));
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(0, 0));
    toolbar->setMinimumHeight(58);

    m_openFileAction = toolbar->addAction(QStringLiteral("Open SC2 File"));
    m_analyzeAction = toolbar->addAction(QStringLiteral("Analyze"));
    m_dryRunAction = toolbar->addAction(QStringLiteral("Optimization"));
    m_applyAction = toolbar->addAction(QStringLiteral("Review Optimization Plan"));
    m_settingsAction = toolbar->addAction(QStringLiteral("Settings"));
    m_dryRunAction->setEnabled(false);
    m_applyAction->setEnabled(false);

    m_pathEdit = new QLineEdit(toolbar);
    m_pathEdit->setObjectName(QStringLiteral("pathEdit"));
    m_pathEdit->setPlaceholderText(QStringLiteral("Selected source path"));
    m_pathEdit->setMinimumWidth(520);
    m_pathEdit->setReadOnly(true);
    m_pathEdit->setCursorPosition(0);
    toolbar->addSeparator();
    toolbar->addWidget(m_pathEdit);
    toolbar->addSeparator();
    m_exitAction = toolbar->addAction(QStringLiteral("Exit"));

    auto *root = new Sc2BackgroundWidget(this);
    root->setObjectName(QStringLiteral("workspaceRoot"));
    auto *rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto *splitterFrame = new QFrame(root);
    splitterFrame->setObjectName(QStringLiteral("workspaceFrame"));
    auto *splitterLayout = new QVBoxLayout(splitterFrame);
    splitterLayout->setContentsMargins(0, 0, 0, 0);

    m_tabs = new QTabWidget(splitterFrame);
    m_tabs->setObjectName(QStringLiteral("workspaceTabs"));
    m_analysisPage = new OverviewPage(m_tabs);
    m_dependenciesPage = new DependenciesPage(m_tabs);
    m_graphPage = new GraphPage(m_tabs);
    m_propertiesPage = new PropertiesPage(m_tabs);
    m_dataCollectionPage = new DataCollectionPage(m_tabs);
    m_renameIdsPage = new RenameIdsPage(m_tabs);
    m_duplicatesPage = new DuplicatesPage(m_tabs);
    m_cleanupPage = new UnusedPage(m_tabs);
    m_dryRunPage = new FormatterPage(this);
    m_dryRunPage->hide();
    m_logPanel = new LogPanel(m_tabs);
    m_xmlSourcePage = new XmlSourcePage(m_tabs);

    m_tabs->addTab(m_analysisPage, QStringLiteral("Objects"));
    m_tabs->addTab(m_dependenciesPage, QStringLiteral("Dependencies"));
    m_tabs->addTab(m_graphPage, QStringLiteral("Graph"));
    m_tabs->addTab(m_propertiesPage, QStringLiteral("Properties"));
    m_tabs->addTab(m_dataCollectionPage, QStringLiteral("Data Collection"));
    m_tabs->addTab(m_renameIdsPage, QStringLiteral("Rename To Standard"));
    m_tabs->addTab(m_duplicatesPage, QStringLiteral("Duplicate Merge"));
    m_tabs->addTab(m_cleanupPage, QStringLiteral("Unused Objects"));
    m_tabs->addTab(m_logPanel, QStringLiteral("Logs"));
    m_tabs->addTab(m_xmlSourcePage, QStringLiteral("XML Source"));
    m_tabs->tabBar()->setExpanding(false);
    m_tabs->tabBar()->setUsesScrollButtons(true);
    m_tabs->tabBar()->setElideMode(Qt::ElideNone);
    for (int index = 0; index < m_tabs->count(); ++index) {
        const QString title = m_tabs->tabText(index);
        m_tabs->setTabToolTip(index, title);
        auto *label = new QLabel(title, m_tabs->tabBar());
        label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        QFont labelFont = label->font(); labelFont.setBold(true); label->setFont(labelFont);
        // Leave room for both edge glyphs and the glow. QTabBar clips custom
        // tab buttons to their size hint, which previously cut off letters.
        label->setFixedWidth(label->fontMetrics().horizontalAdvance(title) + 52);
        label->setMinimumHeight(30);
        label->setContentsMargins(12, 0, 12, 0);
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet(QStringLiteral("background: transparent; color: #e8fffb; padding: 0;"));
        m_tabs->tabBar()->setTabButton(index, QTabBar::LeftSide, label);
        m_tabs->setTabText(index, QString());
    }
    m_tabs->tabBar()->setMouseTracking(true);
    m_tabs->tabBar()->installEventFilter(new PersistentTabToolTipFilter(m_tabs->tabBar()));
    const auto updateTabGlow = [this](int selected) {
        for (int index = 0; index < m_tabs->count(); ++index) {
            auto *label = qobject_cast<QLabel *>(m_tabs->tabBar()->tabButton(index, QTabBar::LeftSide));
            if (!label) continue;
            auto *glow = new QGraphicsDropShadowEffect(label);
            glow->setOffset(0, 0);
            glow->setBlurRadius(index == selected ? 14.0 : 11.0);
            glow->setColor(index == selected ? QColor(255, 120, 38, 235) : QColor(65, 235, 210, 185));
            label->setStyleSheet(index == selected
                ? QStringLiteral("background: transparent; color: #fff4e9; padding: 0;")
                : QStringLiteral("background: transparent; color: #e8fffb; padding: 0;"));
            label->setGraphicsEffect(glow);
        }
    };
    updateTabGlow(m_tabs->currentIndex());
    connect(m_tabs, &QTabWidget::currentChanged, this, updateTabGlow);

    splitterLayout->addWidget(m_tabs);
    rootLayout->addWidget(splitterFrame, 1);
    setCentralWidget(root);
    connect(m_openFileAction, &QAction::triggered, this, &MainWindow::openSc2File);
    connect(m_analyzeAction, &QAction::triggered, this, &MainWindow::analyzeFolder);
    connect(m_dryRunAction, &QAction::triggered, this, &MainWindow::runDryRun);
    connect(m_applyAction, &QAction::triggered, this, &MainWindow::showDryRunTab);
    connect(m_settingsAction, &QAction::triggered, this, &MainWindow::showSettingsDialog);
    connect(m_exitAction, &QAction::triggered, this, &QWidget::close);
    connect(m_duplicatesPage, &DuplicatesPage::previewMergeRequested, this, &MainWindow::previewMerge);
    connect(m_duplicatesPage, &DuplicatesPage::applyMergeRequested, this, &MainWindow::applyMerge);
    connect(m_cleanupPage, &UnusedPage::previewDeletionRequested, this, &MainWindow::previewUnusedDeletion);
    connect(m_cleanupPage, &UnusedPage::applyDeletionRequested, this, &MainWindow::applyUnusedDeletion);
    connect(m_renameIdsPage, &RenameIdsPage::previewRequested, this, &MainWindow::previewStandardRename);
    connect(m_renameIdsPage, &RenameIdsPage::applyRequested, this, &MainWindow::applyStandardRename);
    connect(m_renameIdsPage, &RenameIdsPage::exportRequested, this, &MainWindow::exportStandardRenameReport);
    connect(m_dataCollectionPage, &DataCollectionPage::previewRequested, this, &MainWindow::previewDataCollection);
    connect(m_dataCollectionPage, &DataCollectionPage::applyRequested, this, &MainWindow::applyDataCollection);
    connect(m_dataCollectionPage, &DataCollectionPage::exportRequested, this, &MainWindow::exportDataCollectionReport);
    connect(m_dryRunPage, &FormatterPage::previewBuilt, this, [this] { m_applyAction->setEnabled(true); });
    connect(m_dryRunPage, &FormatterPage::openUnusedRequested, this, [this](const QVector<int> &rows) {
        if (m_optimizationDialog) m_optimizationDialog->accept();
        m_cleanupPage->selectRows(rows); m_tabs->setCurrentWidget(m_cleanupPage);
    });
    connect(m_dryRunPage, &FormatterPage::openDuplicateRequested, this, [this](const MergeRequest &request) {
        if (m_optimizationDialog) m_optimizationDialog->accept();
        m_duplicatesPage->selectRequest(request); m_tabs->setCurrentWidget(m_duplicatesPage);
    });
    connect(m_dryRunPage, &FormatterPage::openRenameRequested, this, [this] {
        if (m_optimizationDialog) m_optimizationDialog->accept(); m_tabs->setCurrentWidget(m_renameIdsPage);
    });
    connect(m_dryRunPage, &FormatterPage::openCollectionRequested, this, [this] {
        if (m_optimizationDialog) m_optimizationDialog->accept(); m_tabs->setCurrentWidget(m_dataCollectionPage);
    });
    connect(m_dryRunPage, &FormatterPage::applyWizardRequested, this, &MainWindow::applyOptimizationWizardPlan);
    connect(m_dryRunPage, &FormatterPage::wizardFinished, this, [this] {
        if (m_optimizationDialog) m_optimizationDialog->accept();
    });
    connect(m_duplicatesPage, &DuplicatesPage::sourceRequested, this, [this](int nodeIndex) {
        m_xmlSourcePage->showNode(nodeIndex);
        m_tabs->setCurrentWidget(m_xmlSourcePage);
    });

    auto *undoAction = new QAction(QStringLiteral("Undo"), this);
    undoAction->setShortcut(QKeySequence::Undo);
    undoAction->setShortcutContext(Qt::ApplicationShortcut);
    addAction(undoAction);
    connect(undoAction, &QAction::triggered, this, &MainWindow::undoFocusedEditor);

    auto *redoAction = new QAction(QStringLiteral("Redo"), this);
    redoAction->setShortcuts({QKeySequence::Redo, QKeySequence(Qt::CTRL | Qt::Key_Y)});
    redoAction->setShortcutContext(Qt::ApplicationShortcut);
    addAction(redoAction);
    connect(redoAction, &QAction::triggered, this, &MainWindow::redoFocusedEditor);
    connect(m_analysisPage, &OverviewPage::folderPathChanged, this, [this](const QString &folder)
            { m_rootFolder = folder; });
    connect(m_analysisPage, &OverviewPage::currentRowChanged, this, [this](int row)
            {
        m_dependenciesPage->setCurrentRow(row);
        m_graphPage->setCurrentRow(row);
        m_propertiesPage->setCurrentRow(row);
        if (m_tabs && m_tabs->currentWidget() == m_graphPage) {
            QMetaObject::invokeMethod(m_graphPage, "fitGraph", Qt::QueuedConnection);
        }
        if (row >= 0 && row < m_result.nodes.size()) {
            const DataNode &node = m_result.nodes[row];
            statusBar()->showMessage(QStringLiteral("Loaded path: %1 | Selected: %2 / %3")
                                         .arg(m_currentSourcePath.isEmpty() ? m_rootFolder : m_currentSourcePath,
                                             node.elementName.isEmpty() ? node.id : node.elementName,
                                              node.id.isEmpty() ? QStringLiteral("-") : node.id));
        } });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index)
            {
        if (m_tabs->widget(index) == m_graphPage) {
            QMetaObject::invokeMethod(m_graphPage, "fitGraph", Qt::QueuedConnection);
        } });
    connect(m_analysisPage, &OverviewPage::objectDoubleClicked, this, [this](int row)
            { showGraphForRow(row); });

    statusBar()->showMessage(QStringLiteral("Ready"));

    auto *effects = new ButtonEffects(this);
    for (QAbstractButton *button : findChildren<QAbstractButton *>())
    {
        if (auto *toolButton = qobject_cast<QToolButton *>(button))
            toolButton->setMinimumWidth(qMax(toolButton->minimumWidth(), toolButton->fontMetrics().horizontalAdvance(toolButton->text()) + 42));
        effects->installOn(button);
    }

    for (QLabel *label : findChildren<QLabel *>())
    {
        const QString name = label->objectName();
        if (name == QStringLiteral("panelTitle") || name == QStringLiteral("modeBadge"))
        {
            auto *glow = new QGraphicsDropShadowEffect(label);
            glow->setBlurRadius(name == QStringLiteral("panelTitle") ? 16.0 : 10.0);
            glow->setColor(name == QStringLiteral("panelTitle")
                               ? QColor(74, 255, 203, 150)
                               : QColor(62, 175, 255, 120));
            glow->setOffset(0.0, 0.0);
            label->setGraphicsEffect(glow);
        }
    }
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

void MainWindow::showSettingsDialog()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("toolDialog"));
    dialog.setWindowTitle(QStringLiteral("SC2 Data Helper Settings"));
    dialog.setFixedSize(600, 430);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("INTERFACE SETTINGS"), &dialog);
    title->setObjectName(QStringLiteral("panelTitle"));
    layout->addWidget(title);

    QSettings settings;
    auto *soundCheck = new QCheckBox(QStringLiteral("Button sounds"), &dialog);
    soundCheck->setChecked(settings.value(QStringLiteral("ui/buttonSounds"), true).toBool());
    soundCheck->setFocusPolicy(Qt::NoFocus);
    auto *animationCheck = new QCheckBox(QStringLiteral("Button animations"), &dialog);
    animationCheck->setChecked(settings.value(QStringLiteral("ui/buttonAnimations"), true).toBool());
    animationCheck->setFocusPolicy(Qt::NoFocus);
    auto *musicCheck = new QCheckBox(QStringLiteral("Background music"), &dialog);
    musicCheck->setChecked(AudioManager::isMusicEnabled());
    musicCheck->setFocusPolicy(Qt::NoFocus);
    auto *musicValue = new QLabel(&dialog);
    musicValue->setObjectName(QStringLiteral("inspectorSubtitle"));
    auto *musicSlider = new QSlider(Qt::Horizontal, &dialog);
    musicSlider->setObjectName(QStringLiteral("backgroundMusicVolume"));
    musicSlider->setRange(0, 100);
    musicSlider->setValue(int(AudioManager::musicVolume() * 100.0));
    musicSlider->setFocusPolicy(Qt::NoFocus);
    QObject::connect(musicSlider, &QSlider::valueChanged, &dialog, [musicValue](int value) {
        musicValue->setText(QStringLiteral("Music volume: %1%").arg(value));
    });
    musicValue->setText(QStringLiteral("Music volume: %1%").arg(musicSlider->value()));
    auto *duplicatesCheck = new QCheckBox(QStringLiteral("Enable Duplicate Merge in Optimization"), &dialog);
    duplicatesCheck->setChecked(settings.value(QStringLiteral("optimization/duplicateMergeEnabled"), false).toBool());
    duplicatesCheck->setToolTip(QStringLiteral("Disabled by default. When enabled, Optimization adds the Duplicate Merge review step."));
    duplicatesCheck->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(soundCheck);
    layout->addWidget(animationCheck);
    layout->addWidget(musicCheck);
    layout->addWidget(musicValue);
    layout->addWidget(musicSlider);
    layout->addWidget(duplicatesCheck);
    layout->addStretch(1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        settings.setValue(QStringLiteral("ui/buttonSounds"), soundCheck->isChecked());
        settings.setValue(QStringLiteral("ui/buttonAnimations"), animationCheck->isChecked());
        settings.setValue(QStringLiteral("optimization/duplicateMergeEnabled"), duplicatesCheck->isChecked());
        AudioManager::setMusicSettings(musicCheck->isChecked(), musicSlider->value() / 100.0);
        setDuplicateMergeEnabled(duplicatesCheck->isChecked());
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
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
    const QString folder = defaultTestFolder();
    m_rootFolder = folder;
    m_sourceKind = SourceKind::Folder;
    m_pathEdit->setText(folder);
    m_analysisPage->setFolderPath(folder);
    m_analysisPage->setModeLabel(QStringLiteral("Mode: ready to analyze"));
    m_analysisPage->setOutputText(QStringLiteral("Loaded folder path. Press Analyze to scan it."));
    setCurrentSourcePath(folder);
    if (QFileInfo::exists(folder))
    {
        logLine(QStringLiteral("Default folder set: %1").arg(folder));
    }
    else
    {
        logLine(QStringLiteral("Default folder does not exist yet: %1").arg(folder));
    }
}

QString MainWindow::runtimePath(const QString &relativePath) const
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/") + relativePath;
}

void MainWindow::logLine(const QString &line)
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
    const QString filter = QStringLiteral("SC2 Files (*.SC2Map *.SC2Mod *.SC2Components *.SC2Campaign *.SC2Archive *.xml);;All Files (*.*)");
    const QString selected = QFileDialog::getOpenFileName(this, QStringLiteral("Open SC2 File"), m_currentSourcePath, filter);
    if (selected.isEmpty())
    {
        return;
    }
    const QFileInfo info(selected);
    const bool previousOptimizationEnabled = m_dryRunAction && m_dryRunAction->isEnabled();
    const bool previousReviewEnabled = m_applyAction && m_applyAction->isEnabled();
    if (info.suffix().compare(QStringLiteral("xml"), Qt::CaseInsensitive) == 0) m_sourceKind = SourceKind::XmlFile;
    else if (info.suffix().startsWith(QStringLiteral("SC2"), Qt::CaseInsensitive)) m_sourceKind = SourceKind::ArchiveFile;
    else m_sourceKind = SourceKind::Unknown;
    m_rootFolder = info.absolutePath();
    setCurrentSourcePath(selected);
    m_analysisPage->setFolderPath(selected);
    m_analysisPage->setModeLabel(QStringLiteral("Mode: ready to analyze"));
    m_analysisPage->setOutputText(QStringLiteral("File selected. Press Analyze to start scanning."));
    if (m_result.nodes.isEmpty())
        refreshPages();
    m_dryRunAction->setEnabled(previousOptimizationEnabled);
    m_applyAction->setEnabled(previousReviewEnabled);
    logLine(QStringLiteral("File selected without analysis: %1").arg(selected));
}

void MainWindow::analyzeFolder()
{
    const QString path = m_pathEdit ? m_pathEdit->text().trimmed() : QString();
    if (path.isEmpty() && m_currentSourcePath.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("Analyze"), QStringLiteral("Select a file or folder first."));
        return;
    }

    const QString effectivePath = path.isEmpty() ? m_currentSourcePath : path;
    if (!loadPathAndAnalyze(effectivePath))
    {
        return;
    }
}

bool MainWindow::loadPathAndAnalyze(const QString &path)
{
    QFileInfo info(path);
    const bool previousOptimizationEnabled = m_dryRunAction && m_dryRunAction->isEnabled();
    const bool previousReviewEnabled = m_applyAction && m_applyAction->isEnabled();
    m_dryRunAction->setEnabled(false);
    m_applyAction->setEnabled(false);
    if (!info.exists())
    {
        QMessageBox::warning(this, QStringLiteral("Load"), QStringLiteral("Path does not exist: %1").arg(path));
        logLine(QStringLiteral("Path does not exist: %1").arg(path));
        return false;
    }

    const AnalysisResult previousResult = m_result;
    QString errorMessage;
    bool ok = false;
    AnalysisProgressDialog progress(this);
    m_activeProgressDialog = &progress;
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
        m_sourceKind = SourceKind::Folder;
        ok = analyzeFolderPath(path, &errorMessage);
    }
    else if (info.suffix().compare(QStringLiteral("xml"), Qt::CaseInsensitive) == 0)
    {
        m_sourceKind = SourceKind::XmlFile;
        ok = analyzeXmlFile(path, &errorMessage);
    }
    else if (info.suffix().compare(QStringLiteral("SC2Map"), Qt::CaseInsensitive) == 0 || info.suffix().compare(QStringLiteral("SC2Mod"), Qt::CaseInsensitive) == 0 || info.suffix().compare(QStringLiteral("SC2Components"), Qt::CaseInsensitive) == 0 || info.suffix().compare(QStringLiteral("SC2Campaign"), Qt::CaseInsensitive) == 0 || info.suffix().compare(QStringLiteral("SC2Archive"), Qt::CaseInsensitive) == 0)
    {
        m_sourceKind = SourceKind::ArchiveFile;
        ok = analyzeArchiveFile(path, &errorMessage);
    }
    else
    {
        m_sourceKind = SourceKind::Unknown;
        errorMessage = QStringLiteral("Unsupported path type: %1").arg(path);
        ok = false;
    }
    if (progress.isCancelled()) {
        ok = false;
        errorMessage = QStringLiteral("Analysis canceled.");
    }
    progress.setProgress(ok ? 88 : 100,
                         ok ? QStringLiteral("Building object registry") : QStringLiteral("Analysis failed"),
                         ok ? QStringLiteral("Preparing tables, references and reports") : errorMessage);
    QApplication::processEvents();

    if (!ok)
    {
        m_result = previousResult;
        m_dryRunAction->setEnabled(previousOptimizationEnabled);
        m_applyAction->setEnabled(previousReviewEnabled);
        m_activeProgressDialog = nullptr;
        progress.close();
        if (errorMessage == QStringLiteral("Analysis canceled.")) {
            statusBar()->showMessage(QStringLiteral("Analysis canceled. No partial result was applied."), 8000);
            logLine(QStringLiteral("Analysis canceled by user."));
        } else {
            QMessageBox::critical(this, QStringLiteral("Analysis failed"), errorMessage);
            logLine(QStringLiteral("Analysis failed: %1").arg(errorMessage));
        }
        return false;
    }

    m_currentSourcePath = path;
    m_rootFolder = info.isDir() ? path : info.absolutePath();
    m_analysisPage->setFolderPath(path);
    m_analysisPage->setModeLabel(modeLabelFor(static_cast<int>(m_sourceKind)));
    m_analysisPage->setAnalysisResult(m_result);
    refreshPages();
    writeAnalysisReportFile();
    progress.setProgress(100,
                         QStringLiteral("Analysis complete"),
                         QStringLiteral("%1 XML files | %2 objects")
                             .arg(m_result.totalXmlFiles())
                             .arg(m_result.totalDataNodes()));
    QApplication::processEvents();
    progress.close();
    m_activeProgressDialog = nullptr;
    showAnalysisTab();
    m_dryRunAction->setEnabled(true);
    m_applyAction->setEnabled(false);
    setCurrentSourcePath(path);
    logLine(QStringLiteral("Scanned files: %1").arg(m_result.totalFilesScanned()));
    logLine(QStringLiteral("XML files: %1").arg(m_result.totalXmlFiles()));
    logLine(QStringLiteral("Data nodes: %1").arg(m_result.totalDataNodes()));
    logLine(QStringLiteral("Duplicate IDs: %1").arg(m_result.duplicateIdGroups.size()));
    logLine(QStringLiteral("Duplicate content groups: %1").arg(m_result.duplicateContentGroups.size()));
    logLine(QStringLiteral("Parse errors: %1").arg(m_result.parseErrors.size()));
    for (const ParseErrorInfo &error : m_result.parseErrors)
    {
        logLine(QStringLiteral("Parse error: %1 -> %2").arg(error.filePath, error.message));
    }
    for (const DuplicateIdGroup &group : m_result.duplicateIdGroups)
    {
        logLine(QStringLiteral("Duplicate ID group: %1 (%2 nodes)").arg(group.id).arg(group.nodeIndices.size()));
    }
    for (const DuplicateContentGroup &group : m_result.duplicateContentGroups)
    {
        logLine(QStringLiteral("Duplicate content group: %1 (%2 nodes)").arg(group.contentHash.left(12)).arg(group.nodeIndices.size()));
    }
    return true;
}

bool MainWindow::analyzeFolderPath(const QString &folderPath, QString *errorMessage)
{
    return m_analyzer.analyzeFolder(folderPath, m_whitelistIds, &m_result, errorMessage,
        [this](int current, int total, const QString &file) {
            if (!m_activeProgressDialog) return;
            const int percent = total > 0 ? 22 + (current * 62 / total) : 22;
            m_activeProgressDialog->setProgress(percent, QStringLiteral("Scanning XML and data files"),
                                                file.isEmpty() ? QStringLiteral("Finalizing scan") : QDir::toNativeSeparators(file));
            QApplication::processEvents();
        },
        [this] { return m_activeProgressDialog && m_activeProgressDialog->isCancelled(); });
}

bool MainWindow::analyzeXmlFile(const QString &filePath, QString *errorMessage)
{
    if (m_activeProgressDialog && m_activeProgressDialog->isCancelled()) {
        if (errorMessage) *errorMessage = QStringLiteral("Analysis canceled.");
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
        if (entry.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)) xmlEntries.append(entry);
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
        if (m_activeProgressDialog) {
            if (m_activeProgressDialog->isCancelled()) {
                if (errorMessage) *errorMessage = QStringLiteral("Analysis canceled.");
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

    if (!m_analyzer.finalizeAnalysisResult(&m_result, m_whitelistIds, errorMessage,
        [this] {
            if (!m_activeProgressDialog)
                return;
            m_activeProgressDialog->setProgress(85,
                                                QStringLiteral("Analyzing extracted XML"),
                                                QStringLiteral("Building references, duplicate groups and candidates"));
            QApplication::processEvents();
        },
        [this] { return m_activeProgressDialog && m_activeProgressDialog->isCancelled(); }))
    {
        return false;
    }

    QSet<QString> knownIds;
    for (const DataNode &node : m_result.nodes)
        if (!node.id.isEmpty()) knownIds.insert(node.id);
    m_archiveReferenceScanComplete = true;
    const QStringList archiveEntries = archive.allEntries();
    int scannedReferenceEntries = 0;
    for (int index = 0; index < archiveEntries.size(); ++index) {
        const QString &entry = archiveEntries[index];
        if (entry.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive) || !isArchiveReferenceEntry(entry))
            continue;
        if (m_activeProgressDialog) {
            if (m_activeProgressDialog->isCancelled()) {
                if (errorMessage) *errorMessage = QStringLiteral("Analysis canceled.");
                return false;
            }
            m_activeProgressDialog->setProgress(86, QStringLiteral("Checking archive references"), entry);
            QApplication::processEvents();
        }
        QByteArray bytes;
        QString readError;
        if (!archive.readEntry(entry, &bytes, &readError)) {
            m_archiveReferenceScanComplete = false;
            logLine(QStringLiteral("Archive reference scan failed for %1: %2").arg(entry, readError));
            continue;
        }
        collectKnownIdTokens(bytes, knownIds, &m_archiveReferencedIds);
        ++scannedReferenceEntries;
    }
    logLine(QStringLiteral("Archive reference-bearing entries scanned: %1; referenced IDs found: %2")
                .arg(scannedReferenceEntries).arg(m_archiveReferencedIds.size()));

    normalizeArchiveAnalysis(&m_result, QString(), filePath);
    return true;
}

void MainWindow::normalizeArchiveAnalysis(AnalysisResult *analysis, const QString &tempRoot,
                                          const QString &archivePath) const
{
    if (!analysis) return;
    if (!tempRoot.isEmpty()) {
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
    if (!analysis) return;
    analysis->possibleUnusedNodeIndices.clear();
    for (UnusedCandidateInfo &candidate : analysis->unusedCandidates) {
        if (candidate.state != CandidateState::Safe) continue;
        const bool externallyReferenced = candidate.nodeIndex >= 0 && candidate.nodeIndex < analysis->nodes.size()
            && m_archiveReferencedIds.contains(analysis->nodes[candidate.nodeIndex].id);
        if (!m_archiveReferenceScanComplete || externallyReferenced) {
            candidate.state = CandidateState::Blocked;
            candidate.protectedObject = true;
            candidate.reason = !m_archiveReferenceScanComplete
                ? QStringLiteral("Archive reference scan was incomplete")
                : QStringLiteral("Referenced by archive placement, trigger, or script data");
            candidate.riskLevel = QStringLiteral("high");
            if (candidate.nodeIndex >= 0 && candidate.nodeIndex < analysis->nodes.size())
                analysis->nodes[candidate.nodeIndex].candidateUnused = false;
        } else {
            candidate.reason = QStringLiteral("No XML, script/text, placement, trigger, or archive references");
            candidate.riskLevel = QStringLiteral("low");
            analysis->possibleUnusedNodeIndices.append(candidate.nodeIndex);
        }
    }
}

bool MainWindow::materializeArchiveAnalysis(const QString &tempRoot, AnalysisResult *analysis, QString *errorMessage) const
{
    if (!analysis || m_sourceKind != SourceKind::ArchiveFile) {
        if (errorMessage) *errorMessage = QStringLiteral("Archive analysis is not available.");
        return false;
    }
    *analysis = m_result;
    QHash<QString, QString> absoluteSources;
    for (auto it = m_result.sourceXmlByFile.cbegin(); it != m_result.sourceXmlByFile.cend(); ++it) {
        const QString relative = QDir::cleanPath(it.key()).replace('\\', '/');
        if (relative.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(relative)) {
            if (errorMessage) *errorMessage = QStringLiteral("Unsafe archive entry path: %1").arg(it.key());
            return false;
        }
        const QString target = QDir(tempRoot).absoluteFilePath(relative);
        QDir().mkpath(QFileInfo(target).absolutePath());
        QSaveFile file(target);
        const QByteArray bytes = it.value().toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            if (errorMessage) *errorMessage = QStringLiteral("Unable to materialize archive XML: %1").arg(relative);
            return false;
        }
        absoluteSources.insert(target, it.value());
    }
    Sc2Archive archive;
    QString archiveError;
    if (archive.load(m_currentSourcePath, &archiveError)) {
        QByteArray listfileBytes;
        if (!archive.readEntry(QStringLiteral("(listfile)"), &listfileBytes, &archiveError)) {
            QStringList entries;
            for (QString entry : archive.allEntries()) entries << entry.replace('/', '\\');
            listfileBytes = entries.join(QStringLiteral("\r\n")).toUtf8() + QByteArrayLiteral("\r\n");
        }
        QSaveFile listfile(QDir(tempRoot).absoluteFilePath(QStringLiteral("(listfile)")));
        if (!listfile.open(QIODevice::WriteOnly) || listfile.write(listfileBytes) != listfileBytes.size() || !listfile.commit()) {
            if (errorMessage) *errorMessage = QStringLiteral("Unable to materialize archive (listfile).");
            return false;
        }
    }
    for (ScannedFileInfo &file : analysis->scannedFiles)
        file.filePath = QDir(tempRoot).absoluteFilePath(QDir::cleanPath(file.filePath));
    for (DataNode &node : analysis->nodes)
        node.sourceFile = QDir(tempRoot).absoluteFilePath(QDir::cleanPath(node.sourceFile));
    analysis->sourceXmlByFile = absoluteSources;
    analysis->rootFolder = tempRoot;
    return true;
}

bool MainWindow::commitArchiveChanges(const QString &tempRoot, const QStringList &changedFiles,
                                      QString *backupPath, QString *errorMessage) const
{
    if (changedFiles.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("No archive entries changed.");
        return false;
    }
    Sc2Archive archive;
    if (!archive.load(m_currentSourcePath, errorMessage)) return false;
    QHash<QString, QByteArray> replacements;
    for (const QString &relativeFile : changedFiles) {
        QString normalized = QDir::cleanPath(relativeFile).replace('\\', '/');
        QString archiveName;
        for (const QString &entry : archive.allEntries()) {
            if (QDir::cleanPath(entry).replace('\\', '/').compare(normalized, Qt::CaseInsensitive) == 0) {
                archiveName = entry;
                break;
            }
        }
        if (archiveName.isEmpty()) archiveName = normalized.replace('/', '\\');
        QFile file(QDir(tempRoot).absoluteFilePath(relativeFile));
        if (!file.open(QIODevice::ReadOnly)) {
            if (errorMessage) *errorMessage = QStringLiteral("Unable to read changed XML: %1").arg(relativeFile);
            return false;
        }
        replacements.insert(archiveName, file.readAll());
    }

    BackupManager backupManager;
    QString backup;
    if (!backupManager.createBackup(m_currentSourcePath, &backup, errorMessage)) return false;
    const QString pending = m_currentSourcePath + QStringLiteral(".sc2dh.pending");
    QFile::remove(pending);
    if (!archive.saveCopy(pending, replacements, {}, errorMessage)) return false;

    QFile pendingFile(pending);
    if (!pendingFile.open(QIODevice::ReadOnly)) {
        if (errorMessage) *errorMessage = QStringLiteral("Unable to read verified archive copy.");
        QFile::remove(pending);
        return false;
    }
    QSaveFile destination(m_currentSourcePath);
    const QByteArray archiveBytes = pendingFile.readAll();
    if (!destination.open(QIODevice::WriteOnly) || destination.write(archiveBytes) != archiveBytes.size() || !destination.commit()) {
        if (errorMessage) *errorMessage = QStringLiteral("Unable to atomically replace the archive; original file was preserved.");
        QFile::remove(pending);
        return false;
    }
    QFile::remove(pending);

    // saveCopy already verifies every rewritten entry before returning. The
    // final QSaveFile write is a byte-for-byte atomic copy of that verified
    // archive, so reopening and extracting every entry here was redundant.
    if (backupPath) *backupPath = backup;
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

    if (m_sourceKind == SourceKind::ArchiveFile)
    {
        QMessageBox::information(this, QStringLiteral("Apply"),
                                 QStringLiteral("Archive apply is not available in this build. Use folder or XML input."));
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

    const QString backupHint = QStringLiteral("This will create a backup folder before editing. Continue?");
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
    if (m_sourceKind == SourceKind::ArchiveFile) {
        QTemporaryDir workspace;
        AnalysisResult materialized;
        QString error;
        if (!workspace.isValid() || !materializeArchiveAnalysis(workspace.path(), &materialized, &error)) {
            m_duplicatesPage->setPreviewText(QStringLiteral("Archive preview failed: %1").arg(error), false);
            return;
        }
        preview = m_mergeService.preview(materialized, request);
    } else {
        preview = m_mergeService.preview(m_result, request);
    }
    m_previewedMerge = request;
    m_mergePreviewValid = preview.valid;
    m_duplicatesPage->setPreviewText(preview.reportText.isEmpty() ? preview.warnings.join(QStringLiteral("\n")) : preview.reportText,
                                     preview.valid);
    if (!preview.reportText.isEmpty()) {
        m_result.analysisReportText = m_analyzer.buildAnalysisReport(m_result)
            + QStringLiteral("\n") + preview.reportText;
        writeAnalysisReportFile();
    }
}

void MainWindow::applyMerge(const MergeRequest &request)
{
    const bool sameRequest = request.keepNodeIndex == m_previewedMerge.keepNodeIndex
        && request.removeNodeIndices == m_previewedMerge.removeNodeIndices;
    if (!m_mergePreviewValid || !sameRequest) {
        QMessageBox::warning(this, QStringLiteral("Apply Merge"),
                             QStringLiteral("Preview this exact merge selection before applying it."));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("Apply Merge"),
                              QStringLiteral("Create a backup, redirect references, verify, and delete the selected duplicates?"))
        != QMessageBox::Yes) return;

    if (m_sourceKind == SourceKind::ArchiveFile) {
        QTemporaryDir workspace;
        AnalysisResult materialized;
        QString error;
        if (!workspace.isValid() || !materializeArchiveAnalysis(workspace.path(), &materialized, &error)) {
            QMessageBox::critical(this, QStringLiteral("Merge failed"), error);
            return;
        }
        const MergeApplyResult result = m_mergeService.apply(materialized, request, workspace.path(), m_whitelistIds);
        if (!result.success) {
            QMessageBox::critical(this, QStringLiteral("Merge failed"), result.error + QStringLiteral("\nThe archive was not changed."));
            return;
        }
        QString archiveBackup;
        if (!commitArchiveChanges(workspace.path(), result.changedFiles, &archiveBackup, &error)) {
            QMessageBox::critical(this, QStringLiteral("Merge failed"), error + QStringLiteral("\nNo partial archive change was retained."));
            return;
        }
        m_mergePreviewValid = false;
        if (!loadPathAndAnalyze(m_currentSourcePath)) {
            QMessageBox::warning(this, QStringLiteral("Merge saved"), QStringLiteral("The archive was saved, but automatic re-analysis failed. Backup: %1").arg(archiveBackup));
            return;
        }
        m_dryRunPage->recordMergeResult(result.nodesDeleted, result.referencesRedirected);
        QMessageBox::information(this, QStringLiteral("Merge complete"),
                                 QStringLiteral("Archive backup: %1\nFiles changed: %2\nReferences redirected: %3\nNodes deleted: %4")
                                     .arg(archiveBackup).arg(result.changedFiles.size())
                                     .arg(result.referencesRedirected).arg(result.nodesDeleted));
        return;
    }
    const MergeApplyResult result = m_mergeService.apply(m_result, request, m_rootFolder, m_whitelistIds);
    if (!result.success) {
        QMessageBox::critical(this, QStringLiteral("Merge failed"),
                              result.error + QStringLiteral("\nNo partial merge was retained."));
        logLine(QStringLiteral("Merge failed: %1").arg(result.error));
        return;
    }
    m_mergePreviewValid = false;
    logLine(QStringLiteral("Merge backup: %1").arg(result.backupFolder));
    logLine(QStringLiteral("Merge redirected %1 references and deleted %2 nodes.")
                .arg(result.referencesRedirected).arg(result.nodesDeleted));
    loadPathAndAnalyze(m_currentSourcePath);
    m_dryRunPage->recordMergeResult(result.nodesDeleted, result.referencesRedirected);
    QMessageBox::information(this, QStringLiteral("Merge complete"),
                             QStringLiteral("Backup: %1\nFiles changed: %2\nReferences redirected: %3\nNodes deleted: %4")
                                 .arg(result.backupFolder).arg(result.changedFiles.size())
                                 .arg(result.referencesRedirected).arg(result.nodesDeleted));
}

void MainWindow::previewUnusedDeletion(const QVector<int> &rows)
{
    m_previewedUnusedRows = rows;
    m_cleanupPage->setPreviewText(m_analyzer.buildDryRunReport(m_result, rows));
}

void MainWindow::applyUnusedDeletion(const QVector<int> &rows)
{
    if (rows.isEmpty() || rows != m_previewedUnusedRows) {
        QMessageBox::warning(this, QStringLiteral("Delete Unused Objects"),
                             QStringLiteral("Preview this exact selection before deletion."));
        return;
    }
    if (m_sourceKind == SourceKind::ArchiveFile) {
        if (!m_archiveReferenceScanComplete) {
            QMessageBox::information(this, QStringLiteral("Delete Unused Objects"),
                                     QStringLiteral("Blocked: the archive reference scan was incomplete."));
            return;
        }
        if (QMessageBox::question(this, QStringLiteral("Delete Selected Unused Objects"),
                                  QStringLiteral("Create an archive backup, delete the selected verified candidates, and atomically save the SC2 archive?"))
            != QMessageBox::Yes) return;
        QTemporaryDir workspace;
        AnalysisResult materialized;
        QString error;
        if (!workspace.isValid() || !materializeArchiveAnalysis(workspace.path(), &materialized, &error)) {
            QMessageBox::critical(this, QStringLiteral("Deletion failed"), error);
            return;
        }
        QVector<WizardNodeRef> selectedRefs;
        for (int row : rows) {
            if (row < 0 || row >= m_result.nodes.size()) continue;
            const DataNode &node = m_result.nodes[row];
            selectedRefs.append({node.id, node.elementName, node.sourceFile, node.originalLocation});
        }
        if (!m_analyzer.analyzeFolder(workspace.path(), m_whitelistIds, &materialized, &error)) {
            QMessageBox::critical(this, QStringLiteral("Deletion failed"), error);
            return;
        }
        applyArchiveReferenceSafety(&materialized);
        QVector<int> refreshedRows;
        for (const WizardNodeRef &ref : selectedRefs) {
            const int index = findNodeIndex(materialized, ref);
            if (index >= 0) refreshedRows.append(index);
        }
        QString workspaceBackup;
        QStringList changedFiles;
        int removed = 0, skipped = 0;
        if (!m_analyzer.applySelectedChanges(materialized, refreshedRows, workspace.path(), m_whitelistIds,
                                             &workspaceBackup, &error, &changedFiles, &removed, &skipped)) {
            QMessageBox::critical(this, QStringLiteral("Deletion failed"), error);
            return;
        }
        QString archiveBackup;
        if (!commitArchiveChanges(workspace.path(), changedFiles, &archiveBackup, &error)) {
            QMessageBox::critical(this, QStringLiteral("Deletion failed"), error + QStringLiteral("\nThe original archive was preserved."));
            return;
        }
        m_previewedUnusedRows.clear();
        loadPathAndAnalyze(m_currentSourcePath);
        m_dryRunPage->recordUnusedResult(removed);
        QMessageBox::information(this, QStringLiteral("Deletion complete"),
                                 QStringLiteral("Archive backup: %1\nDeleted: %2\nSkipped: %3")
                                     .arg(archiveBackup).arg(removed).arg(skipped));
        return;
    }
    QString backupFolder, error;
    QStringList changedFiles;
    int removed = 0, skipped = 0;
    if (QMessageBox::question(this, QStringLiteral("Delete Selected Unused Objects"),
                              QStringLiteral("A backup will be created before deleting the selected safe candidates. Continue?"))
        != QMessageBox::Yes) return;
    if (!m_analyzer.applySelectedChanges(m_result, rows, m_rootFolder, m_whitelistIds,
                                         &backupFolder, &error, &changedFiles, &removed, &skipped)) {
        QMessageBox::critical(this, QStringLiteral("Deletion failed"), error);
        return;
    }
    m_previewedUnusedRows.clear();
    loadPathAndAnalyze(m_currentSourcePath);
    m_dryRunPage->recordUnusedResult(removed);
    QMessageBox::information(this, QStringLiteral("Deletion complete"),
                             QStringLiteral("Backup: %1\nDeleted: %2\nSkipped: %3")
                                 .arg(backupFolder).arg(removed).arg(skipped));
}

void MainWindow::previewStandardRename(const RenamePlan &plan)
{
    const RenamePreviewReport report = m_referenceRenamer.preview(m_result, plan);
    m_previewedRenamePlan = plan;
    m_renamePreviewValid = report.valid;
    m_renameIdsPage->setPreviewReport(report);
    if (m_sourceKind == SourceKind::ArchiveFile) m_renameIdsPage->setApplyAvailable(false);

    const QString reportPath = QDir(m_rootFolder).absoluteFilePath(QStringLiteral("rename_to_standard_preview.txt"));
    QSaveFile file(reportPath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file.write(report.reportText.toUtf8());
        file.commit();
    }
    logLine(QStringLiteral("Rename-to-standard preview: %1 renames, %2 reference updates, valid=%3")
                .arg(report.identitiesRenamed).arg(report.referencesUpdated).arg(report.valid ? QStringLiteral("yes") : QStringLiteral("no")));
}

void MainWindow::applyStandardRename(const RenamePlan &plan)
{
    if (m_sourceKind == SourceKind::ArchiveFile) {
        QMessageBox::information(this, QStringLiteral("Apply Rename"), QStringLiteral("Archive mode is preview-only."));
        return;
    }
    const auto signature = [](const RenamePlan &value) {
        QStringList parts;
        for (const RenamePlanItem &item : value.items) parts << item.oldId + QChar(0x1f) + item.newId;
        std::sort(parts.begin(), parts.end());
        return value.family.rootId + QChar(0x1e) + value.targetRootId + QChar(0x1e) + parts.join(QChar(0x1d));
    };
    if (!m_renamePreviewValid || signature(plan) != signature(m_previewedRenamePlan)) {
        QMessageBox::warning(this, QStringLiteral("Apply Rename"),
                             QStringLiteral("Preview this exact family and rename selection before applying."));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("Apply Rename"),
                              QStringLiteral("Create a backup, rename selected real XML IDs, update references, and verify?"))
        != QMessageBox::Yes) return;
    const RenameApplyResult result = m_referenceRenamer.apply(m_result, plan, m_rootFolder, m_whitelistIds);
    if (!result.success) {
        QMessageBox::critical(this, QStringLiteral("Rename failed"), result.error + QStringLiteral("\nChanges were rolled back."));
        return;
    }
    m_renamePreviewValid = false;
    const QString reportPath = QDir(m_rootFolder).absoluteFilePath(QStringLiteral("rename_to_standard_preview.txt"));
    QSaveFile reportFile(reportPath);
    if (reportFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        reportFile.write(result.finalReport.toUtf8());
        reportFile.commit();
    }
    loadPathAndAnalyze(m_currentSourcePath);
    m_dryRunPage->recordRenameResult(result.identitiesRenamed);
    QMessageBox::information(this, QStringLiteral("Rename complete"),
                             QStringLiteral("Backup: %1\nObjects renamed: %2\nReferences updated: %3")
                                 .arg(result.backupFolder).arg(result.identitiesRenamed).arg(result.referencesUpdated));
}

void MainWindow::exportStandardRenameReport(const QString &reportText)
{
    if (reportText.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Export Rename Report"), QStringLiteral("Preview a rename first."));
        return;
    }
    const QString selected = QFileDialog::getSaveFileName(this, QStringLiteral("Export Rename Report"),
        QDir(m_rootFolder).absoluteFilePath(QStringLiteral("rename_to_standard_preview.txt")), QStringLiteral("Text files (*.txt)"));
    if (selected.isEmpty()) return;
    QSaveFile file(selected);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text) || file.write(reportText.toUtf8()) != reportText.toUtf8().size() || !file.commit())
        QMessageBox::critical(this, QStringLiteral("Export Rename Report"), QStringLiteral("Unable to write %1").arg(selected));
}

void MainWindow::previewDataCollection(const DataCollectionBuildRequest &request)
{
    DataCollectionPreviewReport report;
    if (m_sourceKind == SourceKind::ArchiveFile) {
        QTemporaryDir workspace;
        AnalysisResult materialized;
        QString error;
        if (!workspace.isValid() || !materializeArchiveAnalysis(workspace.path(), &materialized, &error)) {
            report.warnings << error;
            report.reportText = QStringLiteral("Archive Data Collection preview failed: %1").arg(error);
        } else {
            report = m_dataCollectionBuilder.preview(materialized, request);
        }
    } else {
        report = m_dataCollectionBuilder.preview(m_result, request);
    }
    m_previewedCollectionRequest = request;
    m_collectionPreviewValid = report.valid;
    m_dataCollectionPage->setPreviewReport(report);
    const QString path = QDir(m_rootFolder).absoluteFilePath(QStringLiteral("data_collection_preview.txt"));
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) { file.write(report.reportText.toUtf8()); file.commit(); }
    logLine(QStringLiteral("Data Collection preview: %1 records to add, valid=%2")
                .arg(report.recordsToAdd.size()).arg(report.valid ? QStringLiteral("yes") : QStringLiteral("no")));
}

void MainWindow::applyDataCollection(const DataCollectionBuildRequest &request)
{
    const auto signature = [](const DataCollectionBuildRequest &value) {
        QList<int> indices = value.includedNodeIndices.values(); std::sort(indices.begin(), indices.end());
        QStringList indexText; for (int index : indices) indexText << QString::number(index);
        return value.family.rootId + QChar(0x1f) + value.requestedUnitId + QChar(0x1f) + value.parent + QChar(0x1f) + value.editorCategories
            + QChar(0x1f) + indexText.join(QLatin1Char(',')) + QChar(0x1f) + QString::number(value.confirmNonStandard);
    };
    if (!m_collectionPreviewValid || signature(request) != signature(m_previewedCollectionRequest)) {
        QMessageBox::warning(this, QStringLiteral("Apply Collection"),
                             QStringLiteral("Preview this exact collection selection and field configuration before applying.")); return;
    }
    if (QMessageBox::question(this, QStringLiteral("Apply Collection"),
                              QStringLiteral("Create a backup and create or update CDataCollectionUnit?")) != QMessageBox::Yes) return;
    if (m_sourceKind == SourceKind::ArchiveFile) {
        QTemporaryDir workspace;
        AnalysisResult materialized;
        QString error;
        if (!workspace.isValid() || !materializeArchiveAnalysis(workspace.path(), &materialized, &error)) {
            QMessageBox::critical(this, QStringLiteral("Collection failed"), error); return;
        }
        const DataCollectionApplyResult result = m_dataCollectionBuilder.apply(materialized, request, workspace.path(), m_whitelistIds);
        if (!result.success) {
            QMessageBox::critical(this, QStringLiteral("Collection failed"), result.error + QStringLiteral("\nThe archive was not changed.")); return;
        }
        QString archiveBackup;
        if (!commitArchiveChanges(workspace.path(), result.changedFiles, &archiveBackup, &error)) {
            QMessageBox::critical(this, QStringLiteral("Collection failed"), error + QStringLiteral("\nNo partial archive change was retained.")); return;
        }
        m_collectionPreviewValid = false;
        loadPathAndAnalyze(m_currentSourcePath);
        m_dryRunPage->recordCollectionResult(result.recordsAdded);
        QMessageBox::information(this, QStringLiteral("Collection complete"),
                                 QStringLiteral("Archive backup: %1\nDataCollectionData.xml and (listfile) saved.\nRecords added: %2")
                                     .arg(archiveBackup).arg(result.recordsAdded));
        return;
    }
    const DataCollectionApplyResult result = m_dataCollectionBuilder.apply(m_result, request, m_rootFolder, m_whitelistIds);
    if (!result.success) {
        QMessageBox::critical(this, QStringLiteral("Collection failed"), result.error + QStringLiteral("\nChanges were rolled back.")); return;
    }
    m_collectionPreviewValid = false;
    const QString reportPath = QDir(m_rootFolder).absoluteFilePath(QStringLiteral("data_collection_preview.txt"));
    QSaveFile reportFile(reportPath);
    if (reportFile.open(QIODevice::WriteOnly | QIODevice::Text)) { reportFile.write(result.finalReport.toUtf8()); reportFile.commit(); }
    loadPathAndAnalyze(m_currentSourcePath);
    m_dryRunPage->recordCollectionResult(result.recordsAdded);
    QMessageBox::information(this, QStringLiteral("Collection complete"),
                             QStringLiteral("Backup: %1\nRecords added: %2\nDuplicate records skipped: %3")
                                 .arg(result.backupFolder).arg(result.recordsAdded).arg(result.duplicatesSkipped));
}

void MainWindow::exportDataCollectionReport(const QString &reportText)
{
    if (reportText.isEmpty()) { QMessageBox::information(this, QStringLiteral("Export Collection Report"), QStringLiteral("Preview a collection first.")); return; }
    const QString selected = QFileDialog::getSaveFileName(this, QStringLiteral("Export Collection Report"),
        QDir(m_rootFolder).absoluteFilePath(QStringLiteral("data_collection_preview.txt")), QStringLiteral("Text files (*.txt)"));
    if (selected.isEmpty()) return;
    const QByteArray bytes = reportText.toUtf8(); QSaveFile file(selected);
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

void MainWindow::showDryRunTab()
{
    if (m_result.nodes.isEmpty()) return;
    QDialog dialog(this);
    m_optimizationDialog = &dialog;
    dialog.setWindowTitle(QStringLiteral("SC2 Data Optimization Wizard"));
    dialog.setWindowFlags(dialog.windowFlags() | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
    dialog.setMinimumSize(1200, 760);
    auto *layout = new QVBoxLayout(&dialog); layout->setContentsMargins(0, 0, 0, 0);
    m_dryRunPage->setParent(&dialog); m_dryRunPage->show(); layout->addWidget(m_dryRunPage);
    m_dryRunPage->startWizard();
    dialog.resize(1500, 900);
    dialog.showNormal();
    dialog.exec();
    layout->removeWidget(m_dryRunPage); m_dryRunPage->setParent(this); m_dryRunPage->hide();
    m_optimizationDialog = nullptr;
}

void MainWindow::setDuplicateMergeEnabled(bool enabled)
{
    if (m_dryRunPage) m_dryRunPage->setDuplicateMergeEnabled(enabled);
    if (!m_tabs || !m_duplicatesPage) return;
    const int index = m_tabs->indexOf(m_duplicatesPage);
    if (index < 0) return;
    if (!enabled && m_tabs->currentWidget() == m_duplicatesPage) m_tabs->setCurrentWidget(m_analysisPage);
    m_tabs->setTabVisible(index, enabled);
    m_tabs->setTabEnabled(index, enabled);
    m_tabs->setTabToolTip(index, enabled ? QStringLiteral("Duplicate Merge")
                                         : QStringLiteral("Enable Duplicate Merge in Settings"));
}

int MainWindow::findNodeIndex(const AnalysisResult &analysis, const WizardNodeRef &ref) const
{
    for (int index = 0; index < analysis.nodes.size(); ++index) {
        const DataNode &node = analysis.nodes[index];
        if (node.id == ref.id && node.elementName == ref.elementName
            && node.sourceFile == ref.sourceFile && node.originalLocation == ref.originalLocation) {
            return index;
        }
    }
    for (int index = 0; index < analysis.nodes.size(); ++index) {
        const DataNode &node = analysis.nodes[index];
        if (node.id == ref.id && node.elementName == ref.elementName) return index;
    }
    for (int index = 0; index < analysis.nodes.size(); ++index)
        if (analysis.nodes[index].id == ref.id) return index;
    return -1;
}

void MainWindow::applyOptimizationWizardPlan()
{
    if (!m_dryRunPage) return;

    const OptimizationWizardSelection selection = m_dryRunPage->currentSelection();
    if (selection.unused.isEmpty() && selection.duplicates.isEmpty()
        && selection.rename.isEmpty() && selection.collection.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Optimization Wizard"),
                                 QStringLiteral("Select at least one item before applying the optimization plan."));
        return;
    }

    if (QMessageBox::question(this, QStringLiteral("Apply Optimization Plan"),
                              QStringLiteral("Apply the selected optimization steps to files now, then rebuild the preview from the updated data?"))
        != QMessageBox::Yes) {
        return;
    }

    m_dryRunPage->setApplyingState(true, QStringLiteral("Applying selected optimization steps and saving files...\n\nThe wizard will rebuild the preview from updated files when the batch finishes."));
    AnalysisProgressDialog applyProgress(this);
    applyProgress.setTitleText(QStringLiteral("SC2 DATA APPLY"));
    applyProgress.setCancelVisible(false);
    applyProgress.setProgress(5, QStringLiteral("Preparing apply"), QStringLiteral("Building the selected optimization batch"));
    applyProgress.show();
    QApplication::processEvents();
    const auto updateApplyProgress = [&](int percent, const QString &primary, const QString &secondary = QString()) {
        applyProgress.setProgress(percent, primary, secondary);
        QApplication::processEvents();
    };

    int removedUnused = 0;
    int removedDuplicates = 0;
    int redirectedReferences = 0;
    int renamedIds = 0;
    int collectionAdded = 0;
    QStringList warnings;
    QString failure;
    QString archiveBackup;
    bool archiveAnalysisReady = false;

    const auto reloadWorkingAnalysis = [this](const QString &rootFolder, AnalysisResult *analysis, QString *errorMessage) {
        return m_analyzer.analyzeFolder(rootFolder, m_whitelistIds, analysis, errorMessage);
    };
    const auto groupKey = [](const WizardNodeRef &ref) {
        return ref.sourceFile + QChar(0x1f) + ref.originalLocation + QChar(0x1f) + ref.elementName + QChar(0x1f) + ref.id;
    };

    if (m_sourceKind == SourceKind::ArchiveFile) {
        updateApplyProgress(15, QStringLiteral("Preparing archive workspace"), QStringLiteral("Materializing XML and listfile"));
        QTemporaryDir workspace;
        AnalysisResult current;
        QString error;
        if (!workspace.isValid() || !materializeArchiveAnalysis(workspace.path(), &current, &error)) {
            failure = error;
        } else {
            QStringList changedFiles;

            if (!reloadWorkingAnalysis(workspace.path(), &current, &error)) {
                failure = error;
            } else {
                applyArchiveReferenceSafety(&current);
            }

            if (failure.isEmpty() && !selection.unused.isEmpty()) {
                QVector<int> unusedRows;
                for (const WizardNodeRef &ref : selection.unused) {
                    const int index = findNodeIndex(current, ref);
                    if (index >= 0) unusedRows.append(index);
                }
                if (!unusedRows.isEmpty()) {
                    updateApplyProgress(25, QStringLiteral("Deleting unused objects"), QStringLiteral("Rewriting verified archive XML"));
                    QString workspaceBackup;
                    QStringList unusedChangedFiles;
                    int removed = 0;
                    int skipped = 0;
                    if (!m_analyzer.applySelectedChanges(current, unusedRows, workspace.path(), m_whitelistIds,
                                                         &workspaceBackup, &error, &unusedChangedFiles, &removed, &skipped)) {
                        failure = error;
                    } else {
                        removedUnused += removed;
                        changedFiles.append(unusedChangedFiles);
                        changedFiles.removeDuplicates();
                        if (skipped > 0)
                            warnings << QStringLiteral("Skipped %1 unused objects because they were no longer safe or available.").arg(skipped);
                        if (!reloadWorkingAnalysis(workspace.path(), &current, &error))
                            failure = error;
                    }
                }
            }
            if (!selection.rename.isEmpty())
                warnings << QStringLiteral("Rename To Standard was skipped: archive mode stays preview-only.");

            QHash<QString, QPair<WizardNodeRef, QVector<WizardNodeRef>>> mergeGroups;
            for (const WizardMergeSelection &item : selection.duplicates) {
                auto &group = mergeGroups[groupKey(item.keep)];
                group.first = item.keep;
                group.second.append(item.remove);
            }
            updateApplyProgress(35, QStringLiteral("Applying duplicate merges"), QStringLiteral("Redirecting references and removing duplicate objects"));
            for (auto it = mergeGroups.cbegin(); failure.isEmpty() && it != mergeGroups.cend(); ++it) {
                const int keepIndex = findNodeIndex(current, it.value().first);
                if (keepIndex < 0) {
                    warnings << QStringLiteral("Skipped a duplicate merge because keep object %1 is no longer present.").arg(it.value().first.id);
                    continue;
                }
                MergeRequest request;
                request.keepNodeIndex = keepIndex;
                for (const WizardNodeRef &remove : it.value().second) {
                    const int removeIndex = findNodeIndex(current, remove);
                    if (removeIndex >= 0) request.removeNodeIndices.append(removeIndex);
                }
                if (request.removeNodeIndices.isEmpty()) continue;
                const MergeApplyResult result = m_mergeService.apply(current, request, workspace.path(), m_whitelistIds);
                if (!result.success) {
                    failure = result.error;
                    break;
                }
                removedDuplicates += result.nodesDeleted;
                redirectedReferences += result.referencesRedirected;
                changedFiles.append(result.changedFiles);
                changedFiles.removeDuplicates();
                if (!reloadWorkingAnalysis(workspace.path(), &current, &error)) {
                    failure = error;
                    break;
                }
            }

            QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(current);
            QHash<QString, UnitFamily> familyByRoot;
            for (const UnitFamily &family : families) familyByRoot.insert(family.rootId, family);
            bool collectionChanged = false;
            updateApplyProgress(65, QStringLiteral("Applying Data Collection"), QStringLiteral("Adding selected collection records"));
            for (const WizardCollectionSelection &selectedCollection : selection.collection) {
                if (failure.isEmpty()) {
                    const auto match = familyByRoot.constFind(selectedCollection.familyRootId);
                    if (match == familyByRoot.cend()) {
                        warnings << QStringLiteral("Skipped Data Collection family %1 because it is no longer present after apply.").arg(selectedCollection.familyRootId);
                        continue;
                    }
                    DataCollectionBuildRequest request;
                    request.family = match.value();
                    request.requestedUnitId = request.family.rootId;
                    request.confirmNonStandard = true;
                    for (const WizardNodeRef &ref : selectedCollection.nodes) {
                        const int index = findNodeIndex(current, ref);
                        if (index >= 0) request.includedNodeIndices.insert(index);
                    }
                    const DataCollectionApplyResult result = m_dataCollectionBuilder.apply(current, request, workspace.path(), m_whitelistIds);
                    if (!result.success) {
                        failure = result.error;
                        break;
                    }
                    collectionAdded += result.recordsAdded;
                    changedFiles.append(result.changedFiles);
                    changedFiles.removeDuplicates();
                    collectionChanged = true;
                }
            }

            if (failure.isEmpty() && collectionChanged
                && !reloadWorkingAnalysis(workspace.path(), &current, &error))
                failure = error;

            if (failure.isEmpty() && !changedFiles.isEmpty()) {
                updateApplyProgress(85, QStringLiteral("Saving archive"), QStringLiteral("Writing verified XML back to the SC2 archive"));
                if (!commitArchiveChanges(workspace.path(), changedFiles, &archiveBackup, &error)) {
                    failure = error;
                } else {
                    normalizeArchiveAnalysis(&current, workspace.path(), m_currentSourcePath);
                    m_result = std::move(current);
                    archiveAnalysisReady = true;
                }
            }
        }
    } else {
        AnalysisResult current = m_result;
        QString error;

        QVector<int> unusedRows;
        for (const WizardNodeRef &ref : selection.unused) {
            const int index = findNodeIndex(current, ref);
            if (index >= 0) unusedRows.append(index);
        }
        if (!unusedRows.isEmpty()) {
            updateApplyProgress(20, QStringLiteral("Deleting unused objects"), QStringLiteral("Removing selected safe unused objects"));
            QString backupFolder;
            QStringList changedFiles;
            int removed = 0;
            int skipped = 0;
            if (!m_analyzer.applySelectedChanges(current, unusedRows, m_rootFolder, m_whitelistIds,
                                                 &backupFolder, &error, &changedFiles, &removed, &skipped)) {
                failure = error;
            } else {
                removedUnused += removed;
                if (skipped > 0)
                    warnings << QStringLiteral("Skipped %1 unused objects because they were no longer safe or available.").arg(skipped);
                if (!reloadWorkingAnalysis(m_rootFolder, &current, &error))
                    failure = error;
            }
        }

        QHash<QString, QPair<WizardNodeRef, QVector<WizardNodeRef>>> mergeGroups;
        for (const WizardMergeSelection &item : selection.duplicates) {
            auto &group = mergeGroups[groupKey(item.keep)];
            group.first = item.keep;
            group.second.append(item.remove);
        }
        updateApplyProgress(45, QStringLiteral("Applying duplicate merges"), QStringLiteral("Redirecting references and removing duplicate objects"));
        for (auto it = mergeGroups.cbegin(); failure.isEmpty() && it != mergeGroups.cend(); ++it) {
            const int keepIndex = findNodeIndex(current, it.value().first);
            if (keepIndex < 0) {
                warnings << QStringLiteral("Skipped a duplicate merge because keep object %1 is no longer present.").arg(it.value().first.id);
                continue;
            }
            MergeRequest request;
            request.keepNodeIndex = keepIndex;
            for (const WizardNodeRef &remove : it.value().second) {
                const int removeIndex = findNodeIndex(current, remove);
                if (removeIndex >= 0) request.removeNodeIndices.append(removeIndex);
            }
            if (request.removeNodeIndices.isEmpty()) continue;
            const MergeApplyResult result = m_mergeService.apply(current, request, m_rootFolder, m_whitelistIds);
            if (!result.success) {
                failure = result.error;
                break;
            }
            removedDuplicates += result.nodesDeleted;
            redirectedReferences += result.referencesRedirected;
            if (!reloadWorkingAnalysis(m_rootFolder, &current, &error)) {
                failure = error;
                break;
            }
        }

        QVector<UnitFamily> families = UnitFamilyDetector().detect(current);
        QHash<QString, UnitFamily> familyByRoot;
        for (const UnitFamily &family : families) familyByRoot.insert(family.rootId, family);
        QHash<QString, QVector<WizardNodeRef>> renameByFamily;
        for (const WizardRenameSelection &item : selection.rename)
            renameByFamily[item.familyRootId].append(item.node);
        StandardNamePlanner planner;
        updateApplyProgress(60, QStringLiteral("Applying rename changes"), QStringLiteral("Updating IDs and references"));
        for (auto it = renameByFamily.cbegin(); failure.isEmpty() && it != renameByFamily.cend(); ++it) {
            const auto familyIt = familyByRoot.constFind(it.key());
            if (familyIt == familyByRoot.cend()) {
                warnings << QStringLiteral("Skipped rename family %1 because it is no longer present after apply.").arg(it.key());
                continue;
            }
            QSet<int> includedNodeIndices;
            for (const WizardNodeRef &ref : it.value()) {
                const int index = findNodeIndex(current, ref);
                if (index >= 0) includedNodeIndices.insert(index);
            }
            if (includedNodeIndices.isEmpty()) continue;
            const RenamePlan plan = planner.plan(current, familyIt.value(), familyIt.value().rootId, includedNodeIndices);
            if (!plan.valid) {
                warnings << QStringLiteral("Skipped rename family %1 because the refreshed plan is no longer valid.").arg(it.key());
                continue;
            }
            const RenameApplyResult result = m_referenceRenamer.apply(current, plan, m_rootFolder, m_whitelistIds);
            if (!result.success) {
                failure = result.error;
                break;
            }
            renamedIds += result.identitiesRenamed;
            if (!reloadWorkingAnalysis(m_rootFolder, &current, &error)) {
                failure = error;
                break;
            }
            families = UnitFamilyDetector().detect(current);
            familyByRoot.clear();
            for (const UnitFamily &family : families) familyByRoot.insert(family.rootId, family);
        }

        QHash<QString, UnitFamily> collectionFamilyByRoot;
        for (const UnitFamily &family : UnitFamilyDetector().detectCollectionFamilies(current))
            collectionFamilyByRoot.insert(family.rootId, family);
        updateApplyProgress(80, QStringLiteral("Applying Data Collection"), QStringLiteral("Adding selected collection records"));
        for (const WizardCollectionSelection &selectedCollection : selection.collection) {
            if (!failure.isEmpty()) break;
            const auto familyIt = collectionFamilyByRoot.constFind(selectedCollection.familyRootId);
            if (familyIt == collectionFamilyByRoot.cend()) {
                warnings << QStringLiteral("Skipped Data Collection family %1 because it is no longer present after apply.").arg(selectedCollection.familyRootId);
                continue;
            }
            DataCollectionBuildRequest request;
            request.family = familyIt.value();
            request.requestedUnitId = request.family.rootId;
            request.confirmNonStandard = true;
            for (const WizardNodeRef &ref : selectedCollection.nodes) {
                const int index = findNodeIndex(current, ref);
                if (index >= 0) request.includedNodeIndices.insert(index);
            }
            const DataCollectionApplyResult result = m_dataCollectionBuilder.apply(current, request, m_rootFolder, m_whitelistIds);
            if (!result.success) {
                failure = result.error;
                break;
            }
            collectionAdded += result.recordsAdded;
        }
    }

    m_dryRunPage->setApplyingState(false);

    if (!failure.isEmpty()) {
        applyProgress.close();
        loadPathAndAnalyze(m_currentSourcePath);
        QMessageBox::critical(this, QStringLiteral("Optimization Apply Failed"),
                              QStringLiteral("The optimization batch stopped:\n%1").arg(failure));
        return;
    }

    if (archiveAnalysisReady) {
        updateApplyProgress(92, QStringLiteral("Refreshing analysis"), QStringLiteral("Rebuilding tables and reports from updated data"));
        m_rootFolder = QFileInfo(m_currentSourcePath).absolutePath();
        m_analysisPage->setFolderPath(m_currentSourcePath);
        m_analysisPage->setModeLabel(modeLabelFor(static_cast<int>(m_sourceKind)));
        m_analysisPage->setAnalysisResult(m_result);
        refreshPages();
        writeAnalysisReportFile();
        m_dryRunAction->setEnabled(true);
        m_applyAction->setEnabled(false);
        setCurrentSourcePath(m_currentSourcePath);
    } else if (!loadPathAndAnalyze(m_currentSourcePath)) {
        QMessageBox::warning(this, QStringLiteral("Optimization Applied"),
                             QStringLiteral("Changes were saved, but automatic re-analysis failed. Re-open Analyze to refresh the wizard view."));
        return;
    }
    updateApplyProgress(100, QStringLiteral("Apply complete"), QStringLiteral("Optimization changes were saved successfully"));
    applyProgress.close();

    if (removedUnused > 0) m_dryRunPage->recordUnusedResult(removedUnused);
    if (removedDuplicates > 0 || redirectedReferences > 0) m_dryRunPage->recordMergeResult(removedDuplicates, redirectedReferences);
    if (renamedIds > 0) m_dryRunPage->recordRenameResult(renamedIds);
    if (collectionAdded > 0) m_dryRunPage->recordCollectionResult(collectionAdded);
    m_dryRunPage->rebuildAfterApply();

    QString message = QStringLiteral("Selected optimization steps were applied and saved.\n\nUnused deleted: %1\nDuplicates deleted: %2\nReferences redirected: %3\nIDs renamed: %4\nCollection records added: %5")
        .arg(removedUnused).arg(removedDuplicates).arg(redirectedReferences).arg(renamedIds).arg(collectionAdded);
    if (!archiveBackup.isEmpty())
        message += QStringLiteral("\nArchive backup: %1").arg(archiveBackup);
    if (!warnings.isEmpty())
        message += QStringLiteral("\n\nWarnings:\n- ") + warnings.join(QStringLiteral("\n- "));
    QMessageBox::information(this, QStringLiteral("Optimization Applied"), message);
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
    if (m_rootFolder.isEmpty() || m_result.analysisReportText.isEmpty())
    {
        return;
    }

    const QString reportPath = QDir(m_rootFolder).absoluteFilePath(QStringLiteral("analysis_report.txt"));
    QFile file(reportPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
    {
        return;
    }
    file.write(m_result.analysisReportText.toUtf8());
    file.close();
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
