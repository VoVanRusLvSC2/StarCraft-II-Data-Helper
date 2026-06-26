#pragma once

#include "core/DataCollectionAliasMapper.h"

#include <QHash>
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
    QString parent;
    QString editorCategories;
    QSet<int> includedNodeIndices;
    bool confirmNonStandard = false;
    bool summaryOnly = false;
};

enum class DataCollectionPatternState {
    DirectPattern,
    InheritedPattern,
    NoPatternRequired,
    MissingParent,
    MissingReferencedPattern,
    InvalidPatternForEntity,
    InheritanceCycle
};

QString dataCollectionPatternStateName(DataCollectionPatternState state);

struct DataCollectionAuditSummary
{
    int collections = 0;
    int unitCollections = 0;
    int abilityCollections = 0;
    int weaponCollections = 0;
    int unanchoredCollections = 0;
    int mixedRootCollections = 0;
    int rootTypeConflicts = 0;
    int missingPrimaryRecords = 0;
    int invalidCategories = 0;
    int directPatterns = 0;
    int inheritedPatterns = 0;
    int validWithoutPattern = 0;
    int missingParents = 0;
    int brokenInheritance = 0;
    QStringList manualReview;
    QString reportText;
};

DataCollectionAuditSummary auditDataCollections(const AnalysisResult &analysis);

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
    QStringList recordsToMove;
    QStringList recordsToRemove;
    QStringList duplicateRecordsSkipped;
    QStringList manualReviewObjects;
    QStringList warnings;
    DataCollectionEntityType entityType = DataCollectionEntityType::Unit;
    QString rootXmlType;
    QString collectionXmlTag;
    QString directPattern;
    QString inheritedPattern;
    QString effectivePattern;
    DataCollectionPatternState patternState = DataCollectionPatternState::NoPatternRequired;
    QString patternDetail;
    QStringList sharedObjects;
    QStringList ownershipPaths;
    QStringList idConflicts;
    QStringList falsePositiveAssociations;
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
    int recordsRemoved = 0;
    int duplicatesSkipped = 0;
    QString error;
    QString finalReport;
};

class DataCollectionUnitBuilder
{
public:
    DataCollectionPreviewReport preview(const AnalysisResult &analysis, const DataCollectionBuildRequest &request,
                                        const QVector<UnitFamily> *knownFamilies = nullptr) const;
    DataCollectionApplyResult apply(const AnalysisResult &analysis, const DataCollectionBuildRequest &request,
                                    const QString &rootFolder, const QSet<QString> &whitelistIds,
                                    bool verifyWithFullReanalysis = true,
                                    const QVector<UnitFamily> *knownFamilies = nullptr,
                                    bool transientWorkspace = false) const;
    void setFailureInjectionStep(const QString &step) { m_failureInjectionStep = step; }

private:
    mutable const QVector<UnitFamily> *m_cachedOwnerFamilies = nullptr;
    mutable QHash<QString, QSet<QString>> m_cachedOwnersByAlias;
    mutable QHash<QString, QString> m_cachedCanonicalOwnerByAlias;
    QString m_failureInjectionStep;
};
