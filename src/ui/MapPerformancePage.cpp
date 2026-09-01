#include "ui/MapPerformancePage.h"

#include "core/DecorationMapCopyService.h"
#include "core/ScannedFileReader.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <limits>

namespace
{

constexpr qint64 MaxObjectsBytes = 64ll * 1024ll * 1024ll;

enum DecorDoodadColumns {
    DecorDoodadExcludeColumn = 0,
    DecorDoodadForcedZoneColumn,
    DecorDoodadNameColumn,
    DecorDoodadIdColumn,
    DecorDoodadTypeColumn,
    DecorDoodadPositionColumn,
    DecorDoodadStateColumn
};

class NumericItem : public QStandardItem
{
public:
    explicit NumericItem(double value, const QString &label)
        : QStandardItem(label)
    {
        setData(value, Qt::UserRole + 2);
    }

    bool operator<(const QStandardItem &other) const override
    {
        return data(Qt::UserRole + 2).toDouble() < other.data(Qt::UserRole + 2).toDouble();
    }
};

QString formatBytes(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    const double kib = double(bytes) / 1024.0;
    if (kib < 1024.0)
        return QStringLiteral("%1 KiB").arg(kib, 0, 'f', 1);
    const double mib = kib / 1024.0;
    return QStringLiteral("%1 MiB").arg(mib, 0, 'f', 2);
}

QString formatScore(double score)
{
    return QStringLiteral("%1").arg(score, 0, 'f', 1);
}

QString boundsLabel(const sc2dh::perf::MapPerformanceCell &cell)
{
    return QStringLiteral("x %1..%2, y %3..%4")
        .arg(cell.xMin, 0, 'f', 1)
        .arg(cell.xMax, 0, 'f', 1)
        .arg(cell.yMin, 0, 'f', 1)
        .arg(cell.yMax, 0, 'f', 1);
}

QString regionGeometryLabel(const sc2dh::region::RegionGeometry &geometry)
{
    const auto translate = [](const char *source) {
        return QCoreApplication::translate("MapPerformancePage", source);
    };
    switch (geometry.kind) {
    case sc2dh::region::RegionShapeKind::Circle:
        return translate("circle | center %1, %2 | radius %3")
            .arg(geometry.center.x, 0, 'f', 2)
            .arg(geometry.center.y, 0, 'f', 2)
            .arg(geometry.radius, 0, 'f', 2);
    case sc2dh::region::RegionShapeKind::Rectangle:
        return translate("rectangle | x %1..%2 | y %3..%4")
            .arg(geometry.bounds.xMin, 0, 'f', 2)
            .arg(geometry.bounds.xMax, 0, 'f', 2)
            .arg(geometry.bounds.yMin, 0, 'f', 2)
            .arg(geometry.bounds.yMax, 0, 'f', 2);
    case sc2dh::region::RegionShapeKind::Polygon:
        return translate("polygon | %1 points | x %2..%3 | y %4..%5")
            .arg(geometry.points.size())
            .arg(geometry.bounds.xMin, 0, 'f', 2)
            .arg(geometry.bounds.xMax, 0, 'f', 2)
            .arg(geometry.bounds.yMin, 0, 'f', 2)
            .arg(geometry.bounds.yMax, 0, 'f', 2);
    case sc2dh::region::RegionShapeKind::Composite:
        return translate("composite | %1 shapes | bounds x %2..%3 | y %4..%5")
            .arg(geometry.components.size())
            .arg(geometry.bounds.xMin, 0, 'f', 2)
            .arg(geometry.bounds.xMax, 0, 'f', 2)
            .arg(geometry.bounds.yMin, 0, 'f', 2)
            .arg(geometry.bounds.yMax, 0, 'f', 2);
    case sc2dh::region::RegionShapeKind::Unknown:
        return translate("unsupported shape: %1").arg(geometry.rawType);
    }
    return translate("unknown");
}

QString doodadOverrideKey(QString value)
{
    return value.trimmed().toCaseFolded();
}

void addDoodadOverrideKeys(sc2dh::decor::DecorationSafetyContext *context,
                           const QString &id,
                           const QString &name,
                           bool excluded,
                           int forcedZone)
{
    if (!context)
        return;
    const QString idKey = doodadOverrideKey(id);
    const QString nameKey = doodadOverrideKey(name);
    const auto addExcluded = [&](const QString &key) {
        if (!key.isEmpty())
            context->excludedDoodadKeys.insert(key);
    };
    const auto addForced = [&](const QString &key) {
        if (!key.isEmpty() && forcedZone > 0)
            context->forcedZoneByDoodadKey.insert(key, forcedZone);
    };
    if (excluded) {
        addExcluded(idKey);
        addExcluded(nameKey);
    }
    addForced(idKey);
    addForced(nameKey);
}

QColor riskColor(double score)
{
    if (score >= 70.0)
        return QColor(QStringLiteral("#5a2320"));
    if (score >= 40.0)
        return QColor(QStringLiteral("#4c3d20"));
    if (score > 0.0)
        return QColor(QStringLiteral("#213d32"));
    return QColor(QStringLiteral("#1f2937"));
}

class MapHeatmapWidget : public QWidget
{
public:
    explicit MapHeatmapWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(360);
        setMouseTracking(true);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
    }

    void setReport(const sc2dh::perf::MapPerformanceReport &report)
    {
        m_report = report;
        m_selectedCell = -1;
        invalidateStaticLayer();
        update();
    }

    void setDecorZones(const QVector<sc2dh::decor::DecorZone> &zones)
    {
        m_zones = zones;
        update();
    }

    void setRegions(const QVector<sc2dh::region::MapRegion> &regions)
    {
        m_regions = regions;
        invalidateStaticLayer();
        update();
    }

    void setSelectedRegionIndices(const QSet<int> &indices)
    {
        if (m_selectedRegions == indices)
            return;
        m_selectedRegions = indices;
        update();
    }

    void setRegionSelectionEnabled(bool enabled)
    {
        m_regionSelectionEnabled = enabled;
        setCursor(enabled ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }

    void setBackgroundImage(const QImage &image, const QString &sourceLabel)
    {
        m_backgroundImage = image;
        m_backgroundSourceLabel = sourceLabel;
        invalidateStaticLayer();
        update();
    }

    void setSelectedCellIndex(int cellIndex)
    {
        if (m_selectedCell == cellIndex)
            return;
        m_selectedCell = cellIndex;
        update();
    }

    std::function<void(int)> cellClicked;
    std::function<void(int)> regionClicked;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        ensureStaticLayer();
        QPainter painter(this);
        painter.drawPixmap(0, 0, m_staticLayer);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF area = gridArea();
        const sc2dh::region::RegionBounds bounds = displayBounds();
        for (int index = 0; index < m_regions.size(); ++index) {
            const sc2dh::region::MapRegion &region = m_regions.at(index);
            if (!region.geometry.supported)
                continue;
            const QPainterPath path = geometryPath(region.geometry, area, bounds);
            const bool selected = m_selectedRegions.contains(index);
            const bool hovered = index == m_hoveredRegion;
            QColor outline = selected ? QColor(QStringLiteral("#ffca6b"))
                                      : hovered ? QColor(QStringLiteral("#74ffe1"))
                                                : QColor(71, 219, 194, 115);
            painter.setPen(QPen(outline, selected ? 3.0 : hovered ? 2.4 : 1.15,
                                selected ? Qt::SolidLine : Qt::DashLine,
                                Qt::RoundCap,
                                Qt::RoundJoin));
            painter.setBrush(selected ? QColor(255, 160, 54, 72)
                                      : hovered ? QColor(68, 255, 210, 42) : Qt::NoBrush);
            painter.drawPath(path);
            if (selected || hovered) {
                const QRectF labelRect = path.boundingRect().adjusted(5, 4, -5, -4);
                painter.setPen(selected ? QColor(QStringLiteral("#fff1c9"))
                                        : QColor(QStringLiteral("#d9fff6")));
                painter.drawText(labelRect, Qt::AlignLeft | Qt::AlignTop,
                                 region.name.isEmpty() ? QStringLiteral("Region #%1").arg(region.id)
                                                       : region.name);
            }
        }

        if (m_selectedCell >= 0 && m_selectedCell < m_report.cells.size()
            && m_report.columns > 0 && m_report.rows > 0) {
            const sc2dh::perf::MapPerformanceCell &cell = m_report.cells.at(m_selectedCell);
            const auto bounds = displayBounds();
            const QRectF selectedRect(mapToWidget(cell.xMin, cell.yMin, area, bounds),
                                      mapToWidget(cell.xMax, cell.yMax, area, bounds));
            painter.setPen(QPen(QColor(QStringLiteral("#ffffff")), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(selectedRect.normalized().adjusted(1, 1, -1, -1));
        }

        painter.setPen(QColor(QStringLiteral("#91a3b7")));
        painter.drawText(QRect(12, height() - 24, width() - 24, 18),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         m_regionSelectionEnabled
                             ? QStringLiteral("Click an exact Region outline to select it. Doodads blue, units green, destructibles orange.")
                             : QStringLiteral("Region selection paused."));
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (m_regionSelectionEnabled) {
            const int regionIndex = regionIndexAt(event->position());
            if (regionIndex >= 0) {
                if (regionClicked)
                    regionClicked(regionIndex);
                event->accept();
                return;
            }
            event->accept();
            return;
        }
        const int index = cellIndexAt(event->position());
        if (index < 0)
            return;
        m_selectedCell = index;
        update();
        if (cellClicked)
            cellClicked(index);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int hovered = m_regionSelectionEnabled ? regionIndexAt(event->position()) : -1;
        if (hovered != m_hoveredRegion) {
            m_hoveredRegion = hovered;
            if (hovered >= 0 && hovered < m_regions.size()) {
                const auto &region = m_regions.at(hovered);
                setToolTip(QStringLiteral("%1 [#%2]\n%3\nClick to select this exact area.")
                               .arg(region.name, region.id, regionGeometryLabel(region.geometry)));
            } else {
                setToolTip(QString());
            }
            update();
        }
        QWidget::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        m_hoveredRegion = -1;
        update();
        QWidget::leaveEvent(event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        invalidateStaticLayer();
        QWidget::resizeEvent(event);
    }

private:
    QRectF gridArea() const
    {
        return QRectF(12.0, 40.0, double(width() - 24), double(std::max(80, height() - 72)));
    }

    sc2dh::region::RegionBounds displayBounds() const
    {
        sc2dh::region::RegionBounds bounds;
        const auto include = [&bounds](const sc2dh::region::RegionBounds &candidate) {
            if (!candidate.valid)
                return;
            if (!bounds.valid) {
                bounds = candidate;
                return;
            }
            bounds.xMin = std::min(bounds.xMin, candidate.xMin);
            bounds.yMin = std::min(bounds.yMin, candidate.yMin);
            bounds.xMax = std::max(bounds.xMax, candidate.xMax);
            bounds.yMax = std::max(bounds.yMax, candidate.yMax);
        };
        if (!m_report.placements.isEmpty())
            include({m_report.xMin, m_report.yMin, m_report.xMax, m_report.yMax, true});
        for (const auto &region : m_regions)
            if (region.geometry.supported)
                include(region.geometry.bounds);
        if (!bounds.valid || bounds.xMin >= bounds.xMax || bounds.yMin >= bounds.yMax)
            return {0.0, 0.0, 1.0, 1.0, true};
        return bounds;
    }

    QPointF mapToWidget(double x, double y, const QRectF &area,
                        const sc2dh::region::RegionBounds &bounds) const
    {
        const double xSpan = std::max(0.0001, bounds.xMax - bounds.xMin);
        const double ySpan = std::max(0.0001, bounds.yMax - bounds.yMin);
        return {area.left() + ((x - bounds.xMin) / xSpan) * area.width(),
                area.bottom() - ((y - bounds.yMin) / ySpan) * area.height()};
    }

    QPointF widgetToMap(const QPointF &point) const
    {
        const QRectF area = gridArea();
        const auto bounds = displayBounds();
        return {bounds.xMin + ((point.x() - area.left()) / area.width()) * (bounds.xMax - bounds.xMin),
                bounds.yMin + ((area.bottom() - point.y()) / area.height()) * (bounds.yMax - bounds.yMin)};
    }

    QPainterPath geometryPath(const sc2dh::region::RegionGeometry &geometry,
                              const QRectF &area,
                              const sc2dh::region::RegionBounds &bounds) const
    {
        QPainterPath path;
        path.setFillRule(Qt::WindingFill);
        switch (geometry.kind) {
        case sc2dh::region::RegionShapeKind::Circle: {
            const QPointF center = mapToWidget(geometry.center.x, geometry.center.y, area, bounds);
            const QPointF radiusPoint = mapToWidget(geometry.center.x + geometry.radius,
                                                    geometry.center.y + geometry.radius,
                                                    area,
                                                    bounds);
            path.addEllipse(center, qAbs(radiusPoint.x() - center.x()), qAbs(radiusPoint.y() - center.y()));
            break;
        }
        case sc2dh::region::RegionShapeKind::Rectangle: {
            const QPointF first = mapToWidget(geometry.bounds.xMin, geometry.bounds.yMin, area, bounds);
            const QPointF second = mapToWidget(geometry.bounds.xMax, geometry.bounds.yMax, area, bounds);
            path.addRect(QRectF(first, second).normalized());
            break;
        }
        case sc2dh::region::RegionShapeKind::Polygon:
            if (!geometry.points.isEmpty()) {
                path.moveTo(mapToWidget(geometry.points.first().x, geometry.points.first().y, area, bounds));
                for (int index = 1; index < geometry.points.size(); ++index)
                    path.lineTo(mapToWidget(geometry.points.at(index).x, geometry.points.at(index).y, area, bounds));
                path.closeSubpath();
            }
            break;
        case sc2dh::region::RegionShapeKind::Composite:
            for (const auto &component : geometry.components)
                path.addPath(geometryPath(component, area, bounds));
            break;
        case sc2dh::region::RegionShapeKind::Unknown:
            break;
        }
        return path;
    }

    int regionIndexAt(const QPointF &point) const
    {
        const QRectF area = gridArea();
        if (!area.contains(point))
            return -1;
        const QPointF mapPoint = widgetToMap(point);
        const auto bounds = displayBounds();
        const double tolerance = 3.0 * std::max((bounds.xMax - bounds.xMin) / std::max(1.0, area.width()),
                                                (bounds.yMax - bounds.yMin) / std::max(1.0, area.height()));
        int bestIndex = -1;
        double bestArea = std::numeric_limits<double>::max();
        for (int index = 0; index < m_regions.size(); ++index) {
            const auto &geometry = m_regions.at(index).geometry;
            if (!geometry.supported
                || geometry.classify(mapPoint.x(), mapPoint.y(), tolerance) == sc2dh::region::SpatialRelation::Outside)
                continue;
            const double areaHint = std::max(0.0001,
                                             (geometry.bounds.xMax - geometry.bounds.xMin)
                                                 * (geometry.bounds.yMax - geometry.bounds.yMin));
            if (areaHint < bestArea) {
                bestArea = areaHint;
                bestIndex = index;
            }
        }
        return bestIndex;
    }

    void invalidateStaticLayer()
    {
        m_staticLayer = QPixmap();
    }

    void ensureStaticLayer()
    {
        if (!m_staticLayer.isNull() && m_staticLayer.size() == size())
            return;
        m_staticLayer = QPixmap(size());
        m_staticLayer.fill(QColor(QStringLiteral("#03080c")));
        QPainter painter(&m_staticLayer);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        painter.fillRect(rect(), QColor(3, 8, 12));
        if (!m_gridTexture.isNull()) {
            painter.setOpacity(0.18);
            painter.drawTiledPixmap(rect(), m_gridTexture);
        }
        if (!m_pointTexture.isNull()) {
            painter.setOpacity(0.07);
            painter.drawTiledPixmap(rect(), m_pointTexture);
        }
        if (!m_lightTexture.isNull()) {
            painter.setOpacity(0.14);
            painter.drawPixmap(rect(), m_lightTexture, m_lightTexture.rect());
        }
        if (!m_scanlineTexture.isNull()) {
            painter.setOpacity(0.09);
            painter.drawPixmap(rect(), m_scanlineTexture, m_scanlineTexture.rect());
        }
        painter.setOpacity(1.0);
        painter.setPen(QColor(QStringLiteral("#d9f7ee")));
        painter.drawText(QRect(12, 8, width() - 24, 24), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("MAP REGIONS — exact geometry selection"));

        const QRectF area = gridArea();
        if (!m_backgroundImage.isNull()) {
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawImage(area, m_backgroundImage);
            painter.fillRect(area, QColor(3, 9, 14, 118));
        } else {
            painter.fillRect(area, QColor(4, 16, 20, 178));
        }
        painter.setPen(QPen(QColor(74, 220, 195, 76), 1));
        painter.drawRect(area);

        if (m_report.cells.isEmpty() || m_report.columns <= 0 || m_report.rows <= 0) {
            painter.setPen(QColor(QStringLiteral("#91a3b7")));
            painter.drawText(area.adjusted(12, 12, -12, -12), Qt::AlignCenter,
                             QStringLiteral("No positioned decor was found. Regions can still be inspected."));
            return;
        }

        const auto bounds = displayBounds();
        for (const auto &cell : m_report.cells) {
            const QRectF cellRect = QRectF(mapToWidget(cell.xMin, cell.yMin, area, bounds),
                                           mapToWidget(cell.xMax, cell.yMax, area, bounds)).normalized();
            QColor fill = riskColor(cell.combinedRiskScore);
            fill.setAlpha(86);
            painter.fillRect(cellRect.adjusted(1, 1, -1, -1), fill);
            painter.setPen(QPen(QColor(65, 141, 139, 52), 1));
            painter.drawRect(cellRect);
        }
        painter.setRenderHint(QPainter::Antialiasing, false);
        for (const auto &placement : m_report.placements) {
            const QPointF position = mapToWidget(placement.x, placement.y, area, bounds);
            const QString kind = placement.kind.toLower();
            QColor color(QStringLiteral("#6dc9ff"));
            if (kind.contains(QStringLiteral("unit")))
                color = QColor(QStringLiteral("#a9ff75"));
            else if (kind.contains(QStringLiteral("destruct")))
                color = QColor(QStringLiteral("#ffc467"));
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawEllipse(position, 2.2, 2.2);
        }
    }

    int cellIndexAt(const QPointF &point) const
    {
        if (m_report.cells.isEmpty() || m_report.columns <= 0 || m_report.rows <= 0)
            return -1;
        const QRectF area = gridArea();
        if (!area.contains(point))
            return -1;
        const int column = std::clamp(int((point.x() - area.left()) / area.width() * m_report.columns),
                                      0,
                                      m_report.columns - 1);
        const int visualRow = std::clamp(int((point.y() - area.top()) / area.height() * m_report.rows),
                                         0,
                                         m_report.rows - 1);
        const int row = m_report.rows - 1 - visualRow;
        return row * m_report.columns + column;
    }

    sc2dh::perf::MapPerformanceReport m_report;
    QVector<sc2dh::decor::DecorZone> m_zones;
    QVector<sc2dh::region::MapRegion> m_regions;
    QSet<int> m_selectedRegions;
    QImage m_backgroundImage;
    QString m_backgroundSourceLabel;
    QPixmap m_staticLayer;
    QPixmap m_gridTexture{QStringLiteral(":/textures/ui_nova_storymode_bggrid_shimmer_sideways.png")};
    QPixmap m_pointTexture{QStringLiteral(":/textures/ui_nova_storymode_bgpointgrid_25.png")};
    QPixmap m_lightTexture{QStringLiteral(":/textures/ui_nova_login_backgroundlights.png")};
    QPixmap m_scanlineTexture{QStringLiteral(":/textures/ui_nova_archives_backgroundframe_scanlines.png")};
    int m_selectedCell = -1;
    int m_hoveredRegion = -1;
    bool m_regionSelectionEnabled = true;
};

class RegionListWidget final : public QListWidget
{
public:
    using QListWidget::QListWidget;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        QListWidgetItem *clicked = itemAt(event->position().toPoint());
        if (clicked && clicked->flags().testFlag(Qt::ItemIsEnabled)
            && clicked->flags().testFlag(Qt::ItemIsUserCheckable)) {
            setCurrentItem(clicked);
            clicked->setCheckState(clicked->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
            event->accept();
            return;
        }
        QListWidget::mousePressEvent(event);
    }
};

QList<QStandardItem *> makeRow(const sc2dh::perf::MapPerformanceCell &cell, int cellIndex)
{
    const QString cellName = QStringLiteral("%1,%2").arg(cell.column + 1).arg(cell.row + 1);
    QList<QStandardItem *> row = {
        new QStandardItem(cellName),
        new QStandardItem(boundsLabel(cell)),
        new QStandardItem(QString::number(cell.doodadCount)),
        new QStandardItem(QString::number(cell.unitCount + cell.destructibleCount)),
        new QStandardItem(QString::number(cell.uniqueActorModelCount)),
        new QStandardItem(formatBytes(cell.linkedAssetBytes)),
        new QStandardItem(QString::number(cell.periodicTimerCount)),
        new QStandardItem(QString::number(cell.unitGroupScanCount)),
        new NumericItem(cell.combinedRiskScore, formatScore(cell.combinedRiskScore)),
        new QStandardItem(cell.reasons.join(QStringLiteral(" | ")))
    };

    const QColor background = riskColor(cell.combinedRiskScore);
    const QColor foreground = cell.combinedRiskScore >= 40.0
                                  ? QColor(QStringLiteral("#fff4dc"))
                                  : QColor(QStringLiteral("#d9f7ee"));
    for (QStandardItem *item : row) {
        item->setEditable(false);
        item->setBackground(background);
        item->setForeground(foreground);
        item->setData(cellIndex, Qt::UserRole + 1);
    }
    return row;
}

QStringList compactList(QStringList values, int maxItems)
{
    values.removeAll(QString());
    values.removeDuplicates();
    std::sort(values.begin(), values.end(), [](const QString &left, const QString &right) {
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
    if (values.size() <= maxItems)
        return values;
    const int extra = values.size() - maxItems;
    values = values.mid(0, maxItems);
    values << QStringLiteral("... and %1 more").arg(extra);
    return values;
}

QString section(const QString &title, const QStringList &values, int maxItems = 40)
{
    QString output = title + QStringLiteral("\n");
    const QStringList compact = compactList(values, maxItems);
    if (compact.isEmpty()) {
        output += QStringLiteral("  - none\n");
    } else {
        for (const QString &value : compact)
            output += QStringLiteral("  - %1\n").arg(value);
    }
    return output;
}

QString decorZoneName(const QVector<sc2dh::decor::DecorZone> &zones, int zoneId)
{
    for (const sc2dh::decor::DecorZone &zone : zones) {
        if (zone.id == zoneId)
            return zone.name.isEmpty() ? QStringLiteral("Zone %1").arg(zoneId) : zone.name;
    }
    return QStringLiteral("Zone %1").arg(zoneId);
}

QString decorDoodadLabel(const sc2dh::decor::DoodadPlacement &doodad)
{
    const QString name = doodad.name.isEmpty()
        ? (doodad.id.isEmpty() ? doodad.type : doodad.id)
        : doodad.name;
    return QStringLiteral("%1 | id=%2 | type=%3 | pos=%4,%5")
        .arg(name.isEmpty() ? QStringLiteral("<unnamed>") : name)
        .arg(doodad.id.isEmpty() ? QStringLiteral("-") : doodad.id)
        .arg(doodad.type.isEmpty() ? QStringLiteral("-") : doodad.type)
        .arg(doodad.x, 0, 'f', 2)
        .arg(doodad.y, 0, 'f', 2);
}

QString decorationPreviewText(const sc2dh::decor::DecorationOptimizedArtifacts &artifacts,
                              const QVector<sc2dh::decor::DecorZone> &zones,
                              const sc2dh::decor::GalaxyGenerationOptions &options)
{
    const sc2dh::decor::DecorationStreamingPlan &plan = artifacts.plan;
    QString text;
    text += QStringLiteral("DECORATION STREAMING PREVIEW / 3.0 BETA\n");
    text += QStringLiteral("Function prefix: %1\n").arg(options.functionPrefix);
    text += QStringLiteral("Batch per game tick: %1\n\n").arg(options.batchLimit);
    text += QStringLiteral("How to use in Galaxy:\n");
    text += QStringLiteral("  - Create/load zone: %1_Create_1(); or DecorOpt_CreateZone(1);\n").arg(options.functionPrefix);
    text += QStringLiteral("  - Clear created actors from zone: %1_Clear_1(); or DecorOpt_ClearZone(1);\n").arg(options.functionPrefix);
    text += QStringLiteral("  - Create all zones: %1_CreateAll(); or DecorOpt_CreateAll();\n").arg(options.functionPrefix);
    text += QStringLiteral("  - Clear all created dynamic actors: %1_ClearAll(); or DecorOpt_ClearAll();\n").arg(options.functionPrefix);
    text += QStringLiteral("  - Check loaded state: DecorOpt_IsZoneLoaded(1);\n\n");
    text += QStringLiteral("Important:\n");
    text += QStringLiteral("  - Create Decor-Optimized Map Copy removes listed dynamic visual doodads from Objects in the COPY only.\n");
    text += QStringLiteral("  - Original map is not modified.\n");
    text += QStringLiteral("  - Static-only doodads stay in Objects.\n\n");

    int assigned = 0;
    for (const sc2dh::decor::ZoneAssignment &zone : plan.zones)
        assigned += zone.doodadIndices.size();
    text += QStringLiteral("Summary: %1 doodad(s), %2 will be moved to dynamic Galaxy actors, %3 static-only, %4 outside scope, %5 boundary/ambiguous.\n")
                .arg(plan.doodads.size())
                .arg(assigned)
                .arg(plan.staticOnlyDoodads.size())
                .arg(plan.unassignedDoodads.size())
                .arg(plan.boundaryDoodads.size());
    text += QStringLiteral("Round-trip proof: %1 | Outside-scope preservation: %2\n\n")
                .arg(artifacts.roundTripVerified ? QStringLiteral("PASS") : QStringLiteral("BLOCKED"),
                     artifacts.outsideScopePreserved ? QStringLiteral("PASS") : QStringLiteral("BLOCKED"));

    text += QStringLiteral("ZONES / FUNCTIONS / DECOR THAT WILL BE REMOVED FROM OBJECTS\n");
    for (const sc2dh::decor::ZoneAssignment &zone : plan.zones) {
        text += QStringLiteral("\n[%1] %2\n").arg(zone.zoneId).arg(decorZoneName(zones, zone.zoneId));
        text += QStringLiteral("Function: %1_%2()\n").arg(options.functionPrefix).arg(zone.zoneId);
        text += QStringLiteral("API call: DecorOpt_CreateZone(%1)\n").arg(zone.zoneId);
        text += QStringLiteral("Clear call: DecorOpt_ClearZone(%1)\n").arg(zone.zoneId);
        if (zone.doodadIndices.isEmpty()) {
            text += QStringLiteral("  - no dynamic doodads assigned\n");
            continue;
        }
        for (int doodadIndex : zone.doodadIndices) {
            if (doodadIndex >= 0 && doodadIndex < plan.doodads.size())
                text += QStringLiteral("  - DELETE FROM OBJECTS COPY + recreate as actor: %1\n")
                            .arg(decorDoodadLabel(plan.doodads.at(doodadIndex)));
        }
    }

    text += QStringLiteral("\nSTATIC-ONLY / WILL STAY IN OBJECTS\n");
    if (plan.staticOnlyDoodads.isEmpty()) {
        text += QStringLiteral("  - none\n");
    } else {
        for (int doodadIndex : plan.staticOnlyDoodads) {
            if (doodadIndex < 0 || doodadIndex >= plan.doodads.size())
                continue;
            const sc2dh::decor::DoodadPlacement &doodad = plan.doodads.at(doodadIndex);
            text += QStringLiteral("  - KEEP STATIC: %1 | reason: %2\n")
                        .arg(decorDoodadLabel(doodad),
                             doodad.staticOnlyReason.isEmpty() ? QStringLiteral("Static-only") : doodad.staticOnlyReason);
        }
    }

    text += QStringLiteral("\nUNASSIGNED DYNAMIC DOODADS / NOT DELETED UNTIL ASSIGNED\n");
    if (plan.unassignedDoodads.isEmpty()) {
        text += QStringLiteral("  - none\n");
    } else {
        for (int doodadIndex : plan.unassignedDoodads) {
            if (doodadIndex >= 0 && doodadIndex < plan.doodads.size())
                text += QStringLiteral("  - %1\n").arg(decorDoodadLabel(plan.doodads.at(doodadIndex)));
        }
    }

    if (!artifacts.warnings.isEmpty()) {
        text += QStringLiteral("\nWARNINGS\n");
        for (const QString &warning : artifacts.warnings)
            text += QStringLiteral("  - %1\n").arg(warning);
    }

    text += QStringLiteral("\n================ GENERATED GALAXY SCRIPT ================\n\n");
    text += artifacts.galaxySource;
    return text;
}

bool isObjectsEntry(const QString &rootFolder, const ScannedFileInfo &file)
{
    const QString relative = ScannedFileReader::relativePath(rootFolder, file.filePath);
    const QString normalized = relative.trimmed().replace('\\', '/');
    if (normalized.compare(QStringLiteral("Objects"), Qt::CaseInsensitive) == 0)
        return true;
    if (normalized.endsWith(QStringLiteral("/Objects"), Qt::CaseInsensitive))
        return true;
    return QFileInfo(file.filePath).fileName().compare(QStringLiteral("Objects"), Qt::CaseInsensitive) == 0;
}

bool isRegionsEntry(const QString &rootFolder, const ScannedFileInfo &file)
{
    const QString relative = ScannedFileReader::relativePath(rootFolder, file.filePath);
    const QString normalized = relative.trimmed().replace('\\', '/');
    return normalized.compare(QStringLiteral("Regions"), Qt::CaseInsensitive) == 0
        || normalized.endsWith(QStringLiteral("/Regions"), Qt::CaseInsensitive)
        || QFileInfo(file.filePath).fileName().compare(QStringLiteral("Regions"), Qt::CaseInsensitive) == 0;
}

} // namespace

MapPerformancePage::MapPerformancePage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto *header = new QFrame(this);
    header->setObjectName(QStringLiteral("bucketCard"));
    auto *headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(12, 12, 12, 12);
    headerLayout->setSpacing(6);

    auto *title = new QLabel(tr("Map Performance — Region Decor Streaming"), header);
    title->setObjectName(QStringLiteral("panelTitle"));
    headerLayout->addWidget(title);

    m_summaryLabel = new QLabel(tr("Choose a real map Region. Decor inside its exact shape can be removed from Objects in a copy and recreated by generated Galaxy actor functions."), header);
    m_summaryLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    m_summaryLabel->setWordWrap(true);
    headerLayout->addWidget(m_summaryLabel);

    m_warningLabel = new QLabel(tr("The original map is never modified. Region geometry comes directly from the map's Regions component."), header);
    m_warningLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    m_warningLabel->setWordWrap(true);
    headerLayout->addWidget(m_warningLabel);
    layout->addWidget(header);

    m_heatmap = new MapHeatmapWidget(this);
    m_heatmap->setObjectName(QStringLiteral("mapPerformanceHeatmap"));
    static_cast<MapHeatmapWidget *>(m_heatmap)->cellClicked = [this](int cellIndex) {
        selectCellIndex(cellIndex);
    };
    static_cast<MapHeatmapWidget *>(m_heatmap)->regionClicked = [this](int regionIndex) {
        toggleRegionFromMap(regionIndex);
    };
    layout->addWidget(m_heatmap, 1);

    auto *decorGroup = new QGroupBox(QStringLiteral("Decoration Streaming / Оптимизация декорациями"), this);
    decorGroup->setObjectName(QStringLiteral("mapDecorStreamingGroup"));
    auto *decorLayout = new QVBoxLayout(decorGroup);
    decorLayout->setContentsMargins(10, 10, 10, 10);
    decorLayout->setSpacing(8);

    m_decorSummaryLabel = new QLabel(tr("Select one or more exact Regions on the map or in the list. The candidate count updates immediately; generation happens only when you press Preview."), decorGroup);
    m_decorSummaryLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    m_decorSummaryLabel->setWordWrap(true);
    decorLayout->addWidget(m_decorSummaryLabel);

    auto *scopeGroup = new QGroupBox(tr("1. Choose real map Regions"), decorGroup);
    auto *scopeLayout = new QVBoxLayout(scopeGroup);
    m_chooseRegionsButton = new QPushButton(tr("MAP SELECTION ACTIVE — CLICK AN OUTLINED REGION ABOVE"), scopeGroup);
    m_chooseRegionsButton->setObjectName(QStringLiteral("mapRegionPickerButton"));
    m_chooseRegionsButton->setCheckable(true);
    m_chooseRegionsButton->setChecked(true);
    m_chooseRegionsButton->setMinimumHeight(54);
    m_chooseRegionsButton->setToolTip(tr("When active, clicking an exact Region shape on the map toggles that Region."));
    scopeLayout->addWidget(m_chooseRegionsButton);

    m_regionStatusLabel = new QLabel(tr("No Regions component loaded."), scopeGroup);
    m_regionStatusLabel->setWordWrap(true);
    scopeLayout->addWidget(m_regionStatusLabel);
    m_regionList = new RegionListWidget(scopeGroup);
    m_regionList->setSelectionMode(QAbstractItemView::NoSelection);
    m_regionList->setMinimumHeight(150);
    m_regionList->setToolTip(tr("Click anywhere on a row to toggle the Region. Exact parameters are shown in each row."));
    scopeLayout->addWidget(m_regionList);
    auto *regionButtons = new QHBoxLayout();
    auto *selectAllRegions = new QPushButton(tr("Select All"), scopeGroup);
    auto *clearRegions = new QPushButton(tr("Clear"), scopeGroup);
    regionButtons->addWidget(selectAllRegions);
    regionButtons->addWidget(clearRegions);
    regionButtons->addStretch(1);
    scopeLayout->addLayout(regionButtons);

    decorLayout->addWidget(scopeGroup);

    auto *decorControls = new QHBoxLayout();
    m_prefixEdit = new QLineEdit(QStringLiteral("NAME_OUT_FUNK"), decorGroup);
    m_prefixEdit->setMinimumWidth(150);
    m_batchSpin = new QSpinBox(decorGroup);
    m_batchSpin->setRange(1, 4096);
    m_batchSpin->setValue(64);

    auto *form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(6);
    form->addRow(tr("Galaxy function prefix"), m_prefixEdit);
    form->addRow(tr("Actors created per game tick"), m_batchSpin);
    decorControls->addLayout(form, 1);

    auto *buttonLayout = new QVBoxLayout();
    m_previewButton = new QPushButton(tr("2. PREVIEW REMOVED DECOR + GALAXY"), decorGroup);
    m_createCopyButton = new QPushButton(tr("3. CREATE OPTIMIZED MAP COPY"), decorGroup);
    m_previewButton->setMinimumHeight(44);
    m_createCopyButton->setMinimumHeight(44);
    m_previewButton->setToolTip(tr("Calculate the exact decor removed from Objects and show the generated Galaxy functions."));
    m_createCopyButton->setToolTip(tr("Create a new map archive. The original stays unchanged; selected safe visual doodads are removed only from the copy."));
    buttonLayout->addWidget(m_previewButton);
    buttonLayout->addWidget(m_createCopyButton);
    buttonLayout->addStretch(1);
    decorControls->addLayout(buttonLayout);
    decorLayout->addLayout(decorControls);

    m_zoneModel = new QStandardItemModel(this);
    m_zoneModel->setHorizontalHeaderLabels({
        QStringLiteral("Id"),
        QStringLiteral("Name"),
        QStringLiteral("X min"),
        QStringLiteral("Y min"),
        QStringLiteral("X max"),
        QStringLiteral("Y max")
    });
    m_zoneTable = new QTableView(decorGroup);
    m_zoneTable->setObjectName(QStringLiteral("decorZoneTable"));
    m_zoneTable->setModel(m_zoneModel);
    m_zoneTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_zoneTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_zoneTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_zoneTable->horizontalHeader()->setStretchLastSection(true);
    m_zoneTable->verticalHeader()->setVisible(false);
    m_zoneTable->setMinimumHeight(120);
    auto *selectedRegionsLabel = new QLabel(tr("Selected Region parameters (exact geometry is preserved; bounds are shown only as a readable summary):"), decorGroup);
    selectedRegionsLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    selectedRegionsLabel->setWordWrap(true);
    decorLayout->addWidget(selectedRegionsLabel);
    decorLayout->addWidget(m_zoneTable);

    auto *doodadLabel = new QLabel(tr("Preview result: only decor inside the selected exact Regions is listed. Check Keep Static to exclude an item; the Action column states what will be removed from the Objects copy."), decorGroup);
    doodadLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    doodadLabel->setWordWrap(true);
    decorLayout->addWidget(doodadLabel);

    m_doodadModel = new QStandardItemModel(this);
    m_doodadModel->setHorizontalHeaderLabels({
        QStringLiteral("Keep Static"),
        QStringLiteral("Zone Id"),
        QStringLiteral("Name"),
        QStringLiteral("Id"),
        QStringLiteral("Type"),
        QStringLiteral("Position"),
        QStringLiteral("Action")
    });
    m_doodadTable = new QTableView(decorGroup);
    m_doodadTable->setObjectName(QStringLiteral("decorDoodadAssignmentTable"));
    m_doodadTable->setModel(m_doodadModel);
    m_doodadTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_doodadTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_doodadTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_doodadTable->horizontalHeader()->setStretchLastSection(true);
    m_doodadTable->verticalHeader()->setVisible(false);
    m_doodadTable->setMinimumHeight(130);
    decorLayout->addWidget(m_doodadTable);

    m_galaxyPreview = new QPlainTextEdit(decorGroup);
    m_galaxyPreview->setObjectName(QStringLiteral("decorGalaxyPreview"));
    m_galaxyPreview->setReadOnly(true);
    m_galaxyPreview->setMinimumHeight(150);
    m_galaxyPreview->setPlaceholderText(QStringLiteral("Preview will show: zone function names, doodads removed from Objects in the optimized copy, clear functions, and generated Galaxy code."));
    decorLayout->addWidget(m_galaxyPreview);

    connect(m_previewButton, &QPushButton::clicked, this, &MapPerformancePage::updateDecorPreview);
    connect(m_createCopyButton, &QPushButton::clicked, this, &MapPerformancePage::createDecorOptimizedMapCopy);
    connect(m_doodadModel, &QStandardItemModel::itemChanged, this, &MapPerformancePage::updateDecorPreview);
    connect(m_regionList, &QListWidget::itemChanged, this, &MapPerformancePage::updateOptimizationScope);
    connect(m_chooseRegionsButton, &QPushButton::toggled, this, [this](bool enabled) {
        static_cast<MapHeatmapWidget *>(m_heatmap)->setRegionSelectionEnabled(enabled);
        m_chooseRegionsButton->setText(enabled
            ? tr("MAP SELECTION ACTIVE — CLICK AN OUTLINED REGION ABOVE")
            : tr("SELECT REGIONS ON MAP"));
    });
    connect(selectAllRegions, &QPushButton::clicked, this, [this] {
        const QSignalBlocker blocker(m_regionList);
        for (int row = 0; row < m_regionList->count(); ++row) {
            QListWidgetItem *item = m_regionList->item(row);
            if (item->flags().testFlag(Qt::ItemIsEnabled))
                item->setCheckState(Qt::Checked);
        }
        updateOptimizationScope();
    });
    connect(clearRegions, &QPushButton::clicked, this, [this] {
        const QSignalBlocker blocker(m_regionList);
        for (int row = 0; row < m_regionList->count(); ++row)
            m_regionList->item(row)->setCheckState(Qt::Unchecked);
        updateOptimizationScope();
    });
    const auto invalidatePreview = [this] {
        m_createCopyButton->setEnabled(false);
        m_galaxyPreview->setPlainText(tr("Generation settings changed. Press Preview to rebuild the exact removal list and Galaxy code."));
    };
    connect(m_prefixEdit, &QLineEdit::textChanged, this, invalidatePreview);
    connect(m_batchSpin, &QSpinBox::valueChanged, this, invalidatePreview);
    updateOptimizationScope();

    layout->addWidget(decorGroup, 2);

    m_model = new QStandardItemModel(this);
    m_model->setHorizontalHeaderLabels({
        QStringLiteral("Cell"),
        QStringLiteral("Bounds"),
        QStringLiteral("Doodads"),
        QStringLiteral("Units"),
        QStringLiteral("Actor/Model"),
        QStringLiteral("Asset Bytes"),
        QStringLiteral("Periodic Timers"),
        QStringLiteral("Unit/Region Scans"),
        QStringLiteral("Estimated Static Risk"),
        QStringLiteral("Reasons")
    });

    m_table = new QTableView(this);
    m_table->setObjectName(QStringLiteral("mapPerformanceTable"));
    m_table->setModel(m_model);
    m_table->setAlternatingRowColors(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setSortingEnabled(true);
    m_table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(30);
    m_table->hide();

    m_details = new QPlainTextEdit(this);
    m_details->setObjectName(QStringLiteral("mapPerformanceDetails"));
    m_details->setReadOnly(true);
    m_details->setMinimumHeight(190);
    m_details->setPlaceholderText(QStringLiteral("Select a grid cell to inspect exact static-risk reasons."));
    m_details->hide();

    connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        updateDetails();
    });
}

void MapPerformancePage::setAnalysisResult(const AnalysisResult &result)
{
    m_result = result;
    rebuild();
}

void MapPerformancePage::rebuild()
{
    QByteArray regionsBytes;
    QString regionsSource;
    if (readRegionsFile(&regionsBytes, &regionsSource))
        m_regionReadResult = sc2dh::region::MapRegionRepository().parse(regionsBytes, regionsSource);
    else {
        m_regionReadResult = {};
        m_regionReadResult.sourceLabel = QStringLiteral("Regions");
        m_regionReadResult.warnings << tr("The map has no readable Regions component.");
    }
    populateRegionSelector();

    if (readMinimapImage(&m_minimapImage, &m_minimapSourceLabel)) {
        static_cast<MapHeatmapWidget *>(m_heatmap)->setBackgroundImage(m_minimapImage, m_minimapSourceLabel);
    } else {
        m_minimapImage = {};
        m_minimapSourceLabel.clear();
        static_cast<MapHeatmapWidget *>(m_heatmap)->setBackgroundImage({}, {});
    }

    QByteArray objectsBytes;
    QString sourceLabel;
    if (!readObjectsFile(&objectsBytes, &sourceLabel)) {
        m_hasObjects = false;
        m_allDoodads.clear();
        m_objectsBytes.clear();
        m_objectsSourceLabel.clear();
        m_report = {};
        static_cast<MapHeatmapWidget *>(m_heatmap)->setReport(m_report);
        static_cast<MapHeatmapWidget *>(m_heatmap)->setDecorZones({});
        m_model->removeRows(0, m_model->rowCount());
        populateZoneTable({});
        populateDoodadTable({});
        m_summaryLabel->setText(tr("Objects placement file was not found/readable or is larger than %1. Open a map containing the root Objects file. Scanned files: %2.")
                                    .arg(formatBytes(MaxObjectsBytes))
                                    .arg(m_result.scannedFiles.size()));
        m_warningLabel->setText(m_minimapImage.isNull()
                                    ? tr("Region outlines can be inspected, but decor cannot be counted until Objects is readable.")
                                    : tr("Minimap loaded from %1, but Objects placement data is unavailable.").arg(m_minimapSourceLabel));
        m_decorSummaryLabel->setText(QStringLiteral("No readable Objects entry: cannot list decor, assign zones, generate exact actor script, or remove static doodads from a map copy yet."));
        m_galaxyPreview->setPlainText(QStringLiteral(
            "NO OBJECTS ENTRY LOADED\n\n"
            "This mode needs both the map's root Objects file and its real Regions component.\n\n"
            "Workflow:\n"
            "1. Open a .SC2Map/.SC2Mod archive.\n"
            "2. Click an exact Region on the map.\n"
            "3. Press Preview to inspect removed decor and Galaxy.\n"
            "4. Create an optimized copy; the source map stays unchanged.\n"));
        m_createCopyButton->setEnabled(false);
        m_details->clear();
        return;
    }
    m_hasObjects = true;
    m_objectsBytes = objectsBytes;
    m_objectsSourceLabel = sourceLabel;

    sc2dh::perf::MapPerformanceOptions options;
    options.columns = 8;
    options.rows = 8;
    options.padding = 0.0;
    m_report = sc2dh::perf::MapPerformanceAnalyzer().buildReport(objectsBytes, m_result, options);
    static_cast<MapHeatmapWidget *>(m_heatmap)->setReport(m_report);
    populateTable();

    m_summaryLabel->setText(tr("%1 positioned object(s) loaded. Choose exact map Regions to move safe decor from Objects into generated Galaxy actor functions. Source: %2%3")
                                .arg(m_report.placements.size())
                                .arg(sourceLabel)
                                .arg(m_minimapImage.isNull()
                                         ? QString()
                                         : tr(" | Minimap: %1").arg(m_minimapSourceLabel)));
    QString warning = tr("Region outlines use the map's exact circle, polygon, rectangle or composite geometry. The source archive is read-only.");
    if (m_minimapImage.isNull())
        warning += tr(" Minimap.tga was unavailable; using the Graph-style coordinate background.");
    if (!m_report.warnings.isEmpty())
        warning += QStringLiteral(" Warnings: %1").arg(m_report.warnings.join(QStringLiteral(" | ")));
    m_warningLabel->setText(warning);
    sc2dh::decor::DecorationStreamingPlanner decorPlanner;
    m_allDoodads = decorPlanner.parseObjects(m_objectsBytes);
    updateOptimizationScope();
}

bool MapPerformancePage::readObjectsFile(QByteArray *objectsBytes, QString *sourceLabel) const
{
    if (!objectsBytes)
        return false;
    ScannedFileReader reader(m_result);
    for (const ScannedFileInfo &file : m_result.scannedFiles) {
        if (!isObjectsEntry(m_result.rootFolder, file))
            continue;
        QByteArray bytes;
        if (!reader.readBytes(file, MaxObjectsBytes, &bytes))
            return false;
        *objectsBytes = bytes;
        if (sourceLabel)
            *sourceLabel = ScannedFileReader::relativePath(m_result.rootFolder, file.filePath);
        return true;
    }
    return false;
}

bool MapPerformancePage::readRegionsFile(QByteArray *regionsBytes, QString *sourceLabel) const
{
    if (!regionsBytes)
        return false;
    ScannedFileReader reader(m_result);
    for (const ScannedFileInfo &file : m_result.scannedFiles) {
        if (!isRegionsEntry(m_result.rootFolder, file))
            continue;
        QByteArray bytes;
        if (!reader.readBytes(file, MaxObjectsBytes, &bytes))
            return false;
        *regionsBytes = bytes;
        if (sourceLabel)
            *sourceLabel = ScannedFileReader::relativePath(m_result.rootFolder, file.filePath);
        return true;
    }
    return false;
}

bool MapPerformancePage::readMinimapImage(QImage *image, QString *sourceLabel) const
{
    if (!image)
        return false;
    *image = {};
    if (sourceLabel)
        sourceLabel->clear();

    ScannedFileReader reader(m_result);
    for (const ScannedFileInfo &file : m_result.scannedFiles) {
        const QString relative = ScannedFileReader::relativePath(m_result.rootFolder, file.filePath);
        if (QFileInfo(relative).fileName().compare(QStringLiteral("Minimap.tga"), Qt::CaseInsensitive) != 0)
            continue;

        QByteArray bytes;
        if (!reader.readBytes(file, 32ll * 1024ll * 1024ll, &bytes))
            continue;

        QImage loaded;
        if (!loaded.loadFromData(bytes, "TGA") && !loaded.loadFromData(bytes))
            continue;
        if (loaded.isNull())
            continue;

        *image = loaded;
        if (sourceLabel)
            *sourceLabel = relative;
        return true;
    }
    return false;
}

void MapPerformancePage::populateTable()
{
    m_model->removeRows(0, m_model->rowCount());
    for (int i = 0; i < m_report.cells.size(); ++i)
        m_model->appendRow(makeRow(m_report.cells.at(i), i));

    m_table->sortByColumn(8, Qt::DescendingOrder);
    m_table->resizeColumnsToContents();
    if (m_model->rowCount() > 0 && m_table->isVisible())
        m_table->selectRow(0);
}

void MapPerformancePage::updateDetails()
{
    const QModelIndex current = m_table->currentIndex();
    if (!current.isValid()) {
        m_details->clear();
        return;
    }
    const QModelIndex sourceIndex = m_model->index(current.row(), 0);
    const int cellIndex = sourceIndex.data(Qt::UserRole + 1).toInt();
    if (cellIndex < 0 || cellIndex >= m_report.cells.size()) {
        m_details->clear();
        return;
    }
    m_details->setPlainText(detailTextForCell(m_report.cells.at(cellIndex)));
    static_cast<MapHeatmapWidget *>(m_heatmap)->setSelectedCellIndex(cellIndex);
}

void MapPerformancePage::selectCellIndex(int cellIndex)
{
    if (cellIndex < 0 || cellIndex >= m_report.cells.size())
        return;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const int rowCellIndex = m_model->index(row, 0).data(Qt::UserRole + 1).toInt();
        if (rowCellIndex != cellIndex)
            continue;
        m_table->selectRow(row);
        m_table->scrollTo(m_model->index(row, 0), QAbstractItemView::PositionAtCenter);
        updateDetails();
        return;
    }
}

QVector<sc2dh::decor::DecorZone> MapPerformancePage::zonesFromModel() const
{
    // 3.0 deliberately accepts only real Regions from the map. Coordinate
    // rectangles, entire-map mode and auto-grids are not part of this flow.
    return selectedRegionZones();
}

QVector<sc2dh::decor::DecorZone> MapPerformancePage::selectedRegionZones() const
{
    QVector<sc2dh::decor::DecorZone> zones;
    if (!m_regionList)
        return zones;
    int zoneId = 1;
    for (int row = 0; row < m_regionList->count(); ++row) {
        const QListWidgetItem *item = m_regionList->item(row);
        if (item->checkState() != Qt::Checked)
            continue;
        const int regionIndex = item->data(Qt::UserRole).toInt();
        if (regionIndex < 0 || regionIndex >= m_regionReadResult.regions.size())
            continue;
        const sc2dh::region::MapRegion &region = m_regionReadResult.regions.at(regionIndex);
        if (!region.geometry.supported || !region.geometry.bounds.valid)
            continue;
        sc2dh::decor::DecorZone zone;
        zone.id = zoneId++;
        zone.name = region.name;
        zone.xMin = region.geometry.bounds.xMin;
        zone.yMin = region.geometry.bounds.yMin;
        zone.xMax = region.geometry.bounds.xMax;
        zone.yMax = region.geometry.bounds.yMax;
        zone.geometry = region.geometry;
        zones << zone;
    }
    return zones;
}

void MapPerformancePage::populateRegionSelector()
{
    if (!m_regionList || !m_regionStatusLabel)
        return;
    const QSignalBlocker blocker(m_regionList);
    m_regionList->clear();
    for (int index = 0; index < m_regionReadResult.regions.size(); ++index) {
        const sc2dh::region::MapRegion &region = m_regionReadResult.regions.at(index);
        const QString parameters = regionGeometryLabel(region.geometry);
        const QString label = QStringLiteral("%1 [#%2] — %3")
                                  .arg(region.name, region.id, parameters);
        auto *item = new QListWidgetItem(label, m_regionList);
        item->setData(Qt::UserRole, index);
        item->setToolTip(tr("Region #%1\n%2\nExact geometry is used; only ObjectDoodad placements inside it are considered.")
                             .arg(region.id, parameters));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        if (!region.geometry.supported) {
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            item->setToolTip(region.geometry.unsupportedReason);
        }
    }
    if (!m_regionReadResult.success) {
        m_regionStatusLabel->setText(tr("Regions unavailable: %1")
                                         .arg((m_regionReadResult.errors + m_regionReadResult.warnings)
                                                  .join(QStringLiteral(" | "))));
    } else {
        const int supported = std::count_if(
            m_regionReadResult.regions.cbegin(), m_regionReadResult.regions.cend(),
            [](const sc2dh::region::MapRegion &region) { return region.geometry.supported; });
        m_regionStatusLabel->setText(tr("Loaded %1 region(s), %2 usable as exact optimization scopes. Source: %3")
                                         .arg(m_regionReadResult.regions.size())
                                         .arg(supported)
                                         .arg(m_regionReadResult.sourceLabel));
    }
    static_cast<MapHeatmapWidget *>(m_heatmap)->setRegions(m_regionReadResult.regions);
    m_chooseRegionsButton->setEnabled(!m_regionReadResult.regions.isEmpty());
    updateOptimizationScope();
}

void MapPerformancePage::updateOptimizationScope()
{
    if (!m_regionList || !m_zoneModel)
        return;
    const QVector<sc2dh::decor::DecorZone> zones = selectedRegionZones();
    populateZoneTable(zones);

    QSet<int> selectedIndices;
    for (int row = 0; row < m_regionList->count(); ++row) {
        const QListWidgetItem *item = m_regionList->item(row);
        if (item->checkState() == Qt::Checked)
            selectedIndices.insert(item->data(Qt::UserRole).toInt());
    }
    static_cast<MapHeatmapWidget *>(m_heatmap)->setSelectedRegionIndices(selectedIndices);

    int dynamicInside = 0;
    int staticInside = 0;
    int boundary = 0;
    for (const auto &doodad : m_allDoodads) {
        if (!doodad.hasPosition)
            continue;
        sc2dh::region::SpatialRelation best = sc2dh::region::SpatialRelation::Outside;
        for (const auto &zone : zones) {
            const auto relation = zone.geometry.supported
                ? zone.geometry.classify(doodad.x, doodad.y)
                : sc2dh::region::SpatialRelation::Outside;
            if (relation == sc2dh::region::SpatialRelation::Inside) {
                best = relation;
                break;
            }
            if (relation == sc2dh::region::SpatialRelation::Boundary)
                best = relation;
        }
        if (best == sc2dh::region::SpatialRelation::Boundary) {
            ++boundary;
        } else if (best == sc2dh::region::SpatialRelation::Inside) {
            if (doodad.dynamicCandidate)
                ++dynamicInside;
            else
                ++staticInside;
        }
    }

    populateDoodadTable({});
    m_decorPreview = {};
    m_createCopyButton->setEnabled(false);
    m_previewButton->setEnabled(m_hasObjects && !zones.isEmpty());
    if (zones.isEmpty()) {
        m_decorSummaryLabel->setText(tr("No Region selected. Click an outlined area on the map or click anywhere on a Region row below."));
        m_galaxyPreview->setPlainText(tr("SELECT A REAL MAP REGION\n\nExact circles, polygons, rectangles and composite Regions are shown on the map.\nAfter selecting one or more Regions, press Preview."));
    } else {
        m_decorSummaryLabel->setText(tr("Selected %1 Region(s): %2 runtime-safe decor candidate(s) inside, %3 static-only, %4 on an exact boundary. Press Preview to verify removals and generate Galaxy.")
                                         .arg(zones.size())
                                         .arg(dynamicInside)
                                         .arg(staticInside)
                                         .arg(boundary));
        m_galaxyPreview->setPlainText(tr("SELECTION READY\n\n%1 Region(s) selected.\n%2 runtime-safe decor candidate(s) are inside the exact geometry.\n\nPress Preview to show every removed Objects entry and the generated Galaxy functions.")
                                              .arg(zones.size())
                                              .arg(dynamicInside));
    }
}

void MapPerformancePage::toggleRegionFromMap(int regionIndex)
{
    if (!m_regionList || regionIndex < 0)
        return;
    for (int row = 0; row < m_regionList->count(); ++row) {
        QListWidgetItem *item = m_regionList->item(row);
        if (item->data(Qt::UserRole).toInt() != regionIndex)
            continue;
        if (!item->flags().testFlag(Qt::ItemIsEnabled))
            return;
        item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
        m_regionList->scrollToItem(item, QAbstractItemView::PositionAtCenter);
        return;
    }
}

void MapPerformancePage::populateZoneTable(const QVector<sc2dh::decor::DecorZone> &zones)
{
    const QSignalBlocker blocker(m_zoneModel);
    m_decorZones = zones;
    m_zoneModel->removeRows(0, m_zoneModel->rowCount());
    for (const sc2dh::decor::DecorZone &zone : zones) {
        QList<QStandardItem *> row = {
            new QStandardItem(QString::number(zone.id)),
            new QStandardItem(zone.name),
            new QStandardItem(QString::number(zone.xMin, 'f', 3)),
            new QStandardItem(QString::number(zone.yMin, 'f', 3)),
            new QStandardItem(QString::number(zone.xMax, 'f', 3)),
            new QStandardItem(QString::number(zone.yMax, 'f', 3))
        };
        for (QStandardItem *item : row)
            item->setEditable(false);
        m_zoneModel->appendRow(row);
    }
    m_zoneTable->resizeColumnsToContents();
    static_cast<MapHeatmapWidget *>(m_heatmap)->setDecorZones(m_decorZones);
}

sc2dh::decor::DecorationSafetyContext MapPerformancePage::decorationSafetyContextFromDoodadTable() const
{
    sc2dh::decor::DecorationSafetyContext context;
    if (!m_doodadModel)
        return context;

    for (int row = 0; row < m_doodadModel->rowCount(); ++row) {
        const QStandardItem *excludeItem = m_doodadModel->item(row, DecorDoodadExcludeColumn);
        const QStandardItem *forcedItem = m_doodadModel->item(row, DecorDoodadForcedZoneColumn);
        const QStandardItem *nameItem = m_doodadModel->item(row, DecorDoodadNameColumn);
        const QStandardItem *idItem = m_doodadModel->item(row, DecorDoodadIdColumn);
        if (!excludeItem || !forcedItem)
            continue;
        const bool excluded = excludeItem->checkState() == Qt::Checked;
        bool ok = false;
        const int forcedZone = forcedItem->text().trimmed().toInt(&ok);
        addDoodadOverrideKeys(&context,
                              idItem ? idItem->text() : QString(),
                              nameItem ? nameItem->text() : QString(),
                              excluded,
                              ok ? forcedZone : 0);
    }
    return context;
}

void MapPerformancePage::populateDoodadTable(const QVector<sc2dh::decor::DoodadPlacement> &doodads)
{
    if (!m_doodadModel)
        return;
    const sc2dh::decor::DecorationSafetyContext previousOverrides = decorationSafetyContextFromDoodadTable();
    const QSignalBlocker blocker(m_doodadModel);
    m_doodadModel->removeRows(0, m_doodadModel->rowCount());

    for (int doodadIndex = 0; doodadIndex < doodads.size(); ++doodadIndex) {
        const sc2dh::decor::DoodadPlacement &doodad = doodads.at(doodadIndex);
        bool inSelectedRegion = false;
        if (doodad.hasPosition) {
            for (const auto &zone : m_decorZones) {
                if (zone.geometry.supported
                    && zone.geometry.classify(doodad.x, doodad.y) != sc2dh::region::SpatialRelation::Outside) {
                    inSelectedRegion = true;
                    break;
                }
            }
        }
        if (!inSelectedRegion)
            continue;

        const QString idKey = doodadOverrideKey(doodad.id);
        const QString nameKey = doodadOverrideKey(doodad.name);
        const bool wasExcluded = previousOverrides.excludedDoodadKeys.contains(idKey)
            || previousOverrides.excludedDoodadKeys.contains(nameKey);
        auto *exclude = new QStandardItem();
        exclude->setCheckable(true);
        exclude->setCheckState(wasExcluded ? Qt::Checked : Qt::Unchecked);
        exclude->setEditable(false);
        exclude->setData(doodadIndex, Qt::UserRole + 10);

        auto *forcedZone = new QStandardItem();
        forcedZone->setEditable(true);
        forcedZone->setToolTip(QStringLiteral("Optional numeric Zone Id. Empty means automatic coordinate assignment."));
        const int previousForcedZone = previousOverrides.forcedZoneByDoodadKey.value(
            !idKey.isEmpty() ? idKey : nameKey, 0);
        if (previousForcedZone > 0)
            forcedZone->setText(QString::number(previousForcedZone));

        auto *name = new QStandardItem(doodad.name);
        auto *id = new QStandardItem(doodad.id);
        auto *type = new QStandardItem(doodad.type);
        auto *position = new QStandardItem(doodad.hasPosition
                                               ? QStringLiteral("%1, %2, %3").arg(doodad.x, 0, 'f', 2).arg(doodad.y, 0, 'f', 2).arg(doodad.z, 0, 'f', 2)
                                               : QStringLiteral("missing"));
        auto *state = new QStandardItem(doodad.dynamicCandidate ? QStringLiteral("Dynamic candidate")
                                                                : doodad.staticOnlyReason);

        for (QStandardItem *item : {name, id, type, position, state})
            item->setEditable(false);

        m_doodadModel->appendRow({exclude, forcedZone, name, id, type, position, state});
    }
    m_doodadTable->resizeColumnsToContents();
}

void MapPerformancePage::updateDoodadTableState(const sc2dh::decor::DecorationStreamingPlan &plan)
{
    if (!m_doodadModel)
        return;
    const QSignalBlocker blocker(m_doodadModel);

    QHash<int, int> zoneByDoodadIndex;
    for (const sc2dh::decor::ZoneAssignment &assignment : plan.zones) {
        for (int doodadIndex : assignment.doodadIndices)
            zoneByDoodadIndex.insert(doodadIndex, assignment.zoneId);
    }
    QSet<int> unassigned = QSet<int>(plan.unassignedDoodads.cbegin(), plan.unassignedDoodads.cend());

    const int rows = m_doodadModel->rowCount();
    for (int row = 0; row < rows; ++row) {
        const QStandardItem *exclude = m_doodadModel->item(row, DecorDoodadExcludeColumn);
        QStandardItem *state = m_doodadModel->item(row, DecorDoodadStateColumn);
        if (!state || !exclude)
            continue;
        const int doodadIndex = exclude->data(Qt::UserRole + 10).toInt();
        if (doodadIndex < 0 || doodadIndex >= plan.doodads.size())
            continue;
        if (zoneByDoodadIndex.contains(doodadIndex)) {
            state->setText(QStringLiteral("Will be deleted from Objects copy; recreate in zone %1").arg(zoneByDoodadIndex.value(doodadIndex)));
        } else if (unassigned.contains(doodadIndex)) {
            state->setText(QStringLiteral("Dynamic candidate, but not inside any zone; will stay in Objects"));
        } else {
            const QString reason = plan.doodads.at(doodadIndex).staticOnlyReason;
            state->setText(reason.isEmpty() ? QStringLiteral("Keep static in Objects") : reason);
        }
    }
    m_doodadTable->resizeColumnsToContents();
}

void MapPerformancePage::updateDecorPreview()
{
    if (!m_hasObjects) {
        m_decorPreview = {};
        m_decorSummaryLabel->setText(QStringLiteral("No readable Objects entry: open a map archive or extracted map folder with Objects first."));
        m_galaxyPreview->setPlainText(QStringLiteral(
            "NO OBJECTS ENTRY LOADED\n\n"
            "Open a .SC2Map/.SC2Mod archive or extracted map folder with the root Objects file.\n"
            "After that this panel will list every ObjectDoodad, show which function creates each zone, and create a copy with dynamic decor removed from Objects.\n"));
        m_createCopyButton->setEnabled(false);
        return;
    }

    m_decorZones = zonesFromModel();
    static_cast<MapHeatmapWidget *>(m_heatmap)->setDecorZones(m_decorZones);
    if (m_decorZones.isEmpty()) {
        m_decorPreview = {};
        m_decorSummaryLabel->setText(tr("No real map Region selected. Click an exact outlined Region on the map or click its row in the list."));
        m_galaxyPreview->setPlainText(QStringLiteral(
            "SELECT A MAP REGION FIRST\n\n"
            "Only exact Regions loaded from the map are accepted.\n"
            "Then Preview will show:\n"
            "- NAME_OUT_FUNK_N() function names;\n"
            "- which doodads will be deleted from Objects in the optimized copy;\n"
            "- DecorOpt_ClearZone(N) / DecorOpt_ClearAll();\n"
            "- full generated Galaxy script.\n"));
        m_createCopyButton->setEnabled(false);
        return;
    }

    sc2dh::decor::GalaxyGenerationOptions options;
    options.functionPrefix = m_prefixEdit->text().trimmed().isEmpty()
        ? QStringLiteral("NAME_OUT_FUNK")
        : m_prefixEdit->text().trimmed();
    options.batchLimit = m_batchSpin->value();

    const sc2dh::decor::DecorationSafetyContext safetyContext = decorationSafetyContextFromDoodadTable();
    m_decorPreview = sc2dh::decor::DecorationStreamingPlanner().createOptimizedArtifacts(m_objectsBytes,
                                                                                         m_decorZones,
                                                                                         safetyContext,
                                                                                         options);
    const sc2dh::decor::DecorationStreamingPlan &plan = m_decorPreview.plan;
    populateDoodadTable(plan.doodads);
    updateDoodadTableState(plan);
    int dynamicAssigned = 0;
    for (const sc2dh::decor::ZoneAssignment &assignment : plan.zones)
        dynamicAssigned += assignment.doodadIndices.size();

    QString summary = QStringLiteral("%1 doodad(s), %2 will be deleted from Objects in the optimized copy and recreated by Galaxy, %3 static-only, %4 unassigned. Prefix/function example: %5_1(). Batch: %6.")
                          .arg(plan.doodads.size())
                          .arg(dynamicAssigned)
                          .arg(plan.staticOnlyDoodads.size())
                          .arg(plan.unassignedDoodads.size())
                          .arg(options.functionPrefix)
                          .arg(options.batchLimit);
    if (!m_decorPreview.warnings.isEmpty())
        summary += QStringLiteral(" Warnings: %1").arg(m_decorPreview.warnings.join(QStringLiteral(" | ")));
    if (m_decorZones.isEmpty())
        summary += QStringLiteral(" Create at least one decoration zone before creating an optimized map copy.");
    if (sourceArchivePath().isEmpty())
        summary += QStringLiteral(" Create-copy UI currently requires a .SC2Map/.SC2Mod archive source; components folders can still be inspected.");
    summary += QStringLiteral(" Archive creation re-scans scripts/triggers and may keep more doodads static if referenced.");
    m_decorSummaryLabel->setText(summary);
    m_galaxyPreview->setPlainText(decorationPreviewText(m_decorPreview, m_decorZones, options));
    m_createCopyButton->setEnabled(!sourceArchivePath().isEmpty()
                                   && !m_decorZones.isEmpty()
                                   && dynamicAssigned > 0
                                   && m_decorPreview.warnings.isEmpty());
}

QString MapPerformancePage::sourceArchivePath() const
{
    const QFileInfo info(m_result.rootFolder);
    if (info.exists() && info.isFile() && ScannedFileReader::isArchivePath(info.absoluteFilePath()))
        return info.absoluteFilePath();
    return {};
}

QString MapPerformancePage::defaultDecorOutputPath() const
{
    const QString source = sourceArchivePath();
    if (source.isEmpty())
        return {};
    const QFileInfo info(source);
    const QString suffix = info.suffix().isEmpty() ? QStringLiteral("SC2Map") : info.suffix();
    return QDir(info.absolutePath()).absoluteFilePath(QStringLiteral("%1_DecorOptimized.%2").arg(info.completeBaseName(), suffix));
}

void MapPerformancePage::createDecorOptimizedMapCopy()
{
    const QString source = sourceArchivePath();
    if (source.isEmpty()) {
        QMessageBox::warning(this,
                             QStringLiteral("Create Decor-Optimized Map Copy"),
                             QStringLiteral("This action currently requires an opened .SC2Map/.SC2Mod archive source. The original map will not be modified."));
        return;
    }

    const QVector<sc2dh::decor::DecorZone> zones = zonesFromModel();
    if (zones.isEmpty()) {
        QMessageBox::warning(this,
                             QStringLiteral("Create Decor-Optimized Map Copy"),
                             QStringLiteral("Create at least one decoration zone first."));
        return;
    }

    const QString selectedOutput = QFileDialog::getSaveFileName(this,
                                                                QStringLiteral("Create Decor-Optimized Map Copy"),
                                                                defaultDecorOutputPath(),
                                                                QStringLiteral("SC2 archives (*.SC2Map *.SC2Mod *.SC2Campaign);;All files (*)"));
    if (selectedOutput.isEmpty())
        return;

    bool overwrite = false;
    if (QFileInfo::exists(selectedOutput)) {
        const QMessageBox::StandardButton answer =
            QMessageBox::question(this,
                                  QStringLiteral("Overwrite output"),
                                  QStringLiteral("Output archive already exists:\n%1\n\nOverwrite it?").arg(QDir::toNativeSeparators(selectedOutput)),
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
        overwrite = true;
    }

    sc2dh::decor::DecorOptimizedMapRequest request;
    request.sourceArchivePath = source;
    request.outputArchivePath = selectedOutput;
    request.zones = zones;
    request.galaxyOptions.functionPrefix = m_prefixEdit->text().trimmed().isEmpty()
        ? QStringLiteral("NAME_OUT_FUNK")
        : m_prefixEdit->text().trimmed();
    request.galaxyOptions.batchLimit = m_batchSpin->value();
    request.safetyContext = decorationSafetyContextFromDoodadTable();
    request.overwriteExisting = overwrite;

    const sc2dh::decor::DecorOptimizedMapResult result =
        sc2dh::decor::DecorationMapCopyService().createOptimizedCopy(request);
    if (!result.success) {
        m_decorSummaryLabel->setText(QStringLiteral("Create copy failed: %1").arg(result.error));
        m_galaxyPreview->appendPlainText(QStringLiteral("\n\nSAVE ERROR\nOriginal source changed: no\nReason: %1")
                                             .arg(result.error));
        OperationResult operation;
        operation.outcome = OperationOutcome::Failed;
        operation.errorCode = OperationErrorCode::SaveFailed;
        operation.title = tr("Create decor-optimized map copy");
        operation.summary = tr("No output was committed. The original map was not changed.");
        operation.outputPath = selectedOutput;
        operation.originalChanged = false;
        operation.error = result.error;
        operation.details = result.warnings;
        emit operationFinished(operation);
        return;
    }

    QString message = QStringLiteral("Created structural-verified copy:\n%1\n\nRemoved dynamic visual doodads: %2\nFull re-analysis: %3 file(s), %4 data node(s).")
                          .arg(QDir::toNativeSeparators(result.outputArchivePath))
                          .arg(result.removedDoodads)
                          .arg(result.verifiedScannedFiles)
                          .arg(result.verifiedDataNodes);
    if (!result.warnings.isEmpty())
        message += QStringLiteral("\n\nWarnings:\n%1").arg(result.warnings.join(QStringLiteral("\n")));
    m_decorSummaryLabel->setText(message);
    OperationResult operation;
    operation.outcome = OperationOutcome::Succeeded;
    operation.title = tr("Create decor-optimized map copy");
    operation.selected = m_decorPreview.plan.doodads.size();
    operation.applied = result.removedDoodads;
    operation.skipped = m_decorPreview.plan.unassignedDoodads.size();
    operation.blocked = m_decorPreview.plan.staticOnlyDoodads.size();
    operation.outputPath = result.outputArchivePath;
    operation.originalChanged = false;
    operation.details = result.warnings;
    emit operationFinished(operation);
}

QString MapPerformancePage::detailTextForCell(const sc2dh::perf::MapPerformanceCell &cell) const
{
    QString text;
    text += QStringLiteral("Cell: %1,%2\n").arg(cell.column + 1).arg(cell.row + 1);
    text += QStringLiteral("Bounds: %1\n").arg(boundsLabel(cell));
    text += QStringLiteral("Score label: %1\n").arg(cell.scoreLabel);
    text += QStringLiteral("Combined: %1\n").arg(formatScore(cell.combinedRiskScore));
    text += QStringLiteral("Layers: Doodad Density %1 | Units %2 | Assets %3 | Trigger CPU Risk %4\n\n")
                .arg(formatScore(cell.doodadDensityScore))
                .arg(formatScore(cell.unitsScore))
                .arg(formatScore(cell.assetsScore))
                .arg(formatScore(cell.triggerCpuRiskScore));
    text += QStringLiteral("Counts: %1 doodad(s), %2 unit/destructible placement(s), %3 unique Actor/Model id(s), %4 linked asset bytes\n\n")
                .arg(cell.doodadCount)
                .arg(cell.unitCount + cell.destructibleCount)
                .arg(cell.uniqueActorModelCount)
                .arg(cell.linkedAssetBytes);
    text += section(QStringLiteral("Reasons"), cell.reasons);
    text += QStringLiteral("\n") + section(QStringLiteral("Related objects / placements"), cell.relatedObjects, 80);
    text += QStringLiteral("\n") + section(QStringLiteral("Related files"), cell.relatedFiles, 80);
    text += QStringLiteral("\n") + section(QStringLiteral("Trigger risk lines"), cell.triggerLines, 80);
    return text;
}
