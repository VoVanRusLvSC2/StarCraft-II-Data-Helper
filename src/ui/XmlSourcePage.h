#pragma once

#include "core/AnalysisModels.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QPlainTextEdit;

class XmlSourcePage : public QWidget
{
    Q_OBJECT
public:
    explicit XmlSourcePage(QWidget *parent = nullptr);
    void setAnalysisResult(const AnalysisResult &result);
    void showNode(int nodeIndex);

private:
    void showFile(const QString &filePath, int line = -1);
    AnalysisResult m_result;
    QComboBox *m_files = nullptr;
    QLabel *m_location = nullptr;
    QPlainTextEdit *m_editor = nullptr;
};
