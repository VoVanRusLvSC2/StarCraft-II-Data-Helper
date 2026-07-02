#include "app/MainWindowSettings.h"

#include "app/AudioManager.h"
#include "app/MainWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QSettings>
#include <QSlider>
#include <QVBoxLayout>

namespace sc2dh::app
{
MainWindowSettings::MainWindowSettings(MainWindow &window)
    : m_window(window)
{
}

void MainWindowSettings::show()
{
    QDialog dialog(&m_window);
    dialog.setObjectName(QStringLiteral("toolDialog"));
    dialog.setWindowTitle(QStringLiteral("SC2 Data Helper Settings"));
    dialog.setMinimumSize(820, 660);
    dialog.resize(860, 700);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("INTERFACE SETTINGS"), &dialog);
    title->setObjectName(QStringLiteral("panelTitle"));
    layout->addWidget(title);

    QSettings settings;
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
    auto *soundCheck = checkBoxRow(QStringLiteral("Button sounds"),
                                   settings.value(QStringLiteral("ui/buttonSounds"), true).toBool());
    auto *animationCheck = checkBoxRow(QStringLiteral("Button animations"),
                                       settings.value(QStringLiteral("ui/buttonAnimations"), true).toBool());
    auto *backgroundGlowCheck = checkBoxRow(
        QStringLiteral("Blue background glow effects"),
        settings.value(QStringLiteral("ui/backgroundGlows"), true).toBool(),
        QStringLiteral("Shows animated soft blue background glows behind the main interface."));
    auto *musicCheck = checkBoxRow(QStringLiteral("Background music"), AudioManager::isMusicEnabled());
    auto *musicValue = new QLabel(&dialog);
    musicValue->setObjectName(QStringLiteral("inspectorSubtitle"));
    auto *musicSlider = new QSlider(Qt::Horizontal, &dialog);
    musicSlider->setObjectName(QStringLiteral("backgroundMusicVolume"));
    musicSlider->setRange(0, 100);
    musicSlider->setValue(int(AudioManager::musicVolume() * 100.0));
    musicSlider->setFocusPolicy(Qt::NoFocus);
    QObject::connect(musicSlider, &QSlider::valueChanged, &dialog, [musicValue](int value)
                     { musicValue->setText(QStringLiteral("Music volume: %1%").arg(value)); });
    musicValue->setText(QStringLiteral("Music volume: %1%").arg(musicSlider->value()));
    auto *duplicatesCheck = checkBoxRow(
        QStringLiteral("Enable Duplicate Merge in Optimization"),
        settings.value(QStringLiteral("optimization/duplicateMergeEnabled"), true).toBool(),
        QStringLiteral("Enabled by default. When enabled, Optimization adds the Duplicate Merge review step."));
    auto *backupCheck = checkBoxRow(
        QStringLiteral("Create backup files before applying changes"),
        settings.value(QStringLiteral("backup/enabled"), true).toBool(),
        QStringLiteral("When disabled, SC2 archives and folders are edited without creating persistent .bak or backup_ copies."));
    auto *startFullscreenCheck = checkBoxRow(QStringLiteral("Start in full screen"),
                                             settings.value(QStringLiteral("ui/startFullscreen"), true).toBool());
    auto *collectionModeLabel = new QLabel(QStringLiteral("DATA COLLECTION MODE"), &dialog);
    collectionModeLabel->setObjectName(QStringLiteral("panelTitle"));
    auto *collectionMode = new QComboBox(&dialog);
    collectionMode->addItem(QStringLiteral("Unit - one collection per unit family"), QStringLiteral("Unit"));
    collectionMode->addItem(QStringLiteral("UnitAbilWeapon - separate Unit, Ability and Weapon collections"), QStringLiteral("UnitAbilWeapon"));
    const QString savedCollectionMode = settings.value(QStringLiteral("dataCollection/mode"), QStringLiteral("UnitAbilWeapon")).toString();
    collectionMode->setCurrentIndex(qMax(0, collectionMode->findData(savedCollectionMode)));
    collectionMode->setToolTip(QStringLiteral("Unit keeps the current behavior. UnitAbilWeapon creates separate collections like Gargantua, Gargantua_Jump and Gargantua_Weapon."));
    layout->addWidget(soundCheck);
    layout->addWidget(animationCheck);
    layout->addWidget(backgroundGlowCheck);
    layout->addWidget(musicCheck);
    layout->addWidget(musicValue);
    layout->addWidget(musicSlider);
    layout->addWidget(duplicatesCheck);
    layout->addWidget(backupCheck);
    layout->addWidget(startFullscreenCheck);
    layout->addWidget(collectionModeLabel);
    layout->addWidget(collectionMode);
    layout->addStretch(1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]()
            {
        settings.setValue(QStringLiteral("ui/buttonSounds"), soundCheck->isChecked());
        settings.setValue(QStringLiteral("ui/buttonAnimations"), animationCheck->isChecked());
        settings.setValue(QStringLiteral("ui/backgroundGlows"), backgroundGlowCheck->isChecked());
        settings.setValue(QStringLiteral("optimization/duplicateMergeEnabled"), duplicatesCheck->isChecked());
        settings.setValue(QStringLiteral("backup/enabled"), backupCheck->isChecked());
        settings.setValue(QStringLiteral("ui/startFullscreen"), startFullscreenCheck->isChecked());
        settings.setValue(QStringLiteral("dataCollection/mode"), collectionMode->currentData().toString());
        AudioManager::setMusicSettings(musicCheck->isChecked(), musicSlider->value() / 100.0);
        m_window.setDuplicateMergeEnabled(duplicatesCheck->isChecked());
        if (auto *root = m_window.findChild<QWidget *>(QStringLiteral("workspaceRoot")))
            root->update();
        if (!m_window.m_result.nodes.isEmpty())
            m_window.refreshPages();
        dialog.accept(); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}
}

