#pragma once

#include "core/AnalysisModels.h"

#include <QHash>
#include <QString>
#include <QVector>

namespace sc2dh::refs
{

enum class ReferenceKind
{
    TypedXml,
    ScriptText,
    PlacementRoot,
    AssetText,
    WeakText,
    BinaryUnconfirmed
};

enum class ReferenceStrength
{
    Strong,
    Weak,
    Blocking
};

struct ReferenceRecord
{
    ReferenceKind kind = ReferenceKind::WeakText;
    ReferenceStrength strength = ReferenceStrength::Weak;
    QString targetId;
    QString targetAsset;
    QString sourceId;
    QString sourceType;
    QString sourceFile;
    int lineNumber = -1;
    QString detail;
    bool rewritable = false;
};

class UnifiedReferenceIndex
{
public:
    void build(const AnalysisResult &analysis);

    const QVector<ReferenceRecord> &records() const { return m_records; }
    QVector<ReferenceRecord> referencesToId(const QString &id) const;
    QVector<ReferenceRecord> referencesToAsset(const QString &assetPath) const;
    QVector<ReferenceRecord> strongReferencesToId(const QString &id) const;
    bool hasNonRewritableStrongReferenceToId(const QString &id) const;

private:
    void addRecord(const ReferenceRecord &record);

    QVector<ReferenceRecord> m_records;
    QHash<QString, QVector<int>> m_byId;
    QHash<QString, QVector<int>> m_byAsset;
};

QString referenceKindName(ReferenceKind kind);
QString referenceStrengthName(ReferenceStrength strength);

} // namespace sc2dh::refs
