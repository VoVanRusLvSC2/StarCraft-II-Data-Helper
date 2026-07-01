#pragma once

#include <QMessageBox>

class QWidget;

namespace sc2dh::app
{
QMessageBox::StandardButton showSc2MessageDialog(QWidget *parent,
                                                 QMessageBox::Icon icon,
                                                 const QString &title,
                                                 const QString &message,
                                                 QMessageBox::StandardButtons buttons = QMessageBox::Ok,
                                                 int minimumWidth = 620);
}

