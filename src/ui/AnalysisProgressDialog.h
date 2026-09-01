#pragma once

#include <QDialog>
#include <QPoint>

class QLabel;
class QProgressBar;
class QPushButton;
class QEvent;

class AnalysisProgressDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit AnalysisProgressDialog(QWidget *parent = nullptr);
    void setTitleText(const QString &title);
    void setCancelVisible(bool visible);
    void setProgress(int percent, const QString &primaryText, const QString &secondaryText = QString());
    bool isCancelled() const { return m_cancelled; }

signals:
    void cancellationRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QLabel *m_titleLabel = nullptr;
    QLabel *m_primaryLabel = nullptr;
    QLabel *m_secondaryLabel = nullptr;
    QLabel *m_percentLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QPushButton *m_cancelButton = nullptr;
    bool m_cancelled = false;
    QPoint m_dragOffset;
    bool m_dragging = false;
};
