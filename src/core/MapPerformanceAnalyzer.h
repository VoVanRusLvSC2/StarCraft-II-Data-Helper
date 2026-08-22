#pragma once

#include "core/AnalysisModels.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace sc2dh::perf
{

struct MapPerformanceOptions
{
    int columns = 8;
    int rows = 8;
    double padding = 0.0;
};

struct MapPlacement
{
    QString kind;
    QString id;
    QString name;
    QString type;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    bool hasPosition = false;
    qsizetype sourceStart = -1;
    qsizetype sourceEnd = -1;
};

struct MapPerformanceCell
{
    int column = 0;
    int row = 0;
    double xMin = 0.0;
    double yMin = 0.0;
    double xMax = 0.0;
    double yMax = 0.0;
    int doodadCount = 0;
    int unitCount = 0;
    int destructibleCount = 0;
    int uniqueActorModelCount = 0;
    qint64 linkedAssetBytes = 0;
    int periodicTimerCount = 0;
    int unitGroupScanCount = 0;
    double doodadDensityScore = 0.0;
    double unitsScore = 0.0;
    double assetsScore = 0.0;
    double triggerCpuRiskScore = 0.0;
    double combinedRiskScore = 0.0;
    QString scoreLabel = QStringLiteral("Estimated Static Risk");
    QStringList reasons;
    QStringList relatedObjects;
    QStringList relatedFiles;
    QStringList triggerLines;
};

struct MapPerformanceReport
{
    QVector<MapPlacement> placements;
    QVector<MapPerformanceCell> cells;
    QStringList warnings;
    double xMin = 0.0;
    double yMin = 0.0;
    double xMax = 0.0;
    double yMax = 0.0;
    int columns = 0;
    int rows = 0;
};

class MapPerformanceAnalyzer
{
public:
    QVector<MapPlacement> parseObjectsPlacements(const QByteArray &objectsBytes,
                                                 QStringList *warnings = nullptr) const;
    MapPerformanceReport buildReport(const QByteArray &objectsBytes,
                                     const AnalysisResult &analysis,
                                     const MapPerformanceOptions &options = {}) const;
};

} // namespace sc2dh::perf
