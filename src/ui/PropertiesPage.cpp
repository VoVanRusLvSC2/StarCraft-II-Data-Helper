#include "ui/PropertiesPage.h"

#include "ui/ObjectInspectorWidget.h"

#include <QLabel>
#include <QVBoxLayout>

PropertiesPage::PropertiesPage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto *header = new QLabel(QStringLiteral("Properties"), this);
    header->setObjectName(QStringLiteral("panelTitle"));
    layout->addWidget(header);

    m_inspector = new ObjectInspectorWidget(this);
    layout->addWidget(m_inspector, 1);
}

void PropertiesPage::setAnalysisResult(const AnalysisResult &result)
{
    m_inspector->setAnalysisResult(result);
}

void PropertiesPage::setCurrentRow(int row)
{
    m_inspector->setCurrentRow(row);
}
