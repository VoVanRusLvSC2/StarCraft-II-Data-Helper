#pragma once

#include <QDialog>

class QLabel;
class QProgressBar;

class AnalysisProgressDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit AnalysisProgressDialog(QWidget *parent = nullptr);
    void setProgress(int percent, const QString &primaryText, const QString &secondaryText = QString());
    bool isCancelled() const { return m_cancelled; }

signals:
    void cancellationRequested();

private:
    QLabel *m_primaryLabel = nullptr;
    QLabel *m_secondaryLabel = nullptr;
    QLabel *m_percentLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    bool m_cancelled = false;
};
