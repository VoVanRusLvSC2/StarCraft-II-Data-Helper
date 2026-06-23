#pragma once

#include "core/AnalysisModels.h"

#include <QWidget>

class ObjectInspectorWidget;

class PropertiesPage : public QWidget
{
    Q_OBJECT

public:
    explicit PropertiesPage(QWidget *parent = nullptr);

    void setAnalysisResult(const AnalysisResult &result);
    void setCurrentRow(int row);

private:
    ObjectInspectorWidget *m_inspector = nullptr;
};
