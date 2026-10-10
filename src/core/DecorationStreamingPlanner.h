#pragma once

#include "core/MapRegionRepository.h"

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QVector>
#include <QString>
#include <QStringList>

namespace sc2dh::decor
{

struct DoodadPlacement
{
    QString id;
    QString name;
    QString type;
    QString variation;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double rotation = 0.0;
    double pitch = 0.0;
    double roll = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
    double scaleZ = 1.0;
    QString tintColor;
    QString teamColor;
    QString flags;
    QHash<QString, QString> placementFlags;
    QString rawSource;
    QHash<QString, QString> otherAttributes;
    bool hasUnsupportedChildren = false;
    bool hasInvalidTransform = false;
    bool losslessRoundTripSupported = false;
    QStringList safetyReferenceFiles;
    bool hasPosition = false;
    bool dynamicCandidate = false;
    // Visibility-only streaming keeps this original placement in Objects and
    // controls its existing actor by Doodad ID. It deliberately has a
    // separate safety classification from dynamic recreation: height,
    // variation and editor-only fields stay intact because nothing is
    // serialized or recreated.
    bool visibilityCandidate = false;
    QString visibilityStaticOnlyReason;
    bool userExcluded = false;
    int forcedZoneId = 0;
    QString staticOnlyReason;
    qsizetype sourceStart = -1;
    qsizetype sourceEnd = -1;
};

struct UnitPlacement
{
    QString id;
    QString name;
    QString type;
    int player = -1;
    double x = 0.0;
    double y = 0.0;
    double rotation = 0.0;
    bool forcePlacement = false;
    double z = 0.0;
    double scaleX = 1.0, scaleY = 1.0, scaleZ = 1.0;
    int variation = -1;
    int resources = -1;
    int stateFlags = 0;
    int attributePresence = 0;
    bool extendedPlacement = false;
    QString textureProps;
    QString rawSource;
    qsizetype sourceStart = -1;
    qsizetype sourceEnd = -1;
    bool dynamicCandidate = false;
    QString staticOnlyReason;
    QStringList safetyReferenceFiles;
};

struct DecorZone
{
    int id = 0;
    QString name;
    double xMin = 0.0;
    double yMin = 0.0;
    double xMax = 0.0;
    double yMax = 0.0;
    sc2dh::region::RegionGeometry geometry;
};

struct ZoneAssignment
{
    int zoneId = 0;
    QVector<int> doodadIndices;
    QVector<int> unitIndices;
    QString zoneName;
};

struct DecorationStreamingPlan
{
    QVector<DoodadPlacement> doodads;
    QVector<UnitPlacement> units;
    QVector<ZoneAssignment> zones;
    QVector<DecorZone> decorZones;
    QVector<int> staticOnlyDoodads;
    QVector<int> unassignedDoodads;
    QVector<int> boundaryDoodads;
    QVector<int> staticOnlyUnits;
    QVector<int> unassignedUnits;
    QVector<int> boundaryUnits;
    QStringList warnings;
};

struct DecorationVisibilityPlan
{
    QVector<DoodadPlacement> doodads;
    QVector<ZoneAssignment> zones;
    QVector<int> staticOnlyDoodads;
    QVector<int> unassignedDoodads;
    QVector<int> boundaryDoodads;
    QStringList warnings;
};

struct DecorationSafetyContext
{
    QHash<QString, QStringList> referenceFilesByDoodadKey;
    QSet<QString> excludedDoodadKeys;
    QHash<QString, int> forcedZoneByDoodadKey;
    QHash<QString, QString> staticOnlyReasonByDoodadType;
    QHash<QString, QStringList> referenceFilesByUnitType;
    QHash<QString, QStringList> referenceFilesByDoodadType;
    bool keepReferencedTypes = false;
    QSet<QString> excludedUnitKeys;
    QSet<QString> excludedUnitTypeKeys;
};

struct GalaxyGenerationOptions
{
    QString functionPrefix = QStringLiteral("Game_area");
    int batchLimit = 64;
    bool useRegionNames = false;
    int entireMapEntryZoneId = 0;
    QHash<int, QString> publicFunctionNames;
};

QString galaxyRegionFunctionStem(const DecorationStreamingPlan &plan,
                                 const ZoneAssignment &zone,
                                 const GalaxyGenerationOptions &options);


struct DecorationOptimizedArtifacts
{
    DecorationStreamingPlan plan;
    QByteArray optimizedObjectsBytes;
    QString galaxySource;
    QVector<int> removedDoodadIndices;
    QVector<int> removedUnitIndices;
    bool roundTripVerified = false;
    bool outsideScopePreserved = false;
    bool valid = false;
    QStringList warnings;
};

struct DecorationVisibilityArtifacts
{
    DecorationVisibilityPlan plan;
    QString galaxySource;
    QVector<int> controlledDoodadIndices;
    // The visibility mode never adds an Objects replacement entry. The map
    // copy service also verifies this against the staged archive.
    bool objectsPreserved = false;
    bool valid = false;
    QStringList warnings;
};

enum class DecorationOptimizationMode
{
    RecreateActors,
    VisibilityOnly
};

struct DecorationArchivePatch
{
    DecorationOptimizationMode mode = DecorationOptimizationMode::RecreateActors;
    DecorationOptimizedArtifacts artifacts;
    DecorationVisibilityArtifacts visibilityArtifacts;
    QHash<QString, QByteArray> replacementEntries;
    QString objectsEntry = QStringLiteral("Objects");
    QString mapScriptEntry = QStringLiteral("MapScript.galaxy");
    QString runtimeEntry = QStringLiteral("scripts/sc2dh_decor_opt.galaxy");
    bool valid = false;
    QString error;
    QStringList warnings;
};

class DecorationStreamingPlanner
{
public:
    QVector<DoodadPlacement> parseObjects(const QByteArray &objectsBytes, QStringList *warnings = nullptr) const;
    QVector<UnitPlacement> parseUnits(const QByteArray &objectsBytes, QStringList *warnings = nullptr) const;
    QByteArray serializePlacementLosslessly(const DoodadPlacement &placement,
                                            QString *errorMessage = nullptr) const;
    bool verifyPlacementRoundTrip(const DoodadPlacement &placement,
                                  QString *errorMessage = nullptr) const;
    DecorationStreamingPlan buildPlan(const QByteArray &objectsBytes,
                                      const QVector<DecorZone> &zones,
                                      const DecorationSafetyContext &safetyContext,
                                      QStringList *warnings = nullptr) const;
    DecorationStreamingPlan buildPlan(const QByteArray &objectsBytes,
                                      const QVector<DecorZone> &zones,
                                      QStringList *warnings = nullptr) const;
    DecorationVisibilityPlan buildVisibilityPlan(const QByteArray &objectsBytes,
                                                 const QVector<DecorZone> &zones,
                                                 const DecorationSafetyContext &safetyContext,
                                                 QStringList *warnings = nullptr) const;
    DecorationVisibilityPlan buildVisibilityPlan(const QByteArray &objectsBytes,
                                                 const QVector<DecorZone> &zones,
                                                 QStringList *warnings = nullptr) const;
    QString generateGalaxy(const DecorationStreamingPlan &plan,
                           const GalaxyGenerationOptions &options = {}) const;
    // Merge private placement profiles into the map catalog; existing actor
    // entries and their animation/fog settings are not overridden.
    bool generateRuntimeActorCatalog(const DecorationStreamingPlan &plan,
                                     const QByteArray &originalCatalog,
                                     QByteArray *catalog,
                                     QString *errorMessage = nullptr) const;
    bool generateRuntimePreload(const DecorationStreamingPlan &plan,
                                const QByteArray &originalPreload,
                                QByteArray *preload,
                                QString *errorMessage = nullptr) const;
    bool restoreObjectsFromGalaxy(const QByteArray &objects, const QString &galaxy,
                                  QByteArray *restored, int *doodads, int *units,
                                  QString *error = nullptr) const;
    QString staticPlacementGalaxy(const QString &galaxy) const;
    bool validateGeneratedGalaxy(const QString &galaxySource, QStringList *errors = nullptr) const;
    QString generateVisibilityGalaxy(const DecorationVisibilityPlan &plan,
                                     const GalaxyGenerationOptions &options = {}) const;
    bool validateGeneratedVisibilityGalaxy(const QString &galaxySource,
                                           QStringList *errors = nullptr) const;
    bool injectGalaxyInclude(const QByteArray &mapScriptBytes,
                             const QString &scriptEntry,
                             QByteArray *rewrittenMapScriptBytes,
                             QString *errorMessage = nullptr) const;
    QByteArray removeDynamicDoodadsFromObjects(const QByteArray &objectsBytes,
                                               const DecorationStreamingPlan &plan,
                                               QVector<int> *removedDoodadIndices = nullptr,
                                               QStringList *warnings = nullptr,
                                               QVector<int> *removedUnitIndices = nullptr) const;
    DecorationOptimizedArtifacts createOptimizedArtifacts(const QByteArray &objectsBytes,
                                                          const QVector<DecorZone> &zones,
                                                          const DecorationSafetyContext &safetyContext,
                                                          const GalaxyGenerationOptions &options = {}) const;
    DecorationOptimizedArtifacts createOptimizedArtifacts(const QByteArray &objectsBytes,
                                                          const QVector<DecorZone> &zones,
                                                          const GalaxyGenerationOptions &options = {}) const;
    DecorationVisibilityArtifacts createVisibilityArtifacts(const QByteArray &objectsBytes,
                                                            const QVector<DecorZone> &zones,
                                                            const DecorationSafetyContext &safetyContext,
                                                            const GalaxyGenerationOptions &options = {}) const;
    DecorationVisibilityArtifacts createVisibilityArtifacts(const QByteArray &objectsBytes,
                                                            const QVector<DecorZone> &zones,
                                                            const GalaxyGenerationOptions &options = {}) const;
    DecorationArchivePatch prepareArchivePatch(const QByteArray &objectsBytes,
                                               const QByteArray &mapScriptBytes,
                                               const QVector<DecorZone> &zones,
                                               const DecorationSafetyContext &safetyContext,
                                               const GalaxyGenerationOptions &options = {},
                                               const QString &objectsEntry = QStringLiteral("Objects"),
                                               const QString &mapScriptEntry = QStringLiteral("MapScript.galaxy"),
                                               const QString &runtimeEntry = QStringLiteral("scripts/sc2dh_decor_opt.galaxy")) const;
    DecorationArchivePatch prepareArchivePatch(const QByteArray &objectsBytes,
                                               const QByteArray &mapScriptBytes,
                                               const QVector<DecorZone> &zones,
                                               const GalaxyGenerationOptions &options = {},
                                               const QString &objectsEntry = QStringLiteral("Objects"),
                                               const QString &mapScriptEntry = QStringLiteral("MapScript.galaxy"),
                                               const QString &runtimeEntry = QStringLiteral("scripts/sc2dh_decor_opt.galaxy")) const;
    DecorationArchivePatch prepareVisibilityArchivePatch(const QByteArray &objectsBytes,
                                                         const QByteArray &mapScriptBytes,
                                                         const QVector<DecorZone> &zones,
                                                         const DecorationSafetyContext &safetyContext,
                                                         const GalaxyGenerationOptions &options = {},
                                                         const QString &objectsEntry = QStringLiteral("Objects"),
                                                         const QString &mapScriptEntry = QStringLiteral("MapScript.galaxy"),
                                                         const QString &runtimeEntry = QStringLiteral("scripts/sc2dh_decor_opt.galaxy")) const;
    DecorationArchivePatch prepareVisibilityArchivePatch(const QByteArray &objectsBytes,
                                                         const QByteArray &mapScriptBytes,
                                                         const QVector<DecorZone> &zones,
                                                         const GalaxyGenerationOptions &options = {},
                                                         const QString &objectsEntry = QStringLiteral("Objects"),
                                                         const QString &mapScriptEntry = QStringLiteral("MapScript.galaxy"),
                                                         const QString &runtimeEntry = QStringLiteral("scripts/sc2dh_decor_opt.galaxy")) const;
};

} // namespace sc2dh::decor
