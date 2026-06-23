#pragma once

#include "core/AnalysisModels.h"
#include "core/MergeService.h"

#include <QWidget>

class QLabel;
class QStandardItemModel;
class QTreeView;

class DuplicatesPage : public QWidget
{
    Q_OBJECT

public:
    explicit DuplicatesPage(QWidget *parent = nullptr);
    void setAnalysisResult(const AnalysisResult &result);
    void setPreviewText(const QString &text, bool canApply);
    void selectRequest(const MergeRequest &request);

signals:
    void previewMergeRequested(const MergeRequest &request);
    void applyMergeRequested(const MergeRequest &request);
    void sourceRequested(int nodeIndex);

private slots:
    void previewSelectedMerge();
    void applySelectedMerge();

private:
    AnalysisResult m_result;
    QLabel *m_summaryLabel = nullptr;
    QTreeView *m_tree = nullptr;
    QStandardItemModel *m_model = nullptr;
    class QTextEdit *m_preview = nullptr;
    class QPushButton *m_previewButton = nullptr;
    class QPushButton *m_applyButton = nullptr;
    MergeRequest selectedRequest() const;
};
