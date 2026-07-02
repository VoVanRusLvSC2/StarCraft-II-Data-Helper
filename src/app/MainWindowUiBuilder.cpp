#include "app/MainWindowUiBuilder.h"

#include "app/AudioManager.h"
#include "app/MainWindow.h"
#include "app/MainWindowUiSupport.h"

#include "ui/DataCollectionPage.h"
#include "ui/DependenciesPage.h"
#include "ui/DuplicatesPage.h"
#include "ui/FormatterPage.h"
#include "ui/GraphPage.h"
#include "ui/LogPanel.h"
#include "ui/OverviewPage.h"
#include "ui/PropertiesPage.h"
#include "ui/RenameIdsPage.h"
#include "ui/UnusedPage.h"
#include "ui/XmlSourcePage.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFont>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QSizePolicy>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

namespace sc2dh::app
{
MainWindowUiBuilder::MainWindowUiBuilder(MainWindow &window)
    : m_window(window)
{
}

void MainWindowUiBuilder::build()
{
    MainWindow *window = &m_window;
    window->setWindowTitle(QStringLiteral("SC2 Data Helper"));
    window->setWindowIcon(QIcon(QStringLiteral(":/icons/Icon.png")));
    window->resize(1550, 980);

    auto *toolbar = window->addToolBar(QStringLiteral("Main"));
    toolbar->setObjectName(QStringLiteral("mainToolbar"));
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(0, 0));
    toolbar->setMinimumHeight(58);

    window->m_openFileAction = new QAction(QStringLiteral("Open SC2 File"), window);
    window->m_openFolderAction = new QAction(QStringLiteral("Open Folder"), window);
    window->m_analyzeAction = new QAction(QStringLiteral("Analyze"), window);
    window->m_dryRunAction = new QAction(QStringLiteral("Optimization"), window);
    window->m_applyAction = new QAction(QStringLiteral("Review Optimization Plan"), window);
    window->m_settingsAction = new QAction(QStringLiteral("Settings"), window);
    window->m_fullscreenAction = new QAction(QStringLiteral("Fullscreen"), window);
    window->m_exitAction = new QAction(QStringLiteral("Exit"), window);
    window->m_analyzeAction->setShortcut(QKeySequence(Qt::Key_F5));
    window->m_analyzeAction->setShortcutContext(Qt::ApplicationShortcut);
    window->m_analyzeAction->setToolTip(QStringLiteral("Analyze / refresh the current project (F5)"));
    window->m_dryRunAction->setEnabled(false);
    window->m_applyAction->setEnabled(false);

    window->m_pathEdit = new QLineEdit(window);
    window->m_pathEdit->setObjectName(QStringLiteral("pathEdit"));
    window->m_pathEdit->setPlaceholderText(QStringLiteral("Selected source path"));
    window->m_pathEdit->setReadOnly(true);
    window->m_pathEdit->setCursorPosition(0);
    window->m_pathEdit->hide();

    auto *toolbarContent = new QWidget(toolbar);
    toolbarContent->setObjectName(QStringLiteral("mainToolbarContent"));
    toolbarContent->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *toolbarLayout = new QHBoxLayout(toolbarContent);
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(0);
    const auto addActionButton = [toolbarContent, toolbarLayout](QAction *action, int stretch)
    {
        auto *button = new QToolButton(toolbarContent);
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setFocusPolicy(Qt::NoFocus);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        toolbarLayout->addWidget(button, stretch);
        return button;
    };
    addActionButton(window->m_openFileAction, 1);
    addActionButton(window->m_openFolderAction, 1);
    addActionButton(window->m_analyzeAction, 1);
    addActionButton(window->m_dryRunAction, 1);
    addActionButton(window->m_applyAction, 2);
    addActionButton(window->m_settingsAction, 1);
    addActionButton(window->m_fullscreenAction, 1);
    addActionButton(window->m_exitAction, 1);

    auto *discordButton = new QToolButton(toolbarContent);
    discordButton->setObjectName(QStringLiteral("discordPromoButton"));
    discordButton->setIcon(QIcon(QStringLiteral(":/textures/Discord.png")));
    discordButton->setIconSize(QSize(30, 30));
    discordButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    discordButton->setToolTip(QStringLiteral("Join Discord"));
    discordButton->setCursor(Qt::PointingHandCursor);
    discordButton->setFocusPolicy(Qt::NoFocus);
    discordButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbarLayout->addWidget(discordButton, 1);

    auto *boostyButton = new QToolButton(toolbarContent);
    boostyButton->setObjectName(QStringLiteral("boostyPromoButton"));
    boostyButton->setText(QStringLiteral("Boosty"));
    boostyButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    boostyButton->setToolTip(QStringLiteral("Support on Boosty"));
    boostyButton->setCursor(Qt::PointingHandCursor);
    boostyButton->setFocusPolicy(Qt::NoFocus);
    boostyButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbarLayout->addWidget(boostyButton, 1);
    toolbar->addWidget(toolbarContent);

    auto *root = createSc2BackgroundWidget(window);
    root->setObjectName(QStringLiteral("workspaceRoot"));
    auto *rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto *splitterFrame = new QFrame(root);
    splitterFrame->setObjectName(QStringLiteral("workspaceFrame"));
    auto *splitterLayout = new QVBoxLayout(splitterFrame);
    splitterLayout->setContentsMargins(0, 0, 0, 0);

    window->m_tabs = new QTabWidget(splitterFrame);
    window->m_tabs->setObjectName(QStringLiteral("workspaceTabs"));
    window->m_analysisPage = new OverviewPage(window->m_tabs);
    window->m_dependenciesPage = new DependenciesPage(window->m_tabs);
    window->m_graphPage = new GraphPage(window->m_tabs);
    window->m_propertiesPage = new PropertiesPage(window->m_tabs);
    window->m_dataCollectionPage = new DataCollectionPage(window->m_tabs);
    window->m_renameIdsPage = new RenameIdsPage(window->m_tabs);
    window->m_duplicatesPage = new DuplicatesPage(window->m_tabs);
    window->m_cleanupPage = new UnusedPage(window->m_tabs);
    window->m_dryRunPage = new FormatterPage(window);
    window->m_dryRunPage->hide();
    window->m_logPanel = new LogPanel(window->m_tabs);
    window->m_xmlSourcePage = new XmlSourcePage(window->m_tabs);

    window->m_tabs->addTab(window->m_analysisPage, QStringLiteral("Objects"));
    window->m_tabs->addTab(window->m_dependenciesPage, QStringLiteral("Dependencies"));
    window->m_tabs->addTab(window->m_graphPage, QStringLiteral("Graph"));
    window->m_tabs->addTab(window->m_propertiesPage, QStringLiteral("Properties"));
    window->m_tabs->addTab(window->m_dataCollectionPage, QStringLiteral("Data Collection"));
    window->m_tabs->addTab(window->m_renameIdsPage, QStringLiteral("Rename To Standard"));
    window->m_tabs->addTab(window->m_duplicatesPage, QStringLiteral("Duplicate Merge"));
    window->m_tabs->addTab(window->m_cleanupPage, QStringLiteral("Unused Data Objects"));
    window->m_tabs->addTab(window->m_logPanel, QStringLiteral("Logs"));
    window->m_tabs->addTab(window->m_xmlSourcePage, QStringLiteral("XML Source"));
    window->m_tabs->tabBar()->setExpanding(false);
    window->m_tabs->tabBar()->setUsesScrollButtons(true);
    window->m_tabs->tabBar()->setElideMode(Qt::ElideNone);
    window->m_tabs->setFocusPolicy(Qt::NoFocus);
    window->m_tabs->tabBar()->setFocusPolicy(Qt::NoFocus);
    for (int index = 0; index < window->m_tabs->count(); ++index)
    {
        const QString title = window->m_tabs->tabText(index);
        window->m_tabs->setTabToolTip(index, title);
        auto *label = new QLabel(title, window->m_tabs->tabBar());
        label->setObjectName(QStringLiteral("workspaceTabLabel"));
        label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        label->setFocusPolicy(Qt::NoFocus);
        label->setTextInteractionFlags(Qt::NoTextInteraction);
        QFont labelFont = label->font();
        labelFont.setBold(true);
        label->setFont(labelFont);
        // Leave room for both edge glyphs and the glow. QTabBar clips custom
        // tab buttons to their size hint, which previously cut off letters.
        label->setFixedWidth(label->fontMetrics().horizontalAdvance(title) + 52);
        label->setMinimumHeight(30);
        label->setContentsMargins(12, 0, 12, 0);
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet(QStringLiteral("background: transparent; color: #e8fffb; padding: 0;"));
        window->m_tabs->tabBar()->setTabButton(index, QTabBar::LeftSide, label);
        window->m_tabs->setTabText(index, QString());
    }
    window->m_tabs->tabBar()->setMouseTracking(true);
    installPersistentTabToolTips(window->m_tabs->tabBar());
    const auto updateTabGlow = [window](int selected)
    {
        for (int index = 0; index < window->m_tabs->count(); ++index)
        {
            auto *label = qobject_cast<QLabel *>(window->m_tabs->tabBar()->tabButton(index, QTabBar::LeftSide));
            if (!label)
                continue;
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
    updateTabGlow(window->m_tabs->currentIndex());
    QObject::connect(window->m_tabs, &QTabWidget::currentChanged, window, updateTabGlow);

    splitterLayout->addWidget(window->m_tabs);
    rootLayout->addWidget(splitterFrame, 1);
    auto *workspaceBottomEdge = new QFrame(root);
    workspaceBottomEdge->setObjectName(QStringLiteral("workspaceBottomEdge"));
    workspaceBottomEdge->setAttribute(Qt::WA_TransparentForMouseEvents);
    workspaceBottomEdge->setFocusPolicy(Qt::NoFocus);
    workspaceBottomEdge->setFixedHeight(10);
    rootLayout->addWidget(workspaceBottomEdge);
    window->setCentralWidget(root);
    QObject::connect(window->m_openFileAction, &QAction::triggered, window, &MainWindow::openSc2File);
    QObject::connect(window->m_openFolderAction, &QAction::triggered, window, &MainWindow::openSourceFolder);
    QObject::connect(window->m_analyzeAction, &QAction::triggered, window, &MainWindow::analyzeFolder);
    QObject::connect(window->m_dryRunAction, &QAction::triggered, window, &MainWindow::runDryRun);
    QObject::connect(window->m_applyAction, &QAction::triggered, window, &MainWindow::showDryRunTab);
    QObject::connect(window->m_settingsAction, &QAction::triggered, window, &MainWindow::showSettingsDialog);
    QObject::connect(window->m_fullscreenAction, &QAction::triggered, window, &MainWindow::toggleFullscreen);
    QObject::connect(window->m_exitAction, &QAction::triggered, window, []
            {
        AudioManager::instance()->shutdown();
        QApplication::closeAllWindows();
        QCoreApplication::quit(); });
    const auto openSupportUrl = [window](const char *url, const QString &label)
    {
        if (!QDesktopServices::openUrl(QUrl(QString::fromLatin1(url))))
        {
            QMessageBox::warning(window, label, QStringLiteral("Unable to open link: %1").arg(QString::fromLatin1(url)));
        }
    };
    QObject::connect(discordButton, &QToolButton::clicked, window, [openSupportUrl]
            { openSupportUrl(discordInviteUrl(), QStringLiteral("Discord")); });
    QObject::connect(boostyButton, &QToolButton::clicked, window, [openSupportUrl]
            { openSupportUrl(boostyUrl(), QStringLiteral("Boosty")); });
    QObject::connect(window->m_duplicatesPage, &DuplicatesPage::previewMergeRequested, window, &MainWindow::previewMerge);
    QObject::connect(window->m_duplicatesPage, &DuplicatesPage::applyMergeRequested, window, &MainWindow::applyMerge);
    QObject::connect(window->m_cleanupPage, &UnusedPage::previewDeletionRequested, window, &MainWindow::previewUnusedDeletion);
    QObject::connect(window->m_cleanupPage, &UnusedPage::applyDeletionRequested, window, &MainWindow::applyUnusedDeletion);
    QObject::connect(window->m_renameIdsPage, &RenameIdsPage::previewRequested, window, &MainWindow::previewStandardRename);
    QObject::connect(window->m_renameIdsPage, &RenameIdsPage::applyRequested, window, &MainWindow::applyStandardRename);
    QObject::connect(window->m_renameIdsPage, &RenameIdsPage::exportRequested, window, &MainWindow::exportStandardRenameReport);
    QObject::connect(window->m_dataCollectionPage, &DataCollectionPage::previewRequested, window, &MainWindow::previewDataCollection);
    QObject::connect(window->m_dataCollectionPage, &DataCollectionPage::applyRequested, window, &MainWindow::applyDataCollection);
    QObject::connect(window->m_dataCollectionPage, &DataCollectionPage::exportRequested, window, &MainWindow::exportDataCollectionReport);
    QObject::connect(window->m_dryRunPage, &FormatterPage::previewBuilt, window, [window]
            { window->m_applyAction->setEnabled(true); });
    QObject::connect(window->m_dryRunPage, &FormatterPage::openUnusedRequested, window, [window](const QVector<int> &rows)
            {
        if (window->m_optimizationDialog) window->m_optimizationDialog->accept();
        window->m_cleanupPage->selectRows(rows); window->m_tabs->setCurrentWidget(window->m_cleanupPage); });
    QObject::connect(window->m_dryRunPage, &FormatterPage::openDuplicateRequested, window, [window](const MergeRequest &request)
            {
        if (window->m_optimizationDialog) window->m_optimizationDialog->accept();
        window->m_duplicatesPage->selectRequest(request); window->m_tabs->setCurrentWidget(window->m_duplicatesPage); });
    QObject::connect(window->m_dryRunPage, &FormatterPage::openRenameRequested, window, [window]
            {
        if (window->m_optimizationDialog) window->m_optimizationDialog->accept(); window->m_tabs->setCurrentWidget(window->m_renameIdsPage); });
    QObject::connect(window->m_dryRunPage, &FormatterPage::openCollectionRequested, window, [window]
            {
        if (window->m_optimizationDialog) window->m_optimizationDialog->accept(); window->m_tabs->setCurrentWidget(window->m_dataCollectionPage); });
    QObject::connect(window->m_dryRunPage, &FormatterPage::applyWizardRequested, window, &MainWindow::applyOptimizationWizardPlan);
    QObject::connect(window->m_dryRunPage, &FormatterPage::wizardFinished, window, [window]
            {
        if (window->m_optimizationDialog) window->m_optimizationDialog->accept(); });
    QObject::connect(window->m_duplicatesPage, &DuplicatesPage::sourceRequested, window, [window](int nodeIndex)
            {
        window->m_xmlSourcePage->showNode(nodeIndex);
        window->m_tabs->setCurrentWidget(window->m_xmlSourcePage); });

    auto *undoAction = new QAction(QStringLiteral("Undo"), window);
    undoAction->setShortcut(QKeySequence::Undo);
    undoAction->setShortcutContext(Qt::ApplicationShortcut);
    window->addAction(undoAction);
    QObject::connect(undoAction, &QAction::triggered, window, &MainWindow::undoFocusedEditor);

    auto *redoAction = new QAction(QStringLiteral("Redo"), window);
    redoAction->setShortcuts({QKeySequence::Redo, QKeySequence(Qt::CTRL | Qt::Key_Y)});
    redoAction->setShortcutContext(Qt::ApplicationShortcut);
    window->addAction(redoAction);
    QObject::connect(redoAction, &QAction::triggered, window, &MainWindow::redoFocusedEditor);

    auto *fullscreenAction = new QAction(QStringLiteral("Toggle Fullscreen"), window);
    fullscreenAction->setShortcut(QKeySequence(Qt::Key_F11));
    fullscreenAction->setShortcutContext(Qt::ApplicationShortcut);
    window->addAction(fullscreenAction);
    QObject::connect(fullscreenAction, &QAction::triggered, window, &MainWindow::toggleFullscreen);
    QObject::connect(window->m_analysisPage, &OverviewPage::folderPathChanged, window, [window](const QString &folder)
            { window->m_rootFolder = folder; });
    QObject::connect(window->m_analysisPage, &OverviewPage::currentRowChanged, window, [window](int row)
            {
        window->m_dependenciesPage->setCurrentRow(row);
        window->m_graphPage->setCurrentRow(row);
        window->m_propertiesPage->setCurrentRow(row);
        if (window->m_tabs && window->m_tabs->currentWidget() == window->m_graphPage) {
            QMetaObject::invokeMethod(window->m_graphPage, "fitGraph", Qt::QueuedConnection);
        }
        if (row >= 0 && row < window->m_result.nodes.size()) {
            const DataNode &node = window->m_result.nodes[row];
            window->statusBar()->showMessage(QStringLiteral("Loaded path: %1 | Selected: %2 / %3")
                                         .arg(window->m_currentSourcePath.isEmpty() ? window->m_rootFolder : window->m_currentSourcePath,
                                             node.elementName.isEmpty() ? node.id : node.elementName,
                                              node.id.isEmpty() ? QStringLiteral("-") : node.id));
        } });
    QObject::connect(window->m_tabs, &QTabWidget::currentChanged, window, [window](int index)
            {
        if (window->m_tabs->widget(index) == window->m_graphPage) {
            QMetaObject::invokeMethod(window->m_graphPage, "fitGraph", Qt::QueuedConnection);
        } });
    QObject::connect(window->m_analysisPage, &OverviewPage::objectDoubleClicked, window, [window](int row)
            { window->showGraphForRow(row); });

    auto *mainStatusBar = window->statusBar();
    mainStatusBar->setObjectName(QStringLiteral("mainStatusBar"));
    mainStatusBar->setSizeGripEnabled(false);
    mainStatusBar->setFixedHeight(48);
    mainStatusBar->showMessage(QStringLiteral("Ready"));

    const QList<QAbstractButton *> buttons = window->findChildren<QAbstractButton *>();
    for (QAbstractButton *button : buttons)
    {
        if (auto *toolButton = qobject_cast<QToolButton *>(button))
        {
            const QString name = toolButton->objectName();
            if (name != QStringLiteral("discordPromoButton") && name != QStringLiteral("boostyPromoButton"))
            {
                const int textWidth = toolButton->fontMetrics().horizontalAdvance(toolButton->text());
                toolButton->setMinimumWidth(qMax(toolButton->minimumWidth(), textWidth + 58));
            }
        }
    }
    installButtonEffects(window, buttons);
    installPromoButtonAnimations(window, discordButton, boostyButton);

    for (QLabel *label : window->findChildren<QLabel *>())
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
}

