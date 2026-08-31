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
};

struct DecorationStreamingPlan
{
    QVector<DoodadPlacement> doodads;
    QVector<ZoneAssignment> zones;
    QVector<int> staticOnlyDoodads;
    QVector<int> unassignedDoodads;
    QVector<int> boundaryDoodads;
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
};

struct GalaxyGenerationOptions
{
    QString functionPrefix = QStringLiteral("NAME_OUT_FUNK");
    int batchLimit = 64;
};

struct DecorationOptimizedArtifacts
{
    DecorationStreamingPlan plan;
    QByteArray optimizedObjectsBytes;
    QString galaxySource;
    QVector<int> removedDoodadIndices;
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
                                               QStringList *warnings = nullptr) const;
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
