#pragma once

#include "core/DecorationStreamingPlanner.h"

#include <QString>

namespace sc2dh::decor
{

struct DecorOptimizedMapRequest
{
    QString sourceArchivePath;
    QString outputArchivePath;
    QVector<DecorZone> zones;
    DecorationSafetyContext safetyContext;
    GalaxyGenerationOptions galaxyOptions;
    QString objectsEntry = QStringLiteral("Objects");
    QString mapScriptEntry = QStringLiteral("MapScript.galaxy");
    QString runtimeEntry = QStringLiteral("scripts/sc2dh_decor_opt.galaxy");
    bool overwriteExisting = false;
};

struct DecorOptimizedMapResult
{
    bool success = false;
    QString outputArchivePath;
    DecorationArchivePatch patch;
    int removedDoodads = 0;
    bool fullAnalysisVerified = false;
    int verifiedScannedFiles = 0;
    int verifiedDataNodes = 0;
    QStringList verificationParseErrors;
    QStringList warnings;
    QString error;
};

class DecorationMapCopyService
{
public:
    DecorOptimizedMapResult createOptimizedCopy(const DecorOptimizedMapRequest &request) const;
};

} // namespace sc2dh::decor
