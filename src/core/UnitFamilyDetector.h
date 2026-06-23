#pragma once

#include "core/AnalysisModels.h"

#include <QString>
#include <QVector>

enum class UnitFamilyRole {
    Unit, Actor, Button, Model, DeathModel, DeathFireModel, DeathDisintegrateModel,
    DeathBlastModel, PortraitModel, DeathVoice, Death, Attack, Help, Pissed, Yes,
    What, Ready, Weapon, Ability, Effect, Behavior, Validator, Requirement, Upgrade,
    Other, ManualReview
};

struct UnitFamilyObject
{
    int nodeIndex = -1;
    UnitFamilyRole role = UnitFamilyRole::ManualReview;
    QString confidence;
    QString reason;
};

struct UnitFamily
{
    int rootNodeIndex = -1;
    QString rootId;
    QVector<UnitFamilyObject> objects;
};

QString unitFamilyRoleName(UnitFamilyRole role);

class UnitFamilyDetector
{
public:
    QVector<UnitFamily> detect(const AnalysisResult &analysis) const;
};
