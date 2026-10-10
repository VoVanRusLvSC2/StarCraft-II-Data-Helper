#pragma once

#include "core/DecorationStreamingPlanner.h"

namespace sc2dh::decor
{
struct DecorGuiActionOptions
{
    bool enabled = false;
    QString actionName = QStringLiteral("Create Map Environment From Galaxy");
    QString presetName = QStringLiteral("Galaxy Map Region");
};

struct DecorGuiActionArtifacts
{
    bool valid = false;
    QByteArray triggers;
    QByteArray mapScript;
    QHash<QString, QString> triggerStrings;
    int convertedCalls = 0;
    QString error;
};

// Builds editable native GUI nodes. The existing runtime placement functions
// remain the implementation used by each Switch case.
DecorGuiActionArtifacts buildDecorGuiAction(const QByteArray &triggers,
                                            const QByteArray &mapScript,
                                            const QString &runtimeEntry,
                                            const DecorationStreamingPlan &plan,
                                            const GalaxyGenerationOptions &galaxyOptions,
                                            const DecorGuiActionOptions &guiOptions);
}
