#pragma once

#include <QList>

class QAbstractButton;
class QObject;
class QTabBar;
class QToolButton;
class QWidget;

namespace sc2dh::app
{
const char *discordInviteUrl();
const char *boostyUrl();
QWidget *createSc2BackgroundWidget(QWidget *parent);
void installPersistentTabToolTips(QTabBar *tabBar);
QObject *installButtonEffects(QObject *owner, const QList<QAbstractButton *> &buttons);
QObject *installPromoButtonAnimations(QObject *owner, QToolButton *discordButton, QToolButton *boostyButton);
}

