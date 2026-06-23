#pragma once

#include "core/AnalysisModels.h"
#include "core/ReferenceGraph.h"

#include <QColor>
#include <QStringList>
#include <QWidget>

class QLabel;
class QListWidget;

class DependenciesPage : public QWidget
{
    Q_OBJECT

public:
    explicit DependenciesPage(QWidget *parent = nullptr);

    void setAnalysisResult(const AnalysisResult &result);
    void setCurrentRow(int row);

private:
    void refreshView();
    QString describeNodeById(const QString &id) const;
    QStringList uniqueSorted(const QStringList &values) const;
    void populateBucket(class QListWidget *listWidget,
                        const QStringList &values,
                        const QColor &accent,
                        const QString &emptyText);

    AnalysisResult m_result;
    int m_currentRow = -1;
    ReferenceGraph m_graph;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_subtitleLabel = nullptr;
    QLabel *m_incomingCount = nullptr;
    QLabel *m_outgoingCount = nullptr;
    QLabel *m_missingCount = nullptr;
    class QListWidget *m_incomingList = nullptr;
    class QListWidget *m_outgoingList = nullptr;
    class QListWidget *m_missingList = nullptr;
};
