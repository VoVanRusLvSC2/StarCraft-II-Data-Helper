#pragma once

#include "core/DataCollectionAliasMapper.h"

#include <QSet>

struct DataCollectionEntryProposal
{
    int nodeIndex = -1;
    QString realType;
    QString realId;
    UnitFamilyRole role = UnitFamilyRole::ManualReview;
    QString alias;
    QString confidence;
    QString status;
    bool included = true;
};

struct DataCollectionBuildRequest
{
    UnitFamily family;
    QString requestedUnitId;
    QString parent = QStringLiteral("UnitGround");
    QString editorCategories = QStringLiteral("DataFamily:Campaign,DataGroup:Unit,ObjectType:Hero,Race:Terran");
    QSet<int> includedNodeIndices;
    bool confirmNonStandard = false;
};

struct DataCollectionPreviewReport
{
    bool valid = false;
    bool familyStandardized = false;
    bool existingCollection = false;
    DataCollectionBuildRequest request;
    QVector<DataCollectionEntryProposal> entries;
    QStringList missingExpectedObjects;
    QStringList existingRecordsPreserved;
    QStringList recordsToAdd;
    QStringList duplicateRecordsSkipped;
    QStringList manualReviewObjects;
    QStringList warnings;
    QString generatedXml;
    QString targetFile;
    QString listfilePath;
    QString archiveEntry;
    bool targetFileExists = false;
    bool listfileNeedsUpdate = false;
    QString reportText;
};

struct DataCollectionApplyResult
{
    bool success = false;
    QString backupFolder;
    QString changedFile;
    QStringList changedFiles;
    int recordsAdded = 0;
    int duplicatesSkipped = 0;
    QString error;
    QString finalReport;
};

class DataCollectionUnitBuilder
{
public:
    DataCollectionPreviewReport preview(const AnalysisResult &analysis, const DataCollectionBuildRequest &request) const;
    DataCollectionApplyResult apply(const AnalysisResult &analysis, const DataCollectionBuildRequest &request,
                                    const QString &rootFolder, const QSet<QString> &whitelistIds) const;
    void setFailureInjectionStep(const QString &step) { m_failureInjectionStep = step; }

private:
    QString m_failureInjectionStep;
};
