#pragma once

#include <QString>

class QWidget;

namespace sc2dh::app
{
void warmUpSc2FileOpenDialogHighlight(const QWidget *parent);
QString openSc2FileStyled(QWidget *parent, const QString &startPath);
QString openFolderStyled(QWidget *parent, const QString &startPath);
QString saveTextFileStyled(QWidget *parent, const QString &title, const QString &startPath);
}

