#pragma once

#include "core/DecorationStreamingPlanner.h"
#include "core/DecorationGuiActionBuilder.h"

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
    // Optional exact-backup JSON and external import XML. Default exports
    // keep compact identity/catalog bookkeeping in Galaxy for script import.
    bool writeRestoreSidecars = false;
    DecorGuiActionOptions guiAction;
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
    int removedUnits = 0;
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

struct DecorRestoreMapRequest
{
    QString sourceArchivePath;
    QString outputArchivePath;
    QString objectsEntry = QStringLiteral("Objects");
    QString runtimeEntry = QStringLiteral("scripts/sc2dh_decor_opt.galaxy");
};

struct DecorRestoreMapResult
{
    bool success = false;
    QString outputArchivePath;
    bool sourceUnchanged = false;
    int restoredDoodads = 0;
    int restoredUnits = 0;
    QString error;
};

class DecorationMapCopyService
{
public:
    // Read-only archive preflight shared with the GUI preview.
    DecorationSafetyContext inspectSafety(const DecorOptimizedMapRequest &request, QString *error) const;
    DecorOptimizedMapResult createOptimizedCopy(const DecorOptimizedMapRequest &request) const;
    DecorRestoreMapResult restoreGalaxyCopy(const DecorRestoreMapRequest &request) const;
    // Exact backup rollback remains separate from executable Galaxy import.
    DecorRestoreMapResult restoreOptimizedCopy(const DecorRestoreMapRequest &request) const;
};

} // namespace sc2dh::decor
