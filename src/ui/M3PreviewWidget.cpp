#include "ui/M3PreviewWidget.h"

#include "core/M3ModelParser.h"
#include "app/AppSettings.h"

#include <QMatrix4x4>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>

M3PreviewWidget::M3PreviewWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(220);
    setMouseTracking(true);
}

void M3PreviewWidget::clearModel()
{
    m_model = {};
    hide();
    update();
}

bool M3PreviewWidget::setModelData(const QByteArray &data, QString *error)
{
    clearModel();
    if (!M3ModelParser().parseStaticModel(data, &m_model, error))
        return false;

    QVector3D lower(1e30f, 1e30f, 1e30f);
    QVector3D upper(-1e30f, -1e30f, -1e30f);
    for (const M3Vertex &vertex : std::as_const(m_model.vertices))
    {
        lower.setX(qMin(lower.x(), vertex.position.x()));
        lower.setY(qMin(lower.y(), vertex.position.y()));
        lower.setZ(qMin(lower.z(), vertex.position.z()));
        upper.setX(qMax(upper.x(), vertex.position.x()));
        upper.setY(qMax(upper.y(), vertex.position.y()));
        upper.setZ(qMax(upper.z(), vertex.position.z()));
    }
    const QVector3D center = (lower + upper) * 0.5f;
    const float scale = qMax(0.0001f, (upper - lower).length());
    for (M3Vertex &vertex : m_model.vertices)
        vertex.position = (vertex.position - center) / scale;

    QStringList details = m_model.diagnostics;
    for (const M3Material &material : std::as_const(m_model.materials))
    {
        if (!material.diffuseTexturePath.isEmpty())
            details << QStringLiteral("%1 -> %2").arg(material.name, material.diffuseTexturePath);
    }
    if (!m_model.sequences.isEmpty())
    {
        QStringList sequenceNames;
        for (const M3AnimationSequence &sequence : std::as_const(m_model.sequences))
            sequenceNames << sequence.name;
        details << QStringLiteral("Animations: %1").arg(sequenceNames.join(QStringLiteral(", ")));
    }
    setToolTip(details.join(QLatin1Char('\n')));
    show();
    update();
    return true;
}

void M3PreviewWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0, 10, 13));
    painter.setRenderHint(QPainter::Antialiasing, sc2dh::app::AppSettings::modelAntialiasing());

    QMatrix4x4 rotation;
    rotation.rotate(m_pitch, 1, 0, 0);
    rotation.rotate(m_yaw, 0, 1, 0);
    QVector<QVector3D> vertices;
    vertices.reserve(m_model.vertices.size());
    for (const M3Vertex &vertex : std::as_const(m_model.vertices))
        vertices.append(rotation.map(vertex.position));

    struct DrawTriangle
    {
        float depth = 0.0f;
        QPolygonF polygon;
        QColor color;
    };
    QVector<DrawTriangle> drawList;
    drawList.reserve(m_model.triangles.size());
    const float scale = qMin(width(), height()) * 0.82f * m_zoom;
    for (const M3Triangle &face : std::as_const(m_model.triangles))
    {
        const QVector3D &a = vertices.at(int(face.a));
        const QVector3D &b = vertices.at(int(face.b));
        const QVector3D &c = vertices.at(int(face.c));
        const QVector3D normal = QVector3D::crossProduct(b - a, c - a).normalized();
        if (normal.z() >= 0)
            continue;

        const float light = qBound(0.15f,
                                   -QVector3D::dotProduct(normal, QVector3D(0.3f, -0.4f, -0.85f).normalized()),
                                   1.0f);
        QColor baseColor(13, 166, 122);
        if (face.materialIndex >= 0 && face.materialIndex < m_model.materials.size())
        {
            baseColor = m_model.materials.at(face.materialIndex).fallbackColor;
            if (baseColor == QColor(20, 165, 122))
                baseColor = QColor::fromHsv((face.materialIndex * 47 + 145) % 360, 185, 190);
        }
        baseColor.setRedF(baseColor.redF() * light);
        baseColor.setGreenF(baseColor.greenF() * light);
        baseColor.setBlueF(baseColor.blueF() * light);

        DrawTriangle triangle;
        triangle.depth = (a.z() + b.z() + c.z()) / 3.0f;
        triangle.polygon = {
            QPointF(width() / 2.0 + a.x() * scale, height() / 2.0 - a.y() * scale),
            QPointF(width() / 2.0 + b.x() * scale, height() / 2.0 - b.y() * scale),
            QPointF(width() / 2.0 + c.x() * scale, height() / 2.0 - c.y() * scale)};
        triangle.color = baseColor;
        drawList.append(triangle);
    }

    std::sort(drawList.begin(), drawList.end(), [](const DrawTriangle &left, const DrawTriangle &right) {
        return left.depth > right.depth;
    });
    painter.setPen(QPen(QColor(70, 255, 190, 90), 0.5));
    for (const DrawTriangle &triangle : std::as_const(drawList))
    {
        painter.setBrush(triangle.color);
        painter.drawPolygon(triangle.polygon);
    }

    painter.setPen(QColor(190, 255, 235));
    painter.drawText(12, 22,
                     QStringLiteral("Drag: rotate | Wheel: zoom | %1 triangles | %2 materials | %3 bones | %4 animations")
                         .arg(m_model.triangles.size())
                         .arg(m_model.materials.size())
                         .arg(m_model.boneCount)
                         .arg(m_model.sequences.size()));
}

void M3PreviewWidget::mousePressEvent(QMouseEvent *event)
{
    m_lastMouse = event->position().toPoint();
}

void M3PreviewWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton)
    {
        const QPoint delta = event->position().toPoint() - m_lastMouse;
        m_yaw += delta.x() * 0.5f;
        m_pitch += delta.y() * 0.5f;
        m_lastMouse = event->position().toPoint();
        update();
    }
}

void M3PreviewWidget::wheelEvent(QWheelEvent *event)
{
    m_zoom = qBound(0.25f, m_zoom * (event->angleDelta().y() > 0 ? 1.12f : 0.89f), 4.0f);
    update();
    event->accept();
}
