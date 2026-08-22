#pragma once

#include "core/AnalysisModels.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace sc2dh
{

struct DependencyUsageEntry
{
    QString name;
    QString path;
    QString confidence;
    QStringList metadataFiles;
    QStringList directLocalUsers;
    QStringList usageChains;
    QStringList unresolvedExternalIds;
    QStringList possibleImportFiles;
    QHash<QString, int> usedObjectsByType;
};

struct DependencyUsageReport
{
    QVector<DependencyUsageEntry> dependencies;
    QStringList unknownProvenanceIds;
    QStringList notes;
};

class DependencyUsageReportBuilder
{
public:
    DependencyUsageReport build(const AnalysisResult &analysis) const;
    QJsonObject toJson(const DependencyUsageReport &report) const;
    QString toText(const DependencyUsageReport &report) const;
    bool writeJson(const QString &path, const DependencyUsageReport &report, QString *errorMessage = nullptr) const;
    bool writeText(const QString &path, const DependencyUsageReport &report, QString *errorMessage = nullptr) const;
};

} // namespace sc2dh
