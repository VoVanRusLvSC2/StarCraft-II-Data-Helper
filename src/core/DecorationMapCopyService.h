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
    DecorationOptimizationMode mode = DecorationOptimizationMode::RecreateActors;
    bool overwriteExisting = false;
    // Build the exact archive-aware safety plan, but do not create or mutate
    // any output file. Diagnostic runners use this to report the same plan
    // that an apply operation would execute.
    bool dryRun = false;
};

struct DecorOptimizedMapResult
{
    bool success = false;
    QString outputArchivePath;
    QByteArray sourceSha256Before;
    QByteArray sourceSha256After;
    bool sourceUnchanged = false;
    bool dryRun = false;
    // Set only when an existing, explicitly-overwritten output was preserved
    // before the verified replacement was committed.
    QString previousOutputBackupPath;
    DecorationArchivePatch patch;
    int removedDoodads = 0;
    int visibilityControlledDoodads = 0;
    // True only after the staged visibility-only output is reopened and its
    // Objects entry compares byte-for-byte with the source entry.
    bool objectsPreserved = false;
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
