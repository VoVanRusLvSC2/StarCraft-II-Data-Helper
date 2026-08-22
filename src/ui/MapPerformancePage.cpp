#include "ui/MapPerformancePage.h"

#include "core/DecorationMapCopyService.h"
#include "core/ScannedFileReader.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QColor>
#include <QDir>
#include <QDoubleSpinBox>
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
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

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
        setMinimumHeight(230);
        setMouseTracking(true);
    }

    void setReport(const sc2dh::perf::MapPerformanceReport &report)
    {
        m_report = report;
        m_selectedCell = m_report.cells.isEmpty() ? -1 : 0;
        update();
    }

    void setDecorZones(const QVector<sc2dh::decor::DecorZone> &zones)
    {
        m_zones = zones;
        update();
    }

    void setBackgroundImage(const QImage &image, const QString &sourceLabel)
    {
        m_backgroundImage = image;
        m_backgroundSourceLabel = sourceLabel;
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

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), QColor(QStringLiteral("#0b1220")));
        painter.setPen(QColor(QStringLiteral("#d9f7ee")));
        painter.drawText(QRect(12, 8, width() - 24, 24),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("Map grid heatmap — Estimated Static Risk"));

        if (m_report.cells.isEmpty() || m_report.columns <= 0 || m_report.rows <= 0) {
            painter.setPen(QColor(QStringLiteral("#91a3b7")));
            painter.drawText(rect().adjusted(12, 42, -12, -12),
                             Qt::AlignCenter,
                             QStringLiteral("No positioned ObjectDoodad/ObjectUnit/ObjectDestructible placements were found."));
            return;
        }

        const QRectF area = gridArea();
        if (!m_backgroundImage.isNull()) {
            painter.save();
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawImage(area, m_backgroundImage);
            painter.fillRect(area, QColor(5, 10, 18, 95));
            painter.restore();
        } else {
            painter.fillRect(area, QColor(QStringLiteral("#101827")));
        }
        painter.setPen(QPen(QColor(QStringLiteral("#2b3f54")), 1));
        painter.drawRect(area);

        const double cellWidth = area.width() / double(m_report.columns);
        const double cellHeight = area.height() / double(m_report.rows);
        for (const sc2dh::perf::MapPerformanceCell &cell : m_report.cells) {
            const int visualRow = m_report.rows - 1 - cell.row;
            const QRectF cellRect(area.left() + cell.column * cellWidth,
                                  area.top() + visualRow * cellHeight,
                                  cellWidth,
                                  cellHeight);
            QColor fill = riskColor(cell.combinedRiskScore);
            fill.setAlpha(210);
            painter.fillRect(cellRect.adjusted(1, 1, -1, -1), fill);
            painter.setPen(QPen(QColor(QStringLiteral("#25364a")), 1));
            painter.drawRect(cellRect);
            if (cellWidth >= 48.0 && cellHeight >= 26.0) {
                painter.setPen(QColor(QStringLiteral("#f1fff9")));
                painter.drawText(cellRect.adjusted(3, 2, -3, -2),
                                 Qt::AlignLeft | Qt::AlignTop,
                                 QStringLiteral("%1").arg(cell.combinedRiskScore, 0, 'f', 0));
            }
        }

        const double xSpan = std::max(0.0001, m_report.xMax - m_report.xMin);
        const double ySpan = std::max(0.0001, m_report.yMax - m_report.yMin);
        for (const sc2dh::decor::DecorZone &zone : m_zones) {
            const double left = area.left() + ((std::min(zone.xMin, zone.xMax) - m_report.xMin) / xSpan) * area.width();
            const double right = area.left() + ((std::max(zone.xMin, zone.xMax) - m_report.xMin) / xSpan) * area.width();
            const double top = area.bottom() - ((std::max(zone.yMin, zone.yMax) - m_report.yMin) / ySpan) * area.height();
            const double bottom = area.bottom() - ((std::min(zone.yMin, zone.yMax) - m_report.yMin) / ySpan) * area.height();
            QRectF zoneRect(QPointF(left, top), QPointF(right, bottom));
            zoneRect = zoneRect.normalized();
            QPen pen(QColor(QStringLiteral("#ffce78")), 2);
            pen.setStyle(Qt::DashLine);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(zoneRect);
            painter.setPen(QColor(QStringLiteral("#ffe9ba")));
            painter.drawText(zoneRect.adjusted(4, 3, -4, -3),
                             Qt::AlignLeft | Qt::AlignTop,
                             zone.name.isEmpty() ? QStringLiteral("Zone %1").arg(zone.id) : zone.name);
        }

        for (const sc2dh::perf::MapPlacement &placement : m_report.placements) {
            const double px = area.left() + ((placement.x - m_report.xMin) / xSpan) * area.width();
            const double py = area.bottom() - ((placement.y - m_report.yMin) / ySpan) * area.height();
            const QString kind = placement.kind.toLower();
            QColor color = QColor(QStringLiteral("#7fd0ff"));
            if (kind.contains(QStringLiteral("unit")))
                color = QColor(QStringLiteral("#b7ff78"));
            if (kind.contains(QStringLiteral("destruct")))
                color = QColor(QStringLiteral("#ffce78"));
            painter.setPen(QPen(color.lighter(150), 1));
            painter.setBrush(color);
            painter.drawEllipse(QPointF(px, py), 3.2, 3.2);
        }

        if (m_selectedCell >= 0 && m_selectedCell < m_report.cells.size()) {
            const sc2dh::perf::MapPerformanceCell &cell = m_report.cells.at(m_selectedCell);
            const int visualRow = m_report.rows - 1 - cell.row;
            const QRectF selectedRect(area.left() + cell.column * cellWidth,
                                      area.top() + visualRow * cellHeight,
                                      cellWidth,
                                      cellHeight);
            painter.setPen(QPen(QColor(QStringLiteral("#ffffff")), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(selectedRect.adjusted(1, 1, -1, -1));
        }

        painter.setPen(QColor(QStringLiteral("#91a3b7")));
        painter.drawText(QRect(12, height() - 24, width() - 24, 18),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("Dots: doodads blue, units green, destructibles orange. Click a cell for details."));
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        const int index = cellIndexAt(event->position());
        if (index < 0)
            return;
        m_selectedCell = index;
        update();
        if (cellClicked)
            cellClicked(index);
    }

private:
    QRectF gridArea() const
    {
        return QRectF(12.0, 40.0, double(width() - 24), double(std::max(80, height() - 72)));
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
    QImage m_backgroundImage;
    QString m_backgroundSourceLabel;
    int m_selectedCell = -1;
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

    auto *title = new QLabel(QStringLiteral("Map Performance / Карта и проблемные зоны"), header);
    title->setObjectName(QStringLiteral("panelTitle"));
    headerLayout->addWidget(title);

    m_summaryLabel = new QLabel(QStringLiteral("Analyze a SC2Map or extracted map folder to build the static map risk grid."), header);
    m_summaryLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    m_summaryLabel->setWordWrap(true);
    headerLayout->addWidget(m_summaryLabel);

    m_warningLabel = new QLabel(QStringLiteral("Estimated Static Risk is static analysis from map data, not FPS or runtime measurement."), header);
    m_warningLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    m_warningLabel->setWordWrap(true);
    headerLayout->addWidget(m_warningLabel);
    layout->addWidget(header);

    m_heatmap = new MapHeatmapWidget(this);
    m_heatmap->setObjectName(QStringLiteral("mapPerformanceHeatmap"));
    static_cast<MapHeatmapWidget *>(m_heatmap)->cellClicked = [this](int cellIndex) {
        selectCellIndex(cellIndex);
    };
    layout->addWidget(m_heatmap, 1);

    auto *decorGroup = new QGroupBox(QStringLiteral("Decoration Streaming / Оптимизация декорациями"), this);
    decorGroup->setObjectName(QStringLiteral("mapDecorStreamingGroup"));
    auto *decorLayout = new QVBoxLayout(decorGroup);
    decorLayout->setContentsMargins(10, 10, 10, 10);
    decorLayout->setSpacing(8);

    m_decorSummaryLabel = new QLabel(QStringLiteral("Create zones, preview generated Galaxy and create a safe decor-optimized map copy."), decorGroup);
    m_decorSummaryLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    m_decorSummaryLabel->setWordWrap(true);
    decorLayout->addWidget(m_decorSummaryLabel);

    auto *decorControls = new QHBoxLayout();
    m_gridColumnsSpin = new QSpinBox(decorGroup);
    m_gridColumnsSpin->setRange(1, 32);
    m_gridColumnsSpin->setValue(2);
    m_gridRowsSpin = new QSpinBox(decorGroup);
    m_gridRowsSpin->setRange(1, 32);
    m_gridRowsSpin->setValue(1);
    m_gridPaddingSpin = new QDoubleSpinBox(decorGroup);
    m_gridPaddingSpin->setRange(0.0, 1024.0);
    m_gridPaddingSpin->setDecimals(2);
    m_gridPaddingSpin->setValue(0.0);
    m_prefixEdit = new QLineEdit(QStringLiteral("NAME_OUT_FUNK"), decorGroup);
    m_prefixEdit->setMinimumWidth(150);
    m_batchSpin = new QSpinBox(decorGroup);
    m_batchSpin->setRange(1, 4096);
    m_batchSpin->setValue(64);

    auto *form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(6);
    form->addRow(QStringLiteral("Grid columns"), m_gridColumnsSpin);
    form->addRow(QStringLiteral("Grid rows"), m_gridRowsSpin);
    form->addRow(QStringLiteral("Padding"), m_gridPaddingSpin);
    form->addRow(QStringLiteral("Function prefix"), m_prefixEdit);
    form->addRow(QStringLiteral("Batch per tick"), m_batchSpin);
    decorControls->addLayout(form, 1);

    auto *buttonLayout = new QVBoxLayout();
    auto *autoZonesButton = new QPushButton(QStringLiteral("Build Auto Zones"), decorGroup);
    auto *addZoneButton = new QPushButton(QStringLiteral("Add Zone"), decorGroup);
    auto *deleteZoneButton = new QPushButton(QStringLiteral("Delete Selected Zone(s)"), decorGroup);
    auto *previewButton = new QPushButton(QStringLiteral("Preview Galaxy"), decorGroup);
    m_createCopyButton = new QPushButton(QStringLiteral("Create Decor-Optimized Map Copy"), decorGroup);
    buttonLayout->addWidget(autoZonesButton);
    buttonLayout->addWidget(addZoneButton);
    buttonLayout->addWidget(deleteZoneButton);
    buttonLayout->addWidget(previewButton);
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
    decorLayout->addWidget(m_zoneTable);

    auto *doodadLabel = new QLabel(QStringLiteral("Doodad assignments: check Exclude to keep a doodad static, or enter a Zone Id to move it to that dynamic zone."), decorGroup);
    doodadLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
    doodadLabel->setWordWrap(true);
    decorLayout->addWidget(doodadLabel);

    m_doodadModel = new QStandardItemModel(this);
    m_doodadModel->setHorizontalHeaderLabels({
        QStringLiteral("Exclude"),
        QStringLiteral("Forced Zone"),
        QStringLiteral("Name"),
        QStringLiteral("Id"),
        QStringLiteral("Type"),
        QStringLiteral("Position"),
        QStringLiteral("State")
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
    m_galaxyPreview->setPlaceholderText(QStringLiteral("Generated Galaxy preview will appear here."));
    decorLayout->addWidget(m_galaxyPreview);

    connect(autoZonesButton, &QPushButton::clicked, this, &MapPerformancePage::buildAutoDecorZones);
    connect(addZoneButton, &QPushButton::clicked, this, &MapPerformancePage::addDecorZone);
    connect(deleteZoneButton, &QPushButton::clicked, this, &MapPerformancePage::deleteSelectedDecorZones);
    connect(previewButton, &QPushButton::clicked, this, &MapPerformancePage::updateDecorPreview);
    connect(m_createCopyButton, &QPushButton::clicked, this, &MapPerformancePage::createDecorOptimizedMapCopy);
    connect(m_prefixEdit, &QLineEdit::textChanged, this, &MapPerformancePage::updateDecorPreview);
    connect(m_batchSpin, &QSpinBox::valueChanged, this, &MapPerformancePage::updateDecorPreview);
    connect(m_zoneModel, &QStandardItemModel::itemChanged, this, &MapPerformancePage::updateDecorPreview);
    connect(m_doodadModel, &QStandardItemModel::itemChanged, this, &MapPerformancePage::updateDecorPreview);

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
    layout->addWidget(m_table, 2);

    m_details = new QPlainTextEdit(this);
    m_details->setObjectName(QStringLiteral("mapPerformanceDetails"));
    m_details->setReadOnly(true);
    m_details->setMinimumHeight(190);
    m_details->setPlaceholderText(QStringLiteral("Select a grid cell to inspect exact static-risk reasons."));
    layout->addWidget(m_details, 1);

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
        m_objectsBytes.clear();
        m_objectsSourceLabel.clear();
        m_report = {};
        static_cast<MapHeatmapWidget *>(m_heatmap)->setReport(m_report);
        static_cast<MapHeatmapWidget *>(m_heatmap)->setDecorZones({});
        m_model->removeRows(0, m_model->rowCount());
        populateZoneTable({});
        populateDoodadTable({});
        m_summaryLabel->setText(QStringLiteral("Objects placement file was not found or is larger than %1. Open a SC2Map/extracted map with an Objects entry.").arg(formatBytes(MaxObjectsBytes)));
        m_warningLabel->setText(m_minimapImage.isNull()
                                    ? QStringLiteral("Estimated Static Risk is unavailable until the map placement data can be read.")
                                    : QStringLiteral("Minimap.tga background loaded from %1, but Objects placement data is unavailable.").arg(m_minimapSourceLabel));
        m_decorSummaryLabel->setText(QStringLiteral("Decoration Streaming requires a readable Objects entry."));
        m_galaxyPreview->clear();
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

    const int riskyCells = std::count_if(m_report.cells.cbegin(), m_report.cells.cend(), [](const sc2dh::perf::MapPerformanceCell &cell) {
        return cell.combinedRiskScore >= 40.0;
    });
    m_summaryLabel->setText(QStringLiteral("%1 placement(s), %2x%3 grid, %4 cell(s) over medium risk. Source: %5%6")
                                .arg(m_report.placements.size())
                                .arg(m_report.columns)
                                .arg(m_report.rows)
                                .arg(riskyCells)
                                .arg(sourceLabel)
                                .arg(m_minimapImage.isNull()
                                         ? QString()
                                         : QStringLiteral(" | Minimap: %1").arg(m_minimapSourceLabel)));
    QString warning = QStringLiteral("Estimated Static Risk is static analysis from map data, not FPS/runtime measurement.");
    if (m_minimapImage.isNull())
        warning += QStringLiteral(" Minimap.tga was not found or could not be decoded; using coordinate grid fallback.");
    if (!m_report.warnings.isEmpty())
        warning += QStringLiteral(" Warnings: %1").arg(m_report.warnings.join(QStringLiteral(" | ")));
    m_warningLabel->setText(warning);
    populateZoneTable(m_decorZones);
    sc2dh::decor::DecorationStreamingPlanner decorPlanner;
    populateDoodadTable(decorPlanner.parseObjects(m_objectsBytes));
    updateDecorPreview();
    updateDetails();
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
    if (m_model->rowCount() > 0)
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
    QVector<sc2dh::decor::DecorZone> zones;
    if (!m_zoneModel)
        return zones;

    for (int row = 0; row < m_zoneModel->rowCount(); ++row) {
        sc2dh::decor::DecorZone zone;
        zone.id = m_zoneModel->item(row, 0) ? m_zoneModel->item(row, 0)->text().toInt() : row + 1;
        zone.name = m_zoneModel->item(row, 1) ? m_zoneModel->item(row, 1)->text().trimmed() : QStringLiteral("Zone_%1").arg(zone.id);
        zone.xMin = m_zoneModel->item(row, 2) ? m_zoneModel->item(row, 2)->text().toDouble() : 0.0;
        zone.yMin = m_zoneModel->item(row, 3) ? m_zoneModel->item(row, 3)->text().toDouble() : 0.0;
        zone.xMax = m_zoneModel->item(row, 4) ? m_zoneModel->item(row, 4)->text().toDouble() : 0.0;
        zone.yMax = m_zoneModel->item(row, 5) ? m_zoneModel->item(row, 5)->text().toDouble() : 0.0;
        if (zone.name.isEmpty())
            zone.name = QStringLiteral("Zone_%1").arg(zone.id);
        zones << zone;
    }
    return zones;
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
            item->setEditable(true);
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
    const QSignalBlocker blocker(m_doodadModel);
    m_doodadModel->removeRows(0, m_doodadModel->rowCount());

    for (const sc2dh::decor::DoodadPlacement &doodad : doodads) {
        auto *exclude = new QStandardItem();
        exclude->setCheckable(true);
        exclude->setCheckState(Qt::Unchecked);
        exclude->setEditable(false);

        auto *forcedZone = new QStandardItem();
        forcedZone->setEditable(true);
        forcedZone->setToolTip(QStringLiteral("Optional numeric Zone Id. Empty means automatic coordinate assignment."));

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

    const int rows = std::min(m_doodadModel->rowCount(), int(plan.doodads.size()));
    for (int row = 0; row < rows; ++row) {
        QStandardItem *state = m_doodadModel->item(row, DecorDoodadStateColumn);
        if (!state)
            continue;
        if (zoneByDoodadIndex.contains(row)) {
            state->setText(QStringLiteral("Dynamic zone %1").arg(zoneByDoodadIndex.value(row)));
        } else if (unassigned.contains(row)) {
            state->setText(QStringLiteral("Unassigned dynamic doodad"));
        } else {
            const QString reason = plan.doodads.at(row).staticOnlyReason;
            state->setText(reason.isEmpty() ? QStringLiteral("Static-only") : reason);
        }
    }
    m_doodadTable->resizeColumnsToContents();
}

void MapPerformancePage::buildAutoDecorZones()
{
    if (!m_hasObjects) {
        QMessageBox::warning(this,
                             QStringLiteral("Decoration Streaming"),
                             QStringLiteral("Objects entry is not available."));
        return;
    }

    sc2dh::decor::DecorationStreamingPlanner planner;
    QStringList warnings;
    const QVector<sc2dh::decor::DoodadPlacement> doodads = planner.parseObjects(m_objectsBytes, &warnings);

    double xMin = std::numeric_limits<double>::max();
    double yMin = std::numeric_limits<double>::max();
    double xMax = std::numeric_limits<double>::lowest();
    double yMax = std::numeric_limits<double>::lowest();
    int dynamicCandidates = 0;
    for (const sc2dh::decor::DoodadPlacement &doodad : doodads) {
        if (!doodad.dynamicCandidate)
            continue;
        ++dynamicCandidates;
        xMin = std::min(xMin, doodad.x);
        yMin = std::min(yMin, doodad.y);
        xMax = std::max(xMax, doodad.x);
        yMax = std::max(yMax, doodad.y);
    }

    if (dynamicCandidates == 0) {
        QMessageBox::information(this,
                                 QStringLiteral("Decoration Streaming"),
                                 QStringLiteral("No runtime-safe visual doodads were found. Static-only doodads are intentionally kept in Objects."));
        return;
    }

    const double padding = m_gridPaddingSpin->value();
    xMin -= padding;
    yMin -= padding;
    xMax += padding;
    yMax += padding;
    if (xMin == xMax) {
        xMin -= 0.5;
        xMax += 0.5;
    }
    if (yMin == yMax) {
        yMin -= 0.5;
        yMax += 0.5;
    }

    QVector<sc2dh::decor::DecorZone> zones;
    const int columns = m_gridColumnsSpin->value();
    const int rows = m_gridRowsSpin->value();
    const double width = (xMax - xMin) / double(columns);
    const double height = (yMax - yMin) / double(rows);
    int id = 1;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            sc2dh::decor::DecorZone zone;
            zone.id = id++;
            zone.name = QStringLiteral("Auto_R%1_C%2").arg(row + 1).arg(column + 1);
            zone.xMin = xMin + width * column;
            zone.xMax = column == columns - 1 ? xMax : xMin + width * (column + 1);
            zone.yMin = yMin + height * row;
            zone.yMax = row == rows - 1 ? yMax : yMin + height * (row + 1);
            zones << zone;
        }
    }

    populateZoneTable(zones);
    updateDecorPreview();
    if (!warnings.isEmpty())
        m_decorSummaryLabel->setText(m_decorSummaryLabel->text() + QStringLiteral(" Parse warnings: %1").arg(warnings.join(QStringLiteral(" | "))));
}

void MapPerformancePage::addDecorZone()
{
    QVector<sc2dh::decor::DecorZone> zones = zonesFromModel();
    int nextId = 1;
    for (const sc2dh::decor::DecorZone &zone : zones)
        nextId = std::max(nextId, zone.id + 1);

    sc2dh::decor::DecorZone zone;
    zone.id = nextId;
    zone.name = QStringLiteral("Zone_%1").arg(nextId);
    if (!m_report.cells.isEmpty()) {
        zone.xMin = m_report.xMin;
        zone.yMin = m_report.yMin;
        zone.xMax = m_report.xMax;
        zone.yMax = m_report.yMax;
    } else {
        zone.xMin = 0.0;
        zone.yMin = 0.0;
        zone.xMax = 64.0;
        zone.yMax = 64.0;
    }
    zones << zone;
    populateZoneTable(zones);
    updateDecorPreview();
}

void MapPerformancePage::deleteSelectedDecorZones()
{
    QModelIndexList rows = m_zoneTable->selectionModel() ? m_zoneTable->selectionModel()->selectedRows() : QModelIndexList();
    if (rows.isEmpty())
        return;
    std::sort(rows.begin(), rows.end(), [](const QModelIndex &left, const QModelIndex &right) {
        return left.row() > right.row();
    });
    for (const QModelIndex &row : rows)
        m_zoneModel->removeRow(row.row());
    m_decorZones = zonesFromModel();
    static_cast<MapHeatmapWidget *>(m_heatmap)->setDecorZones(m_decorZones);
    updateDecorPreview();
}

void MapPerformancePage::updateDecorPreview()
{
    if (!m_hasObjects) {
        m_decorPreview = {};
        m_decorSummaryLabel->setText(QStringLiteral("Decoration Streaming requires a readable Objects entry."));
        m_galaxyPreview->clear();
        m_createCopyButton->setEnabled(false);
        return;
    }

    m_decorZones = zonesFromModel();
    static_cast<MapHeatmapWidget *>(m_heatmap)->setDecorZones(m_decorZones);
    if (m_decorZones.isEmpty()) {
        m_decorPreview = {};
        m_decorSummaryLabel->setText(QStringLiteral("No decoration zones defined. Use Build Auto Zones or Add Zone."));
        m_galaxyPreview->clear();
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
    updateDoodadTableState(plan);
    int dynamicAssigned = 0;
    for (const sc2dh::decor::ZoneAssignment &assignment : plan.zones)
        dynamicAssigned += assignment.doodadIndices.size();

    QString summary = QStringLiteral("%1 doodad(s), %2 dynamic assigned, %3 static-only, %4 unassigned. Prefix: %5. Batch: %6.")
                          .arg(plan.doodads.size())
                          .arg(dynamicAssigned)
                          .arg(plan.staticOnlyDoodads.size())
                          .arg(plan.unassignedDoodads.size())
                          .arg(options.functionPrefix)
                          .arg(options.batchLimit);
    if (!m_decorPreview.warnings.isEmpty())
        summary += QStringLiteral(" Warnings: %1").arg(m_decorPreview.warnings.join(QStringLiteral(" | ")));
    if (sourceArchivePath().isEmpty())
        summary += QStringLiteral(" Create-copy UI currently requires a .SC2Map/.SC2Mod archive source; components folders can still be inspected.");
    summary += QStringLiteral(" Archive creation re-scans scripts/triggers and may keep more doodads static if referenced.");
    m_decorSummaryLabel->setText(summary);
    m_galaxyPreview->setPlainText(m_decorPreview.galaxySource);
    m_createCopyButton->setEnabled(!sourceArchivePath().isEmpty()
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
        QMessageBox::critical(this,
                              QStringLiteral("Create Decor-Optimized Map Copy"),
                              QStringLiteral("Failed to create optimized copy:\n%1").arg(result.error));
        m_decorSummaryLabel->setText(QStringLiteral("Create copy failed: %1").arg(result.error));
        return;
    }

    QString message = QStringLiteral("Created structural-verified copy:\n%1\n\nRemoved dynamic visual doodads: %2\nFull re-analysis: %3 file(s), %4 data node(s).")
                          .arg(QDir::toNativeSeparators(result.outputArchivePath))
                          .arg(result.removedDoodads)
                          .arg(result.verifiedScannedFiles)
                          .arg(result.verifiedDataNodes);
    if (!result.warnings.isEmpty())
        message += QStringLiteral("\n\nWarnings:\n%1").arg(result.warnings.join(QStringLiteral("\n")));
    QMessageBox::information(this,
                             QStringLiteral("Create Decor-Optimized Map Copy"),
                             message);
    m_decorSummaryLabel->setText(message);
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
