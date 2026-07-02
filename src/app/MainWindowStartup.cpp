#include "app/MainWindowStartup.h"

#include "app/AudioManager.h"
#include "app/MainWindow.h"
#include "app/Sc2FileDialogs.h"

#include <QSettings>

namespace sc2dh::app
{
MainWindowStartup::MainWindowStartup(MainWindow &window)
    : m_window(window)
{
}

void MainWindowStartup::initialize()
{
    m_window.setupUi();
    m_window.setupLogging();
    m_window.setupTheme();
    warmUpSc2FileOpenDialogHighlight(&m_window);
    AudioManager::instance()->initialize();

    QSettings settings;
    const QString collectionModeKey = QStringLiteral("dataCollection/mode");
    const QString collectionModeMigrationKey = QStringLiteral("dataCollection/unitAbilWeaponDefaultMigrated");
    if (!settings.value(collectionModeMigrationKey, false).toBool())
    {
        if (!settings.contains(collectionModeKey)
            || settings.value(collectionModeKey).toString().compare(QStringLiteral("Unit"), Qt::CaseInsensitive) == 0)
            settings.setValue(collectionModeKey, QStringLiteral("UnitAbilWeapon"));
        settings.setValue(collectionModeMigrationKey, true);
    }
    else if (!settings.contains(collectionModeKey))
    {
        settings.setValue(collectionModeKey, QStringLiteral("UnitAbilWeapon"));
    }

    const QString duplicateMergeKey = QStringLiteral("optimization/duplicateMergeEnabled");
    const QString duplicateMergeMigrationKey = QStringLiteral("optimization/duplicateMergeDefaultEnabledMigrated");
    if (!settings.value(duplicateMergeMigrationKey, false).toBool())
    {
        if (!settings.contains(duplicateMergeKey) || !settings.value(duplicateMergeKey).toBool())
            settings.setValue(duplicateMergeKey, true);
        settings.setValue(duplicateMergeMigrationKey, true);
    }
    m_window.setDuplicateMergeEnabled(settings.value(duplicateMergeKey, true).toBool());
    m_window.loadDefaultFolder();

    const QString rulesPath = m_window.runtimePath(QStringLiteral("config/rules.json"));
    const QString whitelistPath = m_window.runtimePath(QStringLiteral("config/whitelist.json"));
    QString errorMessage;
    if (m_window.m_configManager.load(rulesPath, whitelistPath, &errorMessage))
    {
        m_window.m_whitelistIds = m_window.m_configManager.whitelistIds();
        m_window.logLine(QStringLiteral("Loaded whitelist entries: %1").arg(m_window.m_whitelistIds.size()));
    }
    else
    {
        m_window.logLine(QStringLiteral("Config load skipped: %1").arg(errorMessage));
    }
    m_window.updateFullscreenActionText();
}
}

