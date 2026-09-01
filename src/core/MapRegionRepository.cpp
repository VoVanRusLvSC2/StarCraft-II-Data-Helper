#include "core/MapRegionRepository.h"

#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <limits>
#include <pugixml.hpp>
#include <utility>

namespace
{

QVector<double> numbersFrom(const QString &text)
{
    QVector<double> values;
    static const QRegularExpression number(
        QStringLiteral("[-+]?(?:\\d+(?:\\.\\d*)?|\\.\\d+)(?:[eE][-+]?\\d+)?"));
    auto matches = number.globalMatch(text);
    while (matches.hasNext()) {
        bool ok = false;
        const double value = matches.next().captured().toDouble(&ok);
        if (ok && std::isfinite(value))
            values << value;
    }
    return values;
}

QString childValue(const pugi::xml_node &parent, const char *name)
{
    const pugi::xml_node child = parent.child(name);
    return child ? QString::fromUtf8(child.attribute("value").value()) : QString();
}

bool pointFrom(const QString &text, sc2dh::region::RegionPoint *point)
{
    const QVector<double> values = numbersFrom(text);
    if (!point || values.size() < 2)
        return false;
    point->x = values.at(0);
    point->y = values.at(1);
    return true;
}

sc2dh::region::RegionBounds boundsFromPoints(const QVector<sc2dh::region::RegionPoint> &points)
{
    sc2dh::region::RegionBounds bounds;
    if (points.isEmpty())
        return bounds;
    bounds.xMin = bounds.xMax = points.front().x;
    bounds.yMin = bounds.yMax = points.front().y;
    for (const auto &point : points) {
        bounds.xMin = std::min(bounds.xMin, point.x);
        bounds.xMax = std::max(bounds.xMax, point.x);
        bounds.yMin = std::min(bounds.yMin, point.y);
        bounds.yMax = std::max(bounds.yMax, point.y);
    }
    bounds.valid = true;
    return bounds;
}

void includeBounds(sc2dh::region::RegionBounds *target,
                   const sc2dh::region::RegionBounds &source)
{
    if (!target || !source.valid)
        return;
    if (!target->valid) {
        *target = source;
        return;
    }
    target->xMin = std::min(target->xMin, source.xMin);
    target->yMin = std::min(target->yMin, source.yMin);
    target->xMax = std::max(target->xMax, source.xMax);
    target->yMax = std::max(target->yMax, source.yMax);
}

sc2dh::region::RegionGeometry parseShape(const pugi::xml_node &shape)
{
    using namespace sc2dh::region;
    RegionGeometry geometry;
    geometry.rawType = shape ? QString::fromUtf8(shape.attribute("type").value()).trimmed() : QString();
    for (pugi::xml_node parameter : shape.children()) {
        const QString key = QString::fromUtf8(parameter.name());
        geometry.rawParameters[key].append(QString::fromUtf8(parameter.attribute("value").value()));
    }

    const QString type = geometry.rawType.toCaseFolded();
    if (type == QStringLiteral("circle")) {
        geometry.kind = RegionShapeKind::Circle;
        bool radiusOk = false;
        geometry.radius = childValue(shape, "radius").toDouble(&radiusOk);
        if (pointFrom(childValue(shape, "center"), &geometry.center)
            && radiusOk && geometry.radius > 0.0 && std::isfinite(geometry.radius)) {
            geometry.bounds = {geometry.center.x - geometry.radius,
                               geometry.center.y - geometry.radius,
                               geometry.center.x + geometry.radius,
                               geometry.center.y + geometry.radius,
                               true};
            geometry.supported = true;
        }
    } else if (type == QStringLiteral("rect") || type == QStringLiteral("rectangle")) {
        geometry.kind = RegionShapeKind::Rectangle;
        const QStringList candidates{childValue(shape, "quad"), childValue(shape, "bounds"),
                                     childValue(shape, "rect")};
        QVector<double> boundsValues;
        for (const QString &candidate : candidates) {
            boundsValues = numbersFrom(candidate);
            if (boundsValues.size() >= 4)
                break;
        }
        if (boundsValues.size() >= 4) {
            geometry.bounds = {std::min(boundsValues.at(0), boundsValues.at(2)),
                               std::min(boundsValues.at(1), boundsValues.at(3)),
                               std::max(boundsValues.at(0), boundsValues.at(2)),
                               std::max(boundsValues.at(1), boundsValues.at(3)), true};
            geometry.supported = true;
        } else {
            RegionPoint minimum;
            RegionPoint maximum;
            if (pointFrom(childValue(shape, "min"), &minimum)
                && pointFrom(childValue(shape, "max"), &maximum)) {
                geometry.bounds = {std::min(minimum.x, maximum.x), std::min(minimum.y, maximum.y),
                                   std::max(minimum.x, maximum.x), std::max(minimum.y, maximum.y), true};
                geometry.supported = true;
            } else {
                RegionPoint center;
                RegionPoint size;
                if (pointFrom(childValue(shape, "center"), &center)
                    && pointFrom(childValue(shape, "size"), &size)
                    && size.x > 0.0 && size.y > 0.0) {
                    geometry.center = center;
                    geometry.bounds = {center.x - size.x * 0.5, center.y - size.y * 0.5,
                                       center.x + size.x * 0.5, center.y + size.y * 0.5, true};
                    geometry.supported = true;
                }
            }
        }
    } else if (type == QStringLiteral("polygon") || type == QStringLiteral("poly")) {
        geometry.kind = RegionShapeKind::Polygon;
        for (pugi::xml_node parameter : shape.children()) {
            const QString key = QString::fromUtf8(parameter.name()).toCaseFolded();
            if (!key.contains(QStringLiteral("point")) && !key.contains(QStringLiteral("vertex")))
                continue;
            const QVector<double> values = numbersFrom(QString::fromUtf8(parameter.attribute("value").value()));
            for (int index = 0; index + 1 < values.size(); index += 2)
                geometry.points << RegionPoint{values.at(index), values.at(index + 1)};
        }
        geometry.bounds = boundsFromPoints(geometry.points);
        geometry.supported = geometry.points.size() >= 3;
    }

    if (!geometry.supported) {
        geometry.unsupportedReason = shape
            ? QStringLiteral("Unsupported or incomplete region shape '%1'.").arg(geometry.rawType)
            : QStringLiteral("Region has no shape element.");
    }
    return geometry;
}

double distanceToSegment(double x, double y,
                         const sc2dh::region::RegionPoint &a,
                         const sc2dh::region::RegionPoint &b)
{
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double lengthSquared = dx * dx + dy * dy;
    if (lengthSquared <= std::numeric_limits<double>::epsilon())
        return std::hypot(x - a.x, y - a.y);
    const double t = std::clamp(((x - a.x) * dx + (y - a.y) * dy) / lengthSquared, 0.0, 1.0);
    return std::hypot(x - (a.x + t * dx), y - (a.y + t * dy));
}

} // namespace

namespace sc2dh::region
{

SpatialRelation RegionGeometry::classify(double x, double y, double tolerance) const
{
    if (!supported || !bounds.valid)
        return SpatialRelation::Outside;
    tolerance = std::max(0.0, tolerance);
    if (kind == RegionShapeKind::Composite) {
        bool boundary = false;
        for (const RegionGeometry &component : components) {
            const SpatialRelation relation = component.classify(x, y, tolerance);
            if (relation == SpatialRelation::Inside)
                return SpatialRelation::Inside;
            boundary = boundary || relation == SpatialRelation::Boundary;
        }
        return boundary ? SpatialRelation::Boundary : SpatialRelation::Outside;
    }
    if (kind == RegionShapeKind::Circle) {
        const double distance = std::hypot(x - center.x, y - center.y);
        if (std::abs(distance - radius) <= tolerance)
            return SpatialRelation::Boundary;
        return distance < radius ? SpatialRelation::Inside : SpatialRelation::Outside;
    }
    if (kind == RegionShapeKind::Rectangle) {
        if (x < bounds.xMin - tolerance || x > bounds.xMax + tolerance
            || y < bounds.yMin - tolerance || y > bounds.yMax + tolerance)
            return SpatialRelation::Outside;
        if (std::abs(x - bounds.xMin) <= tolerance || std::abs(x - bounds.xMax) <= tolerance
            || std::abs(y - bounds.yMin) <= tolerance || std::abs(y - bounds.yMax) <= tolerance)
            return SpatialRelation::Boundary;
        return SpatialRelation::Inside;
    }
    if (kind == RegionShapeKind::Polygon && points.size() >= 3) {
        bool inside = false;
        for (int i = 0, j = points.size() - 1; i < points.size(); j = i++) {
            const RegionPoint &a = points.at(j);
            const RegionPoint &b = points.at(i);
            if (distanceToSegment(x, y, a, b) <= tolerance)
                return SpatialRelation::Boundary;
            const bool crosses = ((a.y > y) != (b.y > y))
                && (x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x);
            if (crosses)
                inside = !inside;
        }
        return inside ? SpatialRelation::Inside : SpatialRelation::Outside;
    }
    return SpatialRelation::Outside;
}

RegionReadResult MapRegionRepository::parse(const QByteArray &regionsBytes,
                                            const QString &sourceLabel) const
{
    RegionReadResult result;
    result.sourceLabel = sourceLabel;
    pugi::xml_document document;
    const pugi::xml_parse_result parsed = document.load_buffer(
        regionsBytes.constData(), size_t(regionsBytes.size()), pugi::parse_default, pugi::encoding_utf8);
    if (!parsed) {
        result.errors << QStringLiteral("%1: XML parse error at byte %2: %3")
                             .arg(sourceLabel)
                             .arg(parsed.offset)
                             .arg(QString::fromUtf8(parsed.description()));
        return result;
    }
    const pugi::xml_node root = document.child("Regions");
    if (!root) {
        result.errors << QStringLiteral("%1: expected <Regions> root element.").arg(sourceLabel);
        return result;
    }

    QHash<QString, int> names;
    for (pugi::xml_node node : root.children("region")) {
        MapRegion region;
        region.id = QString::fromUtf8(node.attribute("id").value());
        region.name = childValue(node, "name");
        if (region.name.isEmpty())
            region.name = QStringLiteral("Region %1").arg(region.id.isEmpty() ? QStringLiteral("?") : region.id);
        names[region.name] += 1;

        for (pugi::xml_node child : node.children()) {
            if (child.type() == pugi::node_element && !child.attribute("value")
                && !child.first_child() && QString::fromUtf8(child.name()) != QStringLiteral("shape"))
                region.markers << QString::fromUtf8(child.name());
        }

        QVector<RegionGeometry> shapes;
        for (pugi::xml_node shape : node.children("shape"))
            shapes << parseShape(shape);
        if (shapes.size() == 1) {
            region.geometry = shapes.front();
        } else if (shapes.size() > 1) {
            RegionGeometry &geometry = region.geometry;
            geometry.kind = RegionShapeKind::Composite;
            geometry.rawType = QStringLiteral("composite");
            geometry.components = shapes;
            geometry.supported = true;
            for (const RegionGeometry &component : std::as_const(geometry.components)) {
                includeBounds(&geometry.bounds, component.bounds);
                if (!component.supported) {
                    geometry.supported = false;
                    geometry.unsupportedReason = QStringLiteral("Composite region contains an unsupported shape: %1")
                                                     .arg(component.unsupportedReason);
                }
            }
            geometry.supported = geometry.supported && geometry.bounds.valid;
        } else {
            region.geometry.unsupportedReason = QStringLiteral("Region has no shape element.");
        }

        if (!region.geometry.supported) {
            result.warnings << QStringLiteral("%1 [%2]: %3")
                                   .arg(region.name, region.id, region.geometry.unsupportedReason);
        }
        result.regions << region;
    }

    for (MapRegion &region : result.regions) {
        if (names.value(region.name) > 1)
            region.name += QStringLiteral(" [Region #%1]").arg(region.id);
    }
    result.success = true;
    result.complete = result.errors.isEmpty()
        && std::all_of(result.regions.cbegin(), result.regions.cend(), [](const MapRegion &region) {
               return region.geometry.supported;
           });
    return result;
}

QString regionShapeName(RegionShapeKind kind)
{
    switch (kind) {
    case RegionShapeKind::Circle: return QStringLiteral("circle");
    case RegionShapeKind::Rectangle: return QStringLiteral("rectangle");
    case RegionShapeKind::Polygon: return QStringLiteral("polygon");
    case RegionShapeKind::Composite: return QStringLiteral("composite");
    case RegionShapeKind::Unknown: return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

} // namespace sc2dh::region
