#pragma once

#include "core/AnalysisModels.h"

#include <QWidget>

class QLabel;
class QStandardItemModel;
class QTableView;
class QPushButton;

class UnusedPage : public QWidget
{
    Q_OBJECT

public:
    explicit UnusedPage(QWidget *parent = nullptr);
    void setAnalysisResult(const AnalysisResult &result);
    void selectRows(const QVector<int> &rows);
    void setPreviewText(const QString &text);

signals:
    void previewDeletionRequested(const QVector<int> &rows);
    void applyDeletionRequested(const QVector<int> &rows);

private:
    AnalysisResult m_result;
    QLabel *m_summaryLabel = nullptr;
    QTableView *m_table = nullptr;
    QStandardItemModel *m_model = nullptr;
    class QTextEdit *m_preview = nullptr;
    QPushButton *m_previewButton = nullptr;
    QPushButton *m_applyButton = nullptr;
    QVector<int> selectedSafeRows() const;
    void updateActionState();
};
