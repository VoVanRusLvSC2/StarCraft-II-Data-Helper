#include "ui/M3PreviewWidget.h"
#include "core/M3ModelParser.h"

#include <QMatrix4x4>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>

M3PreviewWidget::M3PreviewWidget(QWidget *parent) : QWidget(parent) { setMinimumHeight(220); setMouseTracking(true); }
void M3PreviewWidget::clearModel() { m_model = {}; hide(); update(); }

bool M3PreviewWidget::setModelData(const QByteArray &d, QString *error)
{
    clearModel();
    if (!M3ModelParser().parseStaticModel(d, &m_model, error)) return false;
    QVector3D lo(1e30f,1e30f,1e30f), hi(-1e30f,-1e30f,-1e30f);
    for (const M3Vertex &v : std::as_const(m_model.vertices)) { lo.setX(qMin(lo.x(),v.position.x())); lo.setY(qMin(lo.y(),v.position.y())); lo.setZ(qMin(lo.z(),v.position.z())); hi.setX(qMax(hi.x(),v.position.x())); hi.setY(qMax(hi.y(),v.position.y())); hi.setZ(qMax(hi.z(),v.position.z())); }
    const QVector3D center=(lo+hi)*.5f; const float scale=qMax(.0001f,(hi-lo).length());
    for (M3Vertex &v : m_model.vertices) v.position=(v.position-center)/scale;
    show(); update(); return true;
}

void M3PreviewWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this); p.fillRect(rect(),QColor(0,10,13)); p.setRenderHint(QPainter::Antialiasing); QMatrix4x4 m; m.rotate(m_pitch,1,0,0);m.rotate(m_yaw,0,1,0);
    QVector<QVector3D> v;v.reserve(m_model.vertices.size());for(const auto &x:m_model.vertices)v<<m.map(x.position);
    struct Draw{float z;QPolygonF q;QColor c;}; QVector<Draw> draw;draw.reserve(m_model.triangles.size()); const float s=qMin(width(),height())*.82f*m_zoom;
    for(const M3Triangle &f:m_model.triangles){const auto&a=v[int(f.a)],&b=v[int(f.b)],&c=v[int(f.c)]; QVector3D n=QVector3D::crossProduct(b-a,c-a).normalized(); if(n.z()>=0)continue; float light=qBound(.15f,-QVector3D::dotProduct(n,QVector3D(.3f,-.4f,-.85f).normalized()),1.f); QPolygonF q{QPointF(width()/2+a.x()*s,height()/2-a.y()*s),QPointF(width()/2+b.x()*s,height()/2-b.y()*s),QPointF(width()/2+c.x()*s,height()/2-c.y()*s)};draw.push_back({(a.z()+b.z()+c.z())/3,q,QColor::fromRgbF(.05,.65*light,.48*light)});}
    std::sort(draw.begin(),draw.end(),[](const Draw&a,const Draw&b){return a.z>b.z;});p.setPen(QPen(QColor(70,255,190,90),.5));for(const Draw&d:draw){p.setBrush(d.c);p.drawPolygon(d.q);} p.setPen(QColor(190,255,235));p.drawText(12,22,QStringLiteral("Drag to rotate - Wheel to zoom - %1 triangles").arg(m_model.triangles.size()));
}
void M3PreviewWidget::mousePressEvent(QMouseEvent*e){m_lastMouse=e->position().toPoint();}
void M3PreviewWidget::mouseMoveEvent(QMouseEvent*e){if(e->buttons()&Qt::LeftButton){QPoint d=e->position().toPoint()-m_lastMouse;m_yaw+=d.x()*.5f;m_pitch+=d.y()*.5f;m_lastMouse=e->position().toPoint();update();}}
void M3PreviewWidget::wheelEvent(QWheelEvent*e){m_zoom=qBound(.25f,m_zoom*(e->angleDelta().y()>0?1.12f:.89f),4.f);update();e->accept();}
