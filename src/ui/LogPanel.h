#pragma once

#include "core/AnalysisModels.h"

#include <QWidget>

class QPlainTextEdit;

class LogPanel : public QWidget
{
    Q_OBJECT

public:
    explicit LogPanel(QWidget *parent = nullptr);

public slots:
    void appendMessage(const QString &message);
    void clearMessages();
    void showOperationResult(const OperationResult &result);

private:
    QPlainTextEdit *m_textEdit = nullptr;
};
