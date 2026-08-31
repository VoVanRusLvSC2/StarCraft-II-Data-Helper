#include "ui/LogPanel.h"

#include <QPlainTextEdit>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QLabel>

LogPanel::LogPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto *title = new QLabel(tr("Results and logs"), this);
    title->setObjectName(QStringLiteral("panelTitle"));
    layout->addWidget(title);

    m_textEdit = new QPlainTextEdit(this);
    m_textEdit->setReadOnly(true);
    m_textEdit->setObjectName(QStringLiteral("logView"));
    m_textEdit->setPlaceholderText(tr("Operation results and logs will appear here."));
    m_textEdit->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
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

void LogPanel::showOperationResult(const OperationResult &result)
{
    QStringList lines;
    if (result.outcome == OperationOutcome::Succeeded)
        lines << tr("OPERATION COMPLETED");
    else if (result.outcome == OperationOutcome::Cancelled)
        lines << tr("OPERATION CANCELLED");
    else
        lines << tr("SAVE ERROR");
    lines << result.title;
    if (!result.summary.isEmpty())
        lines << QString() << result.summary;
    lines << QString()
          << tr("Selected: %1").arg(result.selected)
          << tr("Applied: %1").arg(result.applied)
          << tr("Skipped: %1").arg(result.skipped)
          << tr("Blocked: %1").arg(result.blocked)
          << tr("Original changed: %1").arg(result.originalChanged ? tr("yes") : tr("no"));
    if (!result.backupPath.isEmpty())
        lines << tr("Backup: %1").arg(result.backupPath);
    if (!result.outputPath.isEmpty())
        lines << tr("Output: %1").arg(result.outputPath);
    if (!result.temporaryOutputPath.isEmpty())
        lines << tr("Temporary output: %1").arg(result.temporaryOutputPath);
    if (result.errorCode != OperationErrorCode::None)
        lines << tr("Error code: %1").arg(operationErrorCodeName(result.errorCode));
    if (!result.error.isEmpty())
        lines << QString() << tr("Reason:") << result.error;
    if (!result.skippedReasons.isEmpty())
        lines << QString() << tr("Skipped reasons:") << result.skippedReasons;
    if (!result.blockedReasons.isEmpty())
        lines << QString() << tr("Blocked reasons:") << result.blockedReasons;
    if (!result.details.isEmpty())
        lines << QString() << tr("Details:") << result.details;

    m_textEdit->appendPlainText(QStringLiteral("\n%1\n%2\n")
                                    .arg(QString(72, QLatin1Char('=')), lines.join(QLatin1Char('\n'))));
    QTextCursor cursor = m_textEdit->textCursor();
    cursor.movePosition(QTextCursor::End);
    m_textEdit->setTextCursor(cursor);
}
