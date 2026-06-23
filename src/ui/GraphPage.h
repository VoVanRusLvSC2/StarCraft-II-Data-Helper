#pragma once

#include "core/AnalysisModels.h"
#include "core/ReferenceGraph.h"

#include <QWidget>

class QLabel;
class QGraphicsScene;
class QGraphicsView;
class QFrame;
class QPushButton;
class QShowEvent;

class GraphPage : public QWidget
{
    Q_OBJECT

public:
    explicit GraphPage(QWidget *parent = nullptr);

    void setAnalysisResult(const AnalysisResult &result);
    void setCurrentRow(int row);

public slots:
    void fitGraph();

protected:
    void showEvent(QShowEvent *event) override;

private:
    void renderGraph();
    void zoomBy(qreal factor);
    const DataNode *findNodeById(const QString &id) const;
    QString uniqueNodeLabel(const DataNode &node) const;
    QString typeBadgeText(const QString &elementName) const;

    AnalysisResult m_result;
    ReferenceGraph m_graph;
    int m_currentRow = -1;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_subtitleLabel = nullptr;
    QLabel *m_hintBadge = nullptr;
    QFrame *m_controlsFrame = nullptr;
    QGraphicsScene *m_scene = nullptr;
    QGraphicsView *m_view = nullptr;
    QPushButton *m_fitButton = nullptr;
    QPushButton *m_zoomInButton = nullptr;
    QPushButton *m_zoomOutButton = nullptr;
    bool m_fitPending = false;
};
