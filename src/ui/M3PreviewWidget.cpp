#include "ui/M3PreviewWidget.h"

#include <QMatrix4x4>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cstring>

namespace {
quint16 u16(const QByteArray &d, qsizetype p, bool *ok) { if (p < 0 || p + 2 > d.size()) { *ok=false; return 0; } quint16 v; std::memcpy(&v,d.constData()+p,2); return qFromLittleEndian(v); }
quint32 u32(const QByteArray &d, qsizetype p, bool *ok) { if (p < 0 || p + 4 > d.size()) { *ok=false; return 0; } quint32 v; std::memcpy(&v,d.constData()+p,4); return qFromLittleEndian(v); }
float f32(const QByteArray &d, qsizetype p, bool *ok) { const quint32 bits=u32(d,p,ok); float v=0; std::memcpy(&v,&bits,4); return v; }
QString tag(const QByteArray &d, qsizetype p, bool *ok) { if(p<0||p+4>d.size()){*ok=false;return{};} QByteArray s=d.mid(p,4); std::reverse(s.begin(),s.end()); return QString::fromLatin1(s); }
struct Entry { QString tag; quint32 offset=0,count=0,version=0; };
}

M3PreviewWidget::M3PreviewWidget(QWidget *parent) : QWidget(parent) { setMinimumHeight(220); setMouseTracking(true); }
void M3PreviewWidget::clearModel() { m_vertices.clear(); m_faces.clear(); hide(); update(); }

bool M3PreviewWidget::setModelData(const QByteArray &d, QString *error)
{
    clearModel(); bool ok=true;
    if (d.size()<24 || tag(d,0,&ok)!=QStringLiteral("MD34")) { if(error)*error=QStringLiteral("Not an MD34 model"); return false; }
    const quint32 table=u32(d,4,&ok), count=u32(d,8,&ok), modelId=u32(d,16,&ok);
    if(!ok || count>200000 || quint64(table)+quint64(count)*16>quint64(d.size()) || modelId>=count) { if(error)*error=QStringLiteral("Invalid M3 index"); return false; }
    QVector<Entry> entries; entries.reserve(int(count));
    for(quint32 i=0;i<count;++i){ const qsizetype p=table+qsizetype(i)*16; entries.push_back({tag(d,p,&ok),u32(d,p+4,&ok),u32(d,p+8,&ok),u32(d,p+12,&ok)}); }
    const Entry model=entries[int(modelId)];
    if(!ok || model.tag!=QStringLiteral("MODL") || model.offset+124u>quint32(d.size())) { if(error)*error=QStringLiteral("Unsupported M3 model header"); return false; }
    const quint32 flags=u32(d,model.offset+96,&ok), vertexCount=u32(d,model.offset+100,&ok), vertexId=u32(d,model.offset+104,&ok);
    const quint32 divisionCount=u32(d,model.offset+112,&ok), divisionId=u32(d,model.offset+116,&ok);
    if(!ok||!vertexCount||vertexId>=count||!divisionCount||divisionId>=count){if(error)*error=QStringLiteral("M3 has no mesh");return false;}
    int uv=1; if(flags&0x40000)uv=2; else if(flags&0x80000)uv=3; else if(flags&0x100000)uv=4;
    const int stride=(7+uv)*4; const Entry ve=entries[int(vertexId)];
    if(quint64(ve.offset)+quint64(vertexCount)*stride>quint64(d.size())||vertexCount>5000000){if(error)*error=QStringLiteral("Invalid M3 vertices");return false;}
    m_vertices.reserve(int(vertexCount));
    QVector3D lo(1e30f,1e30f,1e30f),hi(-1e30f,-1e30f,-1e30f);
    for(quint32 i=0;i<vertexCount;++i){ QVector3D v(f32(d,ve.offset+qsizetype(i)*stride,&ok),f32(d,ve.offset+qsizetype(i)*stride+4,&ok),f32(d,ve.offset+qsizetype(i)*stride+8,&ok)); if(!ok||!qIsFinite(v.x())||!qIsFinite(v.y())||!qIsFinite(v.z())){if(error)*error=QStringLiteral("Invalid M3 vertex data");clearModel();return false;} m_vertices<<v; lo.setX(qMin(lo.x(),v.x()));lo.setY(qMin(lo.y(),v.y()));lo.setZ(qMin(lo.z(),v.z()));hi.setX(qMax(hi.x(),v.x()));hi.setY(qMax(hi.y(),v.y()));hi.setZ(qMax(hi.z(),v.z())); }
    const QVector3D center=(lo+hi)*.5f; const float scale=qMax(.0001f,(hi-lo).length()); for(QVector3D &v:m_vertices)v=(v-center)/scale;
    const Entry div=entries[int(divisionId)];
    for(quint32 di=0;di<divisionCount;++di){ const qsizetype dp=div.offset+qsizetype(di)*52; const quint32 triCount=u32(d,dp,&ok),triId=u32(d,dp+4,&ok),regCount=u32(d,dp+12,&ok),regId=u32(d,dp+16,&ok); if(!ok||triId>=count||regId>=count)continue; const Entry te=entries[int(triId)],re=entries[int(regId)]; const int rs=re.version==3?36:re.version==4?40:re.version==5?48:0; if(!rs)continue; for(quint32 r=0;r<regCount;++r){qsizetype rp=re.offset+qsizetype(r)*rs; quint32 base=u32(d,rp+8,&ok),first=u32(d,rp+16,&ok),n=u32(d,rp+20,&ok); for(quint32 j=0;j+2<n && m_faces.size()<1000000;j+=3){quint32 a=base+u16(d,te.offset+2*qsizetype(first+j),&ok),b=base+u16(d,te.offset+2*qsizetype(first+j+1),&ok),c=base+u16(d,te.offset+2*qsizetype(first+j+2),&ok);if(a<vertexCount&&b<vertexCount&&c<vertexCount)m_faces.push_back({a,b,c});}} Q_UNUSED(triCount); }
    if(m_faces.isEmpty()){if(error)*error=QStringLiteral("M3 contains no supported triangles");clearModel();return false;} show(); update(); return true;
}

void M3PreviewWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this); p.fillRect(rect(),QColor(0,10,13)); p.setRenderHint(QPainter::Antialiasing); QMatrix4x4 m; m.rotate(m_pitch,1,0,0);m.rotate(m_yaw,0,1,0);
    QVector<QVector3D> v;v.reserve(m_vertices.size());for(const auto &x:m_vertices)v<<m.map(x);
    struct Draw{float z;QPolygonF q;QColor c;}; QVector<Draw> draw;draw.reserve(m_faces.size()); const float s=qMin(width(),height())*.82f*m_zoom;
    for(const Face &f:m_faces){const auto&a=v[int(f.a)],&b=v[int(f.b)],&c=v[int(f.c)]; QVector3D n=QVector3D::crossProduct(b-a,c-a).normalized(); if(n.z()>=0)continue; float light=qBound(.15f,-QVector3D::dotProduct(n,QVector3D(.3f,-.4f,-.85f).normalized()),1.f); QPolygonF q{QPointF(width()/2+a.x()*s,height()/2-a.y()*s),QPointF(width()/2+b.x()*s,height()/2-b.y()*s),QPointF(width()/2+c.x()*s,height()/2-c.y()*s)};draw.push_back({(a.z()+b.z()+c.z())/3,q,QColor::fromRgbF(.05,.65*light,.48*light)});}
    std::sort(draw.begin(),draw.end(),[](const Draw&a,const Draw&b){return a.z>b.z;});p.setPen(QPen(QColor(70,255,190,90),.5));for(const Draw&d:draw){p.setBrush(d.c);p.drawPolygon(d.q);} p.setPen(QColor(190,255,235));p.drawText(12,22,QStringLiteral("Drag to rotate • Wheel to zoom • %1 triangles").arg(m_faces.size()));
}
void M3PreviewWidget::mousePressEvent(QMouseEvent*e){m_lastMouse=e->position().toPoint();}
void M3PreviewWidget::mouseMoveEvent(QMouseEvent*e){if(e->buttons()&Qt::LeftButton){QPoint d=e->position().toPoint()-m_lastMouse;m_yaw+=d.x()*.5f;m_pitch+=d.y()*.5f;m_lastMouse=e->position().toPoint();update();}}
void M3PreviewWidget::wheelEvent(QWheelEvent*e){m_zoom=qBound(.25f,m_zoom*(e->angleDelta().y()>0?1.12f:.89f),4.f);update();e->accept();}
