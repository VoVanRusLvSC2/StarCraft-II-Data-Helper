#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace sc2dh::region
{

struct RegionPoint
{
    double x = 0.0;
    double y = 0.0;
};

struct RegionBounds
{
    double xMin = 0.0;
    double yMin = 0.0;
    double xMax = 0.0;
    double yMax = 0.0;
    bool valid = false;
};

enum class RegionShapeKind
{
    Circle,
    Rectangle,
    Polygon,
    Composite,
    Unknown
};

enum class SpatialRelation
{
    Inside,
    Outside,
    Boundary
};

struct RegionGeometry
{
    RegionShapeKind kind = RegionShapeKind::Unknown;
    QString rawType;
    RegionPoint center;
    double radius = 0.0;
    RegionBounds bounds;
    QVector<RegionPoint> points;
    QVector<RegionGeometry> components;
    QHash<QString, QStringList> rawParameters;
    bool supported = false;
    QString unsupportedReason;

    SpatialRelation classify(double x, double y, double tolerance = 1e-6) const;
};

struct MapRegion
{
    QString id;
    QString name;
    RegionGeometry geometry;
    QStringList markers;
};

struct RegionReadResult
{
    bool success = false;
    bool complete = false;
    QString sourceLabel;
    QVector<MapRegion> regions;
    QStringList warnings;
    QStringList errors;
};

class MapRegionRepository
{
public:
    RegionReadResult parse(const QByteArray &regionsBytes,
                           const QString &sourceLabel = QStringLiteral("Regions")) const;
};

QString regionShapeName(RegionShapeKind kind);

} // namespace sc2dh::region
