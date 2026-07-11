#include "core/M3ModelParser.h"

#include <QHash>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace
{
struct Entry
{
    QString tag;
    quint32 offset = 0;
    quint32 count = 0;
    quint32 version = 0;
};

struct Reference
{
    quint32 count = 0;
    quint32 id = 0;
};

quint16 u16(const QByteArray &bytes, qsizetype offset, bool *ok)
{
    if (offset < 0 || offset + 2 > bytes.size())
    {
        *ok = false;
        return 0;
    }
    quint16 value = 0;
    std::memcpy(&value, bytes.constData() + offset, 2);
    return qFromLittleEndian(value);
}

quint32 u32(const QByteArray &bytes, qsizetype offset, bool *ok)
{
    if (offset < 0 || offset + 4 > bytes.size())
    {
        *ok = false;
        return 0;
    }
    quint32 value = 0;
    std::memcpy(&value, bytes.constData() + offset, 4);
    return qFromLittleEndian(value);
}

float f32(const QByteArray &bytes, qsizetype offset, bool *ok)
{
    const quint32 raw = u32(bytes, offset, ok);
    float value = 0.0f;
    std::memcpy(&value, &raw, 4);
    return value;
}

QString tag(const QByteArray &bytes, qsizetype offset, bool *ok)
{
    if (offset < 0 || offset + 4 > bytes.size())
    {
        *ok = false;
        return {};
    }
    QByteArray value = bytes.mid(offset, 4);
    std::reverse(value.begin(), value.end());
    return QString::fromLatin1(value);
}

Reference referenceAt(const QByteArray &bytes, qsizetype offset, bool *ok)
{
    return {u32(bytes, offset, ok), u32(bytes, offset + 4, ok)};
}

bool validEntry(const QVector<Entry> &entries, const Reference &reference)
{
    return reference.count > 0 && reference.id < quint32(entries.size());
}

QString referenceString(const QByteArray &bytes,
                        const QVector<Entry> &entries,
                        qsizetype offset,
                        bool *ok)
{
    const Reference reference = referenceAt(bytes, offset, ok);
    if (!*ok || !validEntry(entries, reference))
        return {};

    const Entry &entry = entries.at(int(reference.id));
    if (entry.tag != QStringLiteral("CHAR") && entry.tag != QStringLiteral("SCHR"))
        return {};
    if (quint64(entry.offset) + reference.count > quint64(bytes.size()))
        return {};

    QByteArray value = bytes.mid(entry.offset, reference.count);
    const int terminator = value.indexOf('\0');
    if (terminator >= 0)
        value.truncate(terminator);
    return QString::fromUtf8(value);
}

int sequenceSize(quint32 version)
{
    if (version == 1)
        return 96;
    if (version == 2)
        return 92;
    return 0;
}

int regionSize(quint32 version)
{
    if (version == 3)
        return 36;
    if (version == 4)
        return 40;
    if (version == 5)
        return 48;
    return 0;
}
} // namespace

bool M3ModelParser::parseStaticModel(const QByteArray &bytes, M3Model *model, QString *error) const
{
    if (!model)
        return false;

    *model = {};
    bool ok = true;
    if (bytes.size() < 24 || tag(bytes, 0, &ok) != QStringLiteral("MD34"))
    {
        if (error)
            *error = QStringLiteral("Not an MD34 M3 model.");
        return false;
    }

    const quint32 indexOffset = u32(bytes, 4, &ok);
    const quint32 entryCount = u32(bytes, 8, &ok);
    const quint32 modelId = u32(bytes, 16, &ok);
    if (!ok || !entryCount || entryCount > 200000 || modelId >= entryCount
        || quint64(indexOffset) + quint64(entryCount) * 16 > quint64(bytes.size()))
    {
        if (error)
            *error = QStringLiteral("Invalid M3 index.");
        return false;
    }

    QVector<Entry> entries;
    entries.reserve(int(entryCount));
    for (quint32 i = 0; i < entryCount; ++i)
    {
        const qsizetype offset = indexOffset + qsizetype(i) * 16;
        entries.append({tag(bytes, offset, &ok),
                        u32(bytes, offset + 4, &ok),
                        u32(bytes, offset + 8, &ok),
                        u32(bytes, offset + 12, &ok)});
    }

    const Entry header = entries.at(int(modelId));
    if (!ok || header.tag != QStringLiteral("MODL") || header.offset + 124 > quint32(bytes.size()))
    {
        if (error)
            *error = QStringLiteral("Unsupported M3 model header.");
        return false;
    }

    model->name = referenceString(bytes, entries, header.offset, &ok);
    model->diagnostics << QStringLiteral("MODL:%1").arg(header.version);

    const Reference sequencesReference = referenceAt(bytes, header.offset + 16, &ok);
    const Reference bonesReference = referenceAt(bytes, header.offset + 80, &ok);
    model->boneCount = int(bonesReference.count);
    if (validEntry(entries, sequencesReference))
    {
        const Entry &entry = entries.at(int(sequencesReference.id));
        const int size = sequenceSize(entry.version);
        model->diagnostics << QStringLiteral("SEQS:%1 x%2").arg(entry.version).arg(sequencesReference.count);
        if (size > 0
            && quint64(entry.offset) + quint64(sequencesReference.count) * quint64(size) <= quint64(bytes.size()))
        {
            for (quint32 i = 0; i < sequencesReference.count; ++i)
            {
                const qsizetype offset = entry.offset + qsizetype(i) * size;
                M3AnimationSequence sequence;
                sequence.name = referenceString(bytes, entries, offset + 8, &ok);
                sequence.startFrame = u32(bytes, offset + 20, &ok);
                sequence.endFrame = u32(bytes, offset + 24, &ok);
                model->sequences.append(sequence);
            }
        }
    }
    if (validEntry(entries, bonesReference))
    {
        const Entry &entry = entries.at(int(bonesReference.id));
        model->diagnostics << QStringLiteral("BONE:%1 x%2").arg(entry.version).arg(bonesReference.count);
        if (entry.tag == QStringLiteral("BONE") && entry.version == 1
            && quint64(entry.offset) + quint64(bonesReference.count) * 160 <= quint64(bytes.size()))
        {
            model->bones.reserve(int(bonesReference.count));
            for (quint32 i = 0; i < bonesReference.count; ++i)
            {
                const qsizetype offset = entry.offset + qsizetype(i) * 160;
                M3Bone bone;
                bone.name = referenceString(bytes, entries, offset + 4, &ok);
                bone.parent = qint16(u16(bytes, offset + 20, &ok));
                bone.restPosition = {f32(bytes, offset + 32, &ok),
                                     f32(bytes, offset + 36, &ok),
                                     f32(bytes, offset + 40, &ok)};
                bone.restRotation = {f32(bytes, offset + 68, &ok),
                                     f32(bytes, offset + 72, &ok),
                                     f32(bytes, offset + 76, &ok),
                                     f32(bytes, offset + 80, &ok)};
                bone.restScale = {f32(bytes, offset + 108, &ok),
                                  f32(bytes, offset + 112, &ok),
                                  f32(bytes, offset + 116, &ok)};
                model->bones.append(bone);
            }
        }
    }

    // MODL has a stable material-reference position in every supported header
    // version (23, 25, 26, 28 and 29). The first material table is MAT_.
    QVector<int> materialMap;
    const Reference materialReferences = referenceAt(bytes, header.offset + 300, &ok);
    const Reference standardMaterials = referenceAt(bytes, header.offset + 312, &ok);
    if (validEntry(entries, standardMaterials))
    {
        const Entry &materialEntry = entries.at(int(standardMaterials.id));
        if (materialEntry.tag == QStringLiteral("MAT_") && materialEntry.count >= standardMaterials.count)
        {
            const int materialSize = materialEntry.version == 15 ? 268
                                   : materialEntry.version >= 16 && materialEntry.version <= 18 ? 280
                                   : materialEntry.version == 19 ? 340
                                   : materialEntry.version == 20 ? 352
                                                                         : 0;
            if (materialSize > 0
                && quint64(materialEntry.offset) + quint64(standardMaterials.count) * quint64(materialSize)
                       <= quint64(bytes.size()))
            {
                model->materials.reserve(int(standardMaterials.count));
                for (quint32 i = 0; i < standardMaterials.count; ++i)
                {
                    const qsizetype materialOffset = materialEntry.offset + qsizetype(i) * materialSize;
                    M3Material material;
                    material.name = referenceString(bytes, entries, materialOffset, &ok);
                    // MAT_ v20 added twelve bytes before the layer references.
                    const qsizetype diffuseOffset = materialEntry.version >= 20 ? 64 : 52;
                    const Reference diffuseLayer = referenceAt(bytes, materialOffset + diffuseOffset, &ok);
                    if (validEntry(entries, diffuseLayer))
                    {
                        const Entry &layerEntry = entries.at(int(diffuseLayer.id));
                        if (layerEntry.tag == QStringLiteral("LAYR"))
                            material.diffuseTexturePath = referenceString(bytes, entries, layerEntry.offset + 4, &ok);
                    }
                    model->materials.append(material);
                }
                model->diagnostics << QStringLiteral("MAT_:%1 x%2")
                                          .arg(materialEntry.version)
                                          .arg(model->materials.size());
            }
        }
    }

    if (validEntry(entries, materialReferences))
    {
        const Entry &mapEntry = entries.at(int(materialReferences.id));
        if (mapEntry.tag == QStringLiteral("MATM") && mapEntry.version == 0
            && quint64(mapEntry.offset) + quint64(materialReferences.count) * 8 <= quint64(bytes.size()))
        {
            materialMap.reserve(int(materialReferences.count));
            for (quint32 i = 0; i < materialReferences.count; ++i)
            {
                const qsizetype offset = mapEntry.offset + qsizetype(i) * 8;
                const quint32 materialType = u32(bytes, offset, &ok);
                const quint32 materialIndex = u32(bytes, offset + 4, &ok);
                // M3 material type 1 is the standard MAT_ table.
                materialMap.append(materialType == 1 && materialIndex < quint32(model->materials.size())
                                       ? int(materialIndex)
                                       : -1);
            }
        }
    }

    const quint32 vertexFlags = u32(bytes, header.offset + 96, &ok);
    const Reference verticesReference = referenceAt(bytes, header.offset + 100, &ok);
    const Reference divisionsReference = referenceAt(bytes, header.offset + 112, &ok);
    if (!ok || !validEntry(entries, verticesReference) || !validEntry(entries, divisionsReference))
    {
        if (error)
            *error = QStringLiteral("M3 has no supported mesh.");
        return false;
    }

    const int uvSets = vertexFlags & 0x100000 ? 4
                     : vertexFlags & 0x80000  ? 3
                     : vertexFlags & 0x40000  ? 2
                                              : 1;
    const int vertexStride = (7 + uvSets) * 4;
    if (verticesReference.count % quint32(vertexStride) != 0)
    {
        if (error)
            *error = QStringLiteral("Unsupported M3 vertex layout.");
        return false;
    }
    const quint32 vertexCount = verticesReference.count / quint32(vertexStride);
    const Entry &vertexEntry = entries.at(int(verticesReference.id));
    model->diagnostics << QStringLiteral("vertices=%1 stride=%2 uvSets=%3")
                              .arg(vertexCount)
                              .arg(vertexStride)
                              .arg(uvSets);
    if (quint64(vertexEntry.offset) + quint64(vertexCount) * quint64(vertexStride) > quint64(bytes.size()))
    {
        if (error)
            *error = QStringLiteral("Invalid M3 vertex buffer.");
        return false;
    }

    model->vertices.reserve(int(vertexCount));
    for (quint32 i = 0; i < vertexCount; ++i)
    {
        const qsizetype offset = vertexEntry.offset + qsizetype(i) * vertexStride;
        M3Vertex vertex;
        vertex.position = {f32(bytes, offset, &ok), f32(bytes, offset + 4, &ok), f32(bytes, offset + 8, &ok)};
        if (!ok || !qIsFinite(vertex.position.x()) || !qIsFinite(vertex.position.y())
            || !qIsFinite(vertex.position.z()))
        {
            if (error)
                *error = QStringLiteral("Invalid M3 vertex data.");
            return false;
        }
        for (int component = 0; component < 4; ++component)
        {
            vertex.boneWeights[size_t(component)] = quint8(bytes.at(offset + 12 + component));
            vertex.boneIndices[size_t(component)] = quint8(bytes.at(offset + 16 + component));
        }
        vertex.uv = {float(qint16(u16(bytes, offset + 24, &ok))) / 32767.0f,
                     float(qint16(u16(bytes, offset + 26, &ok))) / 32767.0f};
        model->vertices.append(vertex);
    }

    const Entry &divisionEntry = entries.at(int(divisionsReference.id));
    if (divisionEntry.tag != QStringLiteral("DIV_") || divisionEntry.version != 2)
    {
        if (error)
            *error = QStringLiteral("Unsupported M3 division layout.");
        return false;
    }

    for (quint32 divisionIndex = 0; divisionIndex < divisionsReference.count; ++divisionIndex)
    {
        const qsizetype divisionOffset = divisionEntry.offset + qsizetype(divisionIndex) * 52;
        const Reference triangleReference = referenceAt(bytes, divisionOffset, &ok);
        const Reference regionReference = referenceAt(bytes, divisionOffset + 12, &ok);
        const Reference batchReference = referenceAt(bytes, divisionOffset + 24, &ok);
        if (!ok || !validEntry(entries, triangleReference) || !validEntry(entries, regionReference))
            continue;

        QHash<int, int> materialByRegion;
        if (validEntry(entries, batchReference))
        {
            const Entry &batchEntry = entries.at(int(batchReference.id));
            if (batchEntry.tag == QStringLiteral("BAT_") && batchEntry.version == 1
                && quint64(batchEntry.offset) + quint64(batchReference.count) * 14 <= quint64(bytes.size()))
            {
                for (quint32 i = 0; i < batchReference.count; ++i)
                {
                    const qsizetype batchOffset = batchEntry.offset + qsizetype(i) * 14;
                    const int regionIndex = int(u16(bytes, batchOffset + 4, &ok));
                    const int mapIndex = int(u16(bytes, batchOffset + 10, &ok));
                    materialByRegion.insert(regionIndex,
                                            mapIndex >= 0 && mapIndex < materialMap.size() ? materialMap.at(mapIndex) : -1);
                }
            }
        }

        const Entry &triangleEntry = entries.at(int(triangleReference.id));
        const Entry &regionEntry = entries.at(int(regionReference.id));
        const int entrySize = regionSize(regionEntry.version);
        if (!entrySize)
            continue;

        for (quint32 regionIndex = 0; regionIndex < regionReference.count; ++regionIndex)
        {
            const qsizetype regionOffset = regionEntry.offset + qsizetype(regionIndex) * entrySize;
            const quint32 firstVertex = u32(bytes, regionOffset + 8, &ok);
            const quint32 firstIndex = u32(bytes, regionOffset + 16, &ok);
            const quint32 indexCount = u32(bytes, regionOffset + 20, &ok);
            const int materialIndex = materialByRegion.value(int(regionIndex), -1);
            if (!ok || quint64(triangleEntry.offset) + quint64(firstIndex + indexCount) * 2 > quint64(bytes.size()))
                continue;

            for (quint32 i = 0; i + 2 < indexCount && model->triangles.size() < 1000000; i += 3)
            {
                M3Triangle triangle;
                triangle.a = firstVertex + u16(bytes, triangleEntry.offset + 2 * qsizetype(firstIndex + i), &ok);
                triangle.b = firstVertex + u16(bytes, triangleEntry.offset + 2 * qsizetype(firstIndex + i + 1), &ok);
                triangle.c = firstVertex + u16(bytes, triangleEntry.offset + 2 * qsizetype(firstIndex + i + 2), &ok);
                triangle.materialIndex = materialIndex;
                if (ok && triangle.a < vertexCount && triangle.b < vertexCount && triangle.c < vertexCount)
                    model->triangles.append(triangle);
            }
        }
    }

    if (model->triangles.isEmpty())
    {
        if (error)
            *error = QStringLiteral("M3 contains no supported triangles.");
        return false;
    }

    model->diagnostics << QStringLiteral("triangles=%1").arg(model->triangles.size());
    if (error)
        error->clear();
    return true;
}
