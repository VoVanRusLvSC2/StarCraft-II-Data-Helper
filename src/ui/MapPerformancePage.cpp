#include "ui/MapPerformancePage.h"

#include "app/AppSettings.h"
#include "core/ArchiveCompressionService.h"
#include "core/DecorationMapCopyService.h"
#include "core/MapPreviewData.h"
#include "core/ScannedFileReader.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QCheckBox>
#include <QSettings>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QMessageBox>
#include <QOpenGLWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTableView>
#include <QTreeWidget>
#include <QSplitter>
#include <QStorageInfo>
#include <QSurfaceFormat>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <QtConcurrent>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <functional>
#include <memory>
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
        return (geometry.rawType.compare(QStringLiteral("diamond"), Qt::CaseInsensitive) == 0
                    ? translate("diamond | %1 points | x %2..%3 | y %4..%5")
                    : translate("polygon | %1 points | x %2..%3 | y %4..%5"))
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

class MapHeatmapWidget : public QOpenGLWidget
{
public:
    explicit MapHeatmapWidget(QWidget *parent = nullptr)
        : QOpenGLWidget(parent)
    {
        QSurfaceFormat format = this->format();
        format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
        format.setSamples(sc2dh::app::AppSettings::modelAntialiasing() ? 4 : 0);
        setFormat(format);
        setMinimumHeight(300);
        setMouseTracking(true);
        setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
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

    void setDecorPlan(const sc2dh::decor::DecorationStreamingPlan &plan)
    {
        m_decorPlan = plan;
        invalidateStaticLayer();
        update();
    }

    void setDecorPlan(const sc2dh::decor::DecorationVisibilityPlan &plan)
    {
        sc2dh::decor::DecorationStreamingPlan displayPlan;
        displayPlan.doodads = plan.doodads;
        displayPlan.zones = plan.zones;
        displayPlan.staticOnlyDoodads = plan.staticOnlyDoodads;
        displayPlan.unassignedDoodads = plan.unassignedDoodads;
        displayPlan.boundaryDoodads = plan.boundaryDoodads;
        displayPlan.warnings = plan.warnings;
        setDecorPlan(displayPlan);
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

    void focusRegion(int index)
    {
        if (index < 0 || index >= m_regions.size())
            return;
        const auto &regionBounds = m_regions.at(index).geometry.bounds;
        if (!regionBounds.valid)
            return;
        const auto base = baseDisplayBounds();
        const double width = std::max(0.001, regionBounds.xMax - regionBounds.xMin);
        const double height = std::max(0.001, regionBounds.yMax - regionBounds.yMin);
        m_zoom = std::clamp(std::min((base.xMax - base.xMin) / (width * 1.5),
                                     (base.yMax - base.yMin) / (height * 1.5)), 1.0, 16.0);
        m_panX = (regionBounds.xMin + regionBounds.xMax - base.xMin - base.xMax) / 2.0;
        m_panY = (regionBounds.yMin + regionBounds.yMax - base.yMin - base.yMax) / 2.0;
        invalidateStaticLayer();
        update();
    }

    void setRegionSelectionEnabled(bool enabled)
    {
        m_regionSelectionEnabled = enabled;
        setCursor(enabled ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }

    void setMapPreview(const sc2dh::preview::MapPreviewData &preview)
    {
        m_backgroundImage = preview.image;
        m_backgroundSourceLabel = preview.sourceLabel;
        m_worldBounds = preview.worldBounds;
        m_exactWorldBounds = preview.exactWorldBounds;
        m_previewUnavailableReason = preview.unavailableReason;
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
    void paintGL() override
    {
        ensureStaticLayer();
        QPainter painter(this);
        painter.drawPixmap(0, 0, m_staticLayer);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF area = gridArea();
        const sc2dh::region::RegionBounds bounds = displayBounds();
        painter.save();
        painter.setClipRect(area);
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

        painter.restore();

        painter.setPen(QColor(QStringLiteral("#91a3b7")));
        painter.drawText(QRect(12, height() - 24, width() - 24, 18),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         m_regionSelectionEnabled
                             ? QStringLiteral("Click an exact Region outline to select it. Doodads blue, units green, destructibles orange.")
                             : QStringLiteral("Region selection paused."));
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton) {
            m_panning = true;
            m_lastPanPosition = event->position();
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
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
        if (m_panning) {
            const QPointF delta = event->position() - m_lastPanPosition;
            m_lastPanPosition = event->position();
            const QRectF area = gridArea();
            const auto base = baseDisplayBounds();
            m_panX -= delta.x() / std::max(1.0, area.width()) * (base.xMax - base.xMin) / m_zoom;
            m_panY += delta.y() / std::max(1.0, area.height()) * (base.yMax - base.yMin) / m_zoom;
            invalidateStaticLayer();
            update();
            event->accept();
            return;
        }
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

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (m_panning && (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton)) {
            m_panning = false;
            setCursor(m_regionSelectionEnabled ? Qt::PointingHandCursor : Qt::ArrowCursor);
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void wheelEvent(QWheelEvent *event) override
    {
        const double factor = event->angleDelta().y() > 0 ? 1.25 : 0.8;
        m_zoom = std::clamp(m_zoom * factor, 1.0, 16.0);
        invalidateStaticLayer();
        update();
        event->accept();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        m_zoom = 1.0;
        m_panX = 0.0;
        m_panY = 0.0;
        invalidateStaticLayer();
        update();
        event->accept();
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
        QRectF available(48.0, 40.0, double(std::max(80, width() - 64)),
                         double(std::max(80, height() - 84)));
        const auto bounds = displayBounds();
        const double mapWidth = std::max(0.0001, bounds.xMax - bounds.xMin);
        const double mapHeight = std::max(0.0001, bounds.yMax - bounds.yMin);
        const double mapAspect = mapWidth / mapHeight;
        const double availableAspect = available.width() / available.height();
        if (availableAspect > mapAspect) {
            const double fittedWidth = available.height() * mapAspect;
            available.setLeft(available.center().x() - fittedWidth / 2.0);
            available.setWidth(fittedWidth);
        } else {
            const double fittedHeight = available.width() / mapAspect;
            available.setTop(available.center().y() - fittedHeight / 2.0);
            available.setHeight(fittedHeight);
        }
        return available;
    }

    sc2dh::region::RegionBounds baseDisplayBounds() const
    {
        sc2dh::region::RegionBounds bounds;
        if (m_worldBounds.valid && m_worldBounds.xMin < m_worldBounds.xMax
            && m_worldBounds.yMin < m_worldBounds.yMax) {
            return m_worldBounds;
        }
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

    sc2dh::region::RegionBounds displayBounds() const
    {
        const auto base = baseDisplayBounds();
        const double fullWidth = base.xMax - base.xMin;
        const double fullHeight = base.yMax - base.yMin;
        const double viewWidth = fullWidth / m_zoom;
        const double viewHeight = fullHeight / m_zoom;
        const double centerX = (base.xMin + base.xMax) / 2.0 + m_panX;
        const double centerY = (base.yMin + base.yMax) / 2.0 + m_panY;
        return {centerX - viewWidth / 2.0, centerY - viewHeight / 2.0,
                centerX + viewWidth / 2.0, centerY + viewHeight / 2.0, true};
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
        m_staticLayer.fill(QColor(QStringLiteral("#1a2227")));
        QPainter painter(&m_staticLayer);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        painter.fillRect(rect(), QColor(26, 34, 39));
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
                         QStringLiteral("REGIONS | Wheel: zoom | Right drag: pan | Double-click: reset"));

        const QRectF area = gridArea();
        const auto bounds = displayBounds();
        const auto baseBounds = baseDisplayBounds();
        painter.save();
        painter.setClipRect(area);
        if (!m_backgroundImage.isNull()) {
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            const double baseWidth = std::max(0.0001, baseBounds.xMax - baseBounds.xMin);
            const double baseHeight = std::max(0.0001, baseBounds.yMax - baseBounds.yMin);
            QRectF sourceRect(
                (bounds.xMin - baseBounds.xMin) / baseWidth * m_backgroundImage.width(),
                (baseBounds.yMax - bounds.yMax) / baseHeight * m_backgroundImage.height(),
                (bounds.xMax - bounds.xMin) / baseWidth * m_backgroundImage.width(),
                (bounds.yMax - bounds.yMin) / baseHeight * m_backgroundImage.height());
            sourceRect = sourceRect.intersected(m_backgroundImage.rect());
            painter.drawImage(area, m_backgroundImage, sourceRect);
            painter.fillRect(area, QColor(3, 9, 14, 48));
        } else {
            painter.fillRect(area, QColor(38, 46, 51));
        }
        painter.setPen(QPen(QColor(74, 220, 195, 76), 1));
        painter.drawRect(area);

        painter.setPen(QPen(QColor(230, 246, 242, 90), 1, Qt::DotLine));
        for (int tick = 0; tick <= 8; ++tick) {
            const double fraction = double(tick) / 8.0;
            const double x = bounds.xMin + fraction * (bounds.xMax - bounds.xMin);
            const double y = bounds.yMin + fraction * (bounds.yMax - bounds.yMin);
            const QPointF xPoint = mapToWidget(x, bounds.yMin, area, bounds);
            const QPointF yPoint = mapToWidget(bounds.xMin, y, area, bounds);
            painter.drawLine(QPointF(xPoint.x(), area.top()), QPointF(xPoint.x(), area.bottom()));
            painter.drawLine(QPointF(area.left(), yPoint.y()), QPointF(area.right(), yPoint.y()));
        }

        if (m_report.cells.isEmpty() || m_report.columns <= 0 || m_report.rows <= 0) {
            painter.setPen(QColor(QStringLiteral("#91a3b7")));
            painter.drawText(area.adjusted(12, 12, -12, -12), Qt::AlignCenter,
                             QStringLiteral("No positioned decor was found. Regions can still be inspected."));
            painter.restore();
            return;
        }

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
        const auto drawDoodad = [&](int doodadIndex, const QColor &color, double radius) {
            if (doodadIndex < 0 || doodadIndex >= m_decorPlan.doodads.size())
                return;
            const auto &doodad = m_decorPlan.doodads.at(doodadIndex);
            if (!doodad.hasPosition)
                return;
            painter.setPen(QPen(QColor(0, 0, 0, 150), 1));
            painter.setBrush(color);
            painter.drawEllipse(mapToWidget(doodad.x, doodad.y, area, bounds), radius, radius);
        };
        for (const auto &assignment : m_decorPlan.zones)
            for (int index : assignment.doodadIndices)
                drawDoodad(index, QColor(QStringLiteral("#35e6ff")), 3.6);
        for (int index : m_decorPlan.staticOnlyDoodads)
            drawDoodad(index, QColor(QStringLiteral("#ff6f61")), 3.3);
        for (int index : m_decorPlan.unassignedDoodads)
            drawDoodad(index, QColor(QStringLiteral("#a5adb8")), 2.8);
        painter.restore();
        painter.setPen(QColor(QStringLiteral("#e8f4f1")));
        painter.drawText(QRectF(area.left(), area.top() - 22, area.width(), 18), Qt::AlignRight,
                         QStringLiteral("cyan: candidate | red: blocked | gray: outside"));
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
    sc2dh::decor::DecorationStreamingPlan m_decorPlan;
    QVector<sc2dh::decor::DecorZone> m_zones;
    QVector<sc2dh::region::MapRegion> m_regions;
    QSet<int> m_selectedRegions;
    QImage m_backgroundImage;
    QString m_backgroundSourceLabel;
    sc2dh::region::RegionBounds m_worldBounds;
    bool m_exactWorldBounds = false;
    QString m_previewUnavailableReason;
    QPixmap m_staticLayer;
    QPixmap m_gridTexture{QStringLiteral(":/textures/ui_nova_storymode_bggrid_shimmer_sideways.png")};
    QPixmap m_pointTexture{QStringLiteral(":/textures/ui_nova_storymode_bgpointgrid_25.png")};
    QPixmap m_lightTexture{QStringLiteral(":/textures/ui_nova_login_backgroundlights.png")};
    QPixmap m_scanlineTexture{QStringLiteral(":/textures/ui_nova_archives_backgroundframe_scanlines.png")};
    int m_selectedCell = -1;
    int m_hoveredRegion = -1;
    bool m_regionSelectionEnabled = true;
    bool m_panning = false;
    QPointF m_lastPanPosition;
    double m_zoom = 1.0;
    double m_panX = 0.0;
    double m_panY = 0.0;
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

QString decorationPreviewText(const sc2dh::decor::DecorationVisibilityArtifacts &artifacts,
                              const QVector<sc2dh::decor::DecorZone> &zones,
                              const sc2dh::decor::GalaxyGenerationOptions &options)
{
    const sc2dh::decor::DecorationVisibilityPlan &plan = artifacts.plan;
    QString text;
    text += QStringLiteral("DECORATION VISIBILITY PREVIEW / 4.0 BETA\n");
    text += QStringLiteral("Function prefix: %1\n").arg(options.functionPrefix);
    text += QStringLiteral("Objects is preserved byte-for-byte. Existing doodad actors are controlled by ID.\n\n");
    text += QStringLiteral("How to use in Galaxy:\n");
    text += QStringLiteral("  - Hide zone: %1_Hide_1(); or DecorOpt_HideZone(1);\n").arg(options.functionPrefix);
    text += QStringLiteral("  - Restore zone: %1_Restore_1(); or DecorOpt_RestoreZone(1);\n").arg(options.functionPrefix);
    text += QStringLiteral("  - Hide all zones: %1_HideAll(); or DecorOpt_HideAll();\n").arg(options.functionPrefix);
    text += QStringLiteral("  - Restore all zones: %1_RestoreAll(); or DecorOpt_RestoreAll();\n").arg(options.functionPrefix);
    text += QStringLiteral("  - Check hidden state: DecorOpt_IsZoneHidden(1);\n\n");
    text += QStringLiteral("Important:\n");
    text += QStringLiteral("  - Create Decor-Control Map Copy does not remove any doodad from Objects.\n");
    text += QStringLiteral("  - Original map is not modified.\n");
    text += QStringLiteral("  - Static-only doodads stay in Objects.\n\n");

    int assigned = 0;
    for (const sc2dh::decor::ZoneAssignment &zone : plan.zones)
        assigned += zone.doodadIndices.size();
    text += QStringLiteral("Summary: %1 doodad(s), %2 can be hidden/restored by Galaxy, %3 blocked, %4 outside scope, %5 boundary/ambiguous.\n")
                .arg(plan.doodads.size())
                .arg(assigned)
                .arg(plan.staticOnlyDoodads.size())
                .arg(plan.unassignedDoodads.size())
                .arg(plan.boundaryDoodads.size());
    text += QStringLiteral("Objects preservation: %1\n\n")
                .arg(artifacts.objectsPreserved ? QStringLiteral("PASS") : QStringLiteral("BLOCKED"));

    text += QStringLiteral("ZONES / FUNCTIONS / EXISTING DECOR ACTORS\n");
    for (const sc2dh::decor::ZoneAssignment &zone : plan.zones) {
        text += QStringLiteral("\n[%1] %2\n").arg(zone.zoneId).arg(decorZoneName(zones, zone.zoneId));
        text += QStringLiteral("Hide function: %1_Hide_%2()\n").arg(options.functionPrefix).arg(zone.zoneId);
        text += QStringLiteral("Restore function: %1_Restore_%2()\n").arg(options.functionPrefix).arg(zone.zoneId);
        if (zone.doodadIndices.isEmpty()) {
            text += QStringLiteral("  - no dynamic doodads assigned\n");
            continue;
        }
        for (int doodadIndex : zone.doodadIndices) {
            if (doodadIndex >= 0 && doodadIndex < plan.doodads.size())
                text += QStringLiteral("  - KEEP IN OBJECTS + control existing actor: %1\n")
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

    text += QStringLiteral("\nUNASSIGNED DOODADS / KEPT UNCHANGED\n");
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

QString decorationConversionPreviewText(const sc2dh::decor::DecorationOptimizedArtifacts &artifacts,
                                        const QVector<sc2dh::decor::DecorZone> &zones,
                                        const sc2dh::decor::GalaxyGenerationOptions &options)
{
    const auto &plan = artifacts.plan;
    QString text = QStringLiteral("GALAXY CONVERSION PREVIEW / 4.0 BETA\n");
    text += QStringLiteral("Original map stays unchanged. The new copy removes eligible doodads and directly unreferenced, simple units from Objects.\n");
    text += QStringLiteral("Call the Create_<RegionName>() and Clear_<RegionName>() functions listed below from your map triggers.\n");
    text += QStringLiteral("Doodads eligible: %1; units eligible: %2; round-trip: %3; other placements preserved: %4.\n\n")
                .arg(artifacts.removedDoodadIndices.size())
                .arg(artifacts.removedUnitIndices.size())
                .arg(artifacts.roundTripVerified ? QStringLiteral("PASS") : QStringLiteral("BLOCKED"))
                .arg(artifacts.outsideScopePreserved ? QStringLiteral("PASS") : QStringLiteral("BLOCKED"));
    for (const auto &zone : plan.zones) {
        const QString stem = sc2dh::decor::galaxyRegionFunctionStem(plan, zone, options);
        text += QStringLiteral("[%1] %2 | Create_%3(); | Clear_%3();\n")
                    .arg(zone.zoneId)
                    .arg(decorZoneName(zones, zone.zoneId))
                    .arg(stem);
        for (int index : zone.doodadIndices) {
            if (index >= 0 && index < plan.doodads.size())
                text += QStringLiteral("  - REMOVE FROM COPY / RECREATE BY GALAXY: %1\n")
                            .arg(decorDoodadLabel(plan.doodads.at(index)));
        }
        for (int index : zone.unitIndices) {
            if (index >= 0 && index < plan.units.size()) {
                const auto &unit = plan.units.at(index);
                text += QStringLiteral("  - REMOVE UNIT / RECREATE WITH DECOR: %1 [#%2]\n")
                            .arg(unit.type, unit.id);
            }
        }
    }
    text += QStringLiteral("\nStatic-only or excluded: %1; outside Regions: %2; boundary: %3.\n")
                .arg(plan.staticOnlyDoodads.size())
                .arg(plan.unassignedDoodads.size())
                .arg(plan.boundaryDoodads.size());
    for (int index : plan.staticOnlyDoodads) {
        if (index >= 0 && index < plan.doodads.size())
            text += QStringLiteral("  - KEEP IN OBJECTS: %1 | %2\n")
                        .arg(decorDoodadLabel(plan.doodads.at(index)), plan.doodads.at(index).staticOnlyReason);
    }
    text += QStringLiteral("Units kept in Objects: %1; outside Regions: %2.\n")
                .arg(plan.staticOnlyUnits.size()).arg(plan.unassignedUnits.size());
    for (int index : plan.staticOnlyUnits) {
        if (index >= 0 && index < plan.units.size()) {
            const auto &unit = plan.units.at(index);
            text += QStringLiteral("  - KEEP UNIT IN OBJECTS: %1 [#%2] at (%3, %4) | %5\n")
                        .arg(unit.type, unit.id)
                        .arg(unit.x).arg(unit.y)
                        .arg(unit.staticOnlyReason);
        }
    }
    text += QStringLiteral("Generic 'all units in Region' trigger actions are intentionally not used as direct unit references.\n");
    if (!artifacts.warnings.isEmpty())
        text += QStringLiteral("\nWarnings: %1\n").arg(artifacts.warnings.join(QStringLiteral(" | ")));
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

struct PreparedMapPerformance
{
    sc2dh::decor::DecorationSafetyContext safetyContext;
    sc2dh::region::RegionReadResult regions;
    sc2dh::preview::MapPreviewData preview;
    QByteArray objectsBytes;
    QString objectsSource;
    sc2dh::perf::MapPerformanceReport report;
    QVector<sc2dh::decor::DoodadPlacement> doodads;
    bool hasObjects = false;
    QString workerError;
};

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

    auto *title = new QLabel(tr("Map Performance — decor to Galaxy"), header);
    title->setObjectName(QStringLiteral("panelTitle"));
    headerLayout->addWidget(title);

    m_summaryLabel = new QLabel(tr("1. Select Regions  →  2. Preview what moves  →  3. Save a new map copy  →  4. Call Create_<RegionName>() from a GUI trigger."), header);
    m_summaryLabel->setProperty("sc2dh.mapPerformanceRole", QStringLiteral("summary"));
    m_summaryLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    m_summaryLabel->setWordWrap(true);
    headerLayout->addWidget(m_summaryLabel);

    m_warningLabel = new QLabel(tr("The original map stays unchanged. Selected decor and eligible units are removed only from the new copy; Galaxy recreates them when you call its Create function."), header);
    m_warningLabel->setProperty("sc2dh.mapPerformanceRole", QStringLiteral("warning"));
    m_warningLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    m_warningLabel->setWordWrap(true);
    headerLayout->addWidget(m_warningLabel);
    layout->addWidget(header);

    auto *contentSplitter = new QSplitter(Qt::Vertical, this);
    contentSplitter->setObjectName(QStringLiteral("mapPerformanceContentSplitter"));
    contentSplitter->setChildrenCollapsible(false);

    m_heatmap = new MapHeatmapWidget(contentSplitter);
    m_heatmap->setObjectName(QStringLiteral("mapPerformanceHeatmap"));
    static_cast<MapHeatmapWidget *>(m_heatmap)->cellClicked = [this](int cellIndex) {
        selectCellIndex(cellIndex);
    };
    static_cast<MapHeatmapWidget *>(m_heatmap)->regionClicked = [this](int regionIndex) {
        toggleRegionFromMap(regionIndex);
    };
    contentSplitter->addWidget(m_heatmap);

    auto *decorGroup = new QGroupBox(tr("Decor and units by Region"), contentSplitter);
    decorGroup->setObjectName(QStringLiteral("mapDecorStreamingGroup"));
    auto *decorLayout = new QVBoxLayout(decorGroup);
    decorLayout->setContentsMargins(10, 10, 10, 10);
    decorLayout->setSpacing(8);

    m_decorSummaryLabel = new QLabel(tr("Select one or more Regions. The outline and checkboxes show what will be converted."), decorGroup);
    m_decorSummaryLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    m_decorSummaryLabel->setWordWrap(true);
    decorLayout->addWidget(m_decorSummaryLabel);

    auto *scopeGroup = new QGroupBox(tr("1. Choose map Regions"), decorGroup);
    auto *scopeLayout = new QVBoxLayout(scopeGroup);
    m_regionStatusLabel = new QLabel(tr("No Regions component loaded."), scopeGroup);
    m_regionStatusLabel->setWordWrap(true);
    scopeLayout->addWidget(m_regionStatusLabel);
    auto *searchRow = new QHBoxLayout();
    m_regionSearch = new QLineEdit(scopeGroup);
    m_regionSearch->setObjectName(QStringLiteral("mapRegionSearch"));
    m_regionSearch->setPlaceholderText(tr("Search Regions by name or ID…"));
    m_regionSearch->setClearButtonEnabled(true);
    m_showSelectedOnly = new QCheckBox(tr("Selected only"), scopeGroup);
    m_showSelectedOnly->setObjectName(QStringLiteral("mapRegionsSelectedOnly"));
    searchRow->addWidget(m_regionSearch, 1);
    searchRow->addWidget(m_showSelectedOnly);
    scopeLayout->addLayout(searchRow);
    m_selectedRegionsLabel = new QLabel(tr("Selected: none"), scopeGroup);
    m_selectedRegionsLabel->setObjectName(QStringLiteral("mapSelectedRegionsSummary"));
    m_selectedRegionsLabel->setWordWrap(true);
    m_selectedRegionsLabel->setMaximumHeight(56);
    scopeLayout->addWidget(m_selectedRegionsLabel);
    m_regionList = new RegionListWidget(scopeGroup);
    m_regionList->setSelectionMode(QAbstractItemView::NoSelection);
    m_regionList->setMinimumHeight(180);
    m_regionList->setToolTip(tr("Click anywhere on a row to toggle the Region. Exact parameters are shown in each row."));
    scopeLayout->addWidget(m_regionList);
    auto *regionButtons = new QHBoxLayout();
    auto *selectAllRegions = new QPushButton(tr("Select visible"), scopeGroup);
    auto *clearRegions = new QPushButton(tr("Clear selection"), scopeGroup);
    regionButtons->addWidget(selectAllRegions);
    regionButtons->addWidget(clearRegions);
    regionButtons->addStretch(1);
    scopeLayout->addLayout(regionButtons);

    decorLayout->addWidget(scopeGroup);

    auto *decorControls = new QHBoxLayout();
    m_prefixEdit = new QLineEdit(QStringLiteral("Game_area"), decorGroup);
    m_prefixEdit->setMinimumWidth(150);
    m_prefixEdit->setToolTip(tr("Fallback function name when a Region name cannot be used as a Galaxy identifier."));
    auto *form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(6);
    form->addRow(tr("Fallback function prefix"), m_prefixEdit);
    m_restoreSidecars = new QCheckBox(tr("Include restore backup files (JSON/XML)"), decorGroup);
    m_restoreSidecars->setObjectName(QStringLiteral("decorRestoreSidecars"));
    m_restoreSidecars->setToolTip(tr("Optional exact rollback backup. Rebuild placements from Galaxy works without these files. Runtime actor data is always preserved."));
    m_restoreSidecars->setChecked(QSettings().value(QStringLiteral("decor/writeRestoreSidecars"), false).toBool());
    connect(m_restoreSidecars, &QCheckBox::toggled, this, [](bool enabled) {
        QSettings().setValue(QStringLiteral("decor/writeRestoreSidecars"), enabled);
    });
    form->addRow(m_restoreSidecars);
    m_createGuiAction = new QCheckBox(tr("Create GUI action and Region preset"), decorGroup);
    m_createGuiAction->setObjectName(QStringLiteral("decorCreateGuiAction"));
    m_createGuiAction->setChecked(QSettings().value(QStringLiteral("decor/createGuiAction"), false).toBool());
    m_createGuiAction->setToolTip(tr("Create an editable Action Definition with a Region preset and Switch cases calling the Galaxy functions. Add the Galaxy connection when missing."));
    m_guiActionName = new QLineEdit(QSettings().value(QStringLiteral("decor/guiActionName"), QStringLiteral("Create Map Environment From Galaxy")).toString(), decorGroup);
    m_guiPresetName = new QLineEdit(QSettings().value(QStringLiteral("decor/guiPresetName"), QStringLiteral("Galaxy Map Region")).toString(), decorGroup);
    m_guiActionName->setObjectName(QStringLiteral("decorGuiActionName"));
    m_guiPresetName->setObjectName(QStringLiteral("decorGuiPresetName"));
    m_guiActionName->setEnabled(m_createGuiAction->isChecked());
    m_guiPresetName->setEnabled(m_createGuiAction->isChecked());
    form->addRow(m_createGuiAction);
    form->addRow(tr("GUI action name"), m_guiActionName);
    form->addRow(tr("Region preset name"), m_guiPresetName);
    connect(m_createGuiAction, &QCheckBox::toggled, this, [this](bool enabled) {
        QSettings().setValue(QStringLiteral("decor/createGuiAction"), enabled);
        m_guiActionName->setEnabled(enabled);
        m_guiPresetName->setEnabled(enabled);
    });
    connect(m_guiActionName, &QLineEdit::editingFinished, this, [this] {
        QSettings().setValue(QStringLiteral("decor/guiActionName"), m_guiActionName->text());
    });
    connect(m_guiPresetName, &QLineEdit::editingFinished, this, [this] {
        QSettings().setValue(QStringLiteral("decor/guiPresetName"), m_guiPresetName->text());
    });
    decorControls->addLayout(form, 1);

    auto *buttonLayout = new QVBoxLayout();
    m_previewButton = new QPushButton(tr("2. Preview changes"), decorGroup);
    m_createCopyButton = new QPushButton(tr("3. Save converted map copy"), decorGroup);
    m_restoreButton = new QPushButton(tr("Rebuild placements from Galaxy"), decorGroup);
    m_compressButton = new QPushButton(tr("Compress map copy"), decorGroup);
    m_previewButton->setObjectName(QStringLiteral("decorPreviewButton"));
    m_createCopyButton->setObjectName(QStringLiteral("decorCreateCopyButton"));
    m_restoreButton->setObjectName(QStringLiteral("decorRestoreButton"));
    m_compressButton->setObjectName(QStringLiteral("maximumCompatibleCompressionButton"));
    m_previewButton->setMinimumHeight(44);
    m_createCopyButton->setMinimumHeight(44);
    m_restoreButton->setMinimumHeight(44);
    m_compressButton->setMinimumHeight(44);
    m_previewButton->setToolTip(tr("Show decor removed from the copy, items kept static, and generated Galaxy Create functions."));
    m_createCopyButton->setToolTip(tr("Create a separate map copy; the source map is never modified."));
    m_restoreButton->setToolTip(tr("Read XYZ, all three rotation axes, scale, variation and tint from SC2DataHelper Galaxy into native placements in a new map copy. No Objects backup is used."));
    m_compressButton->setToolTip(tr("Create a separate archive using verified MPQ compaction only. Assets and logical entry bytes are not removed or changed."));
    buttonLayout->addWidget(m_previewButton);
    buttonLayout->addWidget(m_createCopyButton);
    buttonLayout->addWidget(m_restoreButton);
    buttonLayout->addWidget(m_compressButton);
    buttonLayout->addStretch(1);
    decorControls->addLayout(buttonLayout);
    decorLayout->addLayout(decorControls);

    m_unitExclusionsButton = new QPushButton(tr("Choose units to keep in Objects..."), decorGroup);
    m_unitExclusionsButton->setObjectName(QStringLiteral("unitExclusionsButton"));
    m_unitExclusionsButton->setToolTip(tr("Search units, keep one unit or every unit of a type in Objects."));
    decorLayout->addWidget(m_unitExclusionsButton);

    auto *doodadLabel = new QLabel(tr("Previewed placements: check Keep in map to exclude an item from conversion. Galaxy creates only unchecked items."), decorGroup);
    doodadLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    doodadLabel->setWordWrap(true);
    decorLayout->addWidget(doodadLabel);

    m_doodadModel = new QStandardItemModel(this);
    m_doodadModel->setHorizontalHeaderLabels({
        tr("Keep in map"),
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
    m_galaxyPreview->setPlaceholderText(tr("Preview shows the exact Objects changes, excluded doodads, and generated Galaxy functions."));
    decorLayout->addWidget(m_galaxyPreview);

    connect(m_previewButton, &QPushButton::clicked, this, &MapPerformancePage::updateDecorPreview);
    connect(m_unitExclusionsButton, &QPushButton::clicked, this, &MapPerformancePage::openUnitExclusions);
    connect(m_createCopyButton, &QPushButton::clicked, this, &MapPerformancePage::createDecorOptimizedMapCopy);
    connect(m_restoreButton, &QPushButton::clicked, this, &MapPerformancePage::restoreDecorFromGalaxyCopy);
    connect(m_compressButton, &QPushButton::clicked, this, &MapPerformancePage::createMaximumCompressedCopy);
    connect(m_doodadModel, &QStandardItemModel::itemChanged, this, &MapPerformancePage::updateDecorPreview);
    connect(m_regionList, &QListWidget::itemChanged, this, &MapPerformancePage::updateOptimizationScope);
    connect(m_regionSearch, &QLineEdit::textChanged, this, &MapPerformancePage::filterRegionList);
    connect(m_showSelectedOnly, &QCheckBox::toggled, this, &MapPerformancePage::filterRegionList);
    const auto focusFirstVisibleRegion = [this] {
        for (int row = 0; row < m_regionList->count(); ++row) {
            if (m_regionList->isRowHidden(row))
                continue;
            const QListWidgetItem *item = m_regionList->item(row);
            if (item->flags().testFlag(Qt::ItemIsEnabled)) {
                static_cast<MapHeatmapWidget *>(m_heatmap)->focusRegion(item->data(Qt::UserRole).toInt());
                m_regionList->scrollToItem(item);
                break;
            }
        }
    };
    connect(m_regionSearch, &QLineEdit::returnPressed, this, focusFirstVisibleRegion);
    connect(m_regionList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (item && item->flags().testFlag(Qt::ItemIsEnabled))
            static_cast<MapHeatmapWidget *>(m_heatmap)->focusRegion(item->data(Qt::UserRole).toInt());
    });
    connect(selectAllRegions, &QPushButton::clicked, this, [this] {
        const QSignalBlocker blocker(m_regionList);
        for (int row = 0; row < m_regionList->count(); ++row) {
            QListWidgetItem *item = m_regionList->item(row);
            if (!m_regionList->isRowHidden(row) && item->flags().testFlag(Qt::ItemIsEnabled))
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
    connect(m_createGuiAction, &QCheckBox::toggled, this, invalidatePreview);
    connect(m_guiActionName, &QLineEdit::textChanged, this, invalidatePreview);
    connect(m_guiPresetName, &QLineEdit::textChanged, this, invalidatePreview);
    updateOptimizationScope();

    auto *decorScroll = new QScrollArea(contentSplitter);
    decorScroll->setObjectName(QStringLiteral("mapPerformanceControlsScroll"));
    decorScroll->setWidgetResizable(true);
    decorScroll->setFrameShape(QFrame::NoFrame);
    decorScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    decorScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    decorScroll->setWidget(decorGroup);
    contentSplitter->addWidget(decorScroll);
    contentSplitter->setStretchFactor(0, 2);
    contentSplitter->setStretchFactor(1, 3);
    contentSplitter->setSizes({360, 460});
    layout->addWidget(contentSplitter, 1);

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
    const quint64 generation = ++m_previewGeneration;
    const AnalysisResult snapshot = m_result;
    m_decorPreview = {};
    m_recreatePreview = {};
    m_minimapImage = {};
    m_minimapSourceLabel.clear();
    sc2dh::preview::MapPreviewData loadingPreview;
    loadingPreview.unavailableReason = tr("Map preview is loading in a worker thread...");
    static_cast<MapHeatmapWidget *>(m_heatmap)->setMapPreview(loadingPreview);
    m_summaryLabel->setText(tr("Loading Regions, Objects, terrain and cached map layers in a worker thread..."));
    m_warningLabel->setText(tr("The source archive remains read-only while Map Performance data is prepared."));
    m_previewButton->setEnabled(false);
    m_createCopyButton->setEnabled(false);
    m_compressButton->setEnabled(false);

    auto *watcher = new QFutureWatcher<PreparedMapPerformance>(this);
    connect(watcher, &QFutureWatcher<PreparedMapPerformance>::finished, this,
            [this, watcher, generation] {
        const PreparedMapPerformance prepared = watcher->result();
        watcher->deleteLater();
        if (generation != m_previewGeneration)
            return;

        m_regionReadResult = prepared.regions;
        populateRegionSelector();
        m_hasObjects = prepared.hasObjects;
        m_objectsBytes = prepared.objectsBytes;
        m_objectsSourceLabel = prepared.objectsSource;
        m_report = prepared.report;
        m_allDoodads = prepared.doodads;
        m_archiveSafetyContext = prepared.safetyContext;
        static_cast<MapHeatmapWidget *>(m_heatmap)->setReport(m_report);
        applyMapPreview(prepared.preview);
        m_compressButton->setEnabled(!sourceArchivePath().isEmpty());

        if (!prepared.hasObjects) {
            m_model->removeRows(0, m_model->rowCount());
            m_decorZones.clear();
            static_cast<MapHeatmapWidget *>(m_heatmap)->setDecorZones({});
            populateDoodadTable({});
            m_summaryLabel->setText(tr("Objects placement file was not found/readable or is larger than %1. Open a map containing the root Objects file. Scanned files: %2.")
                                        .arg(formatBytes(MaxObjectsBytes))
                                        .arg(m_result.scannedFiles.size()));
            m_decorSummaryLabel->setText(QStringLiteral("No readable Objects entry: cannot list decor, assign zones, generate exact actor script, or remove static doodads from a map copy yet."));
            m_galaxyPreview->setPlainText(QStringLiteral(
                "NO OBJECTS ENTRY LOADED\n\n"
                "This mode needs both the map's root Objects file and its real Regions component.\n"));
            if (!prepared.workerError.isEmpty())
                m_warningLabel->setText(QStringLiteral("MAP PREPARATION FAILED: %1").arg(prepared.workerError));
            m_details->clear();
            return;
        }

        populateTable();
        m_summaryLabel->setText(tr("%1 positioned object(s) loaded. Choose exact map Regions, then preview the selected Galaxy decor operation. Source: %2%3")
                                    .arg(m_report.placements.size())
                                    .arg(prepared.objectsSource)
                                    .arg(prepared.preview.image.isNull()
                                             ? QString()
                                             : tr(" | Minimap: %1").arg(prepared.preview.sourceLabel)));
        updateOptimizationScope();
    });
    watcher->setFuture(QtConcurrent::run([snapshot] {
        PreparedMapPerformance prepared;
        try {
            const ScannedFileReader sharedReader(snapshot);
            QByteArray regionsBytes;
            QString regionsSource;
            if (MapPerformancePage::readRegionsFile(snapshot, &regionsBytes, &regionsSource, &sharedReader)) {
                prepared.regions = sc2dh::region::MapRegionRepository().parse(regionsBytes, regionsSource);
            } else {
                prepared.regions.sourceLabel = QStringLiteral("Regions");
                prepared.regions.warnings << QStringLiteral("The map has no readable Regions component.");
            }
            prepared.preview = MapPerformancePage::buildMapPreviewData(snapshot, &sharedReader);
            prepared.hasObjects = MapPerformancePage::readObjectsFile(
                snapshot, &prepared.objectsBytes, &prepared.objectsSource, &sharedReader);
            if (prepared.hasObjects) {
                sc2dh::perf::MapPerformanceOptions options;
                options.columns = 8;
                options.rows = 8;
                options.padding = 0.0;
                prepared.report = sc2dh::perf::MapPerformanceAnalyzer().buildReport(
                    prepared.objectsBytes, snapshot, options);
                prepared.doodads = sc2dh::decor::DecorationStreamingPlanner().parseObjects(
                    prepared.objectsBytes);
                if (QFileInfo(snapshot.rootFolder).isFile() && ScannedFileReader::isArchivePath(snapshot.rootFolder)) {
                    sc2dh::decor::DecorOptimizedMapRequest request;
                    request.sourceArchivePath = snapshot.rootFolder;
                    QString safetyError;
                    prepared.safetyContext = sc2dh::decor::DecorationMapCopyService().inspectSafety(request, &safetyError);
                    if (!safetyError.isEmpty()) {
                        prepared.workerError = safetyError;
                        for (const auto &doodad : prepared.doodads)
                            prepared.safetyContext.staticOnlyReasonByDoodadType.insert(doodad.type.toCaseFolded(),
                                QStringLiteral("Static-only: archive safety scan failed: %1").arg(safetyError));
                        for (const auto &unit : sc2dh::decor::DecorationStreamingPlanner().parseUnits(prepared.objectsBytes))
                            prepared.safetyContext.excludedUnitKeys.insert(unit.id.toCaseFolded());
                    }
                }
            }
        } catch (const std::exception &exception) {
            prepared.workerError = QString::fromUtf8(exception.what());
        } catch (...) {
            prepared.workerError = QStringLiteral("Unknown Map Performance worker exception.");
        }
        return prepared;
    }));
}

bool MapPerformancePage::readObjectsFile(const AnalysisResult &result,
                                         QByteArray *objectsBytes,
                                         QString *sourceLabel,
                                         const ScannedFileReader *sharedReader)
{
    if (!objectsBytes)
        return false;
    const std::unique_ptr<ScannedFileReader> ownedReader = sharedReader
        ? nullptr : std::make_unique<ScannedFileReader>(result);
    const ScannedFileReader &reader = sharedReader ? *sharedReader : *ownedReader;
    for (const ScannedFileInfo &file : result.scannedFiles) {
        if (!isObjectsEntry(result.rootFolder, file))
            continue;
        QByteArray bytes;
        if (!reader.readBytes(file, MaxObjectsBytes, &bytes))
            return false;
        *objectsBytes = bytes;
        if (sourceLabel)
            *sourceLabel = ScannedFileReader::relativePath(result.rootFolder, file.filePath);
        return true;
    }
    return false;
}

bool MapPerformancePage::readRegionsFile(const AnalysisResult &result,
                                         QByteArray *regionsBytes,
                                         QString *sourceLabel,
                                         const ScannedFileReader *sharedReader)
{
    if (!regionsBytes)
        return false;
    const std::unique_ptr<ScannedFileReader> ownedReader = sharedReader
        ? nullptr : std::make_unique<ScannedFileReader>(result);
    const ScannedFileReader &reader = sharedReader ? *sharedReader : *ownedReader;
    for (const ScannedFileInfo &file : result.scannedFiles) {
        if (!isRegionsEntry(result.rootFolder, file))
            continue;
        QByteArray bytes;
        if (!reader.readBytes(file, MaxObjectsBytes, &bytes))
            return false;
        *regionsBytes = bytes;
        if (sourceLabel)
            *sourceLabel = ScannedFileReader::relativePath(result.rootFolder, file.filePath);
        return true;
    }
    return false;
}

bool MapPerformancePage::readMinimapImage(const AnalysisResult &result,
                                          QImage *image,
                                          QString *sourceLabel,
                                          const ScannedFileReader *sharedReader)
{
    if (!image)
        return false;
    *image = {};
    if (sourceLabel)
        sourceLabel->clear();

    const std::unique_ptr<ScannedFileReader> ownedReader = sharedReader
        ? nullptr : std::make_unique<ScannedFileReader>(result);
    const ScannedFileReader &reader = sharedReader ? *sharedReader : *ownedReader;
    for (const ScannedFileInfo &file : result.scannedFiles) {
        const QString relative = ScannedFileReader::relativePath(result.rootFolder, file.filePath);
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

bool MapPerformancePage::readPreviewComponent(const AnalysisResult &result,
                                               const QStringList &fileNames,
                                               qint64 maxBytes,
                                               QByteArray *bytes,
                                               QString *sourceLabel,
                                               const ScannedFileReader *sharedReader)
{
    if (!bytes)
        return false;
    bytes->clear();
    if (sourceLabel)
        sourceLabel->clear();
    const std::unique_ptr<ScannedFileReader> ownedReader = sharedReader
        ? nullptr : std::make_unique<ScannedFileReader>(result);
    const ScannedFileReader &reader = sharedReader ? *sharedReader : *ownedReader;
    for (const ScannedFileInfo &file : result.scannedFiles) {
        const QString relative = ScannedFileReader::relativePath(result.rootFolder, file.filePath);
        const QString baseName = QFileInfo(relative).fileName();
        const bool matches = std::any_of(fileNames.cbegin(), fileNames.cend(), [&](const QString &name) {
            return baseName.compare(name, Qt::CaseInsensitive) == 0
                || relative.trimmed().replace('\\', '/').compare(name, Qt::CaseInsensitive) == 0;
        });
        if (!matches)
            continue;
        QByteArray loaded;
        if (!reader.readBytes(file, maxBytes, &loaded))
            continue;
        *bytes = loaded;
        if (sourceLabel)
            *sourceLabel = relative;
        return true;
    }
    return false;
}

sc2dh::preview::MapPreviewData MapPerformancePage::buildMapPreviewData(
    const AnalysisResult &result,
    const ScannedFileReader *sharedReader)
{
    sc2dh::preview::MapPreviewData preview;
    sc2dh::preview::MapPreviewDataReader reader;

    QByteArray terrainXml;
    QString terrainXmlSource;
    sc2dh::preview::TerrainDescriptor terrain;
    if (readPreviewComponent(result, {QStringLiteral("t3Terrain.xml"), QStringLiteral("t3Terrain")},
                             8ll * 1024ll * 1024ll, &terrainXml, &terrainXmlSource, sharedReader)) {
        terrain = reader.parseTerrainXml(terrainXml);
        preview.warnings += terrain.errors;
        if (terrain.complete) {
            preview.worldBounds = terrain.worldBounds;
            preview.exactWorldBounds = true;
        }
    }

    if (!preview.worldBounds.valid) {
        QByteArray mapInfo;
        QString mapInfoSource;
        if (readPreviewComponent(result, {QStringLiteral("MapInfo")}, 16ll * 1024ll * 1024ll,
                                 &mapInfo, &mapInfoSource, sharedReader)) {
            preview.worldBounds = reader.parseMapInfoDimensions(mapInfo, &preview.warnings);
            if (preview.worldBounds.valid)
                preview.warnings << QStringLiteral("APPROXIMATE_BACKGROUND_ALIGNMENT: MapInfo dimensions provide aspect only; terrain origin was unavailable.");
        }
    }

    QImage minimap;
    QString minimapSource;
    if (readMinimapImage(result, &minimap, &minimapSource, sharedReader)) {
        preview.image = minimap;
        preview.sourceLabel = minimapSource;
        preview.terrainBacked = false;
        if (!preview.exactWorldBounds)
            preview.warnings << QStringLiteral("APPROXIMATE_BACKGROUND_ALIGNMENT: minimap is fitted to fallback map bounds.");
        return preview;
    }

    QByteArray heightMap;
    QString heightMapSource;
    if (terrain.complete
        && readPreviewComponent(result, {QStringLiteral("t3HeightMap")}, 256ll * 1024ll * 1024ll,
                                &heightMap, &heightMapSource, sharedReader)) {
        preview.image = reader.renderHeightMap(heightMap, terrain, &preview.warnings);
        if (!preview.image.isNull()) {
            preview.sourceLabel = QStringLiteral("%1 + %2").arg(terrainXmlSource, heightMapSource);
            preview.terrainBacked = true;
            preview.exactWorldBounds = true;
            return preview;
        }
    }

    preview.unavailableReason = preview.warnings.isEmpty()
        ? QStringLiteral("Minimap.tga is absent and the terrain descriptor/heightmap pair is unavailable.")
        : preview.warnings.join(QStringLiteral(" | "));
    return preview;
}

void MapPerformancePage::startMapPreviewLoad()
{
    const AnalysisResult snapshot = m_result;
    const quint64 generation = m_previewGeneration;
    auto *watcher = new QFutureWatcher<sc2dh::preview::MapPreviewData>(this);
    connect(watcher, &QFutureWatcher<sc2dh::preview::MapPreviewData>::finished, this,
            [this, watcher, generation] {
        const sc2dh::preview::MapPreviewData preview = watcher->result();
        watcher->deleteLater();
        if (generation != m_previewGeneration)
            return;
        applyMapPreview(preview);
    });
    watcher->setFuture(QtConcurrent::run([snapshot] {
        try {
            return MapPerformancePage::buildMapPreviewData(snapshot);
        } catch (const std::exception &exception) {
            sc2dh::preview::MapPreviewData preview;
            preview.unavailableReason = QStringLiteral("Preview worker exception: %1")
                                            .arg(QString::fromUtf8(exception.what()));
            return preview;
        } catch (...) {
            sc2dh::preview::MapPreviewData preview;
            preview.unavailableReason = QStringLiteral("Unknown preview worker exception.");
            return preview;
        }
    }));
}

void MapPerformancePage::applyMapPreview(const sc2dh::preview::MapPreviewData &preview)
{
    m_minimapImage = preview.image;
    m_minimapSourceLabel = preview.sourceLabel;
    static_cast<MapHeatmapWidget *>(m_heatmap)->setMapPreview(preview);

    QString warning = tr("Region outlines use the map's exact circle, polygon, rectangle or composite geometry. The source archive is read-only.");
    if (preview.image.isNull())
        warning += tr(" MAP PREVIEW UNAVAILABLE: %1").arg(preview.unavailableReason);
    else if (preview.terrainBacked)
        warning += tr(" Terrain-backed preview: %1.").arg(preview.sourceLabel);
    else
        warning += tr(" Map preview: %1.").arg(preview.sourceLabel);
    if (!preview.exactWorldBounds)
        warning += tr(" APPROXIMATE_BACKGROUND_ALIGNMENT; spatial classification still uses world coordinates.");
    if (!preview.warnings.isEmpty())
        warning += QStringLiteral(" Preview warnings: %1").arg(preview.warnings.join(QStringLiteral(" | ")));
    if (!m_report.warnings.isEmpty())
        warning += QStringLiteral(" Warnings: %1").arg(m_report.warnings.join(QStringLiteral(" | ")));
    m_warningLabel->setText(warning);
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
    QSet<int> usedIds;
    int generatedId = 1;
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
        bool idOk = false;
        zone.id = region.id.toInt(&idOk);
        if (!idOk || zone.id <= 0 || usedIds.contains(zone.id)) {
            while (usedIds.contains(generatedId))
                ++generatedId;
            zone.id = generatedId++;
        }
        usedIds.insert(zone.id);
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
        item->setToolTip(tr("Region #%1\n%2\nExact geometry is used for eligible doodads and units inside it.")
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
    filterRegionList();
    updateOptimizationScope();
}

void MapPerformancePage::filterRegionList()
{
    if (!m_regionList || !m_regionSearch || !m_showSelectedOnly)
        return;
    const QString query = m_regionSearch->text().trimmed();
    const QString idQuery = query.startsWith(QLatin1Char('#')) ? query.mid(1) : query;
    int visible = 0;
    int supported = 0;
    for (int row = 0; row < m_regionList->count(); ++row) {
        const QListWidgetItem *item = m_regionList->item(row);
        const int index = item->data(Qt::UserRole).toInt();
        const auto &region = m_regionReadResult.regions.at(index);
        if (region.geometry.supported)
            ++supported;
        const bool nameMatches = query.isEmpty() || region.name.contains(query, Qt::CaseInsensitive);
        const bool idMatches = !idQuery.isEmpty() && region.id.contains(idQuery, Qt::CaseInsensitive);
        const bool show = (nameMatches || idMatches)
            && (!m_showSelectedOnly->isChecked() || item->checkState() == Qt::Checked);
        m_regionList->setRowHidden(row, !show);
        visible += show ? 1 : 0;
    }
    if (m_regionReadResult.success) {
        m_regionStatusLabel->setText(tr("Regions: %1 total, %2 usable, %3 shown")
                                         .arg(m_regionList->count()).arg(supported).arg(visible));
        m_regionStatusLabel->setToolTip(m_regionReadResult.sourceLabel);
    }
}

void MapPerformancePage::updateOptimizationScope()
{
    if (!m_regionList)
        return;
    const QVector<sc2dh::decor::DecorZone> zones = selectedRegionZones();
    m_decorZones = zones;
    static_cast<MapHeatmapWidget *>(m_heatmap)->setDecorZones(zones);

    QSet<int> selectedIndices;
    for (int row = 0; row < m_regionList->count(); ++row) {
        const QListWidgetItem *item = m_regionList->item(row);
        if (item->checkState() == Qt::Checked)
            selectedIndices.insert(item->data(Qt::UserRole).toInt());
    }
    static_cast<MapHeatmapWidget *>(m_heatmap)->setSelectedRegionIndices(selectedIndices);
    QStringList selectedNames;
    for (const auto &zone : zones)
        selectedNames << QStringLiteral("%1 [#%2]").arg(zone.name).arg(zone.id);
    const QString fullSelection = selectedNames.join(QStringLiteral(", "));
    if (m_selectedRegionsLabel) {
        QStringList displayNames;
        for (const QString &name : selectedNames.mid(0, 2))
            displayNames << (name.size() > 42 ? name.left(39) + QStringLiteral("...") : name);
        const QString shortSelection = displayNames.join(QStringLiteral(", "));
        m_selectedRegionsLabel->setText(zones.isEmpty()
            ? tr("Selected: none")
            : tr("Selected: %1 — %2%3").arg(zones.size()).arg(shortSelection)
                  .arg(selectedNames.size() > 2 ? tr(" and %1 more").arg(selectedNames.size() - 2) : QString()));
        m_selectedRegionsLabel->setToolTip(fullSelection);
    }
    filterRegionList();

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
    m_recreatePreview = {};
    m_createCopyButton->setEnabled(false);
    m_previewButton->setEnabled(m_hasObjects && !zones.isEmpty());
    if (zones.isEmpty()) {
        m_decorSummaryLabel->setText(tr("No Region selected. Click an outline on the map or check a Region in the list."));
        m_galaxyPreview->setPlainText(tr("SELECT REGIONS\n\nOnly checked Regions will be converted. The source map will stay unchanged.\nClick Preview changes to see each item before saving a copy."));
    } else {
        m_decorSummaryLabel->setText(tr("%1 Region(s) selected: %2 decor candidates, %3 kept in Objects, %4 on a boundary. Preview before saving a copy.")
                                         .arg(zones.size())
                                         .arg(dynamicInside)
                                         .arg(staticInside)
                                         .arg(boundary));
        m_galaxyPreview->setPlainText(tr("SELECTION READY\n\n%1 Region(s) selected; %2 decor candidates are inside them.\n\nClick Preview changes to review placements and the Create_<RegionName>() calls.")
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
        if (m_regionList->isRowHidden(row)) {
            m_regionSearch->clear();
            m_showSelectedOnly->setChecked(false);
        }
        m_regionList->scrollToItem(item, QAbstractItemView::PositionAtCenter);
        return;
    }
}

sc2dh::decor::DecorationSafetyContext MapPerformancePage::decorationSafetyContextFromDoodadTable() const
{
    sc2dh::decor::DecorationSafetyContext context = m_archiveSafetyContext;
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
        if (excludeItem->data(Qt::UserRole + 11).toBool()) {
            if (excluded) {
                // The visible fallback name "Unit" is shared by many unnamed
                // placements. An individual selection must only exclude its ID.
                const QString key = doodadOverrideKey(idItem ? idItem->text() : QString());
                if (!key.isEmpty())
                    context.excludedUnitKeys.insert(key);
            }
            continue;
        }
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

void MapPerformancePage::openUnitExclusions()
{
    if (!m_hasObjects || m_decorZones.isEmpty()) {
        QMessageBox::information(this, tr("Unit exclusions"),
                                 tr("Open a map and select at least one Region first."));
        return;
    }
    if (m_doodadModel->rowCount() == 0)
        updateDecorPreview();

    const QVector<sc2dh::decor::UnitPlacement> sourceUnits =
        sc2dh::decor::DecorationStreamingPlanner().parseUnits(m_objectsBytes);
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Units to keep in Objects"));
    dialog.resize(940, 620);
    auto *layout = new QVBoxLayout(&dialog);
    auto *explanation = new QLabel(tr("Check a unit or its type to keep it in the map. "
                                      "Units marked 'Already kept' cannot currently be recreated safely. "
                                      "A type checkbox applies to all its units, including hidden search results."), &dialog);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto *search = new QLineEdit(&dialog);
    search->setPlaceholderText(tr("Search by type, name, ID or Region..."));
    layout->addWidget(search);
    auto *tree = new QTreeWidget(&dialog);
    tree->setHeaderLabels({tr("Keep"), tr("Type / unit"), tr("Region"), tr("Current action")});
    tree->setRootIsDecorated(true);
    tree->setAlternatingRowColors(true);
    tree->setUniformRowHeights(true);
    layout->addWidget(tree, 1);

    QMap<QString, QTreeWidgetItem *> groups;
    int editableCount = 0;
    int alreadyKeptCount = 0;
    for (int row = 0; row < m_doodadModel->rowCount(); ++row) {
        const QStandardItem *exclude = m_doodadModel->item(row, DecorDoodadExcludeColumn);
        if (!exclude || !exclude->data(Qt::UserRole + 11).toBool())
            continue;
        const int index = exclude->data(Qt::UserRole + 10).toInt();
        if (index < 0 || index >= sourceUnits.size())
            continue;
        const auto &unit = sourceUnits.at(index);
        QTreeWidgetItem *group = groups.value(unit.type, nullptr);
        if (!group) {
            group = new QTreeWidgetItem(tree);
            group->setText(1, unit.type);
            group->setFlags(group->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
            group->setCheckState(0, Qt::Unchecked);
            groups.insert(unit.type, group);
        }
        QString regionName;
        double smallestArea = std::numeric_limits<double>::infinity();
        for (const auto &zone : m_decorZones) {
            if (!zone.geometry.supported
                || zone.geometry.classify(unit.x, unit.y) != sc2dh::region::SpatialRelation::Inside)
                continue;
            const double area = std::abs((zone.xMax - zone.xMin) * (zone.yMax - zone.yMin));
            if (area < smallestArea) {
                smallestArea = area;
                regionName = zone.name;
            }
        }
        auto *child = new QTreeWidgetItem(group);
        child->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        child->setData(0, Qt::UserRole, row);
        child->setText(1, unit.name.isEmpty()
                              ? QStringLiteral("%1  ·  ID %2").arg(unit.type, unit.id)
                              : QStringLiteral("%1  ·  ID %2").arg(unit.name, unit.id));
        child->setText(2, regionName);
        if (sourceUnits.at(index).dynamicCandidate) {
            child->setFlags(child->flags() | Qt::ItemIsUserCheckable);
            child->setCheckState(0, exclude->checkState());
            child->setText(3, exclude->checkState() == Qt::Checked
                                  ? tr("Keep in Objects") : tr("Recreate in Galaxy"));
            ++editableCount;
        } else {
            child->setText(0, tr("Already kept"));
            child->setText(3, unit.staticOnlyReason);
            ++alreadyKeptCount;
        }
    }
    tree->sortItems(1, Qt::AscendingOrder);
    tree->expandAll();
    for (int column = 0; column < 4; ++column)
        tree->resizeColumnToContents(column);
    auto *count = new QLabel(tr("%1 selectable unit(s); %2 already kept by safety rules.")
                                 .arg(editableCount).arg(alreadyKeptCount), &dialog);
    layout->addWidget(count);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(search, &QLineEdit::textChanged, &dialog, [tree](const QString &query) {
        for (int groupIndex = 0; groupIndex < tree->topLevelItemCount(); ++groupIndex) {
            QTreeWidgetItem *group = tree->topLevelItem(groupIndex);
            bool anyVisible = false;
            for (int childIndex = 0; childIndex < group->childCount(); ++childIndex) {
                QTreeWidgetItem *child = group->child(childIndex);
                const bool visible = query.isEmpty()
                    || group->text(1).contains(query, Qt::CaseInsensitive)
                    || child->text(1).contains(query, Qt::CaseInsensitive)
                    || child->text(2).contains(query, Qt::CaseInsensitive);
                child->setHidden(!visible);
                anyVisible = anyVisible || visible;
            }
            group->setHidden(!anyVisible);
        }
    });
    if (dialog.exec() != QDialog::Accepted)
        return;
    {
        const QSignalBlocker blocker(m_doodadModel);
        for (QTreeWidgetItem *group : groups) {
            for (int childIndex = 0; childIndex < group->childCount(); ++childIndex) {
                QTreeWidgetItem *child = group->child(childIndex);
                if (!child->flags().testFlag(Qt::ItemIsUserCheckable))
                    continue;
                const int row = child->data(0, Qt::UserRole).toInt();
                if (QStandardItem *item = m_doodadModel->item(row, DecorDoodadExcludeColumn))
                    item->setCheckState(child->checkState(0));
            }
        }
    }
    updateDecorPreview();
}

void MapPerformancePage::populateDoodadTable(const QVector<sc2dh::decor::DoodadPlacement> &doodads,
                                              const QVector<sc2dh::decor::UnitPlacement> &units)
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
        exclude->setData(false, Qt::UserRole + 11);

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
    for (int unitIndex = 0; unitIndex < units.size(); ++unitIndex) {
        const auto &unit = units.at(unitIndex);
        bool inSelectedRegion = false;
        for (const auto &zone : m_decorZones) {
            if (zone.geometry.supported
                && zone.geometry.classify(unit.x, unit.y) != sc2dh::region::SpatialRelation::Outside) {
                inSelectedRegion = true;
                break;
            }
        }
        if (!inSelectedRegion)
            continue;
        auto *exclude = new QStandardItem();
        exclude->setCheckable(true);
        exclude->setCheckState(previousOverrides.excludedUnitKeys.contains(doodadOverrideKey(unit.id))
                                   || previousOverrides.excludedUnitKeys.contains(doodadOverrideKey(unit.name))
                                   ? Qt::Checked : Qt::Unchecked);
        exclude->setEditable(false);
        exclude->setData(unitIndex, Qt::UserRole + 10);
        exclude->setData(true, Qt::UserRole + 11);
        auto *zone = new QStandardItem();
        zone->setEditable(false);
        auto *name = new QStandardItem(unit.name.isEmpty() ? QStringLiteral("Unit") : unit.name);
        auto *id = new QStandardItem(unit.id);
        auto *type = new QStandardItem(unit.type);
        auto *position = new QStandardItem(QStringLiteral("%1, %2")
                                               .arg(unit.x, 0, 'f', 2).arg(unit.y, 0, 'f', 2));
        auto *state = new QStandardItem(unit.dynamicCandidate ? QStringLiteral("Unit candidate")
                                                               : unit.staticOnlyReason);
        for (QStandardItem *item : {name, id, type, position, state})
            item->setEditable(false);
        m_doodadModel->appendRow({exclude, zone, name, id, type, position, state});
    }
    m_doodadTable->resizeColumnsToContents();
}

void MapPerformancePage::updateDoodadTableState(const sc2dh::decor::DecorationVisibilityPlan &plan)
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
            state->setText(QStringLiteral("Keep in Objects; hide/restore actor in zone %1").arg(zoneByDoodadIndex.value(doodadIndex)));
        } else if (unassigned.contains(doodadIndex)) {
            state->setText(QStringLiteral("Not inside a selected Region; stays unchanged"));
        } else {
            const QString reason = plan.doodads.at(doodadIndex).visibilityStaticOnlyReason;
            state->setText(reason.isEmpty() ? QStringLiteral("Keep static in Objects") : reason);
        }
    }
    m_doodadTable->resizeColumnsToContents();
}

void MapPerformancePage::updateDoodadTableState(const sc2dh::decor::DecorationStreamingPlan &plan)
{
    if (!m_doodadModel)
        return;
    const QSignalBlocker blocker(m_doodadModel);
    QHash<int, int> zoneByDoodadIndex;
    QHash<int, int> zoneByUnitIndex;
    for (const auto &assignment : plan.zones) {
        for (int index : assignment.doodadIndices)
            zoneByDoodadIndex.insert(index, assignment.zoneId);
        for (int index : assignment.unitIndices)
            zoneByUnitIndex.insert(index, assignment.zoneId);
    }
    const QSet<int> unassigned(plan.unassignedDoodads.cbegin(), plan.unassignedDoodads.cend());
    for (int row = 0; row < m_doodadModel->rowCount(); ++row) {
        const auto *indexItem = m_doodadModel->item(row, DecorDoodadExcludeColumn);
        auto *state = m_doodadModel->item(row, DecorDoodadStateColumn);
        if (!indexItem || !state)
            continue;
        const int index = indexItem->data(Qt::UserRole + 10).toInt();
        if (indexItem->data(Qt::UserRole + 11).toBool()) {
            if (index < 0 || index >= plan.units.size())
                continue;
            if (zoneByUnitIndex.contains(index))
                state->setText(QStringLiteral("Remove unit from copy; recreate in Region %1")
                                   .arg(zoneByUnitIndex.value(index)));
            else if (plan.unassignedUnits.contains(index))
                state->setText(QStringLiteral("Outside selected Regions; keep in Objects"));
            else
                state->setText(plan.units.at(index).staticOnlyReason.isEmpty()
                                   ? QStringLiteral("Keep unit in Objects")
                                   : plan.units.at(index).staticOnlyReason);
            continue;
        }
        if (index < 0 || index >= plan.doodads.size())
            continue;
        if (zoneByDoodadIndex.contains(index))
            state->setText(QStringLiteral("Remove from copy; recreate in Galaxy zone %1")
                               .arg(zoneByDoodadIndex.value(index)));
        else if (unassigned.contains(index))
            state->setText(QStringLiteral("Outside selected Regions; keep in Objects"));
        else
            state->setText(plan.doodads.at(index).staticOnlyReason.isEmpty()
                               ? QStringLiteral("Keep static in Objects")
                               : plan.doodads.at(index).staticOnlyReason);
    }
    m_doodadTable->resizeColumnsToContents();
}

void MapPerformancePage::updateDecorPreview()
{
    if (!m_hasObjects) {
        m_decorPreview = {};
        m_recreatePreview = {};
        static_cast<MapHeatmapWidget *>(m_heatmap)->setDecorPlan(sc2dh::decor::DecorationVisibilityPlan{});
        m_decorSummaryLabel->setText(QStringLiteral("No readable Objects entry: open a map archive or extracted map folder with Objects first."));
        m_galaxyPreview->setPlainText(QStringLiteral(
            "NO OBJECTS ENTRY LOADED\n\n"
            "Open a .SC2Map/.SC2Mod archive or extracted map folder with the root Objects file.\n"
            "After that this panel can move eligible decor into Galaxy Create functions.\n"));
        m_createCopyButton->setEnabled(false);
        return;
    }

    m_decorZones = zonesFromModel();
    static_cast<MapHeatmapWidget *>(m_heatmap)->setDecorZones(m_decorZones);
    if (m_decorZones.isEmpty()) {
        m_decorPreview = {};
        m_recreatePreview = {};
        static_cast<MapHeatmapWidget *>(m_heatmap)->setDecorPlan(sc2dh::decor::DecorationVisibilityPlan{});
        m_decorSummaryLabel->setText(tr("No real map Region selected. Click an exact outlined Region on the map or click its row in the list."));
        m_galaxyPreview->setPlainText(QStringLiteral(
            "SELECT A MAP REGION FIRST\n\n"
            "Only exact Regions loaded from the map are accepted.\n"
            "Then Preview will show:\n"
            "- one Create_<RegionName>() function per selected Region;\n"
            "- which doodads can move to Galaxy;\n"
            "- generated Create functions;\n"
            "- full generated Galaxy script.\n"));
        m_createCopyButton->setEnabled(false);
        return;
    }

    sc2dh::decor::GalaxyGenerationOptions options;
    options.functionPrefix = m_prefixEdit->text().trimmed().isEmpty()
        ? QStringLiteral("Game_area")
        : m_prefixEdit->text().trimmed();
    options.useRegionNames = true;

    const sc2dh::decor::DecorationSafetyContext safetyContext = decorationSafetyContextFromDoodadTable();
    m_decorPreview = {};
    m_recreatePreview = sc2dh::decor::DecorationStreamingPlanner().createOptimizedArtifacts(
            m_objectsBytes, m_decorZones, safetyContext, options);
        const auto &plan = m_recreatePreview.plan;
        static_cast<MapHeatmapWidget *>(m_heatmap)->setDecorPlan(plan);
        populateDoodadTable(plan.doodads, plan.units);
        updateDoodadTableState(plan);
        const QString example = plan.zones.isEmpty() ? QStringLiteral("Game_area_1")
            : sc2dh::decor::galaxyRegionFunctionStem(plan, plan.zones.first(), options);
        QString summary = tr("Preview: %2 of %1 decor and %4 of %3 units move to Galaxy. All other items remain in Objects. Call Create_%5(); from your GUI trigger to spawn this Region.")
                              .arg(plan.doodads.size())
                              .arg(m_recreatePreview.removedDoodadIndices.size())
                              .arg(plan.units.size())
                              .arg(m_recreatePreview.removedUnitIndices.size())
                              .arg(example);
        if (!m_recreatePreview.warnings.isEmpty())
            summary += QStringLiteral(" Warnings: %1").arg(m_recreatePreview.warnings.join(QStringLiteral(" | ")));
        if (sourceArchivePath().isEmpty())
            summary += QStringLiteral(" Creating a copy requires an opened map archive.");
        summary += QStringLiteral(" Archive creation re-scans scripts/triggers and can keep more doodads static.");
        m_decorSummaryLabel->setText(summary);
        m_galaxyPreview->setPlainText(decorationConversionPreviewText(m_recreatePreview, m_decorZones, options));
        if (m_createGuiAction->isChecked()) {
            QString guiPreview = QStringLiteral("\n\nGUI Action Definition: %1\nRegion preset: %2\nSwitch cases:\n")
                                     .arg(m_guiActionName->text().trimmed(), m_guiPresetName->text().trimmed());
            for (const auto &zone : plan.zones)
                guiPreview += QStringLiteral("  %1 -> Create_%2();\n").arg(zone.zoneName,
                    sc2dh::decor::galaxyRegionFunctionStem(plan, zone, options));
            guiPreview += tr("The Galaxy file is imported and its GUI/script connection is added when missing.");
            m_galaxyPreview->appendPlainText(guiPreview);
        }
        m_createCopyButton->setEnabled(!sourceArchivePath().isEmpty()
                                       && m_recreatePreview.valid
                                       && (!m_createGuiAction->isChecked() || (!m_guiActionName->text().trimmed().isEmpty()
                                                                            && !m_guiPresetName->text().trimmed().isEmpty()))
                                       && (!m_recreatePreview.removedDoodadIndices.isEmpty()
                                           || !m_recreatePreview.removedUnitIndices.isEmpty()));
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
    return QDir(info.absolutePath()).absoluteFilePath(
        QStringLiteral("%1_DecorGalaxy.%2").arg(info.completeBaseName(), suffix));
}

void MapPerformancePage::createDecorOptimizedMapCopy()
{
    const QString title = tr("Save converted map copy");
    const QString source = sourceArchivePath();
    if (source.isEmpty()) {
        QMessageBox::warning(this,
                             title,
                             QStringLiteral("This action currently requires an opened .SC2Map/.SC2Mod archive source. The original map will not be modified."));
        return;
    }

    const QVector<sc2dh::decor::DecorZone> zones = zonesFromModel();
    if (zones.isEmpty()) {
        QMessageBox::warning(this,
                             title,
                             tr("Select at least one map Region first."));
        return;
    }

    const QString selectedOutput = QFileDialog::getSaveFileName(this,
                                                                title,
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
        ? QStringLiteral("Game_area")
        : m_prefixEdit->text().trimmed();
    request.galaxyOptions.useRegionNames = true;
    request.safetyContext = decorationSafetyContextFromDoodadTable();
    request.mode = sc2dh::decor::DecorationOptimizationMode::RecreateActors;
    request.overwriteExisting = overwrite;
    request.writeRestoreSidecars = m_restoreSidecars->isChecked();
    request.guiAction.enabled = m_createGuiAction->isChecked();
    request.guiAction.actionName = m_guiActionName->text().trimmed();
    request.guiAction.presetName = m_guiPresetName->text().trimmed();
    const int selectedCount = m_recreatePreview.plan.doodads.size() + m_recreatePreview.plan.units.size();
    const int skippedCount = m_recreatePreview.plan.unassignedDoodads.size()
        + m_recreatePreview.plan.unassignedUnits.size();
    const int blockedCount = m_recreatePreview.plan.staticOnlyDoodads.size()
        + m_recreatePreview.plan.staticOnlyUnits.size();

    m_createCopyButton->setEnabled(false);
    m_previewButton->setEnabled(false);
    m_compressButton->setEnabled(false);
    m_decorSummaryLabel->setText(tr("Creating and freshly verifying the optimized copy in a worker thread…"));
    auto *watcher = new QFutureWatcher<sc2dh::decor::DecorOptimizedMapResult>(this);
    connect(watcher, &QFutureWatcher<sc2dh::decor::DecorOptimizedMapResult>::finished, this,
            [this, watcher, selectedOutput, selectedCount, skippedCount, blockedCount] {
        const sc2dh::decor::DecorOptimizedMapResult result = watcher->result();
        watcher->deleteLater();
        m_previewButton->setEnabled(true);
        m_compressButton->setEnabled(!sourceArchivePath().isEmpty());
        updateDecorPreview();
        if (!result.success) {
            m_decorSummaryLabel->setText(QStringLiteral("Create copy failed: %1").arg(result.error));
            m_galaxyPreview->appendPlainText(QStringLiteral("\n\nSAVE ERROR\nSource unchanged: %1\nReason: %2")
                                                 .arg(result.sourceUnchanged ? QStringLiteral("yes") : QStringLiteral("not proven"),
                                                      result.error));
            OperationResult operation;
            operation.outcome = OperationOutcome::Failed;
            operation.errorCode = OperationErrorCode::SaveFailed;
            operation.title = tr("Create decor-optimized map copy");
            operation.summary = tr("No unverified output was committed. Source integrity is reported separately.");
            operation.outputPath = selectedOutput;
            operation.originalChanged = !result.sourceUnchanged;
            operation.error = result.error;
            operation.details = result.warnings;
            emit operationFinished(operation);
            return;
        }

        QString message = QStringLiteral("Created structurally verified map copy:\n%1\n\nRemoved doodads: %2\nRemoved units: %3\nFull re-analysis: %4 file(s), %5 data node(s).\nOriginal SHA-256 unchanged: %6")
                              .arg(QDir::toNativeSeparators(result.outputArchivePath))
                              .arg(result.removedDoodads)
                              .arg(result.removedUnits)
                              .arg(result.verifiedScannedFiles)
                              .arg(result.verifiedDataNodes)
                              .arg(result.sourceUnchanged ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
        if (!result.warnings.isEmpty())
            message += QStringLiteral("\n\nWarnings:\n%1").arg(result.warnings.join(QStringLiteral("\n")));
        m_decorSummaryLabel->setText(message);
        OperationResult operation;
        operation.outcome = OperationOutcome::Succeeded;
        operation.title = tr("Create decor-optimized map copy");
        operation.selected = selectedCount;
        operation.applied = result.removedDoodads + result.removedUnits;
        operation.skipped = skippedCount;
        operation.blocked = blockedCount;
        operation.outputPath = result.outputArchivePath;
        operation.originalChanged = false;
        operation.details = result.warnings;
        emit operationFinished(operation);
    });
    watcher->setFuture(QtConcurrent::run([request] {
        try {
            return sc2dh::decor::DecorationMapCopyService().createOptimizedCopy(request);
        } catch (const std::exception &exception) {
            sc2dh::decor::DecorOptimizedMapResult result;
            result.outputArchivePath = request.outputArchivePath;
            result.error = QStringLiteral("Worker exception: %1").arg(QString::fromUtf8(exception.what()));
            return result;
        } catch (...) {
            sc2dh::decor::DecorOptimizedMapResult result;
            result.outputArchivePath = request.outputArchivePath;
            result.error = QStringLiteral("Unknown worker exception.");
            return result;
        }
    }));
}

void MapPerformancePage::restoreDecorFromGalaxyCopy()
{
    const QString source = sourceArchivePath();
    if (source.isEmpty()) {
        QMessageBox::warning(this, tr("Restore from Galaxy"), tr("Open a converted SC2 map archive first."));
        return;
    }
    const QFileInfo info(source);
    const QString suggested = QDir(info.absolutePath()).absoluteFilePath(
        info.completeBaseName() + QStringLiteral("_DecorRestored.") + info.suffix());
    const QString output = QFileDialog::getSaveFileName(
        this, tr("Restore decor and units into a new map copy"), suggested,
        QStringLiteral("SC2 archives (*.SC2Map *.SC2Mod *.SC2Campaign);;All files (*)"));
    if (output.isEmpty())
        return;
    if (QFileInfo::exists(output)) {
        QMessageBox::warning(this, tr("Restore from Galaxy"),
                             tr("Choose a new output file. Restore never overwrites an existing map."));
        return;
    }
    sc2dh::decor::DecorRestoreMapRequest request{source, output};
    m_restoreButton->setEnabled(false);
    m_decorSummaryLabel->setText(tr("Verifying the Galaxy export and restoring Objects in a worker thread..."));
    auto *watcher = new QFutureWatcher<sc2dh::decor::DecorRestoreMapResult>(this);
    connect(watcher, &QFutureWatcher<sc2dh::decor::DecorRestoreMapResult>::finished, this,
            [this, watcher] {
        const auto result = watcher->result();
        watcher->deleteLater();
        m_restoreButton->setEnabled(true);
        OperationResult operation;
        operation.title = tr("Restore decor and units from Galaxy copy");
        operation.outputPath = result.outputArchivePath;
        operation.originalChanged = false;
        if (result.success) {
            operation.outcome = OperationOutcome::Succeeded;
            operation.applied = result.restoredDoodads + result.restoredUnits;
            m_decorSummaryLabel->setText(tr("Restored %1 doodad(s) and %2 unit(s) into %3. Source unchanged.")
                                              .arg(result.restoredDoodads).arg(result.restoredUnits)
                                              .arg(QDir::toNativeSeparators(result.outputArchivePath)));
        } else {
            operation.outcome = OperationOutcome::Failed;
            operation.errorCode = OperationErrorCode::SaveFailed;
            operation.error = result.error;
            m_decorSummaryLabel->setText(tr("Restore failed: %1").arg(result.error));
        }
        emit operationFinished(operation);
    });
    watcher->setFuture(QtConcurrent::run([request] {
        return sc2dh::decor::DecorationMapCopyService().restoreGalaxyCopy(request);
    }));
}

void MapPerformancePage::createMaximumCompressedCopy()
{
    const QString source = sourceArchivePath();
    if (source.isEmpty()) {
        QMessageBox::warning(this, tr("Maximum Compatible Compression"),
                             tr("Open a packed .SC2Map or .SC2Mod first."));
        return;
    }
    const QFileInfo info(source);
    const QString suffix = info.suffix().isEmpty() ? QStringLiteral("SC2Map") : info.suffix();
    const QString suggested = QDir(info.absolutePath()).absoluteFilePath(
        info.completeBaseName() + QStringLiteral("_MaximumCompressed.") + suffix);
    const QString output = QFileDialog::getSaveFileName(
        this, tr("Maximum Compatible Compression"), suggested,
        tr("SC2 archives (*.SC2Map *.SC2Mod *.SC2Campaign);;All files (*)"));
    if (output.isEmpty())
        return;
    if (QFileInfo::exists(output)) {
        QMessageBox::warning(this, tr("Maximum Compatible Compression"),
                             tr("The output already exists. Choose a new, separately named file; compression never overwrites it."));
        return;
    }

    const QStorageInfo storage(QFileInfo(output).absolutePath());
    const QString preflight = tr(
        "Source: %1\nOutput: %2\nSource bytes: %3\nAvailable bytes: %4\n\n"
        "Logical operation: archive compaction only. No textures, sounds, models, strings, previews, metadata, or other assets are removed. "
        "Every logical entry will be compared before the output is committed. Editor acceptance remains NOT RUN until the real Editor check.")
        .arg(QDir::toNativeSeparators(source), QDir::toNativeSeparators(output))
        .arg(info.size())
        .arg(storage.bytesAvailable());
    if (QMessageBox::information(this, tr("Maximum Compatible Compression — preflight"), preflight,
                                 QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Ok) {
        return;
    }

    sc2dh::compression::ArchiveCompressionRequest request;
    request.sourceArchivePath = source;
    request.outputArchivePath = output;
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    request.isCancelled = [cancelled] { return cancelled->load(std::memory_order_relaxed); };
    auto *progress = new QProgressDialog(tr("Compacting and verifying logical archive entries..."),
                                         tr("Cancel"), 0, 0, this);
    progress->setObjectName(QStringLiteral("maximumCompressionProgress"));
    progress->setWindowModality(Qt::NonModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    connect(progress, &QProgressDialog::canceled, this, [cancelled] {
        cancelled->store(true, std::memory_order_relaxed);
    });
    progress->show();
    m_compressButton->setEnabled(false);
    m_createCopyButton->setEnabled(false);
    m_previewButton->setEnabled(false);
    m_decorSummaryLabel->setText(tr("Compacting and comparing every logical archive entry in a worker thread…"));
    auto *watcher = new QFutureWatcher<sc2dh::compression::ArchiveCompressionResult>(this);
    connect(watcher, &QFutureWatcher<sc2dh::compression::ArchiveCompressionResult>::finished, this,
            [this, watcher, progress] {
        const auto result = watcher->result();
        watcher->deleteLater();
        progress->close();
        progress->deleteLater();
        m_compressButton->setEnabled(!sourceArchivePath().isEmpty());
        m_previewButton->setEnabled(true);
        updateDecorPreview();
        const QString summary = tr(
            "Compression result: %1\nOriginal unchanged: %2\nSource bytes: %3\nOutput bytes: %4\nBytes saved: %5 (%6%)\n"
            "Structural verification: %7\nLogical-entry equality: %8\nEditor acceptance: %9%10")
            .arg(result.status, result.sourceUnchanged ? tr("yes") : tr("no"))
            .arg(result.sourceBytes)
            .arg(result.outputBytes)
            .arg(result.savedBytes)
            .arg(result.savedPercent, 0, 'f', 2)
            .arg(result.structuralVerification ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                 result.logicalEntryEquality ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                 result.editorAcceptance,
                 result.error.isEmpty() ? QString() : QStringLiteral("\n") + result.error);
        m_decorSummaryLabel->setText(summary);
        OperationResult operation;
        operation.outcome = result.success ? OperationOutcome::Succeeded : OperationOutcome::Failed;
        operation.errorCode = result.success ? OperationErrorCode::None : OperationErrorCode::SaveFailed;
        operation.title = tr("Maximum Compatible Compression");
        operation.summary = summary;
        operation.outputPath = result.outputArchivePath;
        operation.originalChanged = !result.sourceUnchanged;
        operation.error = result.error;
        operation.details = result.warnings;
        emit operationFinished(operation);
    });
    watcher->setFuture(QtConcurrent::run([request] {
        try {
            return sc2dh::compression::ArchiveCompressionService().compressCompatibleCopy(request);
        } catch (const std::exception &exception) {
            sc2dh::compression::ArchiveCompressionResult result;
            result.outputArchivePath = request.outputArchivePath;
            result.status = QStringLiteral("BLOCKED_WORKER_EXCEPTION");
            result.error = QStringLiteral("Worker exception: %1").arg(QString::fromUtf8(exception.what()));
            return result;
        } catch (...) {
            sc2dh::compression::ArchiveCompressionResult result;
            result.outputArchivePath = request.outputArchivePath;
            result.status = QStringLiteral("BLOCKED_WORKER_EXCEPTION");
            result.error = QStringLiteral("Unknown worker exception.");
            return result;
        }
    }));
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
