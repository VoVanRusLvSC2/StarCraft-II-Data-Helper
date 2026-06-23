#include "ui/GraphPage.h"

#include <QFrame>
#include <QBrush>
#include <QFont>
#include <QFontMetrics>
#include <QGraphicsItem>
#include <QGraphicsRectItem>
#include <QGraphicsPathItem>
#include <QGraphicsEllipseItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QStringList>
#include <QShowEvent>
#include <QTimer>
#include <QWheelEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

class GraphView : public QGraphicsView
{
public:
    explicit GraphView(QWidget *parent = nullptr)
        : QGraphicsView(parent)
        , m_grid(QStringLiteral(":/textures/ui_nova_storymode_bggrid_shimmer_sideways.png"))
        , m_points(QStringLiteral(":/textures/ui_nova_storymode_bgpointgrid_25.png"))
        , m_lights(QStringLiteral(":/textures/ui_nova_login_backgroundlights.png"))
        , m_scanlines(QStringLiteral(":/textures/ui_nova_archives_backgroundframe_scanlines.png"))
    {
        setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
        setTransformationAnchor(AnchorUnderMouse);
        setResizeAnchor(AnchorViewCenter);
        setDragMode(ScrollHandDrag);
        setViewportUpdateMode(BoundingRectViewportUpdate);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::MiddleButton) {
            m_panning = true;
            m_lastPanPoint = event->pos();
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        if (event->button() == Qt::RightButton) {
            m_rotating = true;
            m_lastRotatePoint = event->pos();
            setCursor(Qt::SizeAllCursor);
            event->accept();
            return;
        }
        QGraphicsView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_panning) {
            const QPoint delta = event->pos() - m_lastPanPoint;
            m_lastPanPoint = event->pos();
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
            verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
            event->accept();
            return;
        }
        if (m_rotating) {
            const QPoint delta = event->pos() - m_lastRotatePoint;
            m_lastRotatePoint = event->pos();
            rotate(delta.x() * 0.35);
            event->accept();
            return;
        }
        QGraphicsView::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::MiddleButton) {
            m_panning = false;
            unsetCursor();
            event->accept();
            return;
        }
        if (event->button() == Qt::RightButton) {
            m_rotating = false;
            unsetCursor();
            event->accept();
            return;
        }
        QGraphicsView::mouseReleaseEvent(event);
    }

    void wheelEvent(QWheelEvent *event) override
    {
        const qreal factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
        const qreal currentScale = std::hypot(transform().m11(), transform().m12());
        const qreal nextScale = currentScale * factor;
        if (nextScale < 0.25 || nextScale > 3.5) {
            event->accept();
            return;
        }
        scale(factor, factor);
        event->accept();
    }

    void drawBackground(QPainter *painter, const QRectF &rect) override
    {
        Q_UNUSED(rect);
        painter->save();
        painter->resetTransform();
        painter->fillRect(viewport()->rect(), QColor(3, 8, 12));

        if (!m_grid.isNull()) {
            painter->setOpacity(0.18);
            painter->drawTiledPixmap(viewport()->rect(), m_grid);
        }
        if (!m_points.isNull()) {
            painter->setOpacity(0.08);
            painter->drawTiledPixmap(viewport()->rect(), m_points);
        }
        if (!m_lights.isNull()) {
            painter->setOpacity(0.17);
            painter->drawPixmap(viewport()->rect(), m_lights, m_lights.rect());
        }
        if (!m_scanlines.isNull()) {
            painter->setOpacity(0.11);
            painter->drawPixmap(viewport()->rect(), m_scanlines, m_scanlines.rect());
        }
        painter->restore();
    }

    void drawForeground(QPainter *painter, const QRectF &rect) override
    {
        QGraphicsView::drawForeground(painter, rect);
        painter->save();
        painter->resetTransform();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(QColor(102, 235, 203, 52), 1.0));
        painter->drawRoundedRect(viewport()->rect().adjusted(6, 6, -7, -7), 12.0, 12.0);
        painter->setPen(QPen(QColor(61, 142, 255, 38), 1.0));
        painter->drawLine(18, 18, 200, 18);
        painter->drawLine(viewport()->width() - 200, viewport()->height() - 18,
                          viewport()->width() - 18, viewport()->height() - 18);
        painter->restore();
    }

private:
    bool m_panning = false;
    bool m_rotating = false;
    QPoint m_lastPanPoint;
    QPoint m_lastRotatePoint;
    QPixmap m_grid;
    QPixmap m_points;
    QPixmap m_lights;
    QPixmap m_scanlines;
};

QString elideText(const QString &text, const QFont &font, int width)
{
    return QFontMetrics(font).elidedText(text, Qt::ElideRight, width);
}

QColor roleColor(const QString &role)
{
    if (role == QStringLiteral("center")) {
        return QColor(QStringLiteral("#3c77c8"));
    }
    if (role == QStringLiteral("incoming")) {
        return QColor(QStringLiteral("#2f8fa9"));
    }
    if (role == QStringLiteral("outgoing")) {
        return QColor(QStringLiteral("#c68d2c"));
    }
    return QColor(QStringLiteral("#b04b4b"));
}

QString firstLetters(const QString &text)
{
    QString badge;
    for (const QChar ch : text) {
        if (ch.isLetterOrNumber()) {
            badge.append(ch.toUpper());
        }
        if (badge.size() == 2) {
            break;
        }
    }
    return badge.isEmpty() ? QStringLiteral("?") : badge;
}

QString nodeSummary(const DataNode &node)
{
    QStringList parts;
    if (!node.elementName.isEmpty()) {
        parts.append(node.elementName);
    }
    if (!node.sourceFile.isEmpty()) {
        parts.append(node.sourceFile);
    }
    return parts.isEmpty() ? QStringLiteral("Unknown node") : parts.join(QStringLiteral(" | "));
}

QGraphicsRectItem *addCard(QGraphicsScene *scene,
                           const QPointF &pos,
                           const QString &titleText,
                           const QString &subtitleText,
                           const QString &badgeText,
                           const QString &role,
                           bool centerCard)
{
    const qreal width = centerCard ? 280.0 : 240.0;
    const qreal height = centerCard ? 96.0 : 76.0;
    const QColor base = roleColor(role);
    const QColor fill = centerCard ? QColor(QStringLiteral("#13273c")) : QColor(QStringLiteral("#151b25"));
    QPen pen(base.lighter(centerCard ? 145 : 130), centerCard ? 2.0 : 1.5);

    auto *frame = scene->addRect(QRectF(0.0, 0.0, width, height), pen, QBrush(fill));
    frame->setPos(pos);
    frame->setZValue(centerCard ? 10.0 : 2.0);

    auto *badge = scene->addEllipse(QRectF(0.0, 0.0, 26.0, 26.0),
                                    QPen(base.lighter(160), 1.0),
                                    QBrush(base.darker(centerCard ? 120 : 145)));
    badge->setParentItem(frame);
    badge->setPos(12.0, centerCard ? 18.0 : 14.0);

    QFont badgeFont;
    badgeFont.setBold(true);
    badgeFont.setPointSize(8);
    auto *badgeTextItem = scene->addSimpleText(badgeText, badgeFont);
    badgeTextItem->setBrush(QColor(QStringLiteral("#f7fbff")));
    badgeTextItem->setParentItem(frame);
    badgeTextItem->setPos(16.0, centerCard ? 21.0 : 18.0);

    QFont titleFont;
    titleFont.setBold(true);
    titleFont.setPointSize(centerCard ? 11 : 10);
    auto *title = scene->addSimpleText(elideText(titleText, titleFont, int(width - 54.0)), titleFont);
    title->setBrush(QColor(QStringLiteral("#f2f6ff")));
    title->setParentItem(frame);
    title->setPos(44.0, centerCard ? 16.0 : 12.0);

    QFont subtitleFont;
    subtitleFont.setPointSize(8);
    auto *subtitle = scene->addSimpleText(elideText(subtitleText, subtitleFont, int(width - 54.0)), subtitleFont);
    subtitle->setBrush(QColor(QStringLiteral("#9aa8bf")));
    subtitle->setParentItem(frame);
    subtitle->setPos(44.0, centerCard ? 42.0 : 35.0);

    if (centerCard) {
        auto *tag = scene->addSimpleText(QStringLiteral("selected"), badgeFont);
        tag->setBrush(QColor(QStringLiteral("#d8e7ff")));
        tag->setParentItem(frame);
        tag->setPos(width - 84.0, 14.0);
    }

    return frame;
}

QPointF leftCenter(const QGraphicsItem *item)
{
    const QRectF rect = item->sceneBoundingRect();
    return QPointF(rect.left(), rect.center().y());
}

QPointF rightCenter(const QGraphicsItem *item)
{
    const QRectF rect = item->sceneBoundingRect();
    return QPointF(rect.right(), rect.center().y());
}

QPointF topCenter(const QGraphicsItem *item)
{
    const QRectF rect = item->sceneBoundingRect();
    return QPointF(rect.center().x(), rect.top());
}

QPointF bottomCenter(const QGraphicsItem *item)
{
    const QRectF rect = item->sceneBoundingRect();
    return QPointF(rect.center().x(), rect.bottom());
}

void addArrow(QGraphicsScene *scene,
              const QPointF &start,
              const QPointF &end,
              const QColor &color,
              qreal thickness = 2.0)
{
    const QPointF delta = end - start;
    const qreal controlOffset = qMax(60.0, qAbs(delta.x()) * 0.35);
    const QPointF control1 = start + QPointF(delta.x() < 0 ? -controlOffset : controlOffset, 0.0);
    const QPointF control2 = end - QPointF(delta.x() < 0 ? -controlOffset : controlOffset, 0.0);

    QPainterPath path(start);
    path.cubicTo(control1, control2, end);
    auto *line = scene->addPath(path, QPen(color, thickness, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    line->setZValue(1.0);

    const QLineF tangent(control2, end);
    const qreal angle = std::atan2(-tangent.dy(), tangent.dx());
    constexpr qreal pi = 3.14159265358979323846;
    const qreal arrowSize = 8.0;
    const QPointF p1 = end + QPointF(std::sin(angle - pi / 3.0) * arrowSize,
                                     std::cos(angle - pi / 3.0) * arrowSize);
    const QPointF p2 = end + QPointF(std::sin(angle - pi + pi / 3.0) * arrowSize,
                                     std::cos(angle - pi + pi / 3.0) * arrowSize);
    scene->addPolygon(QPolygonF{end, p1, p2}, QPen(color.darker(120), 1.0), QBrush(color))->setZValue(1.5);
}

} // namespace

GraphPage::GraphPage(QWidget *parent)
    : QWidget(parent)
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(8);

    auto *header = new QFrame(this);
    header->setObjectName(QStringLiteral("graphHeader"));
    auto *headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(12, 10, 12, 10);
    headerLayout->setSpacing(6);

    m_titleLabel = new QLabel(QStringLiteral("Graph"), header);
    m_titleLabel->setObjectName(QStringLiteral("panelTitle"));
    headerLayout->addWidget(m_titleLabel);

    m_subtitleLabel = new QLabel(QStringLiteral("Select an object to render incoming, outgoing and missing references."), header);
    m_subtitleLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    m_subtitleLabel->setWordWrap(true);
    headerLayout->addWidget(m_subtitleLabel);

    m_hintBadge = new QLabel(QStringLiteral("Incoming, outgoing and missing links will appear here after object selection."), header);
    m_hintBadge->setObjectName(QStringLiteral("graphHintBadge"));
    m_hintBadge->setWordWrap(true);
    headerLayout->addWidget(m_hintBadge);

    m_controlsFrame = new QFrame(header);
    m_controlsFrame->setObjectName(QStringLiteral("graphControlsFrame"));
    m_controlsFrame->setMinimumHeight(56);
    m_controlsFrame->setMaximumHeight(56);
    m_controlsFrame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *controls = new QHBoxLayout(m_controlsFrame);
    controls->setContentsMargins(10, 8, 10, 8);
    controls->setSpacing(8);
    auto *controlsLabel = new QLabel(QStringLiteral("GRAPH CONTROLS"), m_controlsFrame);
    controlsLabel->setObjectName(QStringLiteral("graphControlsLabel"));
    controls->addWidget(controlsLabel);
    controls->addStretch(1);
    m_zoomOutButton = new QPushButton(QStringLiteral("Zoom -"), m_controlsFrame);
    m_zoomInButton = new QPushButton(QStringLiteral("Zoom +"), m_controlsFrame);
    auto *rotateLeftButton = new QPushButton(QStringLiteral("Rotate Left"), m_controlsFrame);
    auto *rotateRightButton = new QPushButton(QStringLiteral("Rotate Right"), m_controlsFrame);
    auto *flipXButton = new QPushButton(QStringLiteral("Rotate X"), m_controlsFrame);
    m_fitButton = new QPushButton(QStringLiteral("Fit Graph"), m_controlsFrame);
    const QList<QPushButton *> graphButtons{
        m_zoomOutButton,
        m_zoomInButton,
        rotateLeftButton,
        rotateRightButton,
        flipXButton,
        m_fitButton
    };
    for (QPushButton *button : graphButtons) {
        button->setObjectName(QStringLiteral("graphControlButton"));
        button->setMinimumSize(112, 34);
        button->setMaximumHeight(34);
        button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        button->setVisible(true);
    }
    controls->addWidget(m_zoomOutButton);
    controls->addWidget(m_zoomInButton);
    controls->addWidget(rotateLeftButton);
    controls->addWidget(rotateRightButton);
    controls->addWidget(flipXButton);
    controls->addWidget(m_fitButton);
    headerLayout->addWidget(m_controlsFrame);
    rootLayout->addWidget(header);

    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("graphCard"));
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(12, 12, 12, 12);
    cardLayout->setSpacing(0);

    m_scene = new QGraphicsScene(this);
    m_view = new GraphView(card);
    m_view->setObjectName(QStringLiteral("graphView"));
    m_view->setScene(m_scene);
    cardLayout->addWidget(m_view, 1);
    rootLayout->addWidget(card, 1);

    connect(m_fitButton, &QPushButton::clicked, this, &GraphPage::fitGraph);
    connect(m_zoomInButton, &QPushButton::clicked, this, [this]() { zoomBy(1.15); });
    connect(m_zoomOutButton, &QPushButton::clicked, this, [this]() { zoomBy(1.0 / 1.15); });
    connect(rotateLeftButton, &QPushButton::clicked, this, [this]() { m_view->rotate(-10.0); });
    connect(rotateRightButton, &QPushButton::clicked, this, [this]() { m_view->rotate(10.0); });
    connect(flipXButton, &QPushButton::clicked, this, [this]() { m_view->scale(1.0, -1.0); });

    renderGraph();
}

void GraphPage::setAnalysisResult(const AnalysisResult &result)
{
    m_result = result;
    m_graph.build(m_result.nodes);
    if (m_currentRow >= m_result.nodes.size()) {
        m_currentRow = -1;
    }
    renderGraph();
}

void GraphPage::setCurrentRow(int row)
{
    m_currentRow = row;
    renderGraph();
}

void GraphPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_fitPending) {
        QTimer::singleShot(0, this, [this]() {
            if (m_fitPending) {
                fitGraph();
                m_fitPending = false;
            }
        });
    }
}

QString GraphPage::uniqueNodeLabel(const DataNode &node) const
{
    if (!node.id.isEmpty()) {
        return node.id;
    }
    if (!node.elementName.isEmpty()) {
        return node.elementName;
    }
    return QStringLiteral("Unknown");
}

QString GraphPage::typeBadgeText(const QString &elementName) const
{
    return firstLetters(elementName);
}

const DataNode *GraphPage::findNodeById(const QString &id) const
{
    if (id.isEmpty()) {
        return nullptr;
    }
    for (const DataNode &node : m_result.nodes) {
        if (node.id == id) {
            return &node;
        }
    }
    return nullptr;
}

void GraphPage::zoomBy(qreal factor)
{
    const qreal currentScale = std::hypot(m_view->transform().m11(), m_view->transform().m12());
    const qreal nextScale = currentScale * factor;
    if (nextScale < 0.25 || nextScale > 3.5) {
        return;
    }
    m_view->scale(factor, factor);
}

void GraphPage::fitGraph()
{
    if (!m_scene || m_scene->items().isEmpty()) {
        return;
    }
    m_view->resetTransform();
    m_view->fitInView(m_scene->itemsBoundingRect().adjusted(-80.0, -80.0, 80.0, 120.0), Qt::KeepAspectRatio);
}

void GraphPage::renderGraph()
{
    m_scene->clear();
    m_scene->setBackgroundBrush(QColor(QStringLiteral("#0f131a")));
    m_fitPending = true;

    if (m_currentRow < 0 || m_currentRow >= m_result.nodes.size()) {
        m_titleLabel->setText(QStringLiteral("Graph"));
        m_subtitleLabel->setText(QStringLiteral("No object selected"));
        m_hintBadge->setText(QStringLiteral("Choose a row in Objects to center the graph and inspect links."));
        auto *label = m_scene->addText(QStringLiteral("No object selected"));
        label->setDefaultTextColor(QColor(QStringLiteral("#b6c4da")));
        QFont font = label->font();
        font.setPointSize(12);
        font.setBold(true);
        label->setFont(font);
        label->setPos(-label->boundingRect().width() / 2.0, -label->boundingRect().height() / 2.0);
        m_scene->setSceneRect(-220, -120, 440, 240);
        if (isVisible()) {
            QTimer::singleShot(0, this, [this]() {
                if (m_fitPending) {
                    fitGraph();
                    m_fitPending = false;
                }
            });
        }
        return;
    }

    const DataNode &node = m_result.nodes[m_currentRow];
    const QString currentLabel = uniqueNodeLabel(node);
    m_titleLabel->setText(currentLabel);
    m_subtitleLabel->setText(nodeSummary(node));

    QSet<QString> knownIds;
    for (const DataNode &candidate : m_result.nodes) {
        if (!candidate.id.isEmpty()) {
            knownIds.insert(candidate.id);
        }
    }

    QStringList incomingIds = m_graph.inboundReferencesFor(node.id);
    incomingIds.removeDuplicates();
    incomingIds.sort(Qt::CaseInsensitive);

    QStringList outgoingIds = node.referencedIds;
    outgoingIds.removeDuplicates();
    outgoingIds.sort(Qt::CaseInsensitive);

    QStringList missingIds;
    for (const QString &reference : outgoingIds) {
        if (!knownIds.contains(reference) && reference != node.id) {
            missingIds.append(reference);
        }
    }
    missingIds.removeDuplicates();
    missingIds.sort(Qt::CaseInsensitive);

    m_hintBadge->setText(QStringLiteral("Incoming: %1 | Outgoing: %2 | Missing: %3 | Controls: middle-drag, right-drag, wheel, fit")
                             .arg(incomingIds.size())
                             .arg(outgoingIds.size())
                             .arg(missingIds.size()));

    const QVector<qreal> incomingY = [&]() {
        QVector<qreal> positions;
        const int count = incomingIds.size();
        const qreal spacing = 88.0;
        const qreal start = -((count - 1) * spacing) / 2.0;
        for (int i = 0; i < count; ++i) {
            positions.append(start + i * spacing);
        }
        return positions;
    }();
    const QVector<qreal> outgoingY = [&]() {
        QVector<qreal> positions;
        const int count = outgoingIds.size();
        const qreal spacing = 88.0;
        const qreal start = -((count - 1) * spacing) / 2.0;
        for (int i = 0; i < count; ++i) {
            positions.append(start + i * spacing);
        }
        return positions;
    }();
    const QVector<qreal> missingX = [&]() {
        QVector<qreal> positions;
        const int count = missingIds.size();
        const qreal spacing = 120.0;
        const qreal start = -((count - 1) * spacing) / 2.0;
        for (int i = 0; i < count; ++i) {
            positions.append(start + i * spacing);
        }
        return positions;
    }();

    auto *center = addCard(m_scene,
                           QPointF(-140.0, -48.0),
                           currentLabel,
                           nodeSummary(node),
                           typeBadgeText(node.elementName),
                           QStringLiteral("center"),
                           true);

    for (int i = 0; i < incomingIds.size(); ++i) {
        const QString &id = incomingIds[i];
        const DataNode *source = findNodeById(id);
        const QString subtitle = source ? nodeSummary(*source) : QStringLiteral("Incoming reference");
        auto *card = addCard(m_scene,
                             QPointF(-410.0, incomingY.value(i, 0.0) - 38.0),
                             id,
                             subtitle,
                             source ? typeBadgeText(source->elementName) : QStringLiteral("IN"),
                             QStringLiteral("incoming"),
                             false);
        addArrow(m_scene, rightCenter(card), leftCenter(center), roleColor(QStringLiteral("incoming")));
    }

    for (int i = 0; i < outgoingIds.size(); ++i) {
        const QString &id = outgoingIds[i];
        const DataNode *target = findNodeById(id);
        const QString subtitle = target ? nodeSummary(*target) : QStringLiteral("Possible target reference");
        auto *card = addCard(m_scene,
                             QPointF(170.0, outgoingY.value(i, 0.0) - 38.0),
                             id,
                             subtitle,
                             target ? typeBadgeText(target->elementName) : QStringLiteral("OUT"),
                             QStringLiteral("outgoing"),
                             false);
        addArrow(m_scene, rightCenter(center), leftCenter(card), roleColor(QStringLiteral("outgoing")));
    }

    for (int i = 0; i < missingIds.size(); ++i) {
        const QString &id = missingIds[i];
        auto *card = addCard(m_scene,
                             QPointF(missingX.value(i, 0.0) - 120.0, 170.0),
                             id,
                             QStringLiteral("Missing from loaded object registry"),
                             QStringLiteral("?"),
                             QStringLiteral("missing"),
                             false);
        addArrow(m_scene, bottomCenter(center), topCenter(card), roleColor(QStringLiteral("missing")), 1.8);
    }

    if (incomingIds.isEmpty() && outgoingIds.isEmpty() && missingIds.isEmpty()) {
        auto *label = m_scene->addText(QStringLiteral("No references found"));
        label->setDefaultTextColor(QColor(QStringLiteral("#b6c4da")));
        QFont font = label->font();
        font.setPointSize(11);
        font.setBold(true);
        label->setFont(font);
        label->setPos(-label->boundingRect().width() / 2.0, 120.0);
    }

    m_scene->setSceneRect(m_scene->itemsBoundingRect().adjusted(-80.0, -80.0, 80.0, 120.0));
    if (isVisible()) {
        QTimer::singleShot(0, this, [this]() {
            if (m_fitPending) {
                fitGraph();
                m_fitPending = false;
            }
        });
    }
}
