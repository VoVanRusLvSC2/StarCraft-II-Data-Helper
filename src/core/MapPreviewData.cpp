#include "core/MapPreviewData.h"

#include <QColor>
#include <QtEndian>

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace
{

bool parseNumbers(const QString &text, int count, QVector<double> *values)
{
    const QStringList parts = text.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() < count)
        return false;
    values->clear();
    for (int index = 0; index < count; ++index) {
        bool ok = false;
        const double value = parts.at(index).toDouble(&ok);
        if (!ok || !std::isfinite(value))
            return false;
        values->append(value);
    }
    return true;
}

pugi::xml_node findElement(pugi::xml_node node, const char *name)
{
    if (QString::fromUtf8(node.name()).compare(QString::fromLatin1(name), Qt::CaseInsensitive) == 0)
        return node;
    for (pugi::xml_node child : node.children()) {
        const pugi::xml_node found = findElement(child, name);
        if (found)
            return found;
    }
    return {};
}

quint32 readU32(const QByteArray &bytes, qsizetype offset)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

quint16 readU16(const QByteArray &bytes, qsizetype offset)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

QColor terrainColor(double normalizedHeight, quint16 mask)
{
    normalizedHeight = std::clamp(normalizedHeight, 0.0, 1.0);
    QColor low(40, 77, 58);
    QColor high(194, 181, 139);
    int red = int(low.red() + (high.red() - low.red()) * normalizedHeight);
    int green = int(low.green() + (high.green() - low.green()) * normalizedHeight);
    int blue = int(low.blue() + (high.blue() - low.blue()) * normalizedHeight);
    if ((mask & 0x9000u) != 0) {
        red = std::min(255, red + 28);
        green = std::max(0, green - 18);
        blue = std::max(0, blue - 18);
    }
    return QColor(red, green, blue);
}

} // namespace

namespace sc2dh::preview
{

TerrainDescriptor MapPreviewDataReader::parseTerrainXml(const QByteArray &bytes) const
{
    TerrainDescriptor result;
    pugi::xml_document document;
    const auto parsed = document.load_buffer(bytes.constData(), size_t(bytes.size()),
                                             pugi::parse_default, pugi::encoding_utf8);
    if (!parsed) {
        result.errors << QStringLiteral("t3Terrain.xml parse error at %1: %2")
                             .arg(parsed.offset)
                             .arg(QString::fromUtf8(parsed.description()));
        return result;
    }
    const pugi::xml_node heightMap = findElement(document.document_element(), "heightMap");
    if (!heightMap) {
        result.errors << QStringLiteral("t3Terrain.xml has no heightMap element.");
        return result;
    }
    QVector<double> values;
    if (!parseNumbers(QString::fromUtf8(heightMap.attribute("dim").value()), 2, &values)) {
        result.errors << QStringLiteral("heightMap dim is missing or invalid.");
        return result;
    }
    result.gridWidth = int(values.at(0));
    result.gridHeight = int(values.at(1));
    if (result.gridWidth < 2 || result.gridHeight < 2 || result.gridWidth > 4097 || result.gridHeight > 4097) {
        result.errors << QStringLiteral("heightMap dimensions are outside the supported safety range.");
        return result;
    }
    if (parseNumbers(QString::fromUtf8(heightMap.attribute("offset").value()), 3, &values)) {
        result.offsetX = values.at(0);
        result.offsetY = values.at(1);
    }
    if (parseNumbers(QString::fromUtf8(heightMap.attribute("scale").value()), 3, &values)) {
        result.scaleX = values.at(0);
        result.scaleY = values.at(1);
    }
    if (result.scaleX <= 0.0 || result.scaleY <= 0.0) {
        result.errors << QStringLiteral("heightMap scale must be positive.");
        return result;
    }
    const pugi::xml_node vertData = findElement(heightMap, "vertData");
    if (vertData) {
        bool ok = false;
        const double bias = QString::fromUtf8(vertData.attribute("quantizeBias").value()).toDouble(&ok);
        if (ok)
            result.quantizeBias = bias;
        const double scale = QString::fromUtf8(vertData.attribute("quantizeScale").value()).toDouble(&ok);
        if (ok && scale > 0.0)
            result.quantizeScale = scale;
    }
    result.worldBounds = {
        result.offsetX,
        result.offsetY,
        result.offsetX + double(result.gridWidth - 1) * result.scaleX,
        result.offsetY + double(result.gridHeight - 1) * result.scaleY,
        true
    };
    result.complete = true;
    return result;
}

sc2dh::region::RegionBounds MapPreviewDataReader::parseMapInfoDimensions(
    const QByteArray &bytes, QStringList *warnings) const
{
    if (bytes.size() < 16) {
        if (warnings)
            *warnings << QStringLiteral("MapInfo is too short for dimensions.");
        return {};
    }
    const quint32 magic = readU32(bytes, 0);
    const quint32 version = readU32(bytes, 4);
    if (magic != 0x4D617049u || version > 39u || version < 4u) {
        if (warnings)
            *warnings << QStringLiteral("MapInfo magic/version is unsupported for dimension fallback.");
        return {};
    }
    const quint32 width = readU32(bytes, 8);
    const quint32 height = readU32(bytes, 12);
    if (width < 1u || height < 1u || width > 256u || height > 256u) {
        if (warnings)
            *warnings << QStringLiteral("MapInfo dimensions failed the 1..256 bounds check.");
        return {};
    }
    return {0.0, 0.0, double(width), double(height), true};
}

QImage MapPreviewDataReader::renderHeightMap(const QByteArray &bytes,
                                             const TerrainDescriptor &descriptor,
                                             QStringList *warnings) const
{
    if (bytes.size() < 32) {
        if (warnings)
            *warnings << QStringLiteral("t3HeightMap is shorter than its 32-byte header.");
        return {};
    }
    const quint32 magic = readU32(bytes, 0);
    const quint32 version = readU32(bytes, 4);
    const quint32 width = readU32(bytes, 8);
    const quint32 height = readU32(bytes, 12);
    if (magic != 0x50414D48u || version != 101u || width < 2u || height < 2u
        || width > 4097u || height > 4097u) {
        if (warnings)
            *warnings << QStringLiteral("t3HeightMap header magic/version/dimensions are unsupported.");
        return {};
    }
    if (descriptor.complete
        && (int(width) != descriptor.gridWidth || int(height) != descriptor.gridHeight)) {
        if (warnings)
            *warnings << QStringLiteral("t3HeightMap dimensions do not match t3Terrain.xml.");
        return {};
    }
    const quint64 cells = quint64(width) * quint64(height);
    const quint64 expected = 32ull + cells * 6ull;
    if (expected != quint64(bytes.size())) {
        if (warnings)
            *warnings << QStringLiteral("t3HeightMap length mismatch: expected %1, got %2.")
                             .arg(expected).arg(bytes.size());
        return {};
    }

    quint32 minimum = std::numeric_limits<quint32>::max();
    quint32 maximum = 0;
    for (quint64 index = 0; index < cells; ++index) {
        const qsizetype offset = 32 + qsizetype(index * 6ull);
        const quint32 value = quint32(readU16(bytes, offset)) + quint32(readU16(bytes, offset + 2));
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    }
    const double span = std::max(1.0, double(maximum - minimum));
    QImage image(int(width), int(height), QImage::Format_RGB32);
    for (quint32 y = 0; y < height; ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(image.scanLine(int(height - 1u - y)));
        for (quint32 x = 0; x < width; ++x) {
            const quint64 index = quint64(y) * width + x;
            const qsizetype offset = 32 + qsizetype(index * 6ull);
            const quint32 value = quint32(readU16(bytes, offset)) + quint32(readU16(bytes, offset + 2));
            const quint16 mask = readU16(bytes, offset + 4);
            row[x] = terrainColor((double(value) - double(minimum)) / span, mask).rgb();
        }
    }
    return image;
}

} // namespace sc2dh::preview
