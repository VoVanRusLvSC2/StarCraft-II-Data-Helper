#pragma once

#include "core/MapRegionRepository.h"

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

namespace sc2dh::preview
{

struct TerrainDescriptor
{
    int gridWidth = 0;
    int gridHeight = 0;
    double offsetX = 0.0;
    double offsetY = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
    double quantizeBias = 0.0;
    double quantizeScale = 0.1;
    sc2dh::region::RegionBounds worldBounds;
    bool complete = false;
    QStringList errors;
};

struct MapPreviewData
{
    QImage image;
    sc2dh::region::RegionBounds worldBounds;
    bool exactWorldBounds = false;
    bool terrainBacked = false;
    QString sourceLabel;
    QString unavailableReason;
    QStringList warnings;
};

class MapPreviewDataReader
{
public:
    TerrainDescriptor parseTerrainXml(const QByteArray &bytes) const;
    sc2dh::region::RegionBounds parseMapInfoDimensions(const QByteArray &bytes,
                                                       QStringList *warnings = nullptr) const;
    QImage renderHeightMap(const QByteArray &bytes,
                           const TerrainDescriptor &descriptor,
                           QStringList *warnings = nullptr) const;
};

} // namespace sc2dh::preview
