#include "core/M3ModelParser.h"

#include <QtEndian>
#include <algorithm>
#include <cstring>

namespace {
struct Entry { QString tag; quint32 offset = 0, count = 0, version = 0; };
quint16 u16(const QByteArray &b, qsizetype p, bool *ok) { if (p < 0 || p + 2 > b.size()) { *ok = false; return 0; } quint16 x; std::memcpy(&x, b.constData() + p, 2); return qFromLittleEndian(x); }
quint32 u32(const QByteArray &b, qsizetype p, bool *ok) { if (p < 0 || p + 4 > b.size()) { *ok = false; return 0; } quint32 x; std::memcpy(&x, b.constData() + p, 4); return qFromLittleEndian(x); }
float f32(const QByteArray &b, qsizetype p, bool *ok) { quint32 x = u32(b, p, ok); float v; std::memcpy(&v, &x, 4); return v; }
QString tag(const QByteArray &b, qsizetype p, bool *ok) { if (p < 0 || p + 4 > b.size()) { *ok = false; return {}; } QByteArray s = b.mid(p, 4); std::reverse(s.begin(), s.end()); return QString::fromLatin1(s); }
}

bool M3ModelParser::parseStaticModel(const QByteArray &bytes, M3Model *model, QString *error) const
{
    if (!model) return false;
    *model = {}; bool ok = true;
    if (bytes.size() < 24 || tag(bytes, 0, &ok) != QStringLiteral("MD34")) { if (error) *error = QStringLiteral("Not an MD34 M3 model."); return false; }
    const quint32 indexOffset = u32(bytes, 4, &ok), count = u32(bytes, 8, &ok), modelId = u32(bytes, 16, &ok);
    if (!ok || !count || count > 200000 || modelId >= count || quint64(indexOffset) + quint64(count) * 16 > quint64(bytes.size())) { if (error) *error = QStringLiteral("Invalid M3 index."); return false; }
    QVector<Entry> entries; entries.reserve(int(count));
    for (quint32 i = 0; i < count; ++i) { const qsizetype p = indexOffset + qsizetype(i) * 16; entries << Entry{tag(bytes,p,&ok),u32(bytes,p+4,&ok),u32(bytes,p+8,&ok),u32(bytes,p+12,&ok)}; }
    const Entry header = entries.at(int(modelId));
    if (!ok || header.tag != QStringLiteral("MODL") || header.offset + 124 > quint32(bytes.size())) { if (error) *error = QStringLiteral("Unsupported M3 model header."); return false; }
    const quint32 flags = u32(bytes, header.offset + 96, &ok), vertexBytes = u32(bytes, header.offset + 100, &ok), vertexId = u32(bytes, header.offset + 104, &ok), divisionCount = u32(bytes, header.offset + 112, &ok), divisionId = u32(bytes, header.offset + 116, &ok);
    if (!ok || !vertexBytes || vertexId >= count || !divisionCount || divisionId >= count) { if (error) *error = QStringLiteral("M3 has no supported mesh."); return false; }
    int uvSets = flags & 0x100000 ? 4 : flags & 0x80000 ? 3 : flags & 0x40000 ? 2 : 1;
    const int stride = (7 + uvSets) * 4; const quint32 vertexCount = vertexBytes / quint32(stride); const Entry vertices = entries.at(int(vertexId));
    if (quint64(vertices.offset) + quint64(vertexCount) * stride > quint64(bytes.size())) { if (error) *error = QStringLiteral("Invalid M3 vertex buffer."); return false; }
    model->vertices.reserve(int(vertexCount));
    for (quint32 i = 0; i < vertexCount; ++i) { const qsizetype p = vertices.offset + qsizetype(i) * stride; M3Vertex v; v.position = {f32(bytes,p,&ok),f32(bytes,p+4,&ok),f32(bytes,p+8,&ok)}; if (!ok || !qIsFinite(v.position.x()) || !qIsFinite(v.position.y()) || !qIsFinite(v.position.z())) { if (error) *error = QStringLiteral("Invalid M3 vertex data."); return false; } v.uv = {float(qint16(u16(bytes,p+24,&ok))) / 32767.0f, float(qint16(u16(bytes,p+26,&ok))) / 32767.0f}; model->vertices << v; }
    const Entry division = entries.at(int(divisionId));
    for (quint32 d = 0; d < divisionCount; ++d) { const qsizetype dp = division.offset + qsizetype(d) * 52; const quint32 trianglesId = u32(bytes,dp+4,&ok), regions = u32(bytes,dp+12,&ok), regionsId = u32(bytes,dp+16,&ok); if (!ok || trianglesId >= count || regionsId >= count) continue; const Entry tri = entries.at(int(trianglesId)), reg = entries.at(int(regionsId)); const int rs = reg.version == 3 ? 36 : reg.version == 4 ? 40 : reg.version == 5 ? 48 : 0; if (!rs) continue; for (quint32 r=0;r<regions;++r) { const qsizetype rp=reg.offset+qsizetype(r)*rs; const quint32 base=u32(bytes,rp+8,&ok), first=u32(bytes,rp+16,&ok), n=u32(bytes,rp+20,&ok); for(quint32 j=0;j+2<n && model->triangles.size()<1000000;j+=3) { M3Triangle t{base+u16(bytes,tri.offset+2*qsizetype(first+j),&ok),base+u16(bytes,tri.offset+2*qsizetype(first+j+1),&ok),base+u16(bytes,tri.offset+2*qsizetype(first+j+2),&ok),-1}; if(t.a<vertexCount&&t.b<vertexCount&&t.c<vertexCount)model->triangles<<t; } } }
    if (model->triangles.isEmpty()) { if (error) *error = QStringLiteral("M3 contains no supported triangles."); return false; }
    return true;
}
