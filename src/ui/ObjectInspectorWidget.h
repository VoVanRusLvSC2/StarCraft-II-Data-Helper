#pragma once

#include "core/AnalysisModels.h"

#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QTableWidget;

class ObjectInspectorWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ObjectInspectorWidget(QWidget *parent = nullptr);

    void setAnalysisResult(const AnalysisResult &result);
    void setCurrentRow(int row);
    void clearSelection();

private:
    void refreshCurrentNode();
    void populateEmptyState();
    void populateNodeState(const DataNode &node);
    QString statusTextForNode(const DataNode &node) const;

    AnalysisResult m_result;
    int m_currentRow = -1;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_subtitleLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_fileValue = nullptr;
    QLabel *m_typeValue = nullptr;
    QLabel *m_parentValue = nullptr;
    QLabel *m_idValue = nullptr;
    QLabel *m_lineValue = nullptr;
    QLabel *m_locationValue = nullptr;
    QLabel *m_hashValue = nullptr;
    QTableWidget *m_attributesTable = nullptr;
    QPlainTextEdit *m_xmlView = nullptr;
};
