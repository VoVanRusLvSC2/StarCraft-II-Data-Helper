#include "ui/LogPanel.h"

#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QLabel>

LogPanel::LogPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto *title = new QLabel(QStringLiteral("Logs"), this);
    title->setObjectName(QStringLiteral("panelTitle"));
    layout->addWidget(title);

    m_textEdit = new QPlainTextEdit(this);
    m_textEdit->setReadOnly(true);
    m_textEdit->setObjectName(QStringLiteral("logView"));
    m_textEdit->setPlaceholderText(QStringLiteral("Logs will appear here."));
    layout->addWidget(m_textEdit, 1);
}

void LogPanel::appendMessage(const QString &message)
{
    m_textEdit->appendPlainText(message);
}

void LogPanel::clearMessages()
{
    m_textEdit->clear();
}
