#pragma once

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

private:
    QPlainTextEdit *m_textEdit = nullptr;
};
