#include <QtTest/QtTest>

#include "app/TranslationManager.h"
#include "core/BackupManager.h"
#include "core/CatalogEnumRepair.h"
#include "core/FolderAnalyzer.h"
#include "core/MergeService.h"
#include "core/M3ModelParser.h"
#include "core/ReferenceRenamer.h"
#include "core/StandardNamePlanner.h"
#include "core/DataCollectionScalePolicy.h"
#include "core/ExternalConsumerSafetyPolicy.h"
#include "core/UnitFamilyDetector.h"
#include "core/DataCollectionAliasMapper.h"
#include "core/DataCollectionPreservation.h"
#include "core/DataCollectionUnitBuilder.h"
#include "core/DeepCleanupService.h"
#include "core/DependencyUsageReport.h"
#include "core/DecorationMapCopyService.h"
#include "core/DecorationStreamingPlanner.h"
#include "core/ArchiveCompressionService.h"
#include "core/MapPerformanceAnalyzer.h"
#include "core/MapPreviewData.h"
#include "core/MapRegionRepository.h"
#include "core/Sc2Archive.h"
#include "core/UnifiedReferenceIndex.h"
#include "core/XmlLoader.h"
#include "ui/ObjectFilterProxyModel.h"
#include "ui/ObjectTableModel.h"

#include <QDir>
#include <QCoreApplication>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QSet>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QStringList>
#include <QTabWidget>
#include <QTranslator>
#include <QtEndian>

#include <algorithm>
#include <optional>
#include <pugixml.hpp>

#ifdef SC2DH_USE_STORMLIB
#include <StormLib.h>
#endif

namespace {

bool writeTextFile(const QString &path, const QByteArray &content)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    if (file.write(content) != content.size()) {
        return false;
    }
    file.close();
    return true;
}

#ifdef SC2DH_USE_STORMLIB
bool createTestMpqArchive(const QString &archivePath,
                          const QHash<QString, QByteArray> &entries,
                          QString *errorMessage)
{
    QFile::remove(archivePath);
    HANDLE archive = nullptr;
    const DWORD createFlags = MPQ_CREATE_ARCHIVE_V1 | MPQ_CREATE_LISTFILE;
    if (!SFileCreateArchive(reinterpret_cast<const TCHAR *>(archivePath.utf16()),
                            createFlags,
                            DWORD(std::max(16, int(entries.size()) + 8)),
                            &archive)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("SFileCreateArchive failed: %1").arg(GetLastError());
        return false;
    }

    QTemporaryDir sourceDir;
    if (!sourceDir.isValid()) {
        SFileCloseArchive(archive);
        if (errorMessage)
            *errorMessage = QStringLiteral("Unable to create temporary MPQ source directory.");
        return false;
    }

    int index = 0;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        const QString sourcePath = QDir(sourceDir.path()).absoluteFilePath(QStringLiteral("entry_%1.bin").arg(index++));
        if (!writeTextFile(sourcePath, it.value())) {
            SFileCloseArchive(archive);
            if (errorMessage)
                *errorMessage = QStringLiteral("Unable to stage MPQ entry %1.").arg(it.key());
            return false;
        }
        const QByteArray archiveName = QDir::cleanPath(it.key()).replace('/', '\\').toUtf8();
        const DWORD flags = MPQ_FILE_REPLACEEXISTING | MPQ_FILE_COMPRESS | MPQ_FILE_SINGLE_UNIT;
        if (!SFileAddFileEx(archive,
                            reinterpret_cast<const TCHAR *>(sourcePath.utf16()),
                            archiveName.constData(),
                            flags,
                            MPQ_COMPRESSION_ZLIB,
                            MPQ_COMPRESSION_NEXT_SAME)) {
            SFileCloseArchive(archive);
            if (errorMessage)
                *errorMessage = QStringLiteral("SFileAddFileEx failed for %1: %2").arg(it.key()).arg(GetLastError());
            return false;
        }
    }

    if (!SFileFlushArchive(archive)) {
        SFileCloseArchive(archive);
        if (errorMessage)
            *errorMessage = QStringLiteral("SFileFlushArchive failed: %1").arg(GetLastError());
        return false;
    }
    if (!SFileCloseArchive(archive)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("SFileCloseArchive failed: %1").arg(GetLastError());
        return false;
    }
    return true;
}
#endif

QString sampleRootXmlA()
{
    return QStringLiteral(
        "<Root>\n"
        "  <Entries>\n"
        "    <CUnit id=\"LocalDup\"/>\n"
        "    <CUnit id=\"LocalDup\"/>\n"
        "    <CUnit id=\"UnitA\">\n"
        "      <Name value=\"Marine\"/>\n"
        "    </CUnit>\n"
        "  </Entries>\n"
        "</Root>\n");
}

QString sampleRootXmlB()
{
    return QStringLiteral(
        "<Root>\n"
        "  <Entries>\n"
        "    <CUnit id=\"UnitB\">\n"
        "      <Name value=\"Marine\"/>\n"
        "    </CUnit>\n"
        "  </Entries>\n"
        "</Root>\n");
}

QString sampleRootXmlC()
{
    return QStringLiteral(
        "<Root>\n"
        "  <Entries>\n"
        "    <CUnit id=\"UnitA\">\n"
        "      <Name value=\"Marine\"/>\n"
        "    </CUnit>\n"
        "  </Entries>\n"
        "</Root>\n");
}

bool createSampleFolder(QTemporaryDir *tempDir, QString *rootFolder)
{
    if (!tempDir || !tempDir->isValid()) {
        return false;
    }

    const QString root = tempDir->path();
    if (!QDir(root).mkpath(QStringLiteral("GameData"))) {
        return false;
    }

    if (!writeTextFile(QDir(root).absoluteFilePath(QStringLiteral("GameData/A.xml")), sampleRootXmlA().toUtf8())) {
        return false;
    }
    if (!writeTextFile(QDir(root).absoluteFilePath(QStringLiteral("GameData/B.xml")), sampleRootXmlB().toUtf8())) {
        return false;
    }
    if (!writeTextFile(QDir(root).absoluteFilePath(QStringLiteral("GameData/C.xml")), sampleRootXmlC().toUtf8())) {
        return false;
    }
    if (!writeTextFile(QDir(root).absoluteFilePath(QStringLiteral("GameData/readme.txt")), QByteArrayLiteral("not xml"))) {
        return false;
    }

    *rootFolder = root;
    return true;
}

class DynamicUiTestTranslator final : public QTranslator
{
public:
    QString translate(const char *context, const char *sourceText,
                      const char *disambiguation = nullptr, int n = -1) const override
    {
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);
        if (QByteArray(context ? context : "") != QByteArrayLiteral("DynamicUI"))
            return {};
        const QByteArray source(sourceText ? sourceText : "");
        if (source == QByteArrayLiteral("Static caption"))
            return QStringLiteral("Localized caption");
        if (source == QByteArrayLiteral("Overview"))
            return QStringLiteral("Localized overview");
        if (source == QByteArrayLiteral("Object"))
            return QStringLiteral("Localized object");
        if (source == QByteArrayLiteral("Status"))
            return QStringLiteral("Localized status");
        if (source == QByteArrayLiteral("Unused candidate"))
            return QStringLiteral("Localized unused candidate");
        // A real map object may legitimately have this same spelling. The
        // widget tree must preserve runtime/domain values rather than treat
        // them as a display enum merely because a catalog key exists.
        if (source == QByteArrayLiteral("Marine"))
            return QStringLiteral("Must not replace map data");
        return {};
    }

    bool isEmpty() const override { return false; }
};

} // namespace

class CoreTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void folderScanAndAnalysis();
    void analysisCompletenessIsCompleteWhenAllSourcesParse();
    void parseErrorBlocksFalseSafeUnused();
    void incompleteAnalysisRejectsDestructiveApply();
    void changedSourceRejectsStaleDestructiveApply();
    void xmlParseAndLookup();
    void duplicateIdDetection();
    void duplicateContentDetection();
    void duplicateBodyRequiresSameTypeAndExactNestedBody();
    void backupCreation();
    void folderTransactionRollsBackOnValidationFailure();
    void folderTransactionRejectsStaleSourceBeforeCommit();
    void localizationWidgetTreeRetranslatesAndPreservesDomainValue();
    void dryRunGeneration();
    void selectedNodeRemoval();
    void removeMultipleSameNameSiblingsWithoutIndexShift();
    void saveFailureSafety();
    void archiveAnalysis();
    void archiveRewriteRoundTrip();
    void archiveDataCollectionCreatesFileAndListfile();
    void archiveSaveCopyRemovesEntriesAndUpdatesListfile();
    void objectFileFilterUsesFullSourcePath();
    void normalizedDuplicateIgnoresOnlyRootIdentity();
    void tokenAwareReplacementVariants();
    void numericOnlyIdsAreNotRewritten();
    void unifiedReferenceIndexClassifiesStrongWeakAssetAndBinaryReferences();
    void mergePreviewAndApplyRedirectBeforeDelete();
    void mergeAllowsManualUnrelatedExactDuplicateAndActorEvents();
    void mergeRewritesNonXmlReferenceFiles();
    void mergeBlocksBinaryNonRewritableReferences();
    void mergeDoesNotRewriteSurvivingCatalogIdentityIds();
    void mergeAllowsResidualOldIdWarning();
    void mergeRollbackOnFailure();
    void unusedSafetyClassification();
    void unusedReachabilityDistinguishesStatesAndPaths();
    void unusedReachabilityUsesActorUnitNameWithoutTestRefs();
    void unusedObjectChainsCoverFullCatalogGraphAndPlacementRoots();
    void unusedDeletionRemovesWholeUnusedChain();
    void unusedDeletionSkipsPartialChainWithoutFailingBatch();
    void unusedDeletionPreservesDataCollectionLinks();
    void editorRuntimeCatalogObjectsAreProtected();
    void deepCleanupAppliesSafeCandidates();
    void deepCleanupRemovesStructurallyInvalidActorEvents();
    void deepCleanupPlanningFailureLeavesOriginalsByteIdentical();
    void binaryAssetReferencesProtectImports();
    void standaloneModExternalConsumersAreProtected();
    void largeDataCollectionPlansRequireExplicitReview();
    void deepCleanupRemovesRedundantInheritedXmlNodes();
    void deepCleanupReportsAssetAndTriggerOptimization();
    void deepCleanupReportsSemanticDuplicateReview();
    void dependencyUsageReportExportsRealUsagePaths();
    void mapPerformanceAnalyzerBuildsEstimatedStaticRiskHeatmap();
    void readsRealSc2RegionXmlAndPreservesGeometry();
    void realMapRegionReader();
    void malformedOrUnsupportedRegionIsFailClosed();
    void mapPreviewParsesTerrainAndRejectsMalformedHeightData();
    void maximumCompressionFailurePathsPreserveSourceAndOutput();
    void decorationXmlRoundTripAndRegionScopeIsolation();
    void decorationStreamingParsesZonesAndGeneratesGalaxy();
    void decorationStreamingKeepsExternallyReferencedDoodadsStatic();
    void decorationStreamingSupportsManualAssignmentOverrides();
    void decorationStreamingSupportsSparseZoneIds();
    void decorationStreamingRejectsInvalidGalaxyOptions();
    void decorationStreamingBuildsOptimizedObjectsArtifacts();
    void decorationStreamingInjectsGalaxyIncludeOnce();
    void decorationStreamingPreparesArchivePatch();
    void decorationVisibilityStreamingPreservesObjectsAndGeneratesRestoreApi();
    void decorationMapCopyServiceCreatesOptimizedArchive();
    void decorationCliCreatesOptimizedArchiveAndReport();
    void unitFamilyDetectionAndStandardPlanning();
    void renamePlannerStandardizesOwnedCustomCatalogTypes();
    void renamePlannerBlocksConflicts();
    void renamePlannerSkipsDependencyCatalogObjects();
    void renamePlannerSkipsActorUnitsOutsideFamilyScope();
    void catalogEnumRepairFixesLegacyRenameDamage();
    void reservedCatalogFilterTokensAreNotReferences();
    void referenceRenamePreviewAndApply();
    void referenceRenameRewritesSafeTextReferences();
    void referenceRenamePreflightCatchesResidualStrongLinks();
    void referenceRenameBlocksBinaryReferences();
    void referenceRenameDoesNotRewriteFilterFields();
    void referenceRenameDoesNotRewriteUntypedEnumValues();
    void referenceRenameDoesNotRewriteParentFields();
    void referenceRenameUsesTypedCatalogFields();
    void referenceRenameActorIdDoesNotRewriteUnitScope();
    void referenceRenameSkipsOccupiedTargetWhenOwnerNotMoved();
    void referenceRenameRollback();
    void dataCollectionAliasMapping();
    void dataCollectionCreatePreviewAndApply();
    void dataCollectionOffersSingleCustomUnit();
    void dataCollectionUsesRootRaceAndFamilyCategories();
    void dataCollectionFallbackDetectsCustomFamiliesWithoutAtSign();
    void dataCollectionUnitAbilWeaponModeSplitsRoots();
    void dataCollectionTypedPreservesLegacyNonScopedAbilityRecords();
    void dataCollectionTypedSplitPreservesEveryCatalogRecord();
    void dataCollectionPreservationRestoresLossyXml();
    void dataCollectionMigrationPreservesExistingTargetRecords();
    void dataCollectionTypedSplitAssignsCanonicalSharedOwnership();
    void dataCollectionMigrationRollbackRestoresAllCollections();
    void dataCollectionPatternInheritanceValidation();
    void dataCollectionRecognizesEvoSemanticPatternNames();
    void dataCollectionEntityRootsAndConflicts();
    void gargantuaReferenceFixture();
    void gargantuaApplyFixture();
    void zombieWorldUpdate3Audit();
    void dataCollectionUpdatePreservesAndSorts();
    void dataCollectionRollback();
    void autoCollectionSurvivesOptimizationBatch();
    void folderAnalysisStoresFullXmlSource();
    void folderAnalysisCanBeCancelled();
    void unrelatedIdenticalBodiesAreAllowed();
    void m3ParserReadsRealModelFixture();
};

void CoreTests::m3ParserReadsRealModelFixture()
{
    const QStringList paths = {
        QStringLiteral("C:/Users/Vladimir/Downloads/SMX3_Marine.m3"),
        QStringLiteral("C:/Users/Vladimir/Downloads/BattleStation_ConstructionRobot.m3")
    };
    if (!QFileInfo::exists(paths.front()))
        QSKIP("Real M3 fixture is unavailable.");

    for (const QString &path : paths)
    {
        if (!QFileInfo::exists(path))
            continue;
        QFile file(path);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(path));
        M3Model model;
        QString error;
        QVERIFY2(M3ModelParser().parseStaticModel(file.readAll(), &model, &error), qPrintable(path + QStringLiteral(": ") + error));
        QVERIFY2(!model.vertices.isEmpty(), qPrintable(path));
        QVERIFY2(!model.triangles.isEmpty(), qPrintable(path));
        QVERIFY2(model.boneCount > 0, qPrintable(path));
        QCOMPARE(model.bones.size(), model.boneCount);
        QVERIFY2(std::any_of(model.bones.cbegin(), model.bones.cend(), [](const M3Bone &bone) {
            return !bone.name.isEmpty();
        }), qPrintable(path));
        QVERIFY2(!model.sequences.isEmpty(), qPrintable(path));
        QVERIFY2(!model.materials.isEmpty(), qPrintable(path));
        QVERIFY2(std::any_of(model.materials.cbegin(), model.materials.cend(), [](const M3Material &material) {
            return !material.diffuseTexturePath.isEmpty();
        }), qPrintable(path));
        QVERIFY2(std::any_of(model.triangles.cbegin(), model.triangles.cend(), [](const M3Triangle &triangle) {
            return triangle.materialIndex >= 0;
        }), qPrintable(path));
        QVERIFY2(!model.diagnostics.isEmpty(), qPrintable(path));
    }
}

void CoreTests::initTestCase()
{
    qputenv("SC2DH_ENABLE_TEST_REFS", "1");
}

void CoreTests::analysisCompletenessIsCompleteWhenAllSourcesParse()
{
    QTemporaryDir dir;
    QString root;
    QVERIFY(createSampleFolder(&dir, &root));

    AnalysisResult analysis;
    QString error;
    QVERIFY2(FolderAnalyzer().analyzeFolder(root, {}, &analysis, &error), qPrintable(error));
    QCOMPARE(analysis.completeness, AnalysisCompleteness::Complete);
    QVERIFY(analysis.sourceDiscoveryComplete);
    QVERIFY(analysis.referenceExtractionComplete);
    QVERIFY(analysis.dependencyGraphComplete);
    QVERIFY(analysis.parseErrors.isEmpty());
    QVERIFY(!analysis.sourceRevisions.isEmpty());
    QVERIFY(canApplyDestructiveChanges(analysis).allowed);
}

void CoreTests::parseErrorBlocksFalseSafeUnused()
{
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("GameData")));
    QVERIFY(writeTextFile(QDir(dir.path()).absoluteFilePath(QStringLiteral("GameData/UnitData.xml")),
                          QByteArrayLiteral("<Catalog><CUnit id=\"MyUnit\"/></Catalog>")));
    QVERIFY(writeTextFile(QDir(dir.path()).absoluteFilePath(QStringLiteral("GameData/BrokenActorData.xml")),
                          QByteArrayLiteral("<Catalog><CActorUnit id=\"Broken\" unitName=\"MyUnit\">")));

    AnalysisResult analysis;
    QString error;
    QVERIFY2(FolderAnalyzer().analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    QCOMPARE(analysis.completeness, AnalysisCompleteness::Partial);
    QCOMPARE(analysis.parseErrors.size(), 1);
    QVERIFY(analysis.possibleUnusedNodeIndices.isEmpty());

    const auto candidate = std::find_if(analysis.unusedCandidates.cbegin(), analysis.unusedCandidates.cend(),
                                        [&analysis](const UnusedCandidateInfo &item) {
                                            return item.nodeIndex >= 0
                                                && item.nodeIndex < analysis.nodes.size()
                                                && analysis.nodes[item.nodeIndex].id == QStringLiteral("MyUnit");
                                        });
    QVERIFY(candidate != analysis.unusedCandidates.cend());
    QCOMPARE(candidate->state, CandidateState::Blocked);
    QCOMPARE(candidate->removalSafety, RemovalSafety::BlockedIncompleteAnalysis);
    QCOMPARE(candidate->usageState, UsageState::Blocked);
    QVERIFY(candidate->reason.contains(QStringLiteral("incomplete analysis"), Qt::CaseInsensitive));
}

void CoreTests::incompleteAnalysisRejectsDestructiveApply()
{
    QTemporaryDir dir;
    const QString unitPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("UnitData.xml"));
    QVERIFY(writeTextFile(unitPath, QByteArrayLiteral("<Catalog><CUnit id=\"MyUnit\"/></Catalog>")));
    QVERIFY(writeTextFile(QDir(dir.path()).absoluteFilePath(QStringLiteral("BrokenActorData.xml")),
                          QByteArrayLiteral("<Catalog><CActorUnit id=\"Broken\" unitName=\"MyUnit\">")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    QString backup;
    QStringList changed;
    int removed = -1;
    int skipped = -1;
    QVERIFY(!analyzer.applySelectedChanges(analysis, {0}, dir.path(), {},
                                           &backup, &error, &changed, &removed, &skipped));
    QVERIFY(error.contains(QStringLiteral("analysis-incomplete")));
    QFile original(unitPath);
    QVERIFY(original.open(QIODevice::ReadOnly));
    QVERIFY(original.readAll().contains("MyUnit"));
}

void CoreTests::changedSourceRejectsStaleDestructiveApply()
{
    QTemporaryDir dir;
    const QString dataPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("Data.xml"));
    QVERIFY(writeTextFile(dataPath, QByteArrayLiteral("<Catalog><CEffect id=\"Orphan\"/></Catalog>")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    QCOMPARE(analysis.completeness, AnalysisCompleteness::Complete);
    QCOMPARE(analysis.possibleUnusedNodeIndices.size(), 1);

    QVERIFY(writeTextFile(dataPath, QByteArrayLiteral("<Catalog><CEffect id=\"Orphan\"/><CUnit id=\"NewUnit\"/></Catalog>")));
    QString backup;
    QStringList changed;
    int removed = -1;
    int skipped = -1;
    QVERIFY(!analyzer.applySelectedChanges(analysis, analysis.possibleUnusedNodeIndices,
                                           dir.path(), {}, &backup, &error,
                                           &changed, &removed, &skipped));
    QVERIFY(error.contains(QStringLiteral("source-changed")));
    QVERIFY(error.contains(QStringLiteral("stale"), Qt::CaseInsensitive));
    QFile current(dataPath);
    QVERIFY(current.open(QIODevice::ReadOnly));
    QVERIFY(current.readAll().contains("NewUnit"));
}

void CoreTests::unrelatedIdenticalBodiesAreAllowed()
{
    QTemporaryDir dir;
    QVERIFY(writeTextFile(QDir(dir.path()).absoluteFilePath(QStringLiteral("Data.xml")), QByteArrayLiteral(
        "<Catalog><CEffect id=\"VasselAttack\"><Amount value=\"5\"/></CEffect>"
        "<CEffect id=\"VasselReady\"><Amount value=\"5\"/></CEffect></Catalog>")));
    FolderAnalyzer analyzer; AnalysisResult analysis; QString error;
    QVERIFY(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error));
    QCOMPARE(analysis.duplicateContentGroups.size(), 1);
    const DuplicateContentGroup &group = analysis.duplicateContentGroups.front();
    QVERIFY(group.mergeCandidate);
    QVERIFY(!group.autoRecommended);
    QCOMPARE(group.commonIdMask, QStringLiteral("unrelated IDs"));
    for (const DataNode &node : analysis.nodes) QVERIFY(node.duplicateContent);
}

void CoreTests::folderAnalysisStoresFullXmlSource()
{
    QTemporaryDir dir; QString root; QVERIFY(createSampleFolder(&dir, &root));
    FolderAnalyzer analyzer; AnalysisResult analysis; QString error;
    QVERIFY(analyzer.analyzeFolder(root, {}, &analysis, &error));
    const QString path = QDir(root).absoluteFilePath(QStringLiteral("GameData/A.xml"));
    QVERIFY(analysis.sourceXmlByFile.contains(path));
    QVERIFY(analysis.sourceXmlByFile.value(path).contains(QStringLiteral("<CUnit id=\"UnitA\"")));
}

void CoreTests::folderAnalysisCanBeCancelled()
{
    QTemporaryDir dir; QString root; QVERIFY(createSampleFolder(&dir, &root));
    FolderAnalyzer analyzer; AnalysisResult analysis; QString error; bool cancel = false; int updates = 0;
    const bool success = analyzer.analyzeFolder(root, {}, &analysis, &error,
        [&cancel, &updates](int current, int, const QString &) { ++updates; if (current >= 0) cancel = true; },
        [&cancel] { return cancel; });
    QVERIFY(!success);
    QCOMPARE(error, QStringLiteral("Analysis canceled."));
    QVERIFY(updates > 0);
}

void CoreTests::dataCollectionAliasMapping()
{
    DataCollectionAliasMapper mapper;
    const auto alias = [&mapper](const QString &type, const QString &id, UnitFamilyRole role) {
        DataNode node; node.elementName = type; node.id = id; return mapper.aliasFor(node, QStringLiteral("Vassel"), role);
    };
    QCOMPARE(alias(QStringLiteral("CUnit"), QStringLiteral("Vassel"), UnitFamilyRole::Unit), QStringLiteral("Unit,Vassel"));
    QCOMPARE(alias(QStringLiteral("CActorUnit"), QStringLiteral("VasselActor"), UnitFamilyRole::Actor), QStringLiteral("Actor,VasselActor"));
    QCOMPARE(alias(QStringLiteral("CActorUnit"), QStringLiteral("Vassel@Actor"), UnitFamilyRole::Actor), QStringLiteral("Actor,Vassel@Actor"));
    QCOMPARE(alias(QStringLiteral("CTexture"), QStringLiteral("Vassel@Texture"), UnitFamilyRole::Other), QStringLiteral("Texture,Vassel@Texture"));
    QCOMPARE(alias(QStringLiteral("CRequirementNode"), QStringLiteral("Vassel@RequirementNode"), UnitFamilyRole::Other), QStringLiteral("RequirementNode,Vassel@RequirementNode"));
    QCOMPARE(alias(QStringLiteral("CConversationState"), QStringLiteral("Vassel@Conversation"), UnitFamilyRole::Other), QStringLiteral("ConversationState,Vassel@Conversation"));
}

void CoreTests::dataCollectionCreatePreviewAndApply()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Family.xml"));
    const QByteArray original = QByteArrayLiteral(
        "<Catalog><CUnit id=\"Vassel@Unit\" refs=\"Vassel@Actor Vassel@Button Vassel@Model Vassel@Attack Vassel@Ready Vassel@Weapon Vassel@AttackDamage\"/>"
        "<CActorUnit id=\"Vassel@Actor\" unitName=\"Vassel@Unit\"/><CButton id=\"Vassel@Button\"/><CModel id=\"Vassel@Model\"/>"
        "<CSound id=\"Vassel@Attack\"/><CSound id=\"Vassel@Ready\"/><CWeapon id=\"Vassel@Weapon\"/><CEffect id=\"Vassel@AttackDamage\"/></Catalog>");
    QVERIFY(writeTextFile(path, original));
    FolderAnalyzer analyzer; AnalysisResult analysis; QString error;
    QVERIFY(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error));
    const QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(analysis, DataCollectionMode::Unit);
    QCOMPARE(families.size(), 1);
    DataCollectionBuildRequest request; request.family = families.front();
    DataCollectionUnitBuilder builder;
    const DataCollectionPreviewReport preview = builder.preview(analysis, request);
    QVERIFY(preview.valid);
    QVERIFY(!preview.existingCollection);
    QVERIFY(preview.generatedXml.contains(QStringLiteral("<CDataCollectionUnit id=\"Vassel\"")));
    QVERIFY(!preview.generatedXml.contains(QStringLiteral("parent=")));
    QVERIFY(preview.generatedXml.startsWith(QStringLiteral("<?xml")));
    QVERIFY(preview.generatedXml.contains(QStringLiteral("<Catalog>")));
    QVERIFY(preview.targetFile.endsWith(QStringLiteral("DataCollectionData.xml")));
    DataCollectionBuildRequest orderedRequest = request;
    orderedRequest.parent = QStringLiteral("UnitGround");
    const QString orderedXml = builder.preview(analysis, orderedRequest).generatedXml;
    const qsizetype firstPattern = orderedXml.indexOf(QStringLiteral("<CDataCollectionPattern"));
    const qsizetype firstTemplate = orderedXml.indexOf(QStringLiteral(" default=\"1\""));
    const qsizetype firstRecord = orderedXml.indexOf(QStringLiteral("<DataRecord"));
    QVERIFY(firstPattern >= 0);
    QVERIFY(firstTemplate > firstPattern);
    QVERIFY(firstRecord > firstTemplate);
    QVERIFY(preview.listfileNeedsUpdate);
    QVERIFY(preview.generatedXml.contains(QStringLiteral("Entry=\"Actor,Vassel@Actor\"")));
    QVERIFY(preview.generatedXml.contains(QStringLiteral("Entry=\"Weapon,Vassel@Weapon\"")));
    QVERIFY(preview.generatedXml.contains(QStringLiteral("Entry=\"Effect,Vassel@AttackDamage\"")));
    DataCollectionBuildRequest renamedRequest = request;
    renamedRequest.requestedUnitId = QStringLiteral("VasselRenamed");
    const DataCollectionPreviewReport renamedPreview = builder.preview(analysis, renamedRequest);
    QVERIFY(!renamedPreview.valid);
    QVERIFY(renamedPreview.warnings.join(QStringLiteral(" ")).contains(QStringLiteral("Collection ID")));
    QFile unchanged(path); QVERIFY(unchanged.open(QIODevice::ReadOnly)); QCOMPARE(unchanged.readAll(), original); unchanged.close();
    const int buttonAt = preview.generatedXml.indexOf(QStringLiteral("Button,Vassel@Button"));
    const int unitAt = preview.generatedXml.indexOf(QStringLiteral("Unit,Vassel@Unit"));
    const int actorAt = preview.generatedXml.indexOf(QStringLiteral("Actor,Vassel@Actor"));
    const int modelAt = preview.generatedXml.indexOf(QStringLiteral("Model,Vassel@Model"));
    const int soundAt = preview.generatedXml.indexOf(QStringLiteral("Sound,Vassel@Attack"));
    const int weaponAt = preview.generatedXml.indexOf(QStringLiteral("Weapon,Vassel@Weapon"));
    const int effectAt = preview.generatedXml.indexOf(QStringLiteral("Effect,Vassel@AttackDamage"));
    QVERIFY(buttonAt < unitAt && unitAt < actorAt && actorAt < modelAt && modelAt < soundAt && soundAt < weaponAt && weaponAt < effectAt);
    const DataCollectionApplyResult applied = builder.apply(analysis, request, dir.path(), {});
    QVERIFY2(applied.success, qPrintable(applied.error));
    QVERIFY(QFileInfo(applied.backupFolder).exists());
    QFile unchangedAfter(path); QVERIFY(unchangedAfter.open(QIODevice::ReadOnly)); QCOMPARE(unchangedAfter.readAll(), original); unchangedAfter.close();
    QFile output(QDir(dir.path()).absoluteFilePath(QStringLiteral("DataCollectionData.xml")));
    QVERIFY(output.open(QIODevice::ReadOnly)); const QString xml = QString::fromUtf8(output.readAll());
    QVERIFY(xml.contains(QStringLiteral("CDataCollectionUnit id=\"Vassel\"")));
    QVERIFY(!xml.contains(QStringLiteral("id=\"Vassel@")));
    QFile listfile(QDir(dir.path()).absoluteFilePath(QStringLiteral("(listfile)")));
    QVERIFY(listfile.open(QIODevice::ReadOnly));
    QVERIFY(QString::fromUtf8(listfile.readAll()).contains(QStringLiteral("DataCollectionData.xml")));
}

void CoreTests::dataCollectionFallbackDetectsCustomFamiliesWithoutAtSign()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Family.xml"));
    QVERIFY(writeTextFile(path, QByteArrayLiteral(
        "<Catalog>"
        "<CUnit id=\"Archon\" actor=\"ArchonActor\" button=\"ArchonButton\" weapon=\"ArchonWeapon\"/>"
        "<CActorUnit id=\"ArchonActor\" unitName=\"Archon\" model=\"ArchonModel\"/>"
        "<CButton id=\"ArchonButton\"/>"
        "<CWeapon id=\"ArchonWeapon\"/>"
        "<CModel id=\"ArchonModel\"/>"
        "</Catalog>")));
    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    const QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(analysis, DataCollectionMode::Unit);
    QCOMPARE(families.size(), 1);
    QCOMPARE(families.front().rootId, QStringLiteral("Archon"));

    DataCollectionBuildRequest request;
    request.family = families.front();
    DataCollectionUnitBuilder builder;
    const DataCollectionPreviewReport preview = builder.preview(analysis, request);
    QVERIFY(preview.valid);
    QVERIFY(preview.generatedXml.contains(QStringLiteral("<CDataCollectionUnit id=\"Archon\"")));
    QVERIFY(preview.generatedXml.contains(QStringLiteral("Entry=\"Unit,Archon\"")));
    QVERIFY(preview.generatedXml.contains(QStringLiteral("Entry=\"Actor,ArchonActor\"")));
    QVERIFY(preview.generatedXml.contains(QStringLiteral("Entry=\"Button,ArchonButton\"")));
    QVERIFY(preview.generatedXml.contains(QStringLiteral("Entry=\"Weapon,ArchonWeapon\"")));
    QVERIFY(preview.warnings.join(QStringLiteral(" ")).contains(QStringLiteral("non-standard")));
}

void CoreTests::dataCollectionUnitAbilWeaponModeSplitsRoots()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Gargantua.xml"));
    QVERIFY(writeTextFile(path, QByteArrayLiteral(
        "<Catalog>"
        "<CUnit id=\"Gargantua\"><AbilArray Link=\"Gargantua_Jump\"/><WeaponArray Link=\"Gargantua_Weapon\"/></CUnit>"
        "<CActorUnit id=\"Gargantua\"/><CModel id=\"Gargantua@Portrait\"/>"
        "<CAbilEffectTarget id=\"Gargantua_Jump\"><Effect value=\"Gargantua_Jump@Damage\"/></CAbilEffectTarget>"
        "<CEffectDamage id=\"Gargantua_Jump@Damage\"/>"
        "<CWeaponLegacy id=\"Gargantua_Weapon\"><Effect value=\"Gargantua_Weapon@Damage\"/></CWeaponLegacy>"
        "<CEffectDamage id=\"Gargantua_Weapon@Damage\"/>"
        "<CDataCollectionUnit id=\"Gargantua\"><DataRecord Entry=\"Unit,Gargantua\"/>"
        "<DataRecord Entry=\"Abil,Gargantua_Jump\"/><DataRecord Entry=\"Weapon,Gargantua_Weapon\"/></CDataCollectionUnit>"
        "</Catalog>")));
    FolderAnalyzer analyzer; AnalysisResult analysis; QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    const QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(analysis, DataCollectionMode::UnitAbilWeapon);
    const auto findFamily = [&](const QString &id) {
        return std::find_if(families.cbegin(), families.cend(), [&](const UnitFamily &family) { return family.rootId == id; });
    };
    const auto unit = findFamily(QStringLiteral("Gargantua"));
    const auto ability = findFamily(QStringLiteral("Gargantua_Jump"));
    const auto weapon = findFamily(QStringLiteral("Gargantua_Weapon"));
    QVERIFY(unit != families.cend()); QVERIFY(ability != families.cend()); QVERIFY(weapon != families.cend());
    QCOMPARE(unit->recommendedParent, QStringLiteral("UnitGround"));
    QCOMPARE(ability->recommendedParent, QStringLiteral("AbilityBase"));
    QCOMPARE(weapon->recommendedParent, QStringLiteral("Weapon_Instant"));
    DataCollectionBuildRequest request; request.family = *unit;
    const DataCollectionPreviewReport preview = DataCollectionUnitBuilder().preview(analysis, request);
    QVERIFY(preview.recordsToRemove.isEmpty());
    QVERIFY(preview.generatedXml.contains(QStringLiteral("<CDataCollectionAbil id=\"Gargantua_Jump\" parent=\"AbilityBase\">")));
    QVERIFY(preview.generatedXml.contains(QStringLiteral("<CDataCollection id=\"Gargantua_Weapon\" parent=\"Weapon_Instant\">")));
    QCOMPARE(preview.recordsToMove.size(), 2);
    const DataCollectionApplyResult applied = DataCollectionUnitBuilder().apply(analysis, request, dir.path(), {});
    QVERIFY2(applied.success, qPrintable(applied.error));
    QCOMPARE(applied.recordsRemoved, 0);
    QFile updated(path); QVERIFY(updated.open(QIODevice::ReadOnly)); const QByteArray updatedXml = updated.readAll();
    QCOMPARE(updatedXml.count("Entry=\"Abil,Gargantua_Jump\""), 2);
    QCOMPARE(updatedXml.count("Entry=\"Weapon,Gargantua_Weapon\""), 2);
    QVERIFY(updatedXml.contains("id=\"Gargantua_Jump\""));
    QVERIFY(updatedXml.contains("id=\"Gargantua_Weapon\""));
    QVERIFY(updatedXml.contains("id=\"Gargantua\" parent=\"UnitGround\""));
    QVERIFY(updatedXml.contains("id=\"Gargantua_Jump\" parent=\"AbilityBase\""));
    QVERIFY(updatedXml.contains("id=\"Gargantua_Weapon\" parent=\"Weapon_Instant\""));
    QVERIFY(updatedXml.contains("id=\"AbilityPattern_Missile\""));
    QVERIFY(updatedXml.contains("id=\"WeaponPattern_Base\""));
    QVERIFY(updatedXml.contains("Entry=\"Abil,Gargantua_Jump\""));
    QVERIFY(updatedXml.contains("Entry=\"Weapon,Gargantua_Weapon\""));
}

void CoreTests::dataCollectionTypedPreservesLegacyNonScopedAbilityRecords()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Ability.xml"));
    QVERIFY(writeTextFile(path, QByteArrayLiteral(
        "<Catalog>"
        "<CDataCollectionPattern id=\"AbilityPattern_Base\"/>"
        "<CDataCollectionAbil default=\"1\" id=\"AbilityBase\"><Pattern value=\"AbilityPattern_Base\"/></CDataCollectionAbil>"
        "<CAbilEffectTarget id=\"AdeptStage2\" refs=\"InfestedTerransCreateEgg2 LegacyMover\"/>"
        "<CButton id=\"AdeptPassive\"/><CButton id=\"AdeptPhaseShift\"/>"
        "<CActorUnit id=\"Adept\"/><CActorUnit id=\"Adept2\"/>"
        "<CEffectCreateUnit id=\"InfestedTerransCreateEgg2\"/>"
        "<CEffectDamage id=\"InfestedTerransImpact2\"/>"
        "<CEffectSet id=\"InfestedTerransInitialSet2\"/>"
        "<CMover id=\"LegacyMover\"/>"
        "<CDataCollectionAbil id=\"AdeptStage2\" parent=\"AbilityBase\">"
        "<DataRecord Entry=\"Button,AdeptPassive\"/>"
        "<DataRecord Entry=\"Button,AdeptPhaseShift\"/>"
        "<DataRecord Entry=\"Actor,Adept\"/>"
        "<DataRecord Entry=\"Actor,Adept2\"/>"
        "<DataRecord Entry=\"Abil,AdeptStage2\"/>"
        "<DataRecord Entry=\"Effect,InfestedTerransCreateEgg2\"/>"
        "<DataRecord Entry=\"Effect,InfestedTerransImpact2\"/>"
        "<DataRecord Entry=\"Effect,InfestedTerransInitialSet2\"/>"
        "<DataRecord Entry=\"Mover,LegacyMover\"/>"
        "</CDataCollectionAbil>"
        "</Catalog>")));
    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    const QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(
        analysis, DataCollectionMode::UnitAbilWeapon);
    const auto family = std::find_if(families.cbegin(), families.cend(), [](const UnitFamily &value) {
        return value.rootId == QStringLiteral("AdeptStage2")
            && value.entityType == DataCollectionEntityType::Ability;
    });
    QVERIFY(family != families.cend());
    QCOMPARE(family->recommendedParent, QStringLiteral("AbilityMisssile"));

    DataCollectionBuildRequest request;
    request.family = *family;
    const DataCollectionPreviewReport preview = DataCollectionUnitBuilder().preview(analysis, request, &families);
    QVERIFY2(preview.valid, qPrintable(preview.warnings.join(QStringLiteral("; "))));
    QVERIFY(preview.existingRecordsPreserved.contains(QStringLiteral("Button,AdeptPassive")));
    QVERIFY(preview.existingRecordsPreserved.contains(QStringLiteral("Actor,Adept")));
    QVERIFY(preview.existingRecordsPreserved.contains(QStringLiteral("Effect,InfestedTerransImpact2")));
    QVERIFY(preview.existingRecordsPreserved.contains(QStringLiteral("Mover,LegacyMover")));
    QVERIFY(preview.generatedXml.contains(QStringLiteral("Entry=\"Button,AdeptPassive\"")));
    QVERIFY(preview.generatedXml.contains(QStringLiteral("Entry=\"Actor,Adept2\"")));
    QVERIFY(preview.generatedXml.contains(QStringLiteral("Entry=\"Effect,InfestedTerransInitialSet2\"")));
    QVERIFY(preview.generatedXml.contains(QStringLiteral("Entry=\"Mover,LegacyMover\"")));
}

void CoreTests::dataCollectionTypedSplitPreservesEveryCatalogRecord()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Titan.xml"));
    const QStringList records{
        QStringLiteral("Unit,Titan"), QStringLiteral("Actor,TitanActor"), QStringLiteral("Model,TitanModel"),
        QStringLiteral("Sound,TitanDeath"), QStringLiteral("Button,TitanButton"),
        QStringLiteral("Abil,TitanLeap"), QStringLiteral("Button,TitanLeapButton"),
        QStringLiteral("Effect,TitanLeapSet"), QStringLiteral("Effect,TitanLeapDamage"),
        QStringLiteral("Validator,TitanLeapRange"), QStringLiteral("Mover,TitanLeapMover"),
        QStringLiteral("Weapon,TitanGun"), QStringLiteral("Effect,TitanGunDamage"),
        QStringLiteral("Upgrade,TitanUpgrade"), QStringLiteral("Effect,TitanUpgradeApply"),
        QStringLiteral("Requirement,TitanUpgradeRequirement"), QStringLiteral("RequirementNode,TitanUpgradeNode")
    };
    QString collectionRecords;
    for (const QString &record : records)
        collectionRecords += QStringLiteral("<DataRecord Entry=\"%1\"/>").arg(record);
    const QString xml = QStringLiteral(
        "<Catalog>"
        "<CUnit id=\"Titan\" refs=\"TitanActor TitanButton TitanLeap TitanGun TitanUpgrade\"/>"
        "<CActorUnit id=\"TitanActor\" unitName=\"Titan\" refs=\"TitanModel TitanDeath\"/>"
        "<CModel id=\"TitanModel\"/><CSound id=\"TitanDeath\"/><CButton id=\"TitanButton\"/>"
        "<CAbilEffectTarget id=\"TitanLeap\" refs=\"TitanLeapButton TitanLeapSet TitanLeapMover\"/>"
        "<CButton id=\"TitanLeapButton\"/><CEffectSet id=\"TitanLeapSet\" refs=\"TitanLeapDamage\"/>"
        "<CEffectDamage id=\"TitanLeapDamage\" refs=\"TitanLeapRange\"/><CValidatorUnitCompareRange id=\"TitanLeapRange\"/>"
        "<CMover id=\"TitanLeapMover\"/>"
        "<CWeaponLegacy id=\"TitanGun\" refs=\"TitanGunDamage\"/><CEffectDamage id=\"TitanGunDamage\"/>"
        "<CUpgrade id=\"TitanUpgrade\" refs=\"TitanUpgradeApply\"/><CEffectModifyUnit id=\"TitanUpgradeApply\" refs=\"TitanUpgradeRequirement\"/>"
        "<CRequirement id=\"TitanUpgradeRequirement\" refs=\"TitanUpgradeNode\"/><CRequirementNode id=\"TitanUpgradeNode\"/>"
        "<CDataCollectionUnit id=\"Titan\">%1</CDataCollectionUnit>"
        "</Catalog>").arg(collectionRecords);
    QVERIFY(writeTextFile(path, xml.toUtf8()));

    FolderAnalyzer analyzer; AnalysisResult analysis; QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    const QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(
        analysis, DataCollectionMode::UnitAbilWeapon);
    QSet<int> allObjects;
    for (const UnitFamily &family : families)
        for (const UnitFamilyObject &object : family.objects) allObjects.insert(object.nodeIndex);
    QCOMPARE(allObjects.size(), 13); // Upgrade and its helper chain are not roots in UnitAbilWeapon mode.
    for (const QString &root : {QStringLiteral("Titan"), QStringLiteral("TitanLeap"),
                                QStringLiteral("TitanGun")})
        QVERIFY(std::any_of(families.cbegin(), families.cend(), [&](const UnitFamily &family) { return family.rootId == root; }));
    QVERIFY(std::none_of(families.cbegin(), families.cend(), [](const UnitFamily &family) {
        return family.rootId == QStringLiteral("TitanUpgrade");
    }));

    const auto unit = std::find_if(families.cbegin(), families.cend(), [](const UnitFamily &family) {
        return family.rootId == QStringLiteral("Titan");
    });
    QVERIFY(unit != families.cend());
    DataCollectionBuildRequest request; request.family = *unit;
    const DataCollectionPreviewReport preview = DataCollectionUnitBuilder().preview(analysis, request);
    QVERIFY2(preview.valid, qPrintable(preview.warnings.join(QStringLiteral("; "))));
    QCOMPARE(preview.recordsToMove.size(), 2);
    QCOMPARE(preview.falsePositiveAssociations.size(), 4);
    QVERIFY(preview.warnings.join(QStringLiteral(" ")).contains(QStringLiteral("preserved in no-loss mode")));
    QVERIFY(preview.generatedXml.contains(QStringLiteral("id=\"TitanLeap\"")));
    QVERIFY(preview.generatedXml.contains(QStringLiteral("Entry=\"Mover,TitanLeapMover\"")));
    QVERIFY(preview.manualReviewObjects.contains(QStringLiteral("Upgrade,TitanUpgrade")));
}

void CoreTests::dataCollectionPreservationRestoresLossyXml()
{
    QByteArray baseline = QByteArrayLiteral(
        "<Catalog>"
        "<CDataCollectionUnit id=\"Alpha\">"
        "<DataRecord Entry=\"Unit,Alpha\" keep=\"1\"/>"
        "<DataRecord Entry=\"Actor,AlphaActor\"/>"
        "<DataRecord Entry=\"Actor,AlphaActor\" duplicate=\"1\"/>"
        "</CDataCollectionUnit>"
        "<CDataCollection id=\"WeaponAlpha\">"
        "<DataRecord Entry=\"Weapon,WeaponAlpha\"/>"
        "<DataRecord Entry=\"Effect,LegacyDamage\"/>"
        "</CDataCollection>"
        "</Catalog>");
    QByteArray lossy = QByteArrayLiteral(
        "<Catalog>"
        "<CDataCollectionUnit id=\"Alpha\">"
        "<DataRecord Entry=\"Unit,Alpha\"/>"
        "<DataRecord Entry=\"Behavior,NewBuff\"/>"
        "</CDataCollectionUnit>"
        "</Catalog>");

    DataCollectionPreservationReport report;
    QString error;
    QVERIFY2(restoreMissingDataCollectionRecords(baseline, &lossy, &report, &error), qPrintable(error));
    QCOMPARE(report.missingBeforeRestore, 4);
    QCOMPARE(report.restoredRecords, 4);
    QCOMPARE(report.missingAfterRestore, 0);
    QCOMPARE(report.addedRecords, 1);
    QVERIFY(lossy.contains("Entry=\"Actor,AlphaActor\""));
    QCOMPARE(lossy.count("Entry=\"Actor,AlphaActor\""), 2);
    QVERIFY(lossy.contains("duplicate=\"1\""));
    QVERIFY(lossy.contains("Entry=\"Effect,LegacyDamage\""));
    QVERIFY(lossy.contains("Entry=\"Behavior,NewBuff\""));
}

void CoreTests::dataCollectionMigrationPreservesExistingTargetRecords()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Carrier.xml"));
    QVERIFY(writeTextFile(path, QByteArrayLiteral(
        "<Catalog>"
        "<CUnit id=\"Carrier\"><AbilArray Link=\"Launch\"/></CUnit>"
        "<CAbilEffectTarget id=\"Launch\"><Effect value=\"LaunchEffect\"/></CAbilEffectTarget>"
        "<CEffectDamage id=\"LaunchEffect\"/>"
        "<CUpgrade id=\"LegacyUpgrade\"/>"
        "<CDataCollectionUnit id=\"Carrier\"><DataRecord Entry=\"Unit,Carrier\"/>"
        "<DataRecord Entry=\"Abil,Launch\"/></CDataCollectionUnit>"
        "<CDataCollectionAbil id=\"Launch\"><DataRecord Entry=\"Upgrade,LegacyUpgrade\"/></CDataCollectionAbil>"
        "</Catalog>")));

    FolderAnalyzer analyzer; AnalysisResult analysis; QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    const QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(
        analysis, DataCollectionMode::UnitAbilWeapon);
    const auto unit = std::find_if(families.cbegin(), families.cend(), [](const UnitFamily &family) {
        return family.rootId == QStringLiteral("Carrier");
    });
    QVERIFY(unit != families.cend());

    DataCollectionBuildRequest request; request.family = *unit;
    const DataCollectionPreviewReport preview = DataCollectionUnitBuilder().preview(analysis, request, &families);
    QVERIFY2(preview.valid, qPrintable(preview.warnings.join(QStringLiteral("; "))));
    QVERIFY(preview.recordsToRemove.isEmpty());
    QVERIFY(std::any_of(preview.recordsToMove.cbegin(), preview.recordsToMove.cend(), [](const QString &move) {
        return move.startsWith(QStringLiteral("Abil,Launch -> Launch"));
    }));

    const DataCollectionApplyResult applied = DataCollectionUnitBuilder().apply(
        analysis, request, dir.path(), {}, true, &families);
    QVERIFY2(applied.success, qPrintable(applied.error));
    QCOMPARE(applied.recordsRemoved, 0);

    QFile updated(path);
    QVERIFY(updated.open(QIODevice::ReadOnly));
    const QByteArray updatedXml = updated.readAll();
    QVERIFY(updatedXml.contains("Entry=\"Upgrade,LegacyUpgrade\""));
    QCOMPARE(updatedXml.count("Entry=\"Abil,Launch\""), 2);
}

void CoreTests::dataCollectionTypedSplitAssignsCanonicalSharedOwnership()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Shared.xml"));
    QVERIFY(writeTextFile(path, QByteArrayLiteral(
        "<Catalog><CUnit id=\"SharedUnit\" refs=\"AbilityOne AbilityTwo\"/>"
        "<CAbilEffectTarget id=\"AbilityOne\" refs=\"SharedEffect\"/>"
        "<CAbilEffectTarget id=\"AbilityTwo\" refs=\"SharedEffect\"/>"
        "<CEffectDamage id=\"SharedEffect\"/>"
        "<CDataCollectionUnit id=\"SharedUnit\">"
        "<DataRecord Entry=\"Unit,SharedUnit\"/><DataRecord Entry=\"Abil,AbilityOne\"/>"
        "<DataRecord Entry=\"Abil,AbilityTwo\"/><DataRecord Entry=\"Effect,SharedEffect\"/>"
        "</CDataCollectionUnit></Catalog>")));
    FolderAnalyzer analyzer; AnalysisResult analysis; QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    const QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(
        analysis, DataCollectionMode::UnitAbilWeapon);
    const auto contains = [&](const QString &root, const QString &id) {
        const auto family = std::find_if(families.cbegin(), families.cend(), [&](const UnitFamily &value) {
            return value.rootId == root;
        });
        if (family == families.cend()) return false;
        return std::any_of(family->objects.cbegin(), family->objects.cend(), [&](const UnitFamilyObject &object) {
            return analysis.nodes[object.nodeIndex].id == id;
        });
    };
    QVERIFY(contains(QStringLiteral("AbilityOne"), QStringLiteral("SharedEffect")));
    QVERIFY(!contains(QStringLiteral("AbilityTwo"), QStringLiteral("SharedEffect")));
    const auto ability = std::find_if(families.cbegin(), families.cend(), [](const UnitFamily &family) {
        return family.rootId == QStringLiteral("AbilityOne");
    });
    QVERIFY(ability != families.cend());
    const auto shared = std::find_if(ability->objects.cbegin(), ability->objects.cend(), [&](const UnitFamilyObject &object) {
        return analysis.nodes[object.nodeIndex].id == QStringLiteral("SharedEffect");
    });
    QVERIFY(shared != ability->objects.cend());
    QCOMPARE(shared->role, UnitFamilyRole::Effect);
    QCOMPARE(shared->confidence, QStringLiteral("Shared canonical"));
    DataCollectionBuildRequest request; request.family = *ability;
    request.confirmNonStandard = true;
    const DataCollectionPreviewReport preview = DataCollectionUnitBuilder().preview(analysis, request, &families);
    QVERIFY2(preview.valid, qPrintable(preview.warnings.join(QStringLiteral("; "))));
    QCOMPARE(preview.generatedXml.count(QStringLiteral("Entry=\"Effect,SharedEffect\"")), 1);

    for (const QString &root : {QStringLiteral("SharedUnit"), QStringLiteral("AbilityOne"), QStringLiteral("AbilityTwo")}) {
        const QVector<UnitFamily> refreshedFamilies = UnitFamilyDetector().detectCollectionFamilies(
            analysis, DataCollectionMode::UnitAbilWeapon);
        const auto family = std::find_if(refreshedFamilies.cbegin(), refreshedFamilies.cend(), [&](const UnitFamily &value) {
            return value.rootId == root;
        });
        QVERIFY(family != refreshedFamilies.cend());
        DataCollectionBuildRequest applyRequest;
        applyRequest.family = *family;
        applyRequest.requestedUnitId = root;
        applyRequest.confirmNonStandard = true;
        for (const UnitFamilyObject &object : family->objects)
            applyRequest.includedNodeIndices.insert(object.nodeIndex);
        const DataCollectionApplyResult applied = DataCollectionUnitBuilder().apply(
            analysis, applyRequest, dir.path(), {}, true, &refreshedFamilies);
        QVERIFY2(applied.success, qPrintable(applied.error));
        QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    }
    QFile updated(path);
    QVERIFY(updated.open(QIODevice::ReadOnly));
    const QByteArray updatedXml = updated.readAll();
    QCOMPARE(updatedXml.count("Entry=\"Effect,SharedEffect\""), 2);
}

void CoreTests::dataCollectionMigrationRollbackRestoresAllCollections()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Gargantua.xml"));
    const QByteArray original = QByteArrayLiteral(
        "<Catalog><CPlacedUnit id=\"Placed\" Unit=\"Gargantua\"/>"
        "<CUnit id=\"Gargantua\"><AbilArray Link=\"Gargantua_Jump\"/><WeaponArray Link=\"Gargantua_Weapon\"/></CUnit>"
        "<CAbilEffectTarget id=\"Gargantua_Jump\"/><CWeaponLegacy id=\"Gargantua_Weapon\"/>"
        "<CDataCollectionUnit id=\"Gargantua\" parent=\"UnitGround\"><DataRecord Entry=\"Unit,Gargantua\"/>"
        "<DataRecord Entry=\"Abil,Gargantua_Jump\"/><DataRecord Entry=\"Weapon,Gargantua_Weapon\"/></CDataCollectionUnit></Catalog>");
    QVERIFY(writeTextFile(path, original));
    FolderAnalyzer analyzer; AnalysisResult analysis; QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    const QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(analysis, DataCollectionMode::UnitAbilWeapon);
    const auto unit = std::find_if(families.cbegin(), families.cend(), [](const UnitFamily &family) { return family.rootId == QStringLiteral("Gargantua"); });
    QVERIFY(unit != families.cend());
    DataCollectionBuildRequest request; request.family = *unit;
    DataCollectionUnitBuilder builder; builder.setFailureInjectionStep(QStringLiteral("after-commit"));
    const DataCollectionApplyResult applied = builder.apply(analysis, request, dir.path(), {});
    QVERIFY(!applied.success);
    QFile restored(path); QVERIFY(restored.open(QIODevice::ReadOnly)); QCOMPARE(restored.readAll(), original);
    QVERIFY(!QFileInfo::exists(QDir(dir.path()).absoluteFilePath(QStringLiteral("(listfile)"))));
}

void CoreTests::dataCollectionPatternInheritanceValidation()
{
    QTemporaryDir dir;
    QVERIFY(writeTextFile(QDir(dir.path()).absoluteFilePath(QStringLiteral("Patterns.xml")), QByteArrayLiteral(R"xml(
<Catalog>
  <CDataCollectionPattern id="UnitPattern_Base"/>
  <CDataCollectionPattern id="AbilityPattern_Base"/>
  <CDataCollectionPattern id="WeaponPattern_Base"/>
  <CDataCollectionUnit default="1" id="UnitBase"><Pattern value="UnitPattern_Base"/></CDataCollectionUnit>
  <CDataCollectionUnit default="1" id="AbilityBase"><Pattern value="AbilityPattern_Base"/></CDataCollectionUnit>
  <CDataCollectionUnit default="1" id="NoPatternBase"/>
  <CDataCollectionUnit default="1" id="MissingPatternBase"><Pattern value="DoesNotExist"/></CDataCollectionUnit>
  <CDataCollectionUnit default="1" id="WrongPatternBase"><Pattern value="WeaponPattern_Base"/></CDataCollectionUnit>
  <CDataCollectionUnit default="1" id="CycleA" parent="CycleB"/>
  <CDataCollectionUnit default="1" id="CycleB" parent="CycleA"/>
  <CUnit id="UnitRoot"/><CAbilEffectTarget id="AbilityRoot"/><CAbilEffectTarget id="DirectRoot"/><CUnit id="NoPatternRoot"/>
  <CAbilEffectTarget id="MissingPatternRoot"/><CAbilEffectTarget id="WrongPatternRoot"/><CUnit id="CycleRoot"/>
  <CUnit id="MissingParentRoot"/>
  <CDataCollectionUnit id="UnitRoot" parent="UnitBase"><EditorCategories value="DataGroup:Unit,ObjectType:Unit"/></CDataCollectionUnit>
  <CDataCollectionUnit id="AbilityRoot" parent="AbilityBase"><EditorCategories value="DataGroup:Ability,ObjectType:Other"/></CDataCollectionUnit>
  <CDataCollectionUnit id="DirectRoot"><Pattern value="AbilityPattern_Base"/><EditorCategories value="DataGroup:Ability,ObjectType:Other"/></CDataCollectionUnit>
  <CDataCollectionUnit id="NoPatternRoot" parent="NoPatternBase"><EditorCategories value="DataGroup:Unit,ObjectType:Unit"/></CDataCollectionUnit>
  <CDataCollectionUnit id="MissingPatternRoot" parent="MissingPatternBase"><EditorCategories value="DataGroup:Ability,ObjectType:Other"/></CDataCollectionUnit>
  <CDataCollectionUnit id="WrongPatternRoot" parent="WrongPatternBase"><EditorCategories value="DataGroup:Ability,ObjectType:Other"/></CDataCollectionUnit>
  <CDataCollectionUnit id="CycleRoot" parent="CycleA"><EditorCategories value="DataGroup:Unit,ObjectType:Unit"/></CDataCollectionUnit>
  <CDataCollectionUnit id="MissingParentRoot" parent="AbsentBase"><EditorCategories value="DataGroup:Unit,ObjectType:Unit"/></CDataCollectionUnit>
</Catalog>)xml")));
    FolderAnalyzer analyzer; AnalysisResult analysis; QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    const QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(analysis, DataCollectionMode::UnitAbilWeapon);
    const auto stateFor = [&](const QString &id) {
        const auto family = std::find_if(families.cbegin(), families.cend(), [&](const UnitFamily &value) { return value.rootId == id; });
        if (family == families.cend()) return DataCollectionPatternState::MissingParent;
        DataCollectionBuildRequest request; request.family = *family;
        return DataCollectionUnitBuilder().preview(analysis, request).patternState;
    };
    QCOMPARE(families.size(), 8);
    QCOMPARE(stateFor(QStringLiteral("UnitRoot")), DataCollectionPatternState::InheritedPattern);
    QCOMPARE(stateFor(QStringLiteral("AbilityRoot")), DataCollectionPatternState::InheritedPattern);
    QCOMPARE(stateFor(QStringLiteral("DirectRoot")), DataCollectionPatternState::DirectPattern);
    QCOMPARE(stateFor(QStringLiteral("NoPatternRoot")), DataCollectionPatternState::NoPatternRequired);
    QCOMPARE(stateFor(QStringLiteral("MissingPatternRoot")), DataCollectionPatternState::MissingReferencedPattern);
    QCOMPARE(stateFor(QStringLiteral("WrongPatternRoot")), DataCollectionPatternState::InvalidPatternForEntity);
    QCOMPARE(stateFor(QStringLiteral("CycleRoot")), DataCollectionPatternState::InheritanceCycle);
    QCOMPARE(stateFor(QStringLiteral("MissingParentRoot")), DataCollectionPatternState::MissingParent);
}

void CoreTests::dataCollectionRecognizesEvoSemanticPatternNames()
{
    QTemporaryDir dir;
    QVERIFY(writeTextFile(QDir(dir.path()).absoluteFilePath(QStringLiteral("EvoPatterns.xml")), QByteArrayLiteral(R"xml(
<Catalog>
  <CDataCollectionPattern id="SCBW_Unit">
    <Fields Reference="Actor,^ParamId^,UnitIcon"/>
    <Fields Reference="Unit,^ParamId^,LifeMax"/>
  </CDataCollectionPattern>
  <CDataCollectionPattern id="SCBW_Abil">
    <Fields Reference="Abil,^ParamId^,Cost.Vital[Energy]"/>
  </CDataCollectionPattern>
  <CDataCollectionPattern id="SCBW_Weapon">
    <Fields Reference="Weapon,^ParamId^,Range"/>
    <Fields Reference="Effect,^ParamId^@Damage,Amount"/>
  </CDataCollectionPattern>
  <CDataCollectionPattern id="SCBW_Weapon_Accumulator" parent="SCBW_Weapon">
    <Fields Reference="Accumulator,^ParamId^@Damage,Amount"/>
  </CDataCollectionPattern>

  <CDataCollectionUnit default="1" id="SCBWUnitTemplate"><Pattern value="SCBW_Unit"/></CDataCollectionUnit>
  <CDataCollectionAbil default="1" id="SCBWAbilTemplate"><Pattern value="SCBW_Abil"/></CDataCollectionAbil>
  <CDataCollectionAbil default="1" id="SCBWWeaponTemplate"><Pattern value="SCBW_Weapon"/></CDataCollectionAbil>

  <CUnit id="DragoonSCBW"/>
  <CAbilEffectTarget id="PsionicStormSCBW"/>
  <CWeaponLegacy id="DragoonSCBWWeapon"/>
  <CDataCollectionUnit id="DragoonSCBW" parent="SCBWUnitTemplate"><DataRecord Entry="Unit,DragoonSCBW"/></CDataCollectionUnit>
  <CDataCollectionAbil id="PsionicStormSCBW" parent="SCBWAbilTemplate"><DataRecord Entry="Abil,PsionicStormSCBW"/></CDataCollectionAbil>
  <CDataCollectionAbil id="DragoonSCBWWeapon" parent="SCBWWeaponTemplate">
    <DataRecord Entry="Weapon,DragoonSCBWWeapon"/>
    <Pattern value="SCBW_Weapon_Accumulator"/>
  </CDataCollectionAbil>
</Catalog>)xml")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    const QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(
        analysis, DataCollectionMode::UnitAbilWeapon);
    const auto previewFor = [&](const QString &id) {
        const auto family = std::find_if(families.cbegin(), families.cend(), [&](const UnitFamily &value) {
            return value.rootId == id;
        });
        if (family == families.cend())
            return DataCollectionPreviewReport{};
        DataCollectionBuildRequest request;
        request.family = *family;
        return DataCollectionUnitBuilder().preview(analysis, request, &families);
    };

    const DataCollectionPreviewReport unit = previewFor(QStringLiteral("DragoonSCBW"));
    QCOMPARE(unit.patternState, DataCollectionPatternState::InheritedPattern);
    QCOMPARE(unit.effectivePattern, QStringLiteral("SCBW_Unit"));
    const DataCollectionPreviewReport ability = previewFor(QStringLiteral("PsionicStormSCBW"));
    QCOMPARE(ability.patternState, DataCollectionPatternState::InheritedPattern);
    QCOMPARE(ability.effectivePattern, QStringLiteral("SCBW_Abil"));
    const DataCollectionPreviewReport weapon = previewFor(QStringLiteral("DragoonSCBWWeapon"));
    QCOMPARE(weapon.patternState, DataCollectionPatternState::DirectPattern);
    QCOMPARE(weapon.effectivePattern, QStringLiteral("SCBW_Weapon_Accumulator"));
}

void CoreTests::dataCollectionEntityRootsAndConflicts()
{
    QTemporaryDir dir;
    QVERIFY(writeTextFile(QDir(dir.path()).absoluteFilePath(QStringLiteral("Roots.xml")), QByteArrayLiteral(
        "<Catalog><CAbilEffectTarget id=\"zGrenade\" refs=\"GrenadeDamage\"/>"
        "<CEffectDamage id=\"GrenadeDamage\"/><CEffectDamage id=\"zGrenadeSimilarButUnlinked\"/>"
        "<CUpgrade id=\"zGrenadeUpgrade\"/><CUnit id=\"Collision\"/><CWeaponLegacy id=\"Collision\"/></Catalog>")));
    FolderAnalyzer analyzer; AnalysisResult analysis; QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    const QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(analysis, DataCollectionMode::UnitAbilWeapon);
    const auto grenade = std::find_if(families.cbegin(), families.cend(), [](const UnitFamily &family) { return family.rootId == QStringLiteral("zGrenade"); });
    QVERIFY(grenade != families.cend());
    QCOMPARE(grenade->entityType, DataCollectionEntityType::Ability);
    QVERIFY(std::any_of(grenade->objects.cbegin(), grenade->objects.cend(), [&](const UnitFamilyObject &object) {
        return analysis.nodes[object.nodeIndex].id == QStringLiteral("GrenadeDamage");
    }));
    QVERIFY(std::none_of(grenade->objects.cbegin(), grenade->objects.cend(), [&](const UnitFamilyObject &object) {
        return analysis.nodes[object.nodeIndex].id == QStringLiteral("zGrenadeSimilarButUnlinked");
    }));
    QVERIFY(std::none_of(families.cbegin(), families.cend(), [](const UnitFamily &family) { return family.rootId == QStringLiteral("zGrenadeUpgrade"); }));
    const int collisionCount = std::count_if(families.cbegin(), families.cend(), [](const UnitFamily &family) {
        return family.rootId == QStringLiteral("Collision") && family.rootTypeConflict;
    });
    QCOMPARE(collisionCount, 2);
    for (const UnitFamily &family : families) if (family.rootId == QStringLiteral("Collision")) {
        DataCollectionBuildRequest request; request.family = family;
        QVERIFY(!DataCollectionUnitBuilder().preview(analysis, request).valid);
    }
}

void CoreTests::gargantuaReferenceFixture()
{
    const QString basePath = QStringLiteral("C:/Users/Vladimir/Downloads/base.xml");
    const QString gargantuaPath = QStringLiteral("C:/Users/Vladimir/Downloads/Gargantua.xml");
    if (!QFileInfo::exists(basePath) || !QFileInfo::exists(gargantuaPath)) QSKIP("Reference XML fixtures are unavailable.");
    QTemporaryDir dir;
    for (const QString &source : {basePath, gargantuaPath}) {
        QFile input(source); QVERIFY(input.open(QIODevice::ReadOnly));
        QVERIFY(writeTextFile(QDir(dir.path()).absoluteFilePath(QFileInfo(source).fileName()), input.readAll()));
    }
    FolderAnalyzer analyzer; AnalysisResult analysis; QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    const QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(analysis, DataCollectionMode::UnitAbilWeapon);
    const auto familyFor = [&](const QString &id) {
        return std::find_if(families.cbegin(), families.cend(), [&](const UnitFamily &family) { return family.rootId == id; });
    };
    const auto unit = familyFor(QStringLiteral("Gargantua"));
    const auto ability = familyFor(QStringLiteral("Gargantua_Jump"));
    const auto weapon = familyFor(QStringLiteral("Gargantua_Weapon"));
    QVERIFY(unit != families.cend()); QVERIFY(ability != families.cend()); QVERIFY(weapon != families.cend());
    QCOMPARE(unit->entityType, DataCollectionEntityType::Unit);
    QCOMPARE(ability->entityType, DataCollectionEntityType::Ability);
    QCOMPARE(weapon->entityType, DataCollectionEntityType::Weapon);
    QCOMPARE(unit->recommendedParent, QStringLiteral("UnitGround"));
    QCOMPARE(ability->recommendedParent, QStringLiteral("AbilityMisssile"));
    QCOMPARE(weapon->recommendedParent, QStringLiteral("Weapon_Instant"));
    const auto contains = [&](const UnitFamily &family, const QString &id) {
        return std::any_of(family.objects.cbegin(), family.objects.cend(), [&](const UnitFamilyObject &object) {
            return analysis.nodes[object.nodeIndex].id == id;
        });
    };
    QVERIFY(!contains(*unit, QStringLiteral("Gargantua_Jump@Damage")));
    QVERIFY(!contains(*unit, QStringLiteral("Gargantua_Weapon@Damage")));
    QVERIFY(contains(*ability, QStringLiteral("Gargantua_Jump@Damage")));
    QVERIFY(contains(*weapon, QStringLiteral("Gargantua_Weapon@Damage")));
    qInfo().noquote() << QStringLiteral("Gargantua fixture: Unit=%1 records, Ability=%2 records, Weapon=%3 records")
                             .arg(unit->objects.size()).arg(ability->objects.size()).arg(weapon->objects.size());
    for (const UnitFamily *family : {&*unit, &*ability, &*weapon}) {
        DataCollectionBuildRequest request; request.family = *family;
        const DataCollectionPreviewReport preview = DataCollectionUnitBuilder().preview(analysis, request, &families);
        QCOMPARE(preview.patternState, DataCollectionPatternState::InheritedPattern);
        QVERIFY(preview.directPattern.isEmpty());
        QVERIFY(!preview.inheritedPattern.isEmpty());
    }
}

void CoreTests::gargantuaApplyFixture()
{
    const QString basePath = QStringLiteral("C:/Users/Vladimir/Downloads/base.xml");
    const QString gargantuaPath = QStringLiteral("C:/Users/Vladimir/Downloads/Gargantua.xml");
    if (!QFileInfo::exists(basePath) || !QFileInfo::exists(gargantuaPath)) QSKIP("Reference XML fixtures are unavailable.");
    QTemporaryDir dir;
    for (const QString &source : {basePath, gargantuaPath}) {
        QFile input(source); QVERIFY(input.open(QIODevice::ReadOnly));
        QVERIFY(writeTextFile(QDir(dir.path()).absoluteFilePath(QFileInfo(source).fileName()), input.readAll()));
    }
    FolderAnalyzer analyzer; AnalysisResult analysis; QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    for (const QString &root : {QStringLiteral("Gargantua"), QStringLiteral("Gargantua_Jump"), QStringLiteral("Gargantua_Weapon")}) {
        const QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(analysis, DataCollectionMode::UnitAbilWeapon);
        const auto family = std::find_if(families.cbegin(), families.cend(), [&](const UnitFamily &value) { return value.rootId == root; });
        QVERIFY(family != families.cend());
        DataCollectionBuildRequest request; request.family = *family; request.requestedUnitId = root;
        const DataCollectionPreviewReport preview = DataCollectionUnitBuilder().preview(analysis, request, &families);
        qInfo().noquote() << root << "false positives:" << preview.falsePositiveAssociations.join(QStringLiteral(", "));
        const DataCollectionApplyResult applied = DataCollectionUnitBuilder().apply(analysis, request, dir.path(), {}, true, &families);
        QVERIFY2(applied.success, qPrintable(applied.error));
        QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    }
    const DataCollectionAuditSummary audit = auditDataCollections(analysis);
    QCOMPARE(audit.collections, 3);
    QCOMPARE(audit.unitCollections, 1);
    QCOMPARE(audit.abilityCollections, 1);
    QCOMPARE(audit.weaponCollections, 1);
    QCOMPARE(audit.unanchoredCollections, 0);
    QCOMPARE(audit.mixedRootCollections, 0);
    QCOMPARE(audit.missingPrimaryRecords, 0);
    QCOMPARE(audit.invalidCategories, 0);
    QCOMPARE(audit.inheritedPatterns, 3);
    QCOMPARE(audit.missingParents, 0);
    QCOMPARE(audit.brokenInheritance, 0);
    QFile output(QDir(dir.path()).absoluteFilePath(QStringLiteral("Gargantua.xml")));
    QVERIFY(output.open(QIODevice::ReadOnly));
    const QByteArray xml = output.readAll();
    QVERIFY(!xml.contains("Entry=\"\""));
    qInfo().noquote() << audit.reportText;
}

void CoreTests::zombieWorldUpdate3Audit()
{
    const QString archivePath = QStringLiteral("C:/Users/Vladimir/Downloads/sc2_DATA_HELPER/Zombie World Legacy Reborn.bak-20260624-235818.SC2Map");
    const QString updatePath = QStringLiteral("C:/Users/Vladimir/Downloads/Data1_3rezijmUPDTADE3.txt");
    if (!QFileInfo::exists(archivePath) || !QFileInfo::exists(updatePath)) QSKIP("Zombie World audit fixtures are unavailable.");
    Sc2Archive archive; QString error;
    QVERIFY2(archive.load(archivePath, &error), qPrintable(error));
    qInfo().noquote() << QStringLiteral("Zombie World archive: %1 total entries, %2 GameData XML entries: %3")
                             .arg(archive.totalEntriesCount()).arg(archive.gameDataXmlEntries().size())
                             .arg(archive.gameDataXmlEntries().join(QStringLiteral(", ")));
    qInfo().noquote() << QStringLiteral("Zombie World collection/listfile entries: %1")
                             .arg(archive.allEntries().filter(QRegularExpression(QStringLiteral("DataCollection|listfile"), QRegularExpression::CaseInsensitiveOption))
                                      .join(QStringLiteral(", ")));
    qInfo().noquote() << QStringLiteral("Zombie World document entries: %1")
                             .arg(archive.allEntries().filter(QRegularExpression(QStringLiteral("Document|Header|Info"), QRegularExpression::CaseInsensitiveOption))
                                      .join(QStringLiteral(", ")));
    QByteArray documentInfo;
    if (archive.readEntry(QStringLiteral("DocumentInfo"), &documentInfo, &error)) {
        QStringList dependencies;
        for (const QString &line : QString::fromUtf8(documentInfo).split(QRegularExpression(QStringLiteral("[\r\n]+"))))
            if (line.contains(QStringLiteral("depend"), Qt::CaseInsensitive)) dependencies << line.trimmed();
        qInfo().noquote() << QStringLiteral("Zombie World dependencies: %1").arg(dependencies.join(QStringLiteral(" | ")));
    }
    QTemporaryDir dir; QVERIFY(dir.isValid());
    for (const QString &entry : archive.gameDataXmlEntries()) {
        if (entry.endsWith(QStringLiteral("DataCollectionData.xml"), Qt::CaseInsensitive)) continue;
        QByteArray bytes; QVERIFY2(archive.readEntry(entry, &bytes, &error), qPrintable(error));
        QString relative = entry; relative.replace('\\', '/');
        const QString output = QDir(dir.path()).absoluteFilePath(relative);
        QVERIFY(QDir().mkpath(QFileInfo(output).absolutePath()));
        QVERIFY(writeTextFile(output, bytes));
    }
    QFile update(updatePath); QVERIFY(update.open(QIODevice::ReadOnly));
    const QString generated = QDir(dir.path()).absoluteFilePath(QStringLiteral("Generated/DataCollectionData.xml"));
    QVERIFY(QDir().mkpath(QFileInfo(generated).absolutePath()));
    QVERIFY(writeTextFile(generated, update.readAll()));
    FolderAnalyzer analyzer; AnalysisResult analysis;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    const DataCollectionAuditSummary audit = auditDataCollections(analysis);
    qInfo().noquote() << QStringLiteral("Zombie World UPDATE3 audit: %1 | Manual review: %2")
                             .arg(audit.reportText).arg(audit.manualReview.size());
    const QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(analysis, DataCollectionMode::UnitAbilWeapon);
    DataCollectionAliasMapper mapper;
    QSet<QString> expected, actual;
    int conflicts = 0, shared = 0;
    for (const UnitFamily &family : families) {
        if (family.rootTypeConflict) { ++conflicts; continue; }
        for (const UnitFamilyObject &object : family.objects) {
            if (object.role == UnitFamilyRole::ManualReview) { ++shared; continue; }
            const QString alias = mapper.aliasFor(analysis.nodes[object.nodeIndex], family.rootId, object.role);
            if (!alias.isEmpty()) expected.insert(alias);
        }
    }
    pugi::xml_document generatedDoc;
    QFile generatedFile(generated); QVERIFY(generatedFile.open(QIODevice::ReadOnly));
    const QByteArray generatedBytes = generatedFile.readAll();
    QVERIFY(generatedDoc.load_buffer(generatedBytes.constData(), size_t(generatedBytes.size())));
    for (pugi::xml_node collection : generatedDoc.child("Catalog").children())
        for (pugi::xml_node record : collection.children("DataRecord"))
            actual.insert(QString::fromUtf8(record.attribute("Entry").value()));
    QSet<QString> missing = expected; missing.subtract(actual);
    QSet<QString> extra = actual; extra.subtract(expected);
    qInfo().noquote() << QStringLiteral("Zombie World graph coverage: families=%1, conflicts=%2, shared/manual=%3, expected=%4, actual=%5, missing=%6, extra=%7")
                             .arg(families.size()).arg(conflicts).arg(shared).arg(expected.size()).arg(actual.size())
                             .arg(missing.size()).arg(extra.size());
    qInfo().noquote() << QStringLiteral("Missing sample: %1").arg(QStringList(missing.cbegin(), missing.cend()).mid(0, 30).join(QStringLiteral(", ")));
    qInfo().noquote() << QStringLiteral("Extra sample: %1").arg(QStringList(extra.cbegin(), extra.cend()).mid(0, 30).join(QStringLiteral(", ")));

    const QString appliedPath = QStringLiteral("C:/Users/Vladimir/Downloads/sc2_DATA_HELPER/Zombie World Legacy Reborn.SC2Map");
    if (QFileInfo::exists(appliedPath)) {
        Sc2Archive appliedArchive; QVERIFY2(appliedArchive.load(appliedPath, &error), qPrintable(error));
        const QStringList collectionEntries = appliedArchive.allEntries().filter(
            QRegularExpression(QStringLiteral("DataCollectionData\\.xml$"), QRegularExpression::CaseInsensitiveOption));
        QByteArray appliedListfile; QVERIFY2(appliedArchive.readEntry(QStringLiteral("(listfile)"), &appliedListfile, &error), qPrintable(error));
        qInfo().noquote() << QStringLiteral("Applied map collection entries: %1 | listed=%2")
                                 .arg(collectionEntries.join(QStringLiteral(", ")),
                                      QString::fromUtf8(appliedListfile).contains(QStringLiteral("DataCollectionData.xml"), Qt::CaseInsensitive)
                                          ? QStringLiteral("yes") : QStringLiteral("no"));
        QVERIFY(!collectionEntries.isEmpty());
        QVERIFY(QString::fromUtf8(appliedListfile).contains(QStringLiteral("DataCollectionData.xml"), Qt::CaseInsensitive));
        QByteArray appliedCollection;
        QVERIFY2(appliedArchive.readEntry(collectionEntries.front(), &appliedCollection, &error), qPrintable(error));
        pugi::xml_document appliedDocument;
        QVERIFY(appliedDocument.load_buffer(appliedCollection.constData(), size_t(appliedCollection.size())));
        int appliedCollections = 0;
        QSet<QString> appliedRecords;
        for (pugi::xml_node collection : appliedDocument.child("Catalog").children()) {
            ++appliedCollections;
            for (pugi::xml_node record : collection.children("DataRecord"))
                appliedRecords.insert(QString::fromUtf8(record.attribute("Entry").value()));
        }
        QCOMPARE(appliedCollections, audit.collections);
        QCOMPARE(appliedRecords, actual);
    }
}

void CoreTests::dataCollectionOffersSingleCustomUnit()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("UnitData.xml"));
    QVERIFY(writeTextFile(path, QByteArrayLiteral(
        "<Catalog><CUnit id=\"LonelyCustomUnit\"><LifeMax value=\"125\"/></CUnit></Catalog>")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    const QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(analysis);
    const auto family = std::find_if(families.cbegin(), families.cend(), [](const UnitFamily &value) {
        return value.rootId == QStringLiteral("LonelyCustomUnit");
    });
    QVERIFY(family != families.cend());
    QCOMPARE(family->objects.size(), 1);

    DataCollectionBuildRequest request;
    request.family = *family;
    const DataCollectionPreviewReport preview = DataCollectionUnitBuilder().preview(analysis, request);
    QVERIFY2(preview.valid, qPrintable(preview.warnings.join(QStringLiteral("; "))));
    QCOMPARE(preview.recordsToAdd, QStringList{QStringLiteral("Unit,LonelyCustomUnit")});
    QVERIFY(preview.generatedXml.contains(QStringLiteral("<CDataCollectionUnit id=\"LonelyCustomUnit\"")));
}

void CoreTests::dataCollectionUsesRootRaceAndFamilyCategories()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Gargantua.xml"));
    QVERIFY(writeTextFile(path, QByteArrayLiteral(
        "<Catalog>"
        "<CButton id=\"Gargantua\"/>"
        "<CActorUnit id=\"Gargantua\" unitName=\"Gargantua\"/>"
        "<CModel id=\"Gargantua\"/>"
        "<CUnit id=\"Gargantua\" race=\"Zerg\"><EditorCategories value=\"ObjectType:Unit,ObjectFamily:Campaign\"/></CUnit>"
        "</Catalog>")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    const QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(analysis, DataCollectionMode::Unit);
    const auto family = std::find_if(families.cbegin(), families.cend(), [](const UnitFamily &value) {
        return value.rootId == QStringLiteral("Gargantua");
    });
    QVERIFY(family != families.cend());

    DataCollectionBuildRequest request;
    request.family = *family;
    const DataCollectionPreviewReport preview = DataCollectionUnitBuilder().preview(analysis, request);
    QVERIFY2(preview.valid, qPrintable(preview.warnings.join(QStringLiteral("; "))));
    QVERIFY(preview.generatedXml.contains(
        QStringLiteral("EditorCategories value=\"DataGroup:Unit,Race:Zerg,DataFamily:Campaign,ObjectType:Unit\"")));
    QVERIFY(preview.generatedXml.contains(QStringLiteral("DataRecord Entry=\"Unit,Gargantua\"")));
    QVERIFY(!preview.generatedXml.contains(QStringLiteral("Unit,Gargantua@Unit")));
}

void CoreTests::dataCollectionUpdatePreservesAndSorts()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Family.xml"));
    QVERIFY(writeTextFile(path, QByteArrayLiteral(
        "<Catalog><CUnit id=\"Vassel@Unit\" refs=\"Vassel@Actor Vassel@Button\"/><CActorUnit id=\"Vassel@Actor\" unitName=\"Vassel@Unit\"/><CButton id=\"Vassel@Button\"/>"
        "<CDataCollectionUnit default=\"1\" id=\"CustomParent\"/>"
        "<CDataCollectionUnit id=\"OtherFamily\"><DataRecord Entry=\"Other,Untouched\" custom=\"yes\"/></CDataCollectionUnit>"
        "<CDataCollectionUnit id=\"Vassel\" parent=\"CustomParent\" custom=\"keep\"><EditorCategories value=\"DataGroup:Unit,ObjectType:Unit\"/><Metadata value=\"PreserveNode\"/>"
        "<DataRecord Entry=\"Actor,Vassel@Actor\" custom=\"preserve-attribute\"/><DataRecord Entry=\"Other,PreserveMe\"/><DataRecord Entry=\"Actor,Vassel@Actor\"/></CDataCollectionUnit></Catalog>")));
    FolderAnalyzer analyzer; AnalysisResult analysis; QString error; QVERIFY(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error));
    DataCollectionBuildRequest request; request.family = UnitFamilyDetector().detectCollectionFamilies(analysis, DataCollectionMode::Unit).front();
    request.parent = QStringLiteral("CustomParent"); request.editorCategories = QStringLiteral("DataGroup:Unit,ObjectType:Unit");
    DataCollectionUnitBuilder builder; const DataCollectionPreviewReport preview = builder.preview(analysis, request);
    QVERIFY(preview.existingCollection);
    QVERIFY(preview.existingRecordsPreserved.contains(QStringLiteral("Other,PreserveMe")));
    QVERIFY(preview.duplicateRecordsSkipped.contains(QStringLiteral("Actor,Vassel@Actor")));
    QCOMPARE(preview.generatedXml.count(QStringLiteral("Actor,Vassel@Actor")), 2);
    QVERIFY(preview.generatedXml.contains(QStringLiteral("Other,PreserveMe")));
    const DataCollectionApplyResult applied = builder.apply(analysis, request, dir.path(), {});
    QVERIFY2(applied.success, qPrintable(applied.error));
    QFile output(path); QVERIFY(output.open(QIODevice::ReadOnly)); const QString xml = QString::fromUtf8(output.readAll());
    QCOMPARE(xml.count(QStringLiteral("Actor,Vassel@Actor")), 2);
    QCOMPARE(xml.count(QStringLiteral("Other,PreserveMe")), 1);
    QVERIFY(xml.contains(QStringLiteral("parent=\"CustomParent\"")));
    QVERIFY(xml.contains(QStringLiteral("value=\"DataGroup:Unit,ObjectType:Unit\"")));
    QVERIFY(xml.contains(QStringLiteral("id=\"OtherFamily\"")));
    QVERIFY(xml.contains(QStringLiteral("Entry=\"Other,Untouched\" custom=\"yes\"")));
    QVERIFY(xml.contains(QStringLiteral("custom=\"keep\"")));
    QVERIFY(xml.contains(QStringLiteral("Metadata value=\"PreserveNode\"")));
    QVERIFY(xml.contains(QStringLiteral("custom=\"preserve-attribute\"")));
    QVERIFY(xml.indexOf(QStringLiteral("Button,Vassel@Button")) < xml.indexOf(QStringLiteral("Unit,Vassel@Unit")));
}

void CoreTests::dataCollectionRollback()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Family.xml"));
    const QByteArray original = QByteArrayLiteral("<Catalog><CUnit id=\"Vassel@Unit\" refs=\"Vassel@Actor\"/><CActorUnit id=\"Vassel@Actor\" unitName=\"Vassel@Unit\"/></Catalog>");
    QVERIFY(writeTextFile(path, original));
    FolderAnalyzer analyzer; AnalysisResult analysis; QString error; QVERIFY(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error));
    DataCollectionBuildRequest request; request.family = UnitFamilyDetector().detectCollectionFamilies(analysis).front();
    DataCollectionUnitBuilder builder; builder.setFailureInjectionStep(QStringLiteral("after-commit"));
    const DataCollectionApplyResult applied = builder.apply(analysis, request, dir.path(), {});
    QVERIFY(!applied.success);
    QFile restored(path); QVERIFY(restored.open(QIODevice::ReadOnly)); QCOMPARE(restored.readAll(), original);
    QVERIFY(!QFileInfo::exists(QDir(dir.path()).absoluteFilePath(QStringLiteral("DataCollectionData.xml"))));
    QVERIFY(!QFileInfo::exists(QDir(dir.path()).absoluteFilePath(QStringLiteral("(listfile)"))));
}

void CoreTests::autoCollectionSurvivesOptimizationBatch()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Batch.xml"));
    QVERIFY(writeTextFile(path, QByteArrayLiteral(
        "<Catalog>"
        "<CDataCollectionPattern id=\"UnitPattern_Base\"/>"
        "<CDataCollectionPattern id=\"AbilityPattern_Base\"/>"
        "<CDataCollectionPattern id=\"WeaponPattern_Base\"/>"
        "<CDataCollectionUnit id=\"UnitTemplate\"/>"
        "<CDataCollectionAbil id=\"AbilityTemplate\"/>"
        "<CDataCollectionWeapon id=\"WeaponTemplate\"/>"
        "<CUnit id=\"BatchUnit\" parent=\"UnitTemplate\" refs=\"BatchActor BatchAbility BatchWeapon BatchBehavior\"/>"
        "<CActorUnit id=\"BatchActor\" refs=\"BatchModel DuplicateDamageB\"><Model value=\"BatchModel\"/></CActorUnit>"
        "<CModel id=\"BatchModel\"/>"
        "<CAbilEffectTarget id=\"BatchAbility\" parent=\"AbilityTemplate\" refs=\"BatchButton DuplicateDamageB BatchBehavior\"/>"
        "<CButton id=\"BatchButton\"/>"
        "<CWeaponLegacy id=\"BatchWeapon\" parent=\"WeaponTemplate\" refs=\"DuplicateDamageB\"/>"
        "<CEffectDamage id=\"DuplicateDamageA\"><Amount value=\"10\"/></CEffectDamage>"
        "<CEffectDamage id=\"DuplicateDamageB\"><Amount value=\"10\"/></CEffectDamage>"
        "<CBehaviorBuff id=\"BatchBehavior\"><Face value=\"BatchButton\"/></CBehaviorBuff>"
        "<CAbilEffectTarget id=\"DeadAbility\" refs=\"DeadEffect\"/>"
        "<CEffectDamage id=\"DeadEffect\"/>"
        "<CDataCollectionUnit id=\"BatchUnit\" parent=\"UnitTemplate\">"
        "<Pattern value=\"UnitPattern_Base\"/>"
        "<EditorNote value=\"KeepMe\"/>"
        "<DataRecord Entry=\"Unit,BatchUnit\"/>"
        "<DataRecord Entry=\"Effect,LegacyManual\"/>"
        "</CDataCollectionUnit>"
        "</Catalog>")));

    auto analyzeInto = [&](AnalysisResult &target) {
        FolderAnalyzer analyzer;
        AnalysisResult result;
        QString error;
        QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &result, &error), qPrintable(error));
        target = result;
    };
    auto nodeById = [](const AnalysisResult &analysis, const QString &id) -> const DataNode * {
        for (const DataNode &node : analysis.nodes) {
            if (node.id == id)
                return &node;
        }
        return nullptr;
    };
    auto nodeIndexById = [](const AnalysisResult &analysis, const QString &id) {
        for (int index = 0; index < analysis.nodes.size(); ++index) {
            if (analysis.nodes.at(index).id == id)
                return index;
        }
        return -1;
    };
    auto readAllXml = [&](QString &combined) {
        combined.clear();
        const QFileInfoList files = QDir(dir.path()).entryInfoList({QStringLiteral("*.xml")}, QDir::Files, QDir::Name);
        for (const QFileInfo &fileInfo : files) {
            QFile file(fileInfo.absoluteFilePath());
            QVERIFY(file.open(QIODevice::ReadOnly));
            combined += QString::fromUtf8(file.readAll());
        }
    };

    AnalysisResult analysis;
    analyzeInto(analysis);
    QVector<int> removeRows;
    removeRows.append(nodeIndexById(analysis, QStringLiteral("DeadAbility")));
    removeRows.append(nodeIndexById(analysis, QStringLiteral("DeadEffect")));
    QVERIFY(removeRows[0] >= 0);
    QVERIFY(removeRows[1] >= 0);
    QString backupFolder;
    QString error;
    QStringList changedFiles;
    int removedNodes = 0;
    int skippedNodes = 0;
    QVERIFY2(FolderAnalyzer().applySelectedChanges(analysis,
                                                   removeRows,
                                                   dir.path(),
                                                   {},
                                                   &backupFolder,
                                                   &error,
                                                   &changedFiles,
                                                   &removedNodes,
                                                   &skippedNodes),
             qPrintable(error));
    QCOMPARE(changedFiles.size(), 1);
    QCOMPARE(removedNodes, 2);
    QCOMPARE(skippedNodes, 0);

    analyzeInto(analysis);
    QVERIFY(!nodeById(analysis, QStringLiteral("DeadAbility")));
    QVERIFY(!nodeById(analysis, QStringLiteral("DeadEffect")));

    const int duplicateA = nodeIndexById(analysis, QStringLiteral("DuplicateDamageA"));
    const int duplicateB = nodeIndexById(analysis, QStringLiteral("DuplicateDamageB"));
    QVERIFY(duplicateA >= 0);
    QVERIFY(duplicateB >= 0);
    MergeRequest mergeRequest;
    mergeRequest.keepNodeIndex = duplicateA;
    mergeRequest.removeNodeIndices = {duplicateB};
    const MergeApplyResult merged = MergeService().apply(analysis, mergeRequest, dir.path(), {});
    QVERIFY2(merged.success, qPrintable(merged.error));
    QVERIFY(merged.nodesDeleted >= 1);

    analyzeInto(analysis);
    QVERIFY(nodeById(analysis, QStringLiteral("DuplicateDamageA")));
    QVERIFY(!nodeById(analysis, QStringLiteral("DuplicateDamageB")));
    const DataNode *weaponAfterMerge = nodeById(analysis, QStringLiteral("BatchWeapon"));
    QVERIFY(weaponAfterMerge);
    QVERIFY(weaponAfterMerge->referencedIds.contains(QStringLiteral("DuplicateDamageA")));
    QVERIFY(!weaponAfterMerge->referencedIds.contains(QStringLiteral("DuplicateDamageB")));

    const int unitBeforeRename = nodeIndexById(analysis, QStringLiteral("BatchUnit"));
    QVERIFY(unitBeforeRename >= 0);
    RenamePlan renamePlan;
    renamePlan.valid = true;
    RenamePlanItem renameItem;
    renameItem.nodeIndex = unitBeforeRename;
    renameItem.oldId = QStringLiteral("BatchUnit");
    renameItem.newId = QStringLiteral("BatchRenamed");
    renamePlan.items.append(renameItem);
    const RenameApplyResult renamed = ReferenceRenamer().apply(analysis, renamePlan, dir.path(), {});
    QVERIFY2(renamed.success, qPrintable(renamed.error));

    analyzeInto(analysis);
    QVERIFY(!nodeById(analysis, QStringLiteral("BatchUnit")));
    QVERIFY(nodeById(analysis, QStringLiteral("BatchRenamed")));

    const QVector<QString> roots = {
        QStringLiteral("BatchRenamed"),
        QStringLiteral("BatchAbility"),
        QStringLiteral("BatchWeapon"),
    };
    for (const QString &rootId : roots) {
        QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(analysis, DataCollectionMode::UnitAbilWeapon);
        auto it = std::find_if(families.begin(), families.end(), [&](const UnitFamily &family) {
            return family.rootId == rootId;
        });
        QVERIFY2(it != families.end(), qPrintable(rootId));
        DataCollectionBuildRequest request;
        request.family = *it;
        const DataCollectionApplyResult applied = DataCollectionUnitBuilder().apply(analysis, request, dir.path(), {}, true, &families);
        QVERIFY2(applied.success, qPrintable(applied.error));
        analyzeInto(analysis);
    }

    const DataCollectionAuditSummary audit = auditDataCollections(analysis);
    QCOMPARE(audit.missingPrimaryRecords, 0);
    QCOMPARE(audit.mixedRootCollections, 0);
    QCOMPARE(audit.rootTypeConflicts, 0);

    QString xml;
    readAllXml(xml);
    QVERIFY(xml.contains(QStringLiteral("EditorNote value=\"KeepMe\"")));
    QVERIFY(xml.contains(QStringLiteral("Entry=\"Effect,LegacyManual\"")));
    QCOMPARE(xml.count(QStringLiteral("Entry=\"Unit,BatchRenamed\"")), 1);
    QCOMPARE(xml.count(QStringLiteral("Entry=\"Abil,BatchAbility\"")), 1);
    QCOMPARE(xml.count(QStringLiteral("Entry=\"Weapon,BatchWeapon\"")), 1);
    QCOMPARE(xml.count(QStringLiteral("Entry=\"Effect,DuplicateDamageA\"")), 1);
    QVERIFY(!xml.contains(QStringLiteral("Entry=\"Effect,DuplicateDamageB\"")));
}

void CoreTests::unitFamilyDetectionAndStandardPlanning()
{
    QTemporaryDir dir;
    const QByteArray xml = QByteArrayLiteral(
        "<Catalog>"
        "<CUnit id=\"Vassel\"><AbilArray Link=\"AbilityVassel\"/><EffectArray value=\"EffectVassel\"/></CUnit>"
        "<CActorUnit id=\"ActorVassel\" unitName=\"Vassel\"><Model value=\"ModelVassel\"/><SoundArray value=\"AttackVassel\"/></CActorUnit>"
        "<CAbilEffectTarget id=\"AbilityVassel\"><CmdButtonArray DefaultButtonFace=\"ButtonVassel\"/></CAbilEffectTarget>"
        "<CButton id=\"ButtonVassel\"/>"
        "<CModel id=\"ModelVassel\"/>"
        "<CSound id=\"AttackVassel\"/>"
        "<CEffect id=\"EffectVassel\"/>"
        "</Catalog>");
    QVERIFY(writeTextFile(QDir(dir.path()).absoluteFilePath(QStringLiteral("Family.xml")), xml));
    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    UnitFamilyDetector detector;
    const QVector<UnitFamily> families = detector.detect(analysis);
    QCOMPARE(families.size(), 1);
    const UnitFamily &family = families.front();
    QCOMPARE(family.rootId, QStringLiteral("Vassel"));
    QHash<QString, UnitFamilyObject> objects;
    for (const UnitFamilyObject &object : family.objects) objects.insert(analysis.nodes[object.nodeIndex].id, object);
    QCOMPARE(objects[QStringLiteral("Vassel")].role, UnitFamilyRole::Unit);
    QCOMPARE(objects[QStringLiteral("ActorVassel")].role, UnitFamilyRole::Actor);
    QCOMPARE(objects[QStringLiteral("ActorVassel")].confidence, QStringLiteral("High"));
    QCOMPARE(objects[QStringLiteral("AbilityVassel")].role, UnitFamilyRole::Ability);
    QCOMPARE(objects[QStringLiteral("ButtonVassel")].role, UnitFamilyRole::Button);
    QCOMPARE(objects[QStringLiteral("ModelVassel")].role, UnitFamilyRole::Model);
    QCOMPARE(objects[QStringLiteral("AttackVassel")].role, UnitFamilyRole::Attack);
    QCOMPARE(objects[QStringLiteral("EffectVassel")].role, UnitFamilyRole::Effect);

    StandardNamePlanner planner;
    const RenamePlan plan = planner.plan(analysis, family, QStringLiteral("Vassel"));
    QHash<QString, QString> proposals;
    for (const RenamePlanItem &item : plan.items) proposals.insert(item.oldId, item.newId);
    QVERIFY(!proposals.contains(QStringLiteral("Vassel")));
    QCOMPARE(proposals[QStringLiteral("ActorVassel")], QStringLiteral("Vassel"));
    QCOMPARE(proposals[QStringLiteral("AbilityVassel")], QStringLiteral("Vassel@Ability"));
    QCOMPARE(proposals[QStringLiteral("ButtonVassel")], QStringLiteral("Vassel@Button"));
    QCOMPARE(proposals[QStringLiteral("ModelVassel")], QStringLiteral("Vassel@Model"));
    QCOMPARE(proposals[QStringLiteral("AttackVassel")], QStringLiteral("Vassel@Attack"));
    QCOMPARE(proposals[QStringLiteral("EffectVassel")], QStringLiteral("Vassel@Effect"));
    QVERIFY(plan.manualReview.isEmpty());
}

void CoreTests::renamePlannerStandardizesOwnedCustomCatalogTypes()
{
    AnalysisResult analysis;
    DataNode root;
    root.elementName = QStringLiteral("CAbilEffectTarget");
    root.id = QStringLiteral("MyCustomAbility");
    analysis.nodes.append(root);
    DataNode custom;
    custom.elementName = QStringLiteral("CAccumulatorConstant");
    custom.id = QStringLiteral("LegacyAccumulator");
    analysis.nodes.append(custom);

    UnitFamily family;
    family.rootId = QStringLiteral("MyCustomAbility");
    family.rootNodeIndex = 0;
    family.entityType = DataCollectionEntityType::Ability;
    family.strictOwnership = true;
    family.objects.append({0, UnitFamilyRole::Ability, QStringLiteral("High"), QStringLiteral("root")});
    family.objects.append({1, UnitFamilyRole::Other, QStringLiteral("High"), QStringLiteral("unique graph owner")});

    const RenamePlan plan = StandardNamePlanner().plan(analysis, family, QStringLiteral("MyCustomAbility"));
    QVERIFY2(plan.valid, qPrintable(plan.conflicts.join(QStringLiteral("; "))));
    QCOMPARE(plan.items.size(), 1);
    QCOMPARE(plan.items.front().oldId, QStringLiteral("LegacyAccumulator"));
    QCOMPARE(plan.items.front().newId, QStringLiteral("MyCustomAbility@AccumulatorConstant"));
}

void CoreTests::renamePlannerBlocksConflicts()
{
    QTemporaryDir dir;
    QVERIFY(writeTextFile(QDir(dir.path()).absoluteFilePath(QStringLiteral("Conflict.xml")), QByteArrayLiteral(
        "<Catalog><CUnit id=\"Vassel\" a=\"ActorVassel\" b=\"ButtonVassel\" c=\"VasselButtonAlt\"/>"
        "<CActorUnit id=\"ActorVassel\" unitName=\"Vassel\"/><CActorUnit id=\"Vassel\"/>"
        "<CButton id=\"ButtonVassel\"/><CButton id=\"VasselButtonAlt\"/></Catalog>")));
    FolderAnalyzer analyzer; AnalysisResult analysis; QString error;
    QVERIFY(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error));
    const UnitFamily family = UnitFamilyDetector().detect(analysis).front();
    const RenamePlan plan = StandardNamePlanner().plan(analysis, family, QStringLiteral("Vassel"));
    QVERIFY(!plan.valid);
    QVERIFY(!plan.conflicts.isEmpty());
    bool targetConflict = false;
    for (const QString &conflict : plan.conflicts) {
        targetConflict |= conflict.contains(QStringLiteral("already exists"));
    }
    QVERIFY(targetConflict);
    const RenamePlan atPlan = StandardNamePlanner().plan(analysis, family, QStringLiteral("Vassel@Alias"));
    QVERIFY(!atPlan.valid);
}

void CoreTests::renamePlannerSkipsDependencyCatalogObjects()
{
    AnalysisResult analysis;
    DataNode node;
    node.sourceFile = QStringLiteral("Mods/Liberty.SC2Mod/GameData/UnitData.xml");
    node.elementName = QStringLiteral("CUnit");
    node.id = QStringLiteral("Marine");
    analysis.nodes.append(node);

    UnitFamily family;
    family.rootId = QStringLiteral("Marine");
    family.rootNodeIndex = 0;
    UnitFamilyObject object;
    object.nodeIndex = 0;
    object.role = UnitFamilyRole::Unit;
    object.confidence = QStringLiteral("High");
    object.reason = QStringLiteral("fixture");
    family.objects.append(object);

    const RenamePlan plan = StandardNamePlanner().plan(analysis, family, QStringLiteral("MarineCustom"));
    QVERIFY(plan.items.isEmpty());
    QCOMPARE(plan.manualReview.size(), 1);
    QVERIFY(plan.manualReview.front().reason.contains(QStringLiteral("protected standard/dependency")));
    QVERIFY(!plan.valid);
}

void CoreTests::renamePlannerSkipsActorUnitsOutsideFamilyScope()
{
    AnalysisResult analysis;
    DataNode root;
    root.elementName = QStringLiteral("CUnit");
    root.id = QStringLiteral("Rofius");
    analysis.nodes.append(root);

    DataNode emptyScopedActor;
    emptyScopedActor.elementName = QStringLiteral("CActorUnit");
    emptyScopedActor.id = QStringLiteral("Probe");
    emptyScopedActor.attributes.insert(QStringLiteral("unitName"), QString());
    analysis.nodes.append(emptyScopedActor);

    DataNode foreignScopedActor;
    foreignScopedActor.elementName = QStringLiteral("CActorUnit");
    foreignScopedActor.id = QStringLiteral("ForeignProbe");
    foreignScopedActor.attributes.insert(QStringLiteral("unitName"), QStringLiteral("Probe"));
    analysis.nodes.append(foreignScopedActor);

    UnitFamily family;
    family.rootId = QStringLiteral("Rofius");
    family.rootNodeIndex = 0;
    family.objects.append({0, UnitFamilyRole::Unit, QStringLiteral("High"), QStringLiteral("Root CUnit")});
    family.objects.append({1, UnitFamilyRole::Actor, QStringLiteral("High"), QStringLiteral("fixture")});
    family.objects.append({2, UnitFamilyRole::Actor, QStringLiteral("High"), QStringLiteral("fixture")});

    const RenamePlan plan = StandardNamePlanner().plan(analysis, family, QStringLiteral("Rofius"));
    QSet<QString> renamedIds;
    for (const RenamePlanItem &item : plan.items)
        renamedIds.insert(item.oldId);
    QVERIFY(!renamedIds.contains(QStringLiteral("Probe")));
    QVERIFY(!renamedIds.contains(QStringLiteral("ForeignProbe")));

    bool sawActorScopeReason = false;
    for (const UnitFamilyObject &object : plan.manualReview)
        sawActorScopeReason = sawActorScopeReason || object.reason.contains(QStringLiteral("actor unitName"));
    QVERIFY(sawActorScopeReason);
}

void CoreTests::catalogEnumRepairFixesLegacyRenameDamage()
{
    QByteArray xml = QByteArrayLiteral(
        "<Catalog>"
        "<CActorUnit id=\"Actor\"><StatusColors index=\"Marine2@Requirement4\" value=\"255,255,255\"/>"
        "<VitalColors index=\"Marine2@Requirement4\" value=\"255,255,255\"/>"
        "<VitalNames index=\"Marine2@Requirement4\" value=\"Shield\"/></CActorUnit>"
        "<CBehaviorBuff id=\"Buff\"><Modification><VitalMaxArray index=\"Marine2@Requirement4\" value=\"1\"/>"
        "<VitalRegenArray index=\"Marine2@Requirement4\" value=\"1\"/></Modification></CBehaviorBuff>"
        "<CActorUnit id=\"Filters\"><TargetFilters value=\"Marine2@Behavior10;Ground\"/>"
        "<On Terms=\"AnimDone Marine2@Behavior4\" Send=\"Create Marine2@Behavior4\"/></CActorUnit>"
        "<CSound id=\"Snd\" parent=\"Marine2@Behavior4\"/>"
        "<AlliedPushPriority value=\"-12\"/>"
        "</Catalog>");
    int changes = 0;
    QString error;
    QVERIFY2(sc2dh::repairKnownCatalogEnumDamage(&xml, &changes, &error), qPrintable(error));
    QVERIFY(changes >= 8);
    const QString repaired = QString::fromUtf8(xml);
    QVERIFY(!repaired.contains(QStringLiteral("Marine2@Requirement4")));
    QVERIFY(!repaired.contains(QStringLiteral("Marine2@Behavior10")));
    QVERIFY(!repaired.contains(QStringLiteral("Marine2@Behavior4")));
    QVERIFY(repaired.contains(QStringLiteral("StatusColors index=\"Shields\"")));
    QVERIFY(repaired.contains(QStringLiteral("VitalColors index=\"Shields\"")));
    QVERIFY(repaired.contains(QStringLiteral("VitalNames index=\"Shields\"")));
    QVERIFY(repaired.contains(QStringLiteral("VitalMaxArray index=\"Shields\"")));
    QVERIFY(repaired.contains(QStringLiteral("VitalRegenArray index=\"Shields\"")));
    QVERIFY(repaired.contains(QStringLiteral("TargetFilters value=\"Dead;Ground\"")));
    QVERIFY(repaired.contains(QStringLiteral("Terms=\"AnimDone Death\"")));
    QVERIFY(repaired.contains(QStringLiteral("Send=\"Create Death\"")));
    QVERIFY(repaired.contains(QStringLiteral("parent=\"Death\"")));
    QVERIFY(repaired.contains(QStringLiteral("AlliedPushPriority value=\"0\"")));
}

void CoreTests::reservedCatalogFilterTokensAreNotReferences()
{
    QTemporaryDir dir;
    QVERIFY(writeTextFile(QDir(dir.path()).absoluteFilePath(QStringLiteral("Filters.xml")), QByteArrayLiteral(
        "<Catalog><CBehaviorBuff id=\"Hidden\"/>"
        "<CWeapon id=\"Gun\"><TargetFilters value=\"Visible;Player,Ally,Hidden,Invulnerable\"/></CWeapon></Catalog>")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    bool sawGun = false;
    for (const DataNode &node : analysis.nodes) {
        if (node.id == QStringLiteral("Gun")) {
            sawGun = true;
            QVERIFY(!node.referencedIds.contains(QStringLiteral("Hidden")));
        }
    }
    QVERIFY(sawGun);
}

void CoreTests::referenceRenamePreviewAndApply()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Family.xml"));
    const QByteArray original = QByteArrayLiteral(
        "<Catalog><CUnit id=\"Vassel\" actor=\"ActorVassel\"/>"
        "<CActorUnit id=\"ActorVassel\" unitName=\"Vassel\"><Event>Unit,Vassel ActorVassel ActorVasselExtra</Event>"
        "<Events><On Terms=\"Unit,Vassel\" Send=\"Create ActorVassel\"/></Events></CActorUnit>"
        "</Catalog>");
    QVERIFY(writeTextFile(path, original));
    FolderAnalyzer analyzer; AnalysisResult analysis; QString error;
    QVERIFY(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error));
    const UnitFamily family = UnitFamilyDetector().detect(analysis).front();
    const RenamePlan plan = StandardNamePlanner().plan(analysis, family, QStringLiteral("Vessel"));
    QVERIFY(plan.valid);
    ReferenceRenamer renamer;
    const RenamePreviewReport preview = renamer.preview(analysis, plan);
    QVERIFY2(preview.valid, qPrintable(preview.conflicts.join(QStringLiteral("; "))));
    QVERIFY(preview.referencesUpdated >= 6);
    QFile unchanged(path); QVERIFY(unchanged.open(QIODevice::ReadOnly)); QCOMPARE(unchanged.readAll(), original); unchanged.close();
    const RenameApplyResult applied = renamer.apply(analysis, plan, dir.path(), {});
    QVERIFY2(applied.success, qPrintable(applied.error));
    QVERIFY(QFileInfo(applied.backupFolder).exists());
    QFile rewritten(path); QVERIFY(rewritten.open(QIODevice::ReadOnly));
    const QString output = QString::fromUtf8(rewritten.readAll());
    QVERIFY(output.contains(QStringLiteral("id=\"Vessel\"")));
    QCOMPARE(output.count(QStringLiteral("id=\"Vessel\"")), 2);
    QVERIFY(output.contains(QStringLiteral("unitName=\"Vessel\"")));
    QVERIFY(output.contains(QStringLiteral("Unit,Vessel Vessel ActorVasselExtra")));
    QVERIFY(output.contains(QStringLiteral("Terms=\"Unit,Vessel\"")));
    QVERIFY(output.contains(QStringLiteral("Send=\"Create Vessel\"")));
}

void CoreTests::referenceRenameRewritesSafeTextReferences()
{
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("GameData")));
    const QString xmlPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("GameData/UnitData.xml"));
    const QString scriptPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("MapScript.galaxy"));
    const QString objectsPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("Objects"));
    QVERIFY(writeTextFile(xmlPath, QByteArrayLiteral("<Catalog><CUnit id=\"OldUnit\"/></Catalog>")));
    QVERIFY(writeTextFile(scriptPath, QByteArrayLiteral("void Init(){ string a = \"OldUnit\"; string b = \"OldUnitExtra\"; }\n")));
    QVERIFY(writeTextFile(objectsPath, QByteArrayLiteral("ObjectUnit { Type=\"OldUnit\" Position={0,0,0} }\n")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    int unitIndex = -1;
    for (int i = 0; i < analysis.nodes.size(); ++i) {
        if (analysis.nodes[i].elementName == QStringLiteral("CUnit") && analysis.nodes[i].id == QStringLiteral("OldUnit"))
            unitIndex = i;
    }
    QVERIFY(unitIndex >= 0);

    RenamePlan plan;
    plan.valid = true;
    RenamePlanItem item;
    item.nodeIndex = unitIndex;
    item.oldId = QStringLiteral("OldUnit");
    item.newId = QStringLiteral("NewUnit");
    item.role = UnitFamilyRole::Unit;
    item.selected = true;
    plan.items << item;

    const RenamePreviewReport preview = ReferenceRenamer().preview(analysis, plan);
    QVERIFY2(preview.valid, qPrintable(preview.conflicts.join(QStringLiteral("; "))));
    QVERIFY(preview.referencesUpdated >= 2);

    const RenameApplyResult applied = ReferenceRenamer().apply(analysis, plan, dir.path(), {});
    QVERIFY2(applied.success, qPrintable(applied.error));
    QVERIFY(applied.changedFiles.contains(QStringLiteral("MapScript.galaxy")));
    QVERIFY(applied.changedFiles.contains(QStringLiteral("Objects")));

    QFile rewrittenScript(scriptPath);
    QVERIFY(rewrittenScript.open(QIODevice::ReadOnly));
    const QString script = QString::fromUtf8(rewrittenScript.readAll());
    QVERIFY(script.contains(QStringLiteral("\"NewUnit\"")));
    QVERIFY(script.contains(QStringLiteral("\"OldUnitExtra\"")));
    QVERIFY(!script.contains(QStringLiteral("\"OldUnit\"")));

    QFile rewrittenObjects(objectsPath);
    QVERIFY(rewrittenObjects.open(QIODevice::ReadOnly));
    const QString objects = QString::fromUtf8(rewrittenObjects.readAll());
    QVERIFY(objects.contains(QStringLiteral("Type=\"NewUnit\"")));

    QFile rewrittenXml(xmlPath);
    QVERIFY(rewrittenXml.open(QIODevice::ReadOnly));
    QVERIFY(QString::fromUtf8(rewrittenXml.readAll()).contains(QStringLiteral("id=\"NewUnit\"")));
}

void CoreTests::referenceRenamePreflightCatchesResidualStrongLinks()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Alerts.xml"));
    const QByteArray original = QByteArrayLiteral(
        "<Catalog>"
        "<CAlert id=\"OldAlert\"/>"
        "<CAbilArmMagazine id=\"Consumer\"><Alert value=\"OldAlert\"/></CAbilArmMagazine>"
        "</Catalog>");
    QVERIFY(writeTextFile(path, original));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    int alertIndex = -1;
    for (int index = 0; index < analysis.nodes.size(); ++index) {
        if (analysis.nodes.at(index).elementName == QStringLiteral("CAlert")
            && analysis.nodes.at(index).id == QStringLiteral("OldAlert")) {
            alertIndex = index;
            break;
        }
    }
    QVERIFY(alertIndex >= 0);

    RenamePlan plan;
    plan.valid = true;
    RenamePlanItem item;
    item.nodeIndex = alertIndex;
    item.oldId = QStringLiteral("OldAlert");
    item.newId = QStringLiteral("NewAlert");
    item.selected = true;
    plan.items << item;

    const RenamePreviewReport preview = ReferenceRenamer().preview(analysis, plan);
    QVERIFY(!preview.valid);
    QVERIFY(preview.conflicts.join(QStringLiteral("\n")).contains(
        QStringLiteral("Pre-save rename verification failed")));
    QVERIFY(preview.conflicts.join(QStringLiteral("\n")).contains(QStringLiteral("OldAlert")));

    const RenameApplyResult applied = ReferenceRenamer().apply(analysis, plan, dir.path(), {});
    QVERIFY(!applied.success);
    QVERIFY(applied.error.contains(QStringLiteral("Pre-save rename verification failed")));

    QFile unchanged(path);
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), original);
}

void CoreTests::referenceRenameBlocksBinaryReferences()
{
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("GameData")));
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("Assets")));
    const QString xmlPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("GameData/UnitData.xml"));
    const QString binaryPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("Assets/Model.m3"));
    const QByteArray originalXml = QByteArrayLiteral("<Catalog><CUnit id=\"BinaryUnit\"/></Catalog>");
    QVERIFY(writeTextFile(xmlPath, originalXml));
    QByteArray modelBytes;
    modelBytes.append('\0');
    modelBytes.append("BinaryUnit");
    modelBytes.append('\0');
    QVERIFY(writeTextFile(binaryPath, modelBytes));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    int unitIndex = -1;
    for (int i = 0; i < analysis.nodes.size(); ++i) {
        if (analysis.nodes[i].elementName == QStringLiteral("CUnit") && analysis.nodes[i].id == QStringLiteral("BinaryUnit"))
            unitIndex = i;
    }
    QVERIFY(unitIndex >= 0);

    RenamePlan plan;
    plan.valid = true;
    RenamePlanItem item;
    item.nodeIndex = unitIndex;
    item.oldId = QStringLiteral("BinaryUnit");
    item.newId = QStringLiteral("RenamedUnit");
    item.role = UnitFamilyRole::Unit;
    item.selected = true;
    plan.items << item;

    const RenamePreviewReport preview = ReferenceRenamer().preview(analysis, plan);
    QVERIFY(!preview.valid);
    const QString previewNotes = (preview.warnings + preview.conflicts).join(QStringLiteral(" "));
    QVERIFY(previewNotes.contains(QStringLiteral("non-rewritable strong references")));
    QVERIFY(previewNotes.contains(QStringLiteral("binary-unconfirmed")));
    QVERIFY(previewNotes.contains(QStringLiteral("Model.m3")));

    const RenameApplyResult applied = ReferenceRenamer().apply(analysis, plan, dir.path(), {});
    QVERIFY(!applied.success);
    QVERIFY(applied.warnings.join(QStringLiteral(" ")).contains(QStringLiteral("Model.m3")));

    QFile unchanged(xmlPath);
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), originalXml);
}

void CoreTests::referenceRenameDoesNotRewriteFilterFields()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Family.xml"));
    const QByteArray original = QByteArrayLiteral(
        "<Catalog><CUnit id=\"Vassel\" actor=\"ActorVassel\"/>"
        "<CActorUnit id=\"ActorVassel\" unitName=\"Vassel\">"
        "<On Send=\"AnimPlay Beam1 Death 0 -1.000000 -1.000000 1.500000 AsDuration\"/>"
        "</CActorUnit>"
        "<CWeapon id=\"Gun\"><TargetFilters value=\"Visible;ActorVassel,Hidden,Invulnerable\"/></CWeapon>"
        "<CEffectDamage id=\"Blast\"><SearchFilters value=\"Visible;ActorVassel,Dead,Hidden\"/></CEffectDamage>"
        "<CBehaviorBuff id=\"Buff\"><Modification><VitalMaxArray index=\"Shields\" value=\"15\"/></Modification></CBehaviorBuff>"
        "<CActorUnit id=\"StatusActor\"><StatusColors index=\"ActorVassel\" value=\"255,255,255\"/>"
        "<VitalColors index=\"ActorVassel\" value=\"255,255,255\"/><VitalNames index=\"ActorVassel\" value=\"Name\"/></CActorUnit>"
        "<CUnit id=\"Indexed\"><Collide index=\"ActorVassel\" value=\"1\"/></CUnit>"
        "<CModel id=\"Model\"><Events><Anim value=\"Death,00\"/></Events></CModel>"
        "</Catalog>");
    QVERIFY(writeTextFile(path, original));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    const UnitFamily family = UnitFamilyDetector().detect(analysis).front();
    const RenamePlan plan = StandardNamePlanner().plan(analysis, family, QStringLiteral("Vessel"));
    QVERIFY(plan.valid);

    const RenameApplyResult applied = ReferenceRenamer().apply(analysis, plan, dir.path(), {});
    QVERIFY2(applied.success, qPrintable(applied.error));

    QFile rewritten(path);
    QVERIFY(rewritten.open(QIODevice::ReadOnly));
    const QString output = QString::fromUtf8(rewritten.readAll());
    QVERIFY(output.contains(QStringLiteral("id=\"Vessel\"")));
    QCOMPARE(output.count(QStringLiteral("id=\"Vessel\"")), 2);
    QVERIFY(output.contains(QStringLiteral("unitName=\"Vessel\"")));
    QVERIFY(output.contains(QStringLiteral("TargetFilters value=\"Visible;ActorVassel,Hidden,Invulnerable\"")));
    QVERIFY(output.contains(QStringLiteral("SearchFilters value=\"Visible;ActorVassel,Dead,Hidden\"")));
    QVERIFY(output.contains(QStringLiteral("VitalMaxArray index=\"Shields\"")));
    QVERIFY(output.contains(QStringLiteral("StatusColors index=\"ActorVassel\"")));
    QVERIFY(output.contains(QStringLiteral("VitalColors index=\"ActorVassel\"")));
    QVERIFY(output.contains(QStringLiteral("VitalNames index=\"ActorVassel\"")));
    QVERIFY(output.contains(QStringLiteral("Collide index=\"ActorVassel\"")));
    QVERIFY(output.contains(QStringLiteral("Send=\"AnimPlay Beam1 Death 0 -1.000000 -1.000000 1.500000 AsDuration\"")));
    QVERIFY(output.contains(QStringLiteral("Anim value=\"Death,00\"")));
}

void CoreTests::referenceRenameDoesNotRewriteUntypedEnumValues()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Enums.xml"));
    QVERIFY(writeTextFile(path, QByteArrayLiteral(
        "<Catalog>"
        "<CValidatorUnitCompareRange id=\"CustomMotionValidator\"/>"
        "<CWeaponLegacy id=\"Weapon\"><AllowedMovement value=\"CustomMotionValidator\"/></CWeaponLegacy>"
        "<CAbilEffectTarget id=\"Ability\"><ValidatorArray value=\"CustomMotionValidator\"/></CAbilEffectTarget>"
        "</Catalog>")));

    AnalysisResult analysis;
    QString analysisError;
    QVERIFY2(FolderAnalyzer().analyzeFolder(dir.path(), {}, &analysis, &analysisError), qPrintable(analysisError));
    const auto validator = std::find_if(analysis.nodes.cbegin(), analysis.nodes.cend(), [](const DataNode &node) {
        return node.elementName == QStringLiteral("CValidatorUnitCompareRange")
            && node.id == QStringLiteral("CustomMotionValidator");
    });
    QVERIFY(validator != analysis.nodes.cend());
    const int validatorIndex = int(std::distance(analysis.nodes.cbegin(), validator));

    RenamePlan plan;
    plan.valid = true;
    RenamePlanItem item;
    item.nodeIndex = validatorIndex;
    item.oldId = QStringLiteral("CustomMotionValidator");
    item.newId = QStringLiteral("Ability@Validator");
    item.role = UnitFamilyRole::Validator;
    plan.items.append(item);

    const RenameApplyResult applied = ReferenceRenamer().apply(analysis, plan, dir.path(), {});
    QVERIFY2(applied.success, qPrintable(applied.error));
    QFile rewritten(path);
    QVERIFY(rewritten.open(QIODevice::ReadOnly));
    const QString output = QString::fromUtf8(rewritten.readAll());
    QVERIFY(output.contains(QStringLiteral("id=\"Ability@Validator\"")));
    QVERIFY(output.contains(QStringLiteral("AllowedMovement value=\"CustomMotionValidator\"")));
    QVERIFY(output.contains(QStringLiteral("ValidatorArray value=\"Ability@Validator\"")));
}

void CoreTests::referenceRenameDoesNotRewriteParentFields()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Family.xml"));
    QVERIFY(writeTextFile(path, QByteArrayLiteral(
        "<Catalog>"
        "<CButton id=\"Zealot\"/>"
        "<CButton id=\"Train\" Face=\"Zealot\"/>"
        "<CUnit id=\"CustomZealot\" parent=\"Zealot\" button=\"Zealot\"/>"
        "</Catalog>")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    int zealotButton = -1;
    for (int i = 0; i < analysis.nodes.size(); ++i) {
        if (analysis.nodes[i].elementName == QStringLiteral("CButton") && analysis.nodes[i].id == QStringLiteral("Zealot"))
            zealotButton = i;
    }
    QVERIFY(zealotButton >= 0);

    RenamePlan plan;
    plan.valid = true;
    RenamePlanItem item;
    item.nodeIndex = zealotButton;
    item.oldId = QStringLiteral("Zealot");
    item.newId = QStringLiteral("Zealot@Ability6");
    item.selected = true;
    plan.items << item;

    const RenameApplyResult applied = ReferenceRenamer().apply(analysis, plan, dir.path(), {});
    QVERIFY2(applied.success, qPrintable(applied.error));

    QFile rewritten(path);
    QVERIFY(rewritten.open(QIODevice::ReadOnly));
    const QString output = QString::fromUtf8(rewritten.readAll());
    QVERIFY(output.contains(QStringLiteral("<CButton id=\"Zealot@Ability6\"")));
    QVERIFY(output.contains(QStringLiteral("Face=\"Zealot@Ability6\"")));
    QVERIFY(output.contains(QStringLiteral("button=\"Zealot@Ability6\"")));
    QVERIFY(output.contains(QStringLiteral("parent=\"Zealot\"")));
    QVERIFY(!output.contains(QStringLiteral("parent=\"Zealot@Ability6\"")));
}

void CoreTests::referenceRenameUsesTypedCatalogFields()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Typed.xml"));
    QVERIFY(writeTextFile(path, QByteArrayLiteral(
        "<Catalog>"
        "<CUnit id=\"Ghost2\"/>"
        "<CActorUnit id=\"GhostActor\" unitName=\"Ghost2\"><Model value=\"Ghost2\"/></CActorUnit>"
        "<CModel id=\"Ghost2\"/>"
        "</Catalog>")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    const auto nodeIndex = [&analysis](const QString &type, const QString &id) {
        for (int index = 0; index < analysis.nodes.size(); ++index) {
            const DataNode &node = analysis.nodes[index];
            if (node.elementName == type && node.id == id)
                return index;
        }
        return -1;
    };

    RenamePlan plan;
    plan.valid = true;
    RenamePlanItem unitItem;
    unitItem.nodeIndex = nodeIndex(QStringLiteral("CUnit"), QStringLiteral("Ghost2"));
    unitItem.oldId = QStringLiteral("Ghost2");
    unitItem.newId = QStringLiteral("GhostCustom");
    unitItem.role = UnitFamilyRole::Unit;
    unitItem.selected = true;
    plan.items.append(unitItem);

    RenamePlanItem modelItem;
    modelItem.nodeIndex = nodeIndex(QStringLiteral("CModel"), QStringLiteral("Ghost2"));
    modelItem.oldId = QStringLiteral("Ghost2");
    modelItem.newId = QStringLiteral("GhostCustom@Model");
    modelItem.role = UnitFamilyRole::Model;
    modelItem.selected = true;
    plan.items.append(modelItem);
    QVERIFY(unitItem.nodeIndex >= 0);
    QVERIFY(modelItem.nodeIndex >= 0);

    const RenameApplyResult applied = ReferenceRenamer().apply(analysis, plan, dir.path(), {});
    QVERIFY2(applied.success, qPrintable(applied.error));

    QFile rewritten(path);
    QVERIFY(rewritten.open(QIODevice::ReadOnly));
    const QString output = QString::fromUtf8(rewritten.readAll());
    QVERIFY(output.contains(QStringLiteral("<CUnit id=\"GhostCustom\"")));
    QVERIFY(output.contains(QStringLiteral("<CModel id=\"GhostCustom@Model\"")));
    QVERIFY(output.contains(QStringLiteral("unitName=\"GhostCustom\"")));
    QVERIFY(output.contains(QStringLiteral("<Model value=\"GhostCustom@Model\"")));
    QVERIFY(!output.contains(QStringLiteral("unitName=\"GhostCustom@Model\"")));
}

void CoreTests::referenceRenameActorIdDoesNotRewriteUnitScope()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("ActorScope.xml"));
    QVERIFY(writeTextFile(path, QByteArrayLiteral(
        "<Catalog>"
        "<CUnit id=\"Probe\"/>"
        "<CActorUnit id=\"Probe\" unitName=\"Probe\"/>"
        "<CActorUnit id=\"Probe2\" unitName=\"Probe2\"/>"
        "</Catalog>")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    int actorProbe = -1;
    for (int i = 0; i < analysis.nodes.size(); ++i) {
        const DataNode &node = analysis.nodes[i];
        if (node.elementName == QStringLiteral("CActorUnit") && node.id == QStringLiteral("Probe"))
            actorProbe = i;
    }
    QVERIFY(actorProbe >= 0);

    RenamePlan plan;
    plan.valid = true;
    RenamePlanItem item;
    item.nodeIndex = actorProbe;
    item.oldId = QStringLiteral("Probe");
    item.newId = QStringLiteral("Probe@Actor");
    item.role = UnitFamilyRole::Actor;
    item.selected = true;
    plan.items.append(item);

    const RenameApplyResult applied = ReferenceRenamer().apply(analysis, plan, dir.path(), {});
    QVERIFY2(applied.success, qPrintable(applied.error));

    QFile rewritten(path);
    QVERIFY(rewritten.open(QIODevice::ReadOnly));
    const QString output = QString::fromUtf8(rewritten.readAll());
    QVERIFY(output.contains(QStringLiteral("<CActorUnit id=\"Probe@Actor\" unitName=\"Probe\"")));
    QVERIFY(output.contains(QStringLiteral("<CActorUnit id=\"Probe2\" unitName=\"Probe2\"")));
    QVERIFY(!output.contains(QStringLiteral("unitName=\"Probe@Actor\"")));
}

void CoreTests::referenceRenameSkipsOccupiedTargetWhenOwnerNotMoved()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Family.xml"));
    QVERIFY(writeTextFile(path, QByteArrayLiteral(
        "<Catalog>"
        "<CUnit id=\"Alpha\"/>"
        "<CUnit id=\"Beta\"/>"
        "<CUnit id=\"Watcher\" ref=\"Alpha Beta\"/>"
        "</Catalog>")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    int alpha = -1;
    for (int i = 0; i < analysis.nodes.size(); ++i) {
        if (analysis.nodes[i].id == QStringLiteral("Alpha"))
            alpha = i;
    }
    QVERIFY(alpha >= 0);

    RenamePlan plan;
    plan.valid = true;
    RenamePlanItem item;
    item.nodeIndex = alpha;
    item.oldId = QStringLiteral("Alpha");
    item.newId = QStringLiteral("Beta");
    item.selected = true;
    plan.items << item;

    const RenameApplyResult applied = ReferenceRenamer().apply(analysis, plan, dir.path(), {});
    QVERIFY(!applied.success);

    QFile unchanged(path);
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    const QString output = QString::fromUtf8(unchanged.readAll());
    QCOMPARE(output.count(QStringLiteral("id=\"Beta\"")), 1);
    QVERIFY(output.contains(QStringLiteral("id=\"Alpha\"")));
}

void CoreTests::referenceRenameRollback()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Family.xml"));
    const QByteArray original = QByteArrayLiteral("<Catalog><CUnit id=\"Vassel\" actor=\"ActorVassel\"/><CActorUnit id=\"ActorVassel\" unitName=\"Vassel\"/></Catalog>");
    QVERIFY(writeTextFile(path, original));
    FolderAnalyzer analyzer; AnalysisResult analysis; QString error;
    QVERIFY(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error));
    const UnitFamily family = UnitFamilyDetector().detect(analysis).front();
    const RenamePlan plan = StandardNamePlanner().plan(analysis, family, QStringLiteral("Vessel"));
    ReferenceRenamer renamer; renamer.setFailureInjectionStep(QStringLiteral("after-commit"));
    const RenameApplyResult applied = renamer.apply(analysis, plan, dir.path(), {});
    QVERIFY(!applied.success);
    QFile restored(path); QVERIFY(restored.open(QIODevice::ReadOnly)); QCOMPARE(restored.readAll(), original);
}

void CoreTests::normalizedDuplicateIgnoresOnlyRootIdentity()
{
    XmlLoader loader;
    QVector<DataNode> nodes;
    QString error;
    const QByteArray xml = R"xml(<Catalog>
      <CEffect id="A" name="First"><Value b="2" a="1"/><Link id="Nested"/></CEffect>
      <CEffect name="Second" id="B">
        <Value a="1" b="2"/><Link id="Nested"/>
      </CEffect>
      <CEffect id="C"><Value a="9" b="2"/><Link id="Nested"/></CEffect>
    </Catalog>)xml";
    QVERIFY2(loader.extractNodes(QStringLiteral("Effects.xml"), xml, &nodes, &error), qPrintable(error));
    const auto hash = [&nodes](const QString &id) {
        for (const DataNode &node : nodes) if (node.id == id) return node.contentHash;
        return QString();
    };
    QCOMPARE(hash(QStringLiteral("A")), hash(QStringLiteral("B")));
    QVERIFY(hash(QStringLiteral("A")) != hash(QStringLiteral("C")));
}

void CoreTests::tokenAwareReplacementVariants()
{
    QString value = QStringLiteral("BossDamageB Effect,BossDamageB CEffect,BossDamageB BossDamageB,Other Other BossDamageB BossDamageBigger");
    QCOMPARE(MergeService::replaceIdTokens(&value, QStringLiteral("BossDamageB"), QStringLiteral("BossDamageA")), 5);
    QCOMPARE(value, QStringLiteral("BossDamageA Effect,BossDamageA CEffect,BossDamageA BossDamageA,Other Other BossDamageA BossDamageBigger"));
    QCOMPARE(MergeService::countIdTokens(value, QStringLiteral("BossDamageB")), 0);
    QVERIFY(value.endsWith(QStringLiteral("BossDamageBigger")));
}

void CoreTests::numericOnlyIdsAreNotRewritten()
{
    QString numeric = QStringLiteral("Period=\"1\" Range=\"2\" PassChance 1.000000 InfoArray[26]");
    QCOMPARE(MergeService::replaceIdTokens(&numeric, QStringLiteral("1"), QStringLiteral("Ghost2@Requirement11")), 0);
    QCOMPARE(MergeService::replaceIdTokens(&numeric, QStringLiteral("26"), QStringLiteral("HugeSwarmQueen@Requirement")), 0);
    QCOMPARE(numeric, QStringLiteral("Period=\"1\" Range=\"2\" PassChance 1.000000 InfoArray[26]"));
    QCOMPARE(MergeService::countIdTokens(numeric, QStringLiteral("1")), 0);

    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Numeric.xml"));
    QVERIFY(writeTextFile(path, QByteArrayLiteral(
        "<Catalog>"
        "<CUnit id=\"Hero\" period=\"1\" effect=\"1\"/>"
        "<CEffectDamage id=\"1\"><Amount value=\"5\"/></CEffectDamage>"
        "<CEffectDamage id=\"2\"><Amount value=\"5\"/></CEffectDamage>"
        "</Catalog>")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    int heroIndex = -1;
    int oneIndex = -1;
    for (int i = 0; i < analysis.nodes.size(); ++i) {
        if (analysis.nodes[i].id == QStringLiteral("Hero"))
            heroIndex = i;
        if (analysis.nodes[i].id == QStringLiteral("1"))
            oneIndex = i;
    }
    QVERIFY(heroIndex >= 0);
    QVERIFY(oneIndex >= 0);
    QVERIFY(!analysis.nodes[heroIndex].referencedIds.contains(QStringLiteral("1")));
    QVERIFY(std::none_of(analysis.duplicateContentGroups.cbegin(), analysis.duplicateContentGroups.cend(),
                         [](const DuplicateContentGroup &group) { return group.mergeCandidate; }));

    RenamePlan plan;
    plan.valid = true;
    RenamePlanItem item;
    item.nodeIndex = oneIndex;
    item.oldId = QStringLiteral("1");
    item.newId = QStringLiteral("Hero@Effect");
    item.selected = true;
    plan.items << item;
    const RenameApplyResult applied = ReferenceRenamer().apply(analysis, plan, dir.path(), {});
    QVERIFY(!applied.success);

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray unchanged = file.readAll();
    QVERIFY(unchanged.contains("period=\"1\""));
    QVERIFY(unchanged.contains("effect=\"1\""));
    QVERIFY(unchanged.contains("id=\"1\""));
}

void CoreTests::unifiedReferenceIndexClassifiesStrongWeakAssetAndBinaryReferences()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QDir root(dir.path());
    QVERIFY(root.mkpath(QStringLiteral("GameData")));
    QVERIFY(root.mkpath(QStringLiteral("scripts")));
    QVERIFY(root.mkpath(QStringLiteral("Assets")));

    const QString objectsPath = root.absoluteFilePath(QStringLiteral("Objects"));
    const QString scriptPath = root.absoluteFilePath(QStringLiteral("scripts/MapScript.galaxy"));
    const QString mapInfoPath = root.absoluteFilePath(QStringLiteral("MapInfo.txt"));
    const QString treeAssetPath = root.absoluteFilePath(QStringLiteral("Assets/Tree.m3"));
    const QString binaryAssetPath = root.absoluteFilePath(QStringLiteral("Assets/BinaryBlob.m3"));

    QVERIFY(writeTextFile(objectsPath, QByteArrayLiteral(
        "ObjectUnit { Id = 1 Unit = \"Marine\" Position = (10, 10, 0) }\n"
        "ObjectDoodad { Id = 2 Type = \"TreeDoodad\" Position = (20, 20, 0) }\n")));
    QVERIFY(writeTextFile(scriptPath, QByteArrayLiteral(
        "void InitMap() { TriggerDebugOutput(1, StringToText(\"DamageEffect\"), true); }\n")));
    QVERIFY(writeTextFile(mapInfoPath, QByteArrayLiteral("Preview=Assets/Tree.m3\n")));
    QVERIFY(writeTextFile(treeAssetPath, QByteArrayLiteral("binary model payload")));
    QVERIFY(writeTextFile(binaryAssetPath, QByteArrayLiteral("opaque Marine token in binary payload")));

    AnalysisResult analysis;
    analysis.rootFolder = dir.path();
    const auto scanned = [](const QString &path, bool xml = false, bool data = false) {
        ScannedFileInfo file;
        file.filePath = path;
        file.isXml = xml;
        file.isSc2DataLike = data;
        file.size = QFileInfo(path).size();
        return file;
    };
    analysis.scannedFiles = {
        scanned(objectsPath),
        scanned(scriptPath),
        scanned(mapInfoPath),
        scanned(treeAssetPath),
        scanned(binaryAssetPath)
    };

    DataNode marine;
    marine.sourceFile = QStringLiteral("GameData/UnitData.xml");
    marine.elementName = QStringLiteral("CUnit");
    marine.id = QStringLiteral("Marine");
    marine.referencedIds = {QStringLiteral("MarineActor")};
    marine.lineNumber = 3;

    DataNode actor;
    actor.sourceFile = QStringLiteral("GameData/ActorData.xml");
    actor.elementName = QStringLiteral("CActorUnit");
    actor.id = QStringLiteral("MarineActor");

    DataNode doodad;
    doodad.sourceFile = QStringLiteral("GameData/DoodadData.xml");
    doodad.elementName = QStringLiteral("CDoodad");
    doodad.id = QStringLiteral("TreeDoodad");

    DataNode effect;
    effect.sourceFile = QStringLiteral("GameData/EffectData.xml");
    effect.elementName = QStringLiteral("CEffectDamage");
    effect.id = QStringLiteral("DamageEffect");

    analysis.nodes = {marine, actor, doodad, effect};

    sc2dh::refs::UnifiedReferenceIndex index;
    index.build(analysis);

    const QVector<sc2dh::refs::ReferenceRecord> actorRefs =
        index.referencesToId(QStringLiteral("MarineActor"));
    QVERIFY(std::any_of(actorRefs.cbegin(), actorRefs.cend(), [](const sc2dh::refs::ReferenceRecord &record) {
        return record.kind == sc2dh::refs::ReferenceKind::TypedXml
            && record.strength == sc2dh::refs::ReferenceStrength::Strong
            && record.sourceId == QStringLiteral("Marine")
            && record.rewritable;
    }));

    const QVector<sc2dh::refs::ReferenceRecord> marineRefs =
        index.referencesToId(QStringLiteral("Marine"));
    QVERIFY(std::any_of(marineRefs.cbegin(), marineRefs.cend(), [](const sc2dh::refs::ReferenceRecord &record) {
        return record.kind == sc2dh::refs::ReferenceKind::PlacementRoot
            && record.strength == sc2dh::refs::ReferenceStrength::Strong
            && record.sourceFile == QStringLiteral("Objects");
    }));
    QVERIFY(std::any_of(marineRefs.cbegin(), marineRefs.cend(), [](const sc2dh::refs::ReferenceRecord &record) {
        return record.kind == sc2dh::refs::ReferenceKind::BinaryUnconfirmed
            && record.strength == sc2dh::refs::ReferenceStrength::Blocking
            && !record.rewritable;
    }));
    QVERIFY(index.hasNonRewritableStrongReferenceToId(QStringLiteral("Marine")));

    const QVector<sc2dh::refs::ReferenceRecord> effectRefs =
        index.referencesToId(QStringLiteral("DamageEffect"));
    QVERIFY(std::any_of(effectRefs.cbegin(), effectRefs.cend(), [](const sc2dh::refs::ReferenceRecord &record) {
        return record.kind == sc2dh::refs::ReferenceKind::ScriptText
            && record.strength == sc2dh::refs::ReferenceStrength::Strong
            && record.sourceFile == QStringLiteral("scripts/MapScript.galaxy")
            && record.lineNumber == 1;
    }));
    QVERIFY(!index.hasNonRewritableStrongReferenceToId(QStringLiteral("DamageEffect")));

    const QVector<sc2dh::refs::ReferenceRecord> assetRefs =
        index.referencesToAsset(QStringLiteral("Assets/Tree.m3"));
    QVERIFY(std::any_of(assetRefs.cbegin(), assetRefs.cend(), [](const sc2dh::refs::ReferenceRecord &record) {
        return record.kind == sc2dh::refs::ReferenceKind::AssetText
            && record.strength == sc2dh::refs::ReferenceStrength::Strong
            && record.sourceFile == QStringLiteral("MapInfo.txt");
    }));
}

void CoreTests::mergePreviewAndApplyRedirectBeforeDelete()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = QDir(dir.path()).absoluteFilePath(QStringLiteral("Effects.xml"));
    QVERIFY(writeTextFile(file, QByteArrayLiteral(
        "<Catalog><CEffect id=\"BossDamage01\"><Amount value=\"5\"/></CEffect>"
        "<CEffect id=\"BossDamage02\"><Amount value=\"5\"/></CEffect>"
        "<CActor id=\"Actor\" effect=\"Effect,BossDamage02\"><Links>BossDamage02 Other</Links><Ref id=\"BossDamage02\"/></CActor></Catalog>")));
    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    int keep = -1, remove = -1;
    for (int i = 0; i < analysis.nodes.size(); ++i) {
        if (analysis.nodes[i].id == QStringLiteral("BossDamage01")) keep = i;
        if (analysis.nodes[i].id == QStringLiteral("BossDamage02") && analysis.nodes[i].elementName == QStringLiteral("CEffect")) remove = i;
    }
    QVERIFY(keep >= 0 && remove >= 0);
    MergeRequest request{keep, {remove}};
    MergeService service;
    const MergePreview preview = service.preview(analysis, request);
    QVERIFY(preview.valid);
    QCOMPARE(preview.keptId, QStringLiteral("BossDamage01"));
    QCOMPARE(preview.nodesDeleted, 1);
    QCOMPARE(preview.referencesRedirected, 3);
    const MergeApplyResult applied = service.apply(analysis, request, dir.path(), {});
    QVERIFY2(applied.success, qPrintable(applied.error));
    QFile rewritten(file);
    QVERIFY(rewritten.open(QIODevice::ReadOnly));
    const QString output = QString::fromUtf8(rewritten.readAll());
    QCOMPARE(MergeService::countIdTokens(output, QStringLiteral("BossDamage02")), 0);
    QVERIFY(output.contains(QStringLiteral("Effect,BossDamage01")));
    QVERIFY(output.contains(QStringLiteral(">BossDamage01 Other<")));
    QVERIFY(output.contains(QStringLiteral("<Ref id=\"BossDamage01\"")));
}

void CoreTests::mergeAllowsManualUnrelatedExactDuplicateAndActorEvents()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = QDir(dir.path()).absoluteFilePath(QStringLiteral("Effects.xml"));
    QVERIFY(writeTextFile(file, QByteArrayLiteral(
        "<Catalog>"
        "<CEffectDamage id=\"AlphaEffect\"><Amount value=\"5\"/></CEffectDamage>"
        "<CEffectDamage id=\"BetaEffect\"><Amount value=\"5\"/></CEffectDamage>"
        "<CActorUnit id=\"Actor\"><On Terms=\"Effect,BetaEffect,Start\" Send=\"Create BetaEffect\"/>"
        "<Events><On index=\"9\" Terms=\"Effect,BetaEffect,Start\" Send=\"Create BetaEffect\"/></Events>"
        "<EventText>BetaEffect BetaEffectExtra</EventText></CActorUnit>"
        "</Catalog>")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    int keep = -1;
    int remove = -1;
    for (int i = 0; i < analysis.nodes.size(); ++i) {
        if (analysis.nodes[i].elementName == QStringLiteral("CEffectDamage")
            && analysis.nodes[i].id == QStringLiteral("AlphaEffect"))
            keep = i;
        if (analysis.nodes[i].elementName == QStringLiteral("CEffectDamage")
            && analysis.nodes[i].id == QStringLiteral("BetaEffect"))
            remove = i;
    }
    QVERIFY(keep >= 0);
    QVERIFY(remove >= 0);

    const auto duplicateGroup = std::find_if(analysis.duplicateContentGroups.cbegin(),
                                             analysis.duplicateContentGroups.cend(),
                                             [&](const DuplicateContentGroup &group) {
                                                 return group.nodeIndices.contains(keep)
                                                     && group.nodeIndices.contains(remove);
                                             });
    QVERIFY(duplicateGroup != analysis.duplicateContentGroups.cend());
    QVERIFY(duplicateGroup->mergeCandidate);
    QVERIFY(!duplicateGroup->autoRecommended);
    QCOMPARE(duplicateGroup->commonIdMask, QStringLiteral("unrelated IDs"));

    const MergePreview preview = MergeService().preview(analysis, MergeRequest{keep, {remove}});
    QVERIFY2(preview.valid, qPrintable(preview.warnings.join(QStringLiteral("; "))));
    QCOMPARE(preview.keptId, QStringLiteral("AlphaEffect"));

    const MergeApplyResult applied = MergeService().apply(analysis, MergeRequest{keep, {remove}}, dir.path(), {});
    QVERIFY2(applied.success, qPrintable(applied.error));
    QFile rewritten(file);
    QVERIFY(rewritten.open(QIODevice::ReadOnly));
    const QString output = QString::fromUtf8(rewritten.readAll());
    QVERIFY(!output.contains(QStringLiteral("<CEffectDamage id=\"BetaEffect\"")));
    QVERIFY(output.contains(QStringLiteral("Terms=\"Effect,AlphaEffect,Start\"")));
    QVERIFY(output.contains(QStringLiteral("Send=\"Create AlphaEffect\"")));
    QVERIFY(output.contains(QStringLiteral("index=\"9\" Terms=\"Effect,AlphaEffect,Start\"")));
    QVERIFY(output.contains(QStringLiteral(">AlphaEffect BetaEffectExtra<")));
    QVERIFY(output.contains(QStringLiteral("<CEffectDamage id=\"AlphaEffect\"")));
    QCOMPARE(MergeService::countIdTokens(output, QStringLiteral("BetaEffect")), 0);
}

void CoreTests::mergeRewritesNonXmlReferenceFiles()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("scripts")));
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("TriggerLibs")));
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("Triggers")));

    const QString dataFile = QDir(dir.path()).absoluteFilePath(QStringLiteral("Effects.xml"));
    const QString scriptFile = QDir(dir.path()).absoluteFilePath(QStringLiteral("scripts/MapScript.galaxy"));
    const QString triggerLibFile = QDir(dir.path()).absoluteFilePath(QStringLiteral("TriggerLibs/DecorLib.galaxy"));
    const QString triggersFile = QDir(dir.path()).absoluteFilePath(QStringLiteral("Triggers/MapTriggers.SC2Triggers"));
    const QString objectsFile = QDir(dir.path()).absoluteFilePath(QStringLiteral("Objects"));
    QVERIFY(writeTextFile(dataFile, QByteArrayLiteral(
        "<Catalog><CEffect id=\"BossDamage01\"><Amount value=\"5\"/></CEffect>"
        "<CEffect id=\"BossDamage02\"><Amount value=\"5\"/></CEffect>"
        "<CActor id=\"Actor\" effect=\"BossDamage02\"/></Catalog>")));
    QVERIFY(writeTextFile(scriptFile, QByteArrayLiteral(
        "void Test(){ string effect = \"BossDamage02\"; string keep = \"BossDamage02Extra\"; }\n")));
    QVERIFY(writeTextFile(triggerLibFile, QByteArrayLiteral(
        "void LibTest(){ string effect = \"BossDamage02\"; }\n")));
    QVERIFY(writeTextFile(triggersFile, QByteArrayLiteral(
        "<Trigger><Param>BossDamage02</Param><Param>BossDamage02Extra</Param></Trigger>\n")));
    QVERIFY(writeTextFile(objectsFile, QByteArrayLiteral(
        "ObjectDoodad { Type = BossDamage02 Name = BossDamage02Extra }\n")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    int keep = -1;
    int remove = -1;
    for (int i = 0; i < analysis.nodes.size(); ++i) {
        if (analysis.nodes[i].elementName == QStringLiteral("CEffect")
            && analysis.nodes[i].id == QStringLiteral("BossDamage01"))
            keep = i;
        if (analysis.nodes[i].elementName == QStringLiteral("CEffect")
            && analysis.nodes[i].id == QStringLiteral("BossDamage02"))
            remove = i;
    }
    QVERIFY(keep >= 0);
    QVERIFY(remove >= 0);

    const MergePreview preview = MergeService().preview(analysis, MergeRequest{keep, {remove}});
    QVERIFY2(preview.valid, qPrintable(preview.warnings.join(QStringLiteral("; "))));
    QVERIFY(preview.filesChanged.contains(scriptFile));
    QVERIFY(preview.filesChanged.contains(objectsFile));
    QVERIFY(preview.referencesRedirected >= 3);

    const MergeApplyResult applied = MergeService().apply(analysis, MergeRequest{keep, {remove}}, dir.path(), {});
    QVERIFY2(applied.success, qPrintable(applied.error));
    QVERIFY(applied.changedFiles.contains(QStringLiteral("scripts/MapScript.galaxy")));
    QVERIFY(applied.changedFiles.contains(QStringLiteral("TriggerLibs/DecorLib.galaxy")));
    QVERIFY(applied.changedFiles.contains(QStringLiteral("Triggers/MapTriggers.SC2Triggers")));
    QVERIFY(applied.changedFiles.contains(QStringLiteral("Objects")));

    QFile script(scriptFile);
    QVERIFY(script.open(QIODevice::ReadOnly));
    const QString scriptOutput = QString::fromUtf8(script.readAll());
    QVERIFY(scriptOutput.contains(QStringLiteral("\"BossDamage01\"")));
    QVERIFY(scriptOutput.contains(QStringLiteral("\"BossDamage02Extra\"")));
    QCOMPARE(MergeService::countIdTokens(scriptOutput, QStringLiteral("BossDamage02")), 0);

    QFile triggerLib(triggerLibFile);
    QVERIFY(triggerLib.open(QIODevice::ReadOnly));
    const QString triggerLibOutput = QString::fromUtf8(triggerLib.readAll());
    QVERIFY(triggerLibOutput.contains(QStringLiteral("\"BossDamage01\"")));
    QCOMPARE(MergeService::countIdTokens(triggerLibOutput, QStringLiteral("BossDamage02")), 0);

    QFile triggers(triggersFile);
    QVERIFY(triggers.open(QIODevice::ReadOnly));
    const QString triggersOutput = QString::fromUtf8(triggers.readAll());
    QVERIFY(triggersOutput.contains(QStringLiteral(">BossDamage01<")));
    QVERIFY(triggersOutput.contains(QStringLiteral(">BossDamage02Extra<")));
    QCOMPARE(MergeService::countIdTokens(triggersOutput, QStringLiteral("BossDamage02")), 0);

    QFile objects(objectsFile);
    QVERIFY(objects.open(QIODevice::ReadOnly));
    const QString objectsOutput = QString::fromUtf8(objects.readAll());
    QVERIFY(objectsOutput.contains(QStringLiteral("Type = BossDamage01")));
    QVERIFY(objectsOutput.contains(QStringLiteral("Name = BossDamage02Extra")));
    QCOMPARE(MergeService::countIdTokens(objectsOutput, QStringLiteral("BossDamage02")), 0);
}

void CoreTests::mergeBlocksBinaryNonRewritableReferences()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("GameData")));
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("Assets")));

    const QString file = QDir(dir.path()).absoluteFilePath(QStringLiteral("GameData/Effects.xml"));
    const QString binaryFile = QDir(dir.path()).absoluteFilePath(QStringLiteral("Assets/Model.m3"));
    const QByteArray originalXml = QByteArrayLiteral(
        "<Catalog><CEffect id=\"BossDamage01\"><Amount value=\"5\"/></CEffect>"
        "<CEffect id=\"BossDamage02\"><Amount value=\"5\"/></CEffect></Catalog>");
    QVERIFY(writeTextFile(file, originalXml));
    QByteArray binaryBytes;
    binaryBytes.append('\0');
    binaryBytes.append("BossDamage02");
    binaryBytes.append('\0');
    QVERIFY(writeTextFile(binaryFile, binaryBytes));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    int keep = -1;
    int remove = -1;
    for (int i = 0; i < analysis.nodes.size(); ++i) {
        if (analysis.nodes[i].id == QStringLiteral("BossDamage01"))
            keep = i;
        if (analysis.nodes[i].id == QStringLiteral("BossDamage02"))
            remove = i;
    }
    QVERIFY(keep >= 0 && remove >= 0);

    const MergePreview preview = MergeService().preview(analysis, MergeRequest{keep, {remove}});
    QVERIFY(!preview.valid);
    QVERIFY(preview.warnings.join(QStringLiteral("\n")).contains(QStringLiteral("not safe text")));
    QVERIFY(preview.warnings.join(QStringLiteral("\n")).contains(QStringLiteral("Assets/Model.m3")));

    const MergeApplyResult applied = MergeService().apply(analysis, MergeRequest{keep, {remove}}, dir.path(), {});
    QVERIFY(!applied.success);

    QFile unchanged(file);
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), originalXml);
}

void CoreTests::mergeDoesNotRewriteSurvivingCatalogIdentityIds()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = QDir(dir.path()).absoluteFilePath(QStringLiteral("Effects.xml"));
    QVERIFY(writeTextFile(file, QByteArrayLiteral(
        "<Catalog><CEffect id=\"BossDamage01\"><Amount value=\"5\"/></CEffect>"
        "<CEffect id=\"BossDamage02\"><Amount value=\"5\"/></CEffect>"
        "<CActor id=\"BossDamage02\" effect=\"BossDamage02\"/></Catalog>")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    int keep = -1;
    int remove = -1;
    for (int i = 0; i < analysis.nodes.size(); ++i) {
        if (analysis.nodes[i].elementName == QStringLiteral("CEffect")
            && analysis.nodes[i].id == QStringLiteral("BossDamage01")) {
            keep = i;
        }
        if (analysis.nodes[i].elementName == QStringLiteral("CEffect")
            && analysis.nodes[i].id == QStringLiteral("BossDamage02")) {
            remove = i;
        }
    }
    QVERIFY(keep >= 0 && remove >= 0);

    const MergeApplyResult applied = MergeService().apply(analysis, MergeRequest{keep, {remove}}, dir.path(), {});
    QVERIFY2(applied.success, qPrintable(applied.error));

    QFile rewritten(file);
    QVERIFY(rewritten.open(QIODevice::ReadOnly));
    const QString output = QString::fromUtf8(rewritten.readAll());
    QVERIFY(output.contains(QStringLiteral("<CActor id=\"BossDamage02\" effect=\"BossDamage01\"")));
    QVERIFY(!output.contains(QStringLiteral("<CActor id=\"BossDamage01\"")));
}

void CoreTests::mergeAllowsResidualOldIdWarning()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = QDir(dir.path()).absoluteFilePath(QStringLiteral("Effects.xml"));
    QVERIFY(writeTextFile(file, QByteArrayLiteral(
        "<Catalog><CEffect id=\"BossDamage01\"><Amount value=\"5\"/></CEffect>"
        "<CEffect id=\"BossDamage02\"><Amount value=\"5\"/></CEffect>"
        "<CActor id=\"Actor\" effect=\"BossDamage02\"><StatusColors index=\"BossDamage02\" value=\"255,255,255\"/></CActor></Catalog>")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    int keep = -1;
    int remove = -1;
    for (int i = 0; i < analysis.nodes.size(); ++i) {
        if (analysis.nodes[i].id == QStringLiteral("BossDamage01"))
            keep = i;
        if (analysis.nodes[i].id == QStringLiteral("BossDamage02"))
            remove = i;
    }
    QVERIFY(keep >= 0 && remove >= 0);

    const MergeApplyResult applied = MergeService().apply(analysis, MergeRequest{keep, {remove}}, dir.path(), {});
    QVERIFY2(applied.success, qPrintable(applied.error));
    QVERIFY(!applied.warnings.isEmpty());
    QVERIFY(applied.warnings.join(QLatin1Char('\n')).contains(QStringLiteral("residual old ID token")));

    QFile rewritten(file);
    QVERIFY(rewritten.open(QIODevice::ReadOnly));
    const QString output = QString::fromUtf8(rewritten.readAll());
    QVERIFY(!output.contains(QStringLiteral("<CEffect id=\"BossDamage02\"")));
    QVERIFY(output.contains(QStringLiteral("effect=\"BossDamage01\"")));
    QVERIFY(output.contains(QStringLiteral("StatusColors index=\"BossDamage02\"")));
}

void CoreTests::mergeRollbackOnFailure()
{
    QTemporaryDir dir;
    const QString file = QDir(dir.path()).absoluteFilePath(QStringLiteral("Data.xml"));
    const QByteArray original = QByteArrayLiteral("<Catalog><CUnit id=\"Unit01\"><V value=\"1\"/></CUnit><CUnit id=\"Unit02\"><V value=\"1\"/></CUnit><X id=\"X\" value=\"Unit02\"/></Catalog>");
    QVERIFY(writeTextFile(file, original));
    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error));
    int keep = -1, remove = -1;
    for (int i = 0; i < analysis.nodes.size(); ++i) {
        if (analysis.nodes[i].id == QStringLiteral("Unit01")) keep = i;
        if (analysis.nodes[i].id == QStringLiteral("Unit02")) remove = i;
    }
    MergeService service;
    service.setFailureInjectionStep(QStringLiteral("after-commit"));
    const MergeApplyResult result = service.apply(analysis, MergeRequest{keep, {remove}}, dir.path(), {});
    QVERIFY(!result.success);
    QFile restored(file);
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QCOMPARE(restored.readAll(), original);
}

void CoreTests::unusedSafetyClassification()
{
    QTemporaryDir dir;
    const QString file = QDir(dir.path()).absoluteFilePath(QStringLiteral("Data.xml"));
    QVERIFY(writeTextFile(file, QByteArrayLiteral("<Catalog><CUnit id=\"White\"/><CUnit id=\"Scripted\"/><CUnit id=\"Safe\"/></Catalog>")));
    QVERIFY(writeTextFile(QDir(dir.path()).absoluteFilePath(QStringLiteral("logic.galaxy")), QByteArrayLiteral("use Scripted;")));
    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY(analyzer.analyzeFolder(dir.path(), {QStringLiteral("White")}, &analysis, &error));
    QHash<QString, UnusedCandidateInfo> byId;
    for (const UnusedCandidateInfo &candidate : analysis.unusedCandidates)
        byId.insert(analysis.nodes[candidate.nodeIndex].id, candidate);
    QCOMPARE(byId[QStringLiteral("White")].state, CandidateState::Blocked);
    QVERIFY(byId[QStringLiteral("White")].whitelisted);
    QCOMPARE(byId[QStringLiteral("Scripted")].state, CandidateState::Blocked);
    QVERIFY(byId[QStringLiteral("Scripted")].scriptReferences > 0);
    QCOMPARE(byId[QStringLiteral("Safe")].state, CandidateState::Safe);
    QVERIFY(analysis.possibleUnusedNodeIndices.contains(byId[QStringLiteral("Safe")].nodeIndex));
}

void CoreTests::unusedReachabilityDistinguishesStatesAndPaths()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Reachability.xml"));
    QVERIFY(writeTextFile(path, QByteArrayLiteral(
        "<Catalog>"
        "<CPlacedUnit id=\"Placement01\" Unit=\"UsedUnit\"/>"
        "<CUnit id=\"UsedUnit\"><WeaponArray Link=\"UsedWeapon\"/></CUnit>"
        "<CWeaponLegacy id=\"UsedWeapon\"><Effect value=\"UsedDamage\"/></CWeaponLegacy>"
        "<CEffectDamage id=\"UsedDamage\"/>"
        "<CAbilEffectTarget id=\"OrphanAbil\"><Effect value=\"OrphanEffect\"/></CAbilEffectTarget>"
        "<CEffectDamage id=\"OrphanEffect\"/>"
        "<CUnit id=\"DisconnectedUnit\"/>"
        "<CUnit id=\"CollectionOnly\"/>"
        "<CDataCollectionUnit id=\"Group\"><DataRecord Entry=\"Unit,CollectionOnly\"/></CDataCollectionUnit>"
        "</Catalog>")));
    FolderAnalyzer analyzer; AnalysisResult analysis; QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    QHash<QString, UnusedCandidateInfo> byId;
    for (const UnusedCandidateInfo &candidate : analysis.unusedCandidates)
        byId.insert(analysis.nodes[candidate.nodeIndex].id, candidate);
    QCOMPARE(byId[QStringLiteral("UsedUnit")].usageState, UsageState::Used);
    QCOMPARE(byId[QStringLiteral("UsedDamage")].usageState, UsageState::Used);
    QVERIFY(byId[QStringLiteral("UsedDamage")].usagePath.join(QStringLiteral(" -> ")).contains(QStringLiteral("Placed Unit(Placement01)")));
    QVERIFY(byId[QStringLiteral("UsedDamage")].usagePath.join(QStringLiteral(" -> ")).contains(QStringLiteral("WeaponLegacy(UsedWeapon)")));
    QCOMPARE(byId[QStringLiteral("OrphanAbil")].usageState, UsageState::UnusedSubgraph);
    QCOMPARE(byId[QStringLiteral("OrphanAbil")].state, CandidateState::Safe);
    QVERIFY(analysis.possibleUnusedNodeIndices.contains(byId[QStringLiteral("OrphanAbil")].nodeIndex));
    QCOMPARE(byId[QStringLiteral("OrphanEffect")].usageState, UsageState::UnusedSubgraph);
    QCOMPARE(byId[QStringLiteral("OrphanEffect")].state, CandidateState::Safe);
    QVERIFY(analysis.possibleUnusedNodeIndices.contains(byId[QStringLiteral("OrphanEffect")].nodeIndex));
    QCOMPARE(byId[QStringLiteral("DisconnectedUnit")].usageState, UsageState::Disconnected);
    QVERIFY(analysis.possibleUnusedNodeIndices.contains(byId[QStringLiteral("DisconnectedUnit")].nodeIndex));
    QCOMPARE(byId[QStringLiteral("CollectionOnly")].usageState, UsageState::Disconnected);
    QCOMPARE(byId[QStringLiteral("CollectionOnly")].dataCollectionReferences, 1);
}

void CoreTests::unusedReachabilityUsesActorUnitNameWithoutTestRefs()
{
    struct EnvGuard
    {
        bool had = qEnvironmentVariableIsSet("SC2DH_ENABLE_TEST_REFS");
        QByteArray previous = qgetenv("SC2DH_ENABLE_TEST_REFS");
        ~EnvGuard()
        {
            if (had)
                qputenv("SC2DH_ENABLE_TEST_REFS", previous);
            else
                qunsetenv("SC2DH_ENABLE_TEST_REFS");
        }
    } guard;
    qunsetenv("SC2DH_ENABLE_TEST_REFS");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dataPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("ActorUnitName.xml"));
    const QString objectsPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("Objects"));
    QVERIFY(writeTextFile(objectsPath, QByteArrayLiteral(
        "ObjectUnit { Id = 1 Unit = \"HeroUnit\" Position = (10, 10, 0) }\n")));
    QVERIFY(writeTextFile(dataPath, QByteArrayLiteral(
        "<Catalog>"
        "<CUnit id=\"HeroUnit\"/>"
        "<CActorUnit id=\"HeroActor\" unitName=\"HeroUnit\" Model=\"HeroModel\"><SoundArray value=\"HeroSound\"/>"
        "<Events><On Terms=\"Effect,NestedEffect,Start\" Send=\"Create\"/>"
        "<On Terms=\"Effect.DotEffect.Start\" Send=\"Create\"/></Events></CActorUnit>"
        "<CModel id=\"HeroModel\"/>"
        "<CSound id=\"HeroSound\"/>"
        "<CEffectDamage id=\"NestedEffect\"/>"
        "<CEffectDamage id=\"DotEffect\"/>"
        "<CUnit id=\"DeadUnit\"/>"
        "<CActorUnit id=\"DeadActor\" unitName=\"DeadUnit\" Model=\"DeadModel\"/>"
        "<CModel id=\"DeadModel\"/>"
        "</Catalog>")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    QHash<QString, int> indexById;
    QHash<QString, UnusedCandidateInfo> byId;
    for (int i = 0; i < analysis.nodes.size(); ++i)
        indexById.insert(analysis.nodes.at(i).id, i);
    for (const UnusedCandidateInfo &candidate : analysis.unusedCandidates)
        byId.insert(analysis.nodes.at(candidate.nodeIndex).id, candidate);

    const QStringList usedVisualChain = {
        QStringLiteral("HeroUnit"), QStringLiteral("HeroActor"),
        QStringLiteral("HeroModel"), QStringLiteral("HeroSound"),
        QStringLiteral("NestedEffect"), QStringLiteral("DotEffect")
    };
    for (const QString &id : usedVisualChain) {
        QVERIFY2(byId.contains(id), qPrintable(id));
        QVERIFY2(byId.value(id).usageState == UsageState::Used
                     || byId.value(id).usageState == UsageState::Blocked,
                 qPrintable(id));
        QVERIFY2(!analysis.possibleUnusedNodeIndices.contains(indexById.value(id)), qPrintable(id));
    }
    QVERIFY(analysis.nodes.at(indexById.value(QStringLiteral("HeroUnit"))).referencedIds.contains(QStringLiteral("HeroActor")));
    QVERIFY(analysis.nodes.at(indexById.value(QStringLiteral("HeroActor"))).referencedIds.contains(QStringLiteral("HeroModel")));
    QVERIFY(analysis.nodes.at(indexById.value(QStringLiteral("HeroActor"))).referencedIds.contains(QStringLiteral("HeroSound")));
    QVERIFY(analysis.nodes.at(indexById.value(QStringLiteral("HeroActor"))).referencedIds.contains(QStringLiteral("NestedEffect")));
    QVERIFY(analysis.nodes.at(indexById.value(QStringLiteral("HeroActor"))).referencedIds.contains(QStringLiteral("DotEffect")));

    QCOMPARE(byId.value(QStringLiteral("DeadActor")).state, CandidateState::Safe);
    QCOMPARE(byId.value(QStringLiteral("DeadModel")).state, CandidateState::Safe);
}

void CoreTests::unusedObjectChainsCoverFullCatalogGraphAndPlacementRoots()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dataPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("FullChain.xml"));
    const QString objectsPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("Objects"));
    const QString scriptPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("MapScript.galaxy"));
    QVERIFY(writeTextFile(objectsPath, QByteArrayLiteral(
        "ObjectUnit { Id = 1 Unit = \"UsedUnit\" Position = (10, 10, 0) }\n")));
    QVERIFY(writeTextFile(scriptPath, QByteArrayLiteral(
        "void InitMap() { TriggerDebugOutput(1, StringToText(\"GalaxyRootEffect\"), true); }\n")));
    QVERIFY(writeTextFile(dataPath, QByteArrayLiteral(
        "<Catalog>"
        "<CUnit id=\"UsedUnit\" refs=\"UsedActor UsedWeapon\"/>"
        "<CActorUnit id=\"UsedActor\" refs=\"UsedModel UsedSound\"><On Terms=\"Effect,UsedEffect,Start\" Send=\"Create\"/></CActorUnit>"
        "<CModel id=\"UsedModel\" refs=\"UsedSound\"/>"
        "<CSound id=\"UsedSound\"/>"
        "<CWeaponLegacy id=\"UsedWeapon\" refs=\"UsedEffect\"/>"
        "<CEffectApplyBehavior id=\"UsedEffect\" refs=\"UsedBehavior UsedValidator\"/>"
        "<CBehaviorBuff id=\"UsedBehavior\" refs=\"UsedValidator UsedButton\"/>"
        "<CValidatorCondition id=\"UsedValidator\" refs=\"UsedRequirement\"/>"
        "<CRequirementAllowUnit id=\"UsedRequirement\" refs=\"UsedButton\"/>"
        "<CButton id=\"UsedButton\"/>"

        "<CUnit id=\"DeadUnit\" refs=\"DeadActor DeadWeapon\"/>"
        "<CActorUnit id=\"DeadActor\" refs=\"DeadModel DeadSound\"/>"
        "<CModel id=\"DeadModel\" refs=\"DeadSound\"/>"
        "<CSound id=\"DeadSound\"/>"
        "<CWeaponLegacy id=\"DeadWeapon\" refs=\"DeadEffect\"/>"
        "<CEffectApplyBehavior id=\"DeadEffect\" refs=\"DeadBehavior DeadValidator\"/>"
        "<CBehaviorBuff id=\"DeadBehavior\" refs=\"DeadValidator DeadButton\"/>"
        "<CValidatorCondition id=\"DeadValidator\" refs=\"DeadRequirement\"/>"
        "<CRequirementAllowUnit id=\"DeadRequirement\" refs=\"DeadButton\"/>"
        "<CButton id=\"DeadButton\"/>"

        "<CWeaponLegacy id=\"PartialWeapon\" refs=\"PartialEffect\"/>"
        "<CEffectDamage id=\"PartialEffect\"/>"

        "<CEffectApplyBehavior id=\"GalaxyRootEffect\" refs=\"GalaxyBehavior\"/>"
        "<CBehaviorBuff id=\"GalaxyBehavior\"/>"

        "<CDataCollectionUnit id=\"ExistingCollection\"><DataRecord Entry=\"Unit,DeadUnit\"/></CDataCollectionUnit>"
        "</Catalog>")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    QHash<QString, int> indexById;
    QHash<QString, UnusedCandidateInfo> byId;
    for (int i = 0; i < analysis.nodes.size(); ++i)
        indexById.insert(analysis.nodes[i].id, i);
    for (const UnusedCandidateInfo &candidate : analysis.unusedCandidates)
        byId.insert(analysis.nodes[candidate.nodeIndex].id, candidate);

    const QStringList usedChain = {
        QStringLiteral("UsedUnit"), QStringLiteral("UsedActor"), QStringLiteral("UsedModel"),
        QStringLiteral("UsedSound"), QStringLiteral("UsedWeapon"), QStringLiteral("UsedEffect"),
        QStringLiteral("UsedBehavior"), QStringLiteral("UsedValidator"), QStringLiteral("UsedRequirement"),
        QStringLiteral("UsedButton")
    };
    for (const QString &id : usedChain) {
        QVERIFY2(byId.contains(id), qPrintable(id));
        QVERIFY2(byId.value(id).usageState == UsageState::Used
                     || byId.value(id).usageState == UsageState::Blocked,
                 qPrintable(id));
        QVERIFY2(!analysis.possibleUnusedNodeIndices.contains(indexById.value(id)), qPrintable(id));
    }
    QVERIFY(byId.value(QStringLiteral("UsedUnit")).externalReferenceSources.join(QStringLiteral("\n")).contains(QStringLiteral("Objects")));
    QVERIFY(analysis.nodes[indexById.value(QStringLiteral("UsedActor"))].referencedIds.contains(QStringLiteral("UsedEffect")));

    const QStringList deadChain = {
        QStringLiteral("DeadUnit"), QStringLiteral("DeadActor"), QStringLiteral("DeadModel"),
        QStringLiteral("DeadSound"), QStringLiteral("DeadWeapon"), QStringLiteral("DeadEffect"),
        QStringLiteral("DeadBehavior"), QStringLiteral("DeadValidator"), QStringLiteral("DeadRequirement"),
        QStringLiteral("DeadButton")
    };
    QVector<int> rowsToDelete;
    for (const QString &id : deadChain) {
        QVERIFY2(byId.contains(id), qPrintable(id));
        QCOMPARE(byId.value(id).state, CandidateState::Safe);
        QCOMPARE(byId.value(id).usageState, UsageState::UnusedSubgraph);
        rowsToDelete << indexById.value(id);
    }
    QCOMPARE(byId.value(QStringLiteral("DeadUnit")).dataCollectionReferences, 1);

    QCOMPARE(byId.value(QStringLiteral("PartialEffect")).state, CandidateState::Safe);
    QCOMPARE(byId.value(QStringLiteral("PartialEffect")).usageState, UsageState::UnusedSubgraph);
    rowsToDelete << indexById.value(QStringLiteral("PartialEffect"));

    QCOMPARE(byId.value(QStringLiteral("GalaxyRootEffect")).usageState, UsageState::Blocked);
    QVERIFY(byId.value(QStringLiteral("GalaxyRootEffect")).scriptReferences > 0);
    QCOMPARE(byId.value(QStringLiteral("GalaxyBehavior")).usageState, UsageState::Used);

    QString backup;
    QStringList changed;
    int removed = 0;
    int skipped = 0;
    QVERIFY2(analyzer.applySelectedChanges(analysis,
                                           rowsToDelete,
                                           dir.path(),
                                           {},
                                           &backup,
                                           &error,
                                           &changed,
                                           &removed,
                                           &skipped),
             qPrintable(error));

    QFile rewritten(dataPath);
    QVERIFY(rewritten.open(QIODevice::ReadOnly));
    const QString output = QString::fromUtf8(rewritten.readAll());
    for (const QString &id : deadChain)
        QVERIFY2(!output.contains(QStringLiteral("id=\"%1\"").arg(id)), qPrintable(id));
    QVERIFY(output.contains(QStringLiteral("id=\"PartialEffect\"")));
    QVERIFY(output.contains(QStringLiteral("id=\"PartialWeapon\"")));
    QVERIFY(output.contains(QStringLiteral("id=\"UsedUnit\"")));
    QVERIFY(output.contains(QStringLiteral("id=\"GalaxyRootEffect\"")));
    QVERIFY(output.contains(QStringLiteral("DataRecord Entry=\"Unit,DeadUnit\"")));
    QCOMPARE(removed, deadChain.size());
    QCOMPARE(skipped, 1);
}

void CoreTests::unusedDeletionRemovesWholeUnusedChain()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Reachability.xml"));
    QVERIFY(writeTextFile(path, QByteArrayLiteral(
        "<Catalog>"
        "<CPlacedUnit id=\"Placement01\" Unit=\"UsedUnit\"/>"
        "<CUnit id=\"UsedUnit\"><WeaponArray Link=\"UsedWeapon\"/></CUnit>"
        "<CWeaponLegacy id=\"UsedWeapon\"><Effect value=\"UsedDamage\"/></CWeaponLegacy>"
        "<CEffectDamage id=\"UsedDamage\"/>"
        "<CAbilEffectTarget id=\"OrphanAbil\"><Effect value=\"OrphanEffect\"/></CAbilEffectTarget>"
        "<CEffectDamage id=\"OrphanEffect\"/>"
        "</Catalog>")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    QVector<int> rows;
    for (const UnusedCandidateInfo &candidate : analysis.unusedCandidates) {
        const QString id = analysis.nodes[candidate.nodeIndex].id;
        if (id == QStringLiteral("OrphanAbil") || id == QStringLiteral("OrphanEffect"))
            rows.append(candidate.nodeIndex);
    }
    QCOMPARE(rows.size(), 2);

    QString backup;
    QStringList changed;
    int removed = 0;
    int skipped = 0;
    QVERIFY2(analyzer.applySelectedChanges(analysis, rows, dir.path(), {}, &backup, &error, &changed, &removed, &skipped), qPrintable(error));
    QFile output(path);
    QVERIFY(output.open(QIODevice::ReadOnly));
    const QByteArray xml = output.readAll();
    QVERIFY(!xml.contains("OrphanAbil"));
    QVERIFY(!xml.contains("OrphanEffect"));
    QVERIFY(xml.contains("UsedDamage"));
    QCOMPARE(removed, 2);
    QCOMPARE(skipped, 0);
}

void CoreTests::unusedDeletionSkipsPartialChainWithoutFailingBatch()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Reachability.xml"));
    QVERIFY(writeTextFile(path, QByteArrayLiteral(
        "<Catalog>"
        "<CAbilEffectTarget id=\"OrphanAbil\"><Effect value=\"OrphanEffect\"/></CAbilEffectTarget>"
        "<CEffectDamage id=\"OrphanEffect\"/>"
        "<CUnit id=\"LonelyUnused\"/>"
        "</Catalog>")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    QVector<int> rows;
    for (const UnusedCandidateInfo &candidate : analysis.unusedCandidates) {
        const QString id = analysis.nodes[candidate.nodeIndex].id;
        if (id == QStringLiteral("OrphanEffect") || id == QStringLiteral("LonelyUnused"))
            rows.append(candidate.nodeIndex);
    }
    QCOMPARE(rows.size(), 2);

    QString backup;
    QStringList changed;
    int removed = 0;
    int skipped = 0;
    QVERIFY2(analyzer.applySelectedChanges(analysis, rows, dir.path(), {}, &backup, &error, &changed, &removed, &skipped), qPrintable(error));
    QFile output(path);
    QVERIFY(output.open(QIODevice::ReadOnly));
    const QByteArray xml = output.readAll();
    QVERIFY(xml.contains("OrphanEffect"));
    QVERIFY(xml.contains("OrphanAbil"));
    QVERIFY(!xml.contains("LonelyUnused"));
    QCOMPARE(removed, 1);
    QCOMPARE(skipped, 1);
}

void CoreTests::unusedDeletionPreservesDataCollectionLinks()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Data.xml"));
    QVERIFY(writeTextFile(path, QByteArrayLiteral(
        "<Catalog><CUnit id=\"UnusedUnit\"/><CDataCollectionUnit id=\"ExistingCollection\">"
        "<DataRecord Entry=\"Unit,UnusedUnit\"/></CDataCollectionUnit></Catalog>")));
    FolderAnalyzer analyzer; AnalysisResult analysis; QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    int row = -1;
    for (int index = 0; index < analysis.nodes.size(); ++index)
        if (analysis.nodes[index].id == QStringLiteral("UnusedUnit")) row = index;
    QVERIFY(row >= 0);
    const auto candidate = std::find_if(analysis.unusedCandidates.cbegin(), analysis.unusedCandidates.cend(),
                                        [row](const UnusedCandidateInfo &info) { return info.nodeIndex == row; });
    QVERIFY(candidate != analysis.unusedCandidates.cend());
    QCOMPARE(candidate->state, CandidateState::Safe);
    QCOMPARE(candidate->dataCollectionReferences, 1);
    QString backup; QStringList changed; int removed = 0; int skipped = 0;
    QVERIFY2(analyzer.applySelectedChanges(analysis, {row}, dir.path(), {}, &backup, &error, &changed, &removed, &skipped), qPrintable(error));
    QFile result(path); QVERIFY(result.open(QIODevice::ReadOnly)); const QByteArray xml = result.readAll();
    QVERIFY(!xml.contains("id=\"UnusedUnit\""));
    QVERIFY(xml.contains("Unit,UnusedUnit"));
    QCOMPARE(removed, 1);
}

void CoreTests::editorRuntimeCatalogObjectsAreProtected()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("TerrainData.xml"));
    QVERIFY(writeTextFile(path, QByteArrayLiteral(
        "<Catalog>"
        "<CPlacedUnit id=\"Placement01\" Unit=\"UsedUnit\"/>"
        "<CUnit id=\"UsedUnit\"/>"
        "<CUnit id=\"UnusedUnit\"/>"
        "<CTerrain id=\"TerrainBase\" Paint=\"Agria\"/>"
        "<CTerrain id=\"TerrainChild\" parent=\"TerrainBase\" Paint=\"Agria\"/>"
        "<CWater id=\"Agria\"/>"
        "<CLight id=\"AgriaLight\"/>"
        "<CCamera id=\"Dflt\"/>"
        "<CTexture id=\"TerrainDiffuse1\"><Slot value=\"Diffuse\"/></CTexture>"
        "<CTexture id=\"TerrainDiffuse2\"><Slot value=\"Diffuse\"/></CTexture>"
        "</Catalog>")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    QHash<QString, int> indexById;
    QHash<QString, UnusedCandidateInfo> candidateById;
    for (int i = 0; i < analysis.nodes.size(); ++i)
        indexById.insert(analysis.nodes[i].id, i);
    for (const UnusedCandidateInfo &candidate : analysis.unusedCandidates)
        candidateById.insert(analysis.nodes[candidate.nodeIndex].id, candidate);

    const QStringList protectedIds = {
        QStringLiteral("TerrainBase"),
        QStringLiteral("TerrainChild"),
        QStringLiteral("Agria"),
        QStringLiteral("AgriaLight"),
        QStringLiteral("Dflt"),
        QStringLiteral("TerrainDiffuse1"),
        QStringLiteral("TerrainDiffuse2")
    };
    for (const QString &id : protectedIds) {
        QVERIFY2(candidateById.contains(id), qPrintable(id));
        QCOMPARE(candidateById.value(id).protectedObject, true);
        QCOMPARE(candidateById.value(id).state, CandidateState::Blocked);
        QVERIFY(!analysis.possibleUnusedNodeIndices.contains(indexById.value(id)));
    }
    QCOMPARE(candidateById.value(QStringLiteral("UnusedUnit")).state, CandidateState::Safe);

    QVERIFY(std::none_of(analysis.duplicateContentGroups.cbegin(), analysis.duplicateContentGroups.cend(),
                         [](const DuplicateContentGroup &group) {
                             return group.elementName.compare(QStringLiteral("CTexture"), Qt::CaseInsensitive) == 0
                                 || group.elementName.compare(QStringLiteral("CTerrain"), Qt::CaseInsensitive) == 0;
                         }));

    MergeRequest mergeRequest;
    mergeRequest.keepNodeIndex = indexById.value(QStringLiteral("TerrainDiffuse1"));
    mergeRequest.removeNodeIndices = {indexById.value(QStringLiteral("TerrainDiffuse2"))};
    const MergePreview preview = MergeService().preview(analysis, mergeRequest);
    QVERIFY(!preview.valid);
    QVERIFY(preview.warnings.join(QLatin1Char('\n')).contains(QStringLiteral("editor/runtime catalog object")));

    QVERIFY(std::none_of(analysis.deepCleanupCandidates.cbegin(), analysis.deepCleanupCandidates.cend(),
                         [](const DeepCleanupCandidate &candidate) {
                             return candidate.kind == DeepCleanupKind::RedundantDefaultField
                                 && candidate.label == QStringLiteral("TerrainChild.Paint");
                         }));

    QString backup;
    QStringList changed;
    int removed = 0;
    int skipped = 0;
    QVERIFY2(analyzer.applySelectedChanges(analysis,
                                           analysis.possibleUnusedNodeIndices,
                                           dir.path(),
                                           {},
                                           &backup,
                                           &error,
                                           &changed,
                                           &removed,
                                           &skipped),
             qPrintable(error));

    QFile result(path);
    QVERIFY(result.open(QIODevice::ReadOnly));
    const QByteArray xml = result.readAll();
    QVERIFY(!xml.contains("id=\"UnusedUnit\""));
    QVERIFY(xml.contains("id=\"TerrainBase\""));
    QVERIFY(xml.contains("id=\"TerrainChild\""));
    QVERIFY(xml.contains("id=\"Agria\""));
    QVERIFY(xml.contains("id=\"AgriaLight\""));
    QVERIFY(xml.contains("id=\"Dflt\""));
    QVERIFY(xml.contains("id=\"TerrainDiffuse1\""));
    QVERIFY(xml.contains("id=\"TerrainDiffuse2\""));
    QCOMPARE(removed, 1);
}

void CoreTests::deepCleanupAppliesSafeCandidates()
{
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("GameData")));
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("Assets")));
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("Imported")));
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("Base.SC2Data/UI/Layout")));
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("enUS.SC2Data/LocalizedData")));
    const QString xmlPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("GameData/Data.xml"));
    const QString documentInfoPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("DocumentInfo"));
    const QString locPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("enUS.SC2Data/LocalizedData/GameStrings.txt"));
    const QString assetPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("Assets/OrphanTexture.dds"));
    const QString modelVariantPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("Assets/OrphanAnimClip.m3a"));
    const QString importedXmlPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("Imported/OrphanConfig.xml"));
    const QString catalogXmlPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("GameData/UnusedCatalog.xml"));
    const QString referencedCardPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("MapCard.jpg"));
    const QString thumbnailPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("Fallen_Thumnail_1.jpg"));
    const QString screenshotPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("MapScreenshot_01.jpg"));
    const QString descIndexPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("Base.SC2Data/UI/Layout/DescIndex.SC2Layout"));
    const QString minimapPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("Minimap.tga"));
    const QString lightingPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("LightingMap.tga"));
    const QString preloadPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("PreloadAssetDB.txt"));
    QVERIFY(writeTextFile(xmlPath, QByteArrayLiteral(
        "<Catalog>"
        "<CActor id=\"Actor\"><Event>Effect,MissingFx</Event><Event>Effect,ExistingFx</Event></CActor>"
        "<CEffectDamage id=\"ExistingFx\"/>"
        "<CUnit id=\"Parent\" Life=\"100\" flag=\"same\"/>"
        "<CUnit id=\"Child\" parent=\"Parent\" Life=\"100\" flag=\"diff\"/>"
        "</Catalog>")));
    QVERIFY(writeTextFile(documentInfoPath, QByteArrayLiteral(
        "<DocInfo><Value>MapCard.jpg</Value><Screenshot><File>MapScreenshot_01.jpg</File></Screenshot></DocInfo>")));
    QVERIFY(writeTextFile(locPath, QByteArrayLiteral("Unit/Name/MissingUnit=Old name\r\nUnit/Name/ExistingFx=Keep\r\n")));
    QVERIFY(writeTextFile(assetPath, QByteArrayLiteral("unused asset bytes")));
    QVERIFY(writeTextFile(modelVariantPath, QByteArrayLiteral("unused model animation bytes")));
    QVERIFY(writeTextFile(importedXmlPath, QByteArrayLiteral("<Config><Flag value=\"1\"/></Config>")));
    QVERIFY(writeTextFile(catalogXmlPath, QByteArrayLiteral("<Catalog><CUnit id=\"CatalogOnly\"/></Catalog>")));
    QVERIFY(writeTextFile(referencedCardPath, QByteArrayLiteral("referenced map card image")));
    QVERIFY(writeTextFile(thumbnailPath, QByteArrayLiteral("editor map thumbnail")));
    QVERIFY(writeTextFile(screenshotPath, QByteArrayLiteral("editor map screenshot")));
    QVERIFY(writeTextFile(descIndexPath, QByteArrayLiteral("<Desc><Include path=\"UI/Layout/UnitFrame1.SC2Layout\"/></Desc>")));
    QVERIFY(writeTextFile(minimapPath, QByteArrayLiteral("editor minimap")));
    QVERIFY(writeTextFile(lightingPath, QByteArrayLiteral("editor lighting")));
    QVERIFY(writeTextFile(preloadPath, QByteArrayLiteral("editor preload asset db")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    auto hasKind = [&](DeepCleanupKind kind) {
        return std::any_of(analysis.deepCleanupCandidates.cbegin(), analysis.deepCleanupCandidates.cend(),
                           [kind](const DeepCleanupCandidate &candidate) {
                               return candidate.kind == kind && candidate.state == CandidateState::Safe;
                           });
    };
    QVERIFY(hasKind(DeepCleanupKind::UnusedAsset));
    QVERIFY(hasKind(DeepCleanupKind::LocalizationEntry));
    QVERIFY(hasKind(DeepCleanupKind::RedundantDefaultField));
    QVERIFY(hasKind(DeepCleanupKind::BrokenActorEvent));
    QVERIFY(std::any_of(analysis.deepCleanupCandidates.cbegin(), analysis.deepCleanupCandidates.cend(),
                        [&](const DeepCleanupCandidate &candidate) {
                            return candidate.kind == DeepCleanupKind::UnusedAsset
                                && candidate.filePath == modelVariantPath;
                        }));
    QVERIFY(std::any_of(analysis.deepCleanupCandidates.cbegin(), analysis.deepCleanupCandidates.cend(),
                        [&](const DeepCleanupCandidate &candidate) {
                            return candidate.kind == DeepCleanupKind::UnusedAsset
                                && candidate.filePath == importedXmlPath;
                        }));
    for (const DeepCleanupCandidate &candidate : analysis.deepCleanupCandidates) {
        QCOMPARE_NE(candidate.filePath, catalogXmlPath);
        QCOMPARE_NE(QFileInfo(candidate.filePath).fileName(), QStringLiteral("Minimap.tga"));
        QCOMPARE_NE(QFileInfo(candidate.filePath).fileName(), QStringLiteral("LightingMap.tga"));
        QCOMPARE_NE(QFileInfo(candidate.filePath).fileName(), QStringLiteral("PreloadAssetDB.txt"));
        QCOMPARE_NE(QFileInfo(candidate.filePath).fileName(), QStringLiteral("DescIndex.SC2Layout"));
        QCOMPARE_NE(QFileInfo(candidate.filePath).fileName(), QStringLiteral("MapCard.jpg"));
        QCOMPARE_NE(QFileInfo(candidate.filePath).fileName(), QStringLiteral("Fallen_Thumnail_1.jpg"));
        QCOMPARE_NE(QFileInfo(candidate.filePath).fileName(), QStringLiteral("MapScreenshot_01.jpg"));
    }

    QVector<int> selected;
    for (const DeepCleanupCandidate &candidate : analysis.deepCleanupCandidates)
        if (candidate.state == CandidateState::Safe && candidate.recommended && candidate.action != DeepCleanupAction::ReportOnly)
            selected.append(candidate.index);
    QVERIFY(!selected.isEmpty());

    const DeepCleanupApplyResult applied = DeepCleanupService().apply(analysis, selected, dir.path(), true);
    QVERIFY2(applied.success, qPrintable(applied.error));
    QVERIFY2(QFileInfo::exists(applied.backupFolder), qPrintable(applied.backupFolder));
    QVERIFY(applied.filesDeleted >= 1);
    QVERIFY(applied.textLinesRemoved >= 1);
    QVERIFY(applied.xmlAttributesRemoved >= 1);
    QVERIFY(applied.xmlNodesRemoved >= 1);
    QVERIFY(!QFileInfo::exists(assetPath));
    QVERIFY(!QFileInfo::exists(modelVariantPath));
    QVERIFY(!QFileInfo::exists(importedXmlPath));
    QVERIFY(QFileInfo::exists(catalogXmlPath));
    QVERIFY(QFileInfo::exists(referencedCardPath));
    QVERIFY(QFileInfo::exists(thumbnailPath));
    QVERIFY(QFileInfo::exists(screenshotPath));
    QVERIFY(QFileInfo::exists(descIndexPath));
    QVERIFY(QFileInfo::exists(minimapPath));
    QVERIFY(QFileInfo::exists(lightingPath));
    QVERIFY(QFileInfo::exists(preloadPath));

    QFile xmlFile(xmlPath);
    QVERIFY(xmlFile.open(QIODevice::ReadOnly));
    const QByteArray xml = xmlFile.readAll();
    QVERIFY(!xml.contains("Effect,MissingFx"));
    QVERIFY(xml.contains("Effect,ExistingFx"));
    QVERIFY(!xml.contains("id=\"Child\" parent=\"Parent\" Life=\"100\""));

    QFile locFile(locPath);
    QVERIFY(locFile.open(QIODevice::ReadOnly));
    const QByteArray loc = locFile.readAll();
    QVERIFY(!loc.contains("MissingUnit"));
    QVERIFY(loc.contains("ExistingFx"));
}

void CoreTests::deepCleanupRemovesStructurallyInvalidActorEvents()
{
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("GameData")));
    const QString xmlPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("GameData/ActorData.xml"));
    QVERIFY(writeTextFile(xmlPath, QByteArrayLiteral(
        "<Catalog>"
        "<CActorUnit id=\"BrokenActor\">"
        "<On index=\"0\" Terms=\"UnitBirth.BrokenActor\"/>"
        "<On index=\"1\" Send=\"Create\"/>"
        "<On index=\"2\"/>"
        "<On index=\"3\" removed=\"1\"/>"
        "<On index=\"4\" Terms=\"UnitBirth.BrokenActor\" Send=\"Create\"/>"
        "<Events><On index=\"5\" Terms=\"UnitBirth.NestedBroken\"/></Events>"
        "</CActorUnit>"
        "</Catalog>")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    QVector<int> selected;
    for (const DeepCleanupCandidate &candidate : analysis.deepCleanupCandidates) {
        if (candidate.kind == DeepCleanupKind::BrokenActorEvent
            && candidate.state == CandidateState::Safe
            && candidate.recommended) {
            selected.append(candidate.index);
        }
    }
    QCOMPARE(selected.size(), 4);

    const DeepCleanupApplyResult applied = DeepCleanupService().apply(analysis, selected, dir.path(), true);
    QVERIFY2(applied.success, qPrintable(applied.error));
    QCOMPARE(applied.xmlNodesRemoved, 4);

    QFile result(xmlPath);
    QVERIFY(result.open(QIODevice::ReadOnly));
    const QByteArray xml = result.readAll();
    QVERIFY(!xml.contains("index=\"0\" Terms=\"UnitBirth.BrokenActor\""));
    QVERIFY(!xml.contains("index=\"1\" Send=\"Create\""));
    QVERIFY(!xml.contains("index=\"2\"/>"));
    QVERIFY(!xml.contains("index=\"5\" Terms=\"UnitBirth.NestedBroken\""));
    QVERIFY(xml.contains("index=\"3\" removed=\"1\""));
    QVERIFY(xml.contains("index=\"4\" Terms=\"UnitBirth.BrokenActor\" Send=\"Create\""));
}

void CoreTests::deepCleanupPlanningFailureLeavesOriginalsByteIdentical()
{
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("GameData")));
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("enUS.SC2Data/LocalizedData")));
    const QString localizationPath = QDir(dir.path()).absoluteFilePath(
        QStringLiteral("enUS.SC2Data/LocalizedData/GameStrings.txt"));
    const QString xmlPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("GameData/UnitData.xml"));
    const QByteArray localizationOriginal = QByteArrayLiteral("Unit/Name/RemoveMe=Old\r\nUnit/Name/KeepMe=Keep\r\n");
    const QByteArray xmlOriginal = QByteArrayLiteral("<Catalog><CUnit id=\"KeepMe\"/></Catalog>");
    QVERIFY(writeTextFile(localizationPath, localizationOriginal));
    QVERIFY(writeTextFile(xmlPath, xmlOriginal));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    QCOMPARE(analysis.completeness, AnalysisCompleteness::Complete);

    DeepCleanupCandidate lineRemoval;
    lineRemoval.index = 0;
    lineRemoval.state = CandidateState::Safe;
    lineRemoval.action = DeepCleanupAction::RemoveTextLine;
    lineRemoval.filePath = localizationPath;
    lineRemoval.lineNumber = 0;

    DeepCleanupCandidate invalidXmlRemoval;
    invalidXmlRemoval.index = 1;
    invalidXmlRemoval.state = CandidateState::Safe;
    invalidXmlRemoval.action = DeepCleanupAction::RemoveXmlNode;
    invalidXmlRemoval.filePath = xmlPath;
    invalidXmlRemoval.xmlLocation = QStringLiteral("/Catalog[1]/CUnit[99]");
    analysis.deepCleanupCandidates = {lineRemoval, invalidXmlRemoval};

    const DeepCleanupApplyResult applied = DeepCleanupService().apply(analysis, {0, 1}, dir.path(), true);
    QVERIFY(!applied.success);
    QVERIFY(applied.error.contains(QStringLiteral("Unable to locate XML cleanup node")));

    QFile localization(localizationPath);
    QVERIFY(localization.open(QIODevice::ReadOnly));
    QCOMPARE(localization.readAll(), localizationOriginal);
    QFile xml(xmlPath);
    QVERIFY(xml.open(QIODevice::ReadOnly));
    QCOMPARE(xml.readAll(), xmlOriginal);
}

void CoreTests::binaryAssetReferencesProtectImports()
{
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("GameData")));
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("Assets")));
    const QString xmlPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("GameData/ModelData.xml"));
    const QString modelPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("Assets/Model.m3"));
    const QString usedPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("Assets/Used.dds"));
    const QString unusedPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("Assets/Unused.dds"));

    QVERIFY(writeTextFile(xmlPath, QByteArrayLiteral("<Catalog><CModel id=\"Model\" File=\"Assets/Model.m3\"/></Catalog>")));
    QByteArray modelBytes;
    modelBytes.append('\0');
    modelBytes.append("Assets\\Used.dds");
    modelBytes.append('\0');
    QVERIFY(writeTextFile(modelPath, modelBytes));
    QVERIFY(writeTextFile(usedPath, QByteArrayLiteral("used texture")));
    QVERIFY(writeTextFile(unusedPath, QByteArrayLiteral("unused texture")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    bool usedTextureWasOfferedForDeletion = false;
    bool unusedTextureWasOfferedForDeletion = false;
    for (const DeepCleanupCandidate &candidate : analysis.deepCleanupCandidates) {
        if (candidate.kind != DeepCleanupKind::UnusedAsset)
            continue;
        if (candidate.label.endsWith(QStringLiteral("Used.dds")))
            usedTextureWasOfferedForDeletion = true;
        if (candidate.label.endsWith(QStringLiteral("Unused.dds")))
            unusedTextureWasOfferedForDeletion = true;
    }
    QVERIFY(!usedTextureWasOfferedForDeletion);
    QVERIFY(unusedTextureWasOfferedForDeletion);
}

void CoreTests::standaloneModExternalConsumersAreProtected()
{
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("GameData")));
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("Assets")));
    QVERIFY(writeTextFile(QDir(dir.path()).absoluteFilePath(QStringLiteral("GameData/UnitData.xml")),
                          QByteArrayLiteral("<Catalog><CUnit id=\"ExportedUnit\"/></Catalog>")));
    QVERIFY(writeTextFile(QDir(dir.path()).absoluteFilePath(QStringLiteral("Assets/Exported.dds")),
                          QByteArrayLiteral("external texture")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));
    analysis.externalConsumersUnknown = true;
    applyExternalConsumerSafety(&analysis);

    QVERIFY(analysis.possibleUnusedNodeIndices.isEmpty());
    QVERIFY(!analysis.unusedCandidates.isEmpty());
    for (const UnusedCandidateInfo &candidate : analysis.unusedCandidates)
        QVERIFY(candidate.state != CandidateState::Safe);

    bool protectedAsset = false;
    for (const DeepCleanupCandidate &candidate : analysis.deepCleanupCandidates) {
        if (candidate.kind == DeepCleanupKind::UnusedAsset
            && candidate.label.endsWith(QStringLiteral("Exported.dds"))) {
            protectedAsset = true;
            QCOMPARE(candidate.action, DeepCleanupAction::ReportOnly);
            QCOMPARE(candidate.state, CandidateState::Risky);
            QVERIFY(!candidate.recommended);
        }
    }
    QVERIFY(protectedAsset);

    UnitFamily family;
    family.rootId = QStringLiteral("ExportedUnit");
    family.rootNodeIndex = 0;
    family.objects.append({0, UnitFamilyRole::Unit, QStringLiteral("High"), QString()});
    const RenamePlan rename = StandardNamePlanner().plan(analysis, family, QStringLiteral("RenamedUnit"));
    QVERIFY(!rename.items.isEmpty());
    QVERIFY(rename.items.first().blocked);
    QVERIFY(!rename.items.first().selected);

    analysis.externalConsumersUnknown = false;
    const RenamePlan closedProjectRename = StandardNamePlanner().plan(analysis, family, QStringLiteral("RenamedUnit"));
    QVERIFY(closedProjectRename.valid);
    QVERIFY(!closedProjectRename.items.first().blocked);
    QVERIFY(closedProjectRename.items.first().selected);
}

void CoreTests::largeDataCollectionPlansRequireExplicitReview()
{
    UnitFamily smallFamily;
    smallFamily.rootId = QStringLiteral("Small");
    smallFamily.objects.resize(10);
    QVERIFY(assessDataCollectionScale({smallFamily}).automaticBatchAllowed);

    UnitFamily largeFamily;
    largeFamily.rootId = QStringLiteral("Large");
    largeFamily.objects.resize(20001);
    const DataCollectionScaleAssessment assessment = assessDataCollectionScale({largeFamily});
    QVERIFY(!assessment.automaticBatchAllowed);
    QCOMPARE(assessment.totalMemberships, qsizetype(20001));
    QVERIFY(assessment.reason.contains(QStringLiteral("explicit review")));
    QVERIFY(assessDataCollectionScale({largeFamily}, true).automaticBatchAllowed);
}

void CoreTests::deepCleanupRemovesRedundantInheritedXmlNodes()
{
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("GameData")));
    const QString xmlPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("GameData/UnitData.xml"));
    QVERIFY(writeTextFile(xmlPath, QByteArrayLiteral(
        "<Catalog>"
        "<CUnit id=\"Parent\"><Life value=\"100\"/><Flags><Flag value=\"Heroic\"/></Flags></CUnit>"
        "<CUnit id=\"Child\" parent=\"Parent\"><Life value=\"100\"/><Flags><Flag value=\"Heroic\"/></Flags><Cost value=\"50\"/></CUnit>"
        "</Catalog>")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    QVector<int> selected;
    for (const DeepCleanupCandidate &candidate : analysis.deepCleanupCandidates) {
        if (candidate.kind == DeepCleanupKind::RedundantDefaultNode
            && candidate.state == CandidateState::Safe
            && candidate.recommended) {
            selected.append(candidate.index);
        }
    }
    QCOMPARE(selected.size(), 2);

    const DeepCleanupApplyResult applied = DeepCleanupService().apply(analysis, selected, dir.path(), true);
    QVERIFY2(applied.success, qPrintable(applied.error));
    QCOMPARE(applied.xmlNodesRemoved, 2);

    QFile result(xmlPath);
    QVERIFY(result.open(QIODevice::ReadOnly));
    const QByteArray xml = result.readAll();
    QCOMPARE(xml.count("<Life"), 1);
    QCOMPARE(xml.count("<Flags"), 1);
    QVERIFY(xml.contains("<Cost"));
}

void CoreTests::deepCleanupReportsAssetAndTriggerOptimization()
{
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("Assets")));
    const QString assetA = QDir(dir.path()).absoluteFilePath(QStringLiteral("Assets/A.dds"));
    const QString assetB = QDir(dir.path()).absoluteFilePath(QStringLiteral("Assets/B.dds"));
    const QString script = QDir(dir.path()).absoluteFilePath(QStringLiteral("MapScript.galaxy"));
    QVERIFY(writeTextFile(assetA, QByteArrayLiteral("same bytes")));
    QVERIFY(writeTextFile(assetB, QByteArrayLiteral("same bytes")));
    QVERIFY(writeTextFile(script, QByteArrayLiteral(
        "void HotLoop() {\n"
        "    TriggerAddEventTimePeriodic(null, 0.125);\n"
        "    UnitGroupLoopBegin(g);\n"
        "}\n")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    bool assetAudit = false;
    bool triggerAudit = false;
    for (const DeepCleanupCandidate &candidate : analysis.deepCleanupCandidates) {
        if (candidate.kind == DeepCleanupKind::AssetAudit) {
            assetAudit = true;
            QCOMPARE(candidate.action, DeepCleanupAction::ReportOnly);
            QCOMPARE(candidate.state, CandidateState::Risky);
        }
        if (candidate.kind == DeepCleanupKind::TriggerPerformance) {
            triggerAudit = true;
            QCOMPARE(candidate.action, DeepCleanupAction::ReportOnly);
            QCOMPARE(candidate.state, CandidateState::Risky);
        }
    }
    QVERIFY(assetAudit);
    QVERIFY(triggerAudit);
}

void CoreTests::dependencyUsageReportExportsRealUsagePaths()
{
    AnalysisResult analysis;
    analysis.rootFolder = QStringLiteral("C:/Maps/TestMap.SC2Components");
    analysis.externalConsumersUnknown = true;
    analysis.scannedFiles = {
        {QStringLiteral("C:/Maps/TestMap.SC2Components/DocumentInfo"), false, false, 128},
        {QStringLiteral("C:/Maps/TestMap.SC2Components/Mods/Void.SC2Mod/Assets/Textures/Dep.dds"), false, false, 64}
    };

    DataNode localUnit;
    localUnit.sourceFile = QStringLiteral("GameData/UnitData.xml");
    localUnit.elementName = QStringLiteral("CUnit");
    localUnit.id = QStringLiteral("LocalUnit");
    localUnit.referencedIds = {QStringLiteral("DepWeapon"), QStringLiteral("MissingExternal")};

    DataNode localEffect;
    localEffect.sourceFile = QStringLiteral("GameData/EffectData.xml");
    localEffect.elementName = QStringLiteral("CEffectDamage");
    localEffect.id = QStringLiteral("LocalEffect");
    localEffect.referencedIds = {QStringLiteral("DepBehavior")};

    DataNode dependencyWeapon;
    dependencyWeapon.sourceFile = QStringLiteral("Mods/Void.SC2Mod/Base.SC2Data/GameData/WeaponData.xml");
    dependencyWeapon.elementName = QStringLiteral("CWeaponLegacy");
    dependencyWeapon.id = QStringLiteral("DepWeapon");

    DataNode dependencyBehavior;
    dependencyBehavior.sourceFile = QStringLiteral("Mods/Void.SC2Mod/Base.SC2Data/GameData/BehaviorData.xml");
    dependencyBehavior.elementName = QStringLiteral("CBehaviorBuff");
    dependencyBehavior.id = QStringLiteral("DepBehavior");
    dependencyBehavior.referencedIds = {QStringLiteral("DepValidator")};

    DataNode dependencyValidator;
    dependencyValidator.sourceFile = QStringLiteral("Mods/Void.SC2Mod/Base.SC2Data/GameData/ValidatorData.xml");
    dependencyValidator.elementName = QStringLiteral("CValidatorUnitCompare");
    dependencyValidator.id = QStringLiteral("DepValidator");

    DataNode unusedDependencyRequirement;
    unusedDependencyRequirement.sourceFile = QStringLiteral("Mods/Void.SC2Mod/Base.SC2Data/GameData/RequirementData.xml");
    unusedDependencyRequirement.elementName = QStringLiteral("CRequirement");
    unusedDependencyRequirement.id = QStringLiteral("UnusedRequirement");

    analysis.nodes = {localUnit, localEffect, dependencyWeapon, dependencyBehavior,
                      dependencyValidator, unusedDependencyRequirement};

    const sc2dh::DependencyUsageReportBuilder builder;
    const sc2dh::DependencyUsageReport report = builder.build(analysis);
    QCOMPARE(report.dependencies.size(), 1);
    const sc2dh::DependencyUsageEntry &entry = report.dependencies.front();
    QCOMPARE(entry.path, QStringLiteral("Mods/Void.SC2Mod"));
    QCOMPARE(entry.usedObjectsByType.value(QStringLiteral("CWeaponLegacy")), 1);
    QCOMPARE(entry.usedObjectsByType.value(QStringLiteral("CBehaviorBuff")), 1);
    QCOMPARE(entry.usedObjectsByType.value(QStringLiteral("CValidatorUnitCompare")), 1);
    QCOMPARE(entry.usedObjectsByType.value(QStringLiteral("CRequirement")), 0);
    QCOMPARE(entry.availableObjectsByType.value(QStringLiteral("CRequirement")), 1);
    QVERIFY(entry.directLocalUsers.join(QStringLiteral("\n")).contains(QStringLiteral("CUnit(LocalUnit) -> CWeaponLegacy(DepWeapon)")));
    QVERIFY(entry.usageChains.join(QStringLiteral("\n")).contains(QStringLiteral("CEffectDamage(LocalEffect) -> CBehaviorBuff(DepBehavior)")));
    QVERIFY(entry.usageChains.join(QStringLiteral("\n")).contains(QStringLiteral("CEffectDamage(LocalEffect) -> CBehaviorBuff(DepBehavior) -> CValidatorUnitCompare(DepValidator)")));
    QVERIFY(entry.unresolvedExternalIds.contains(QStringLiteral("MissingExternal")));
    QVERIFY(entry.possibleImportFiles.join(QStringLiteral("\n")).contains(QStringLiteral("Mods/Void.SC2Mod/Assets/Textures/Dep.dds")));
    QVERIFY(report.unknownProvenanceIds.contains(QStringLiteral("MissingExternal")));

    const QJsonObject json = builder.toJson(report);
    const QJsonArray dependencies = json.value(QStringLiteral("dependencies")).toArray();
    QCOMPARE(dependencies.size(), 1);
    const QJsonObject dependency = dependencies.first().toObject();
    QCOMPARE(dependency.value(QStringLiteral("path")).toString(), QStringLiteral("Mods/Void.SC2Mod"));
    QCOMPARE(dependency.value(QStringLiteral("usedObjectsByType")).toObject().value(QStringLiteral("CRequirement")).toInt(), 0);
    QCOMPARE(dependency.value(QStringLiteral("availableObjectsByType")).toObject().value(QStringLiteral("CRequirement")).toInt(), 1);
    bool jsonHasLocalUnit = false;
    for (const QJsonValue &value : dependency.value(QStringLiteral("directLocalUsers")).toArray())
        jsonHasLocalUnit = jsonHasLocalUnit || value.toString().contains(QStringLiteral("LocalUnit"));
    QVERIFY(jsonHasLocalUnit);
    QVERIFY(dependency.value(QStringLiteral("unresolvedExternalIds")).toArray().contains(QStringLiteral("MissingExternal")));

    const QString text = builder.toText(report);
    QVERIFY(text.contains(QStringLiteral("Dependency Usage Report")));
    QVERIFY(text.contains(QStringLiteral("Mods/Void.SC2Mod")));
    QVERIFY(text.contains(QStringLiteral("Unknown provenance IDs: MissingExternal")));
    QVERIFY(text.contains(QStringLiteral("Dependency cleanup is report-only")));

    QTemporaryDir exportDir;
    QVERIFY(exportDir.isValid());
    QString error;
    const QString jsonPath = QDir(exportDir.path()).absoluteFilePath(QStringLiteral("dependency_report.json"));
    const QString textPath = QDir(exportDir.path()).absoluteFilePath(QStringLiteral("dependency_report.txt"));
    QVERIFY2(builder.writeJson(jsonPath, report, &error), qPrintable(error));
    QVERIFY2(builder.writeText(textPath, report, &error), qPrintable(error));
    QFile jsonFile(jsonPath);
    QVERIFY(jsonFile.open(QIODevice::ReadOnly));
    QVERIFY(QString::fromUtf8(jsonFile.readAll()).contains(QStringLiteral("\"dependencies\"")));
    QFile textFile(textPath);
    QVERIFY(textFile.open(QIODevice::ReadOnly));
    QVERIFY(QString::fromUtf8(textFile.readAll()).contains(QStringLiteral("Dependency Usage Report")));
}

void CoreTests::mapPerformanceAnalyzerBuildsEstimatedStaticRiskHeatmap()
{
    const QByteArray objects = QByteArrayLiteral(
        "ObjectDoodad { Id = 71 Name = \"LeftTree\" Type = \"TreeVisual\" Position = (10, 10, 0) }\n"
        "ObjectDoodad { Id = 72 Name = \"LeftRock\" Type = \"RockVisual\" Position = (20, 10, 0) }\n"
        "ObjectUnit { Id = 73 Name = \"RightMarine\" Type = \"Marine\" Position = (110, 10, 0) }\n"
        "ObjectDestructible { Id = 74 Name = \"RightDebris\" Type = \"DebrisDestructible\" Position = (120, 10, 0) }\n");

    AnalysisResult analysis;
    analysis.rootFolder = QStringLiteral("C:/Maps/Perf.SC2Components");
    analysis.scannedFiles = {
        {QStringLiteral("Assets/TreeVisual.m3"), false, false, 2 * 1024 * 1024},
        {QStringLiteral("scripts/Perf.galaxy"), false, false, 512}
    };

    DataNode tree;
    tree.sourceFile = QStringLiteral("GameData/ActorData.xml");
    tree.elementName = QStringLiteral("CActorModel");
    tree.id = QStringLiteral("TreeVisual");
    tree.referencedIds = {QStringLiteral("TreeModel")};

    DataNode treeModel;
    treeModel.sourceFile = QStringLiteral("GameData/ModelData.xml");
    treeModel.elementName = QStringLiteral("CModel");
    treeModel.id = QStringLiteral("TreeModel");

    DataNode marine;
    marine.sourceFile = QStringLiteral("GameData/UnitData.xml");
    marine.elementName = QStringLiteral("CUnit");
    marine.id = QStringLiteral("Marine");
    marine.referencedIds = {QStringLiteral("MarineActor")};

    DataNode marineActor;
    marineActor.sourceFile = QStringLiteral("GameData/ActorData.xml");
    marineActor.elementName = QStringLiteral("CActorUnit");
    marineActor.id = QStringLiteral("MarineActor");

    analysis.nodes = {tree, treeModel, marine, marineActor};

    DeepCleanupCandidate periodic;
    periodic.kind = DeepCleanupKind::TriggerPerformance;
    periodic.filePath = QStringLiteral("scripts/Perf.galaxy");
    periodic.lineNumber = 12;
    periodic.reason = QStringLiteral("Periodic timer below 0.5s in trigger/Galaxy code.");
    DeepCleanupCandidate scan;
    scan.kind = DeepCleanupKind::TriggerPerformance;
    scan.filePath = QStringLiteral("scripts/Perf.galaxy");
    scan.lineNumber = 20;
    scan.reason = QStringLiteral("Unit-group or region scan in trigger/Galaxy code.");
    analysis.deepCleanupCandidates = {periodic, scan};

    sc2dh::perf::MapPerformanceOptions options;
    options.columns = 2;
    options.rows = 1;
    const sc2dh::perf::MapPerformanceReport report =
        sc2dh::perf::MapPerformanceAnalyzer().buildReport(objects, analysis, options);

    QCOMPARE(report.cells.size(), 2);
    const sc2dh::perf::MapPerformanceCell &left = report.cells.at(0);
    const sc2dh::perf::MapPerformanceCell &right = report.cells.at(1);
    QCOMPARE(left.doodadCount, 2);
    QCOMPARE(left.unitCount, 0);
    QVERIFY(left.uniqueActorModelCount >= 1);
    QVERIFY(left.linkedAssetBytes >= qint64(2 * 1024 * 1024));
    QVERIFY(left.reasons.join(QStringLiteral("\n")).contains(QStringLiteral("Doodad Density")));
    QVERIFY(left.reasons.join(QStringLiteral("\n")).contains(QStringLiteral("Trigger CPU Risk")));
    QVERIFY(left.relatedObjects.join(QStringLiteral("\n")).contains(QStringLiteral("LeftTree")));
    QVERIFY(left.relatedFiles.join(QStringLiteral("\n")).contains(QStringLiteral("TreeVisual.m3")));
    QVERIFY(left.triggerLines.join(QStringLiteral("\n")).contains(QStringLiteral("scripts/Perf.galaxy:12")));
    QCOMPARE(left.scoreLabel, QStringLiteral("Estimated Static Risk"));
    QVERIFY(!left.scoreLabel.contains(QStringLiteral("FPS"), Qt::CaseInsensitive));
    QVERIFY(left.combinedRiskScore > 0.0);

    QCOMPARE(right.doodadCount, 0);
    QCOMPARE(right.unitCount, 2);
    QCOMPARE(right.destructibleCount, 1);
    QVERIFY(right.uniqueActorModelCount >= 1);
    QVERIFY(right.reasons.join(QStringLiteral("\n")).contains(QStringLiteral("Units")));
    QVERIFY(right.relatedObjects.join(QStringLiteral("\n")).contains(QStringLiteral("RightMarine")));
    QCOMPARE(right.periodicTimerCount, 1);
    QCOMPARE(right.unitGroupScanCount, 1);
}

void CoreTests::readsRealSc2RegionXmlAndPreservesGeometry()
{
    const QByteArray regions = QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<Regions>"
        "<region id=\"17\"><name value=\"Spawn\"/><invisible/>"
        "<shape type=\"circle\"><center value=\"10,20\"/><radius value=\"5\"/></shape></region>"
        "<region id=\"42\"><name value=\"Spawn\"/>"
        "<shape type=\"rect\"><quad value=\"30,40,50,70\"/></shape></region>"
        "<region id=\"77\"><name value=\"Boss Floors\"/>"
        "<shape type=\"rect\"><quad value=\"0,0,10,10\"/></shape>"
        "<shape type=\"rect\"><quad value=\"30,0,40,10\"/></shape></region>"
        "<region id=\"88\"><name value=\"Ramp\"/>"
        "<shape type=\"diamond\"><center value=\"100,200\"/><width value=\"8\"/><height value=\"4\"/></shape></region>"
        "</Regions>");

    const sc2dh::region::RegionReadResult parsed = sc2dh::region::MapRegionRepository().parse(regions);
    QVERIFY2(parsed.success, qPrintable(parsed.errors.join(QStringLiteral("; "))));
    QVERIFY2(parsed.complete, qPrintable(parsed.warnings.join(QStringLiteral("; "))));
    QCOMPARE(parsed.regions.size(), 4);
    QCOMPARE(parsed.regions.at(0).id, QStringLiteral("17"));
    QCOMPARE(parsed.regions.at(0).name, QStringLiteral("Spawn [Region #17]"));
    QCOMPARE(parsed.regions.at(0).geometry.kind, sc2dh::region::RegionShapeKind::Circle);
    QCOMPARE(parsed.regions.at(0).geometry.classify(10.0, 20.0), sc2dh::region::SpatialRelation::Inside);
    QCOMPARE(parsed.regions.at(0).geometry.classify(15.0, 20.0), sc2dh::region::SpatialRelation::Boundary);
    QCOMPARE(parsed.regions.at(0).geometry.classify(16.0, 20.0), sc2dh::region::SpatialRelation::Outside);
    QVERIFY(parsed.regions.at(0).markers.contains(QStringLiteral("invisible")));
    QCOMPARE(parsed.regions.at(1).name, QStringLiteral("Spawn [Region #42]"));
    QCOMPARE(parsed.regions.at(1).geometry.kind, sc2dh::region::RegionShapeKind::Rectangle);
    QCOMPARE(parsed.regions.at(1).geometry.bounds.xMin, 30.0);
    QCOMPARE(parsed.regions.at(1).geometry.bounds.yMax, 70.0);
    QCOMPARE(parsed.regions.at(2).geometry.kind, sc2dh::region::RegionShapeKind::Composite);
    QCOMPARE(parsed.regions.at(2).geometry.components.size(), 2);
    QCOMPARE(parsed.regions.at(2).geometry.classify(5.0, 5.0), sc2dh::region::SpatialRelation::Inside);
    QCOMPARE(parsed.regions.at(2).geometry.classify(35.0, 5.0), sc2dh::region::SpatialRelation::Inside);
    QCOMPARE(parsed.regions.at(2).geometry.classify(20.0, 5.0), sc2dh::region::SpatialRelation::Outside);
    const auto &diamond = parsed.regions.at(3).geometry;
    QCOMPARE(diamond.kind, sc2dh::region::RegionShapeKind::Polygon);
    QCOMPARE(diamond.rawType, QStringLiteral("diamond"));
    QCOMPARE(diamond.points.size(), 4);
    QCOMPARE(diamond.bounds.xMin, 96.0);
    QCOMPARE(diamond.bounds.xMax, 104.0);
    QCOMPARE(diamond.bounds.yMin, 198.0);
    QCOMPARE(diamond.bounds.yMax, 202.0);
    QCOMPARE(diamond.classify(100.0, 200.0), sc2dh::region::SpatialRelation::Inside);
    QCOMPARE(diamond.classify(104.0, 200.0), sc2dh::region::SpatialRelation::Boundary);
    QCOMPARE(diamond.classify(103.0, 201.0), sc2dh::region::SpatialRelation::Outside);
}

void CoreTests::realMapRegionReader()
{
    const QStringList candidates{
        QStringLiteral("C:/Program Files (x86)/StarCraft II/Maps/Кампания_Империя_KSP_Миссия_1_OPRIMIzATION.SC2Map"),
        QStringLiteral("E:/SK2/SC1ToSC2Converter/local-data/bank-debug/input/NydusConspiracy-current.SC2Map"),
        QStringLiteral("E:/SK2/SC1ToSC2Converter/out/StealTheBeacon_R666.SC2Map"),
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../../../logs/DecorSampleSource.SC2Map"))
    };
    for (const QString &candidate : candidates) {
        if (!QFileInfo::exists(candidate))
            continue;

        const QString mapPath = QFileInfo(candidate).absoluteFilePath();
        Sc2Archive archive;
        QString error;
        if (!archive.load(mapPath, &error))
            continue;

        QString regionsEntry;
        QString objectsEntry;
        for (const QString &entry : archive.allEntries()) {
            const QString normalized = QDir::cleanPath(entry).replace('\\', '/');
            if (normalized.compare(QStringLiteral("Regions"), Qt::CaseInsensitive) == 0) {
                regionsEntry = entry;
            } else if (normalized.compare(QStringLiteral("Objects"), Qt::CaseInsensitive) == 0) {
                objectsEntry = entry;
            }
        }

        // A real map without this component is not a failed region parser test;
        // continue looking for the next available real map fixture.
        if (regionsEntry.isEmpty())
            continue;

        QByteArray bytes;
        QVERIFY2(archive.readEntry(regionsEntry, &bytes, &error), qPrintable(error));
        const auto parsed = sc2dh::region::MapRegionRepository().parse(bytes, mapPath + QStringLiteral("::") + regionsEntry);
        QVERIFY2(parsed.success, qPrintable(parsed.errors.join(QStringLiteral("; "))));
        QVERIFY2(parsed.complete, qPrintable(parsed.warnings.join(QStringLiteral("; "))));
        QVERIFY(!parsed.regions.isEmpty());
        for (const auto &region : parsed.regions) {
            QVERIFY(!region.id.isEmpty());
            QVERIFY(!region.name.isEmpty());
            QVERIFY(region.geometry.supported);
            QVERIFY(region.geometry.bounds.valid);
        }
        if (!objectsEntry.isEmpty()) {
            QByteArray objectsBytes;
            QVERIFY2(archive.readEntry(objectsEntry, &objectsBytes, &error), qPrintable(error));
            QStringList objectWarnings;
            const QVector<sc2dh::decor::DoodadPlacement> doodads =
                sc2dh::decor::DecorationStreamingPlanner().parseObjects(objectsBytes, &objectWarnings);
            QVERIFY2(!doodads.isEmpty(), qPrintable(objectWarnings.join(QStringLiteral("; "))));
        }
        return;
    }

    QSKIP("No locally available real SC2Map fixture contains a Regions component.");
}

void CoreTests::malformedOrUnsupportedRegionIsFailClosed()
{
    const auto malformed = sc2dh::region::MapRegionRepository().parse(
        QByteArrayLiteral("<Regions><region id=\"1\"></Regions>"));
    QVERIFY(!malformed.success);
    QVERIFY(!malformed.complete);
    QVERIFY(!malformed.errors.isEmpty());

    const auto unsupported = sc2dh::region::MapRegionRepository().parse(QByteArrayLiteral(
        "<Regions><region id=\"9\"><name value=\"Spline\"/>"
        "<shape type=\"bezier\"><control value=\"1,2,3,4\"/></shape></region></Regions>"));
    QVERIFY(unsupported.success);
    QVERIFY(!unsupported.complete);
    QCOMPARE(unsupported.regions.size(), 1);
    QVERIFY(!unsupported.regions.front().geometry.supported);
    QVERIFY(!unsupported.warnings.isEmpty());
}

void CoreTests::mapPreviewParsesTerrainAndRejectsMalformedHeightData()
{
    sc2dh::preview::MapPreviewDataReader reader;
    const auto descriptor = reader.parseTerrainXml(QByteArrayLiteral(
        "<terrain><heightMap dim=\"3 2\" offset=\"10 20 0\" scale=\"2 4 1\">"
        "<vertData quantizeBias=\"-1\" quantizeScale=\"0.25\"/>"
        "</heightMap></terrain>"));
    QVERIFY2(descriptor.complete, qPrintable(descriptor.errors.join(QStringLiteral("; "))));
    QCOMPARE(descriptor.gridWidth, 3);
    QCOMPARE(descriptor.gridHeight, 2);
    QCOMPARE(descriptor.worldBounds.xMin, 10.0);
    QCOMPARE(descriptor.worldBounds.yMin, 20.0);
    QCOMPARE(descriptor.worldBounds.xMax, 14.0);
    QCOMPARE(descriptor.worldBounds.yMax, 24.0);

    const auto appendU32 = [](QByteArray *bytes, quint32 value) {
        const quint32 little = qToLittleEndian(value);
        bytes->append(reinterpret_cast<const char *>(&little), sizeof(little));
    };
    const auto appendU16 = [](QByteArray *bytes, quint16 value) {
        const quint16 little = qToLittleEndian(value);
        bytes->append(reinterpret_cast<const char *>(&little), sizeof(little));
    };
    QByteArray heightMap;
    appendU32(&heightMap, 0x50414D48u); // HMAP
    appendU32(&heightMap, 101u);
    appendU32(&heightMap, 3u);
    appendU32(&heightMap, 2u);
    heightMap.append(16, '\0');
    for (quint16 value = 0; value < 6; ++value) {
        appendU16(&heightMap, quint16(100 + value));
        appendU16(&heightMap, quint16(10 + value));
        appendU16(&heightMap, value == 2 ? quint16(0x9000) : quint16(0));
    }
    QStringList warnings;
    const QImage rendered = reader.renderHeightMap(heightMap, descriptor, &warnings);
    QVERIFY2(!rendered.isNull(), qPrintable(warnings.join(QStringLiteral("; "))));
    QCOMPARE(rendered.size(), QSize(3, 2));

    const QByteArray truncated = heightMap.left(heightMap.size() - 1);
    warnings.clear();
    QVERIFY(reader.renderHeightMap(truncated, descriptor, &warnings).isNull());
    QVERIFY(!warnings.isEmpty());

    QByteArray mapInfo;
    appendU32(&mapInfo, 0x4D617049u); // IpaM
    appendU32(&mapInfo, 27u);
    appendU32(&mapInfo, 192u);
    appendU32(&mapInfo, 128u);
    const auto approximateBounds = reader.parseMapInfoDimensions(mapInfo, &warnings);
    QVERIFY(approximateBounds.valid);
    QCOMPARE(approximateBounds.xMax, 192.0);
    QCOMPARE(approximateBounds.yMax, 128.0);
}

void CoreTests::maximumCompressionFailurePathsPreserveSourceAndOutput()
{
#ifndef SC2DH_USE_STORMLIB
    QSKIP("StormLib archive writer is unavailable.");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sourcePath = QDir(dir.path()).absoluteFilePath(QStringLiteral("CompressionSource.SC2Map"));
    QString error;
    QVERIFY2(createTestMpqArchive(sourcePath,
                                  {{QStringLiteral("MapScript.galaxy"), QByteArrayLiteral("void InitMap() {}\n")},
                                   {QStringLiteral("Objects"), QByteArray(8192, 'A')}},
                                  &error),
             qPrintable(error));
    QFile sourceFile(sourcePath);
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = sourceFile.readAll();
    sourceFile.close();

    sc2dh::compression::ArchiveCompressionService service;
    const QString existingOutput = QDir(dir.path()).absoluteFilePath(QStringLiteral("Existing.SC2Map"));
    QVERIFY(writeTextFile(existingOutput, QByteArrayLiteral("do not replace")));
    sc2dh::compression::ArchiveCompressionRequest existingRequest{sourcePath, existingOutput};
    const auto existingResult = service.compressCompatibleCopy(existingRequest);
    QVERIFY(!existingResult.success);
    QFile existingFile(existingOutput);
    QVERIFY(existingFile.open(QIODevice::ReadOnly));
    QCOMPARE(existingFile.readAll(), QByteArrayLiteral("do not replace"));

    const QString noSpaceOutput = QDir(dir.path()).absoluteFilePath(QStringLiteral("NoSpace.SC2Map"));
    sc2dh::compression::ArchiveCompressionRequest noSpaceRequest{sourcePath, noSpaceOutput};
    noSpaceRequest.availableBytesOverride = 0;
    const auto noSpaceResult = service.compressCompatibleCopy(noSpaceRequest);
    QCOMPARE(noSpaceResult.status, QStringLiteral("BLOCKED_INSUFFICIENT_SPACE"));
    QVERIFY(!QFileInfo::exists(noSpaceOutput));

    const QString cancelledOutput = QDir(dir.path()).absoluteFilePath(QStringLiteral("Cancelled.SC2Map"));
    sc2dh::compression::ArchiveCompressionRequest cancelledRequest{sourcePath, cancelledOutput};
    cancelledRequest.availableBytesOverride = std::numeric_limits<qint64>::max();
    cancelledRequest.isCancelled = [] { return true; };
    const auto cancelledResult = service.compressCompatibleCopy(cancelledRequest);
    QCOMPARE(cancelledResult.status, QStringLiteral("CANCELLED"));
    QVERIFY(!QFileInfo::exists(cancelledOutput));

    const QString verifyCancelledOutput = QDir(dir.path()).absoluteFilePath(QStringLiteral("VerifyCancelled.SC2Map"));
    int cancellationChecks = 0;
    sc2dh::compression::ArchiveCompressionRequest verifyCancelledRequest{sourcePath, verifyCancelledOutput};
    verifyCancelledRequest.availableBytesOverride = std::numeric_limits<qint64>::max();
    verifyCancelledRequest.isCancelled = [&cancellationChecks] { return ++cancellationChecks >= 3; };
    const auto verifyCancelledResult = service.compressCompatibleCopy(verifyCancelledRequest);
    QVERIFY2(verifyCancelledResult.status == QStringLiteral("CANCELLED"),
             qPrintable(verifyCancelledResult.status + QStringLiteral(": ") + verifyCancelledResult.error));
    QVERIFY(!QFileInfo::exists(verifyCancelledOutput));

    const QString corruptPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("Corrupt.SC2Map"));
    QVERIFY(writeTextFile(corruptPath, QByteArrayLiteral("not an MPQ archive")));
    const QString corruptOutput = QDir(dir.path()).absoluteFilePath(QStringLiteral("CorruptOutput.SC2Map"));
    sc2dh::compression::ArchiveCompressionRequest corruptRequest{corruptPath, corruptOutput};
    corruptRequest.availableBytesOverride = std::numeric_limits<qint64>::max();
    const auto corruptResult = service.compressCompatibleCopy(corruptRequest);
    QVERIFY(!corruptResult.success);
    QVERIFY(!QFileInfo::exists(corruptOutput));

    const QString compactedOutput = QDir(dir.path()).absoluteFilePath(QStringLiteral("Compacted.SC2Map"));
    sc2dh::compression::ArchiveCompressionRequest compactRequest{sourcePath, compactedOutput};
    compactRequest.availableBytesOverride = std::numeric_limits<qint64>::max();
    const auto compactResult = service.compressCompatibleCopy(compactRequest);
    QVERIFY2(compactResult.success, qPrintable(compactResult.error));
    QVERIFY(compactResult.sourceUnchanged);
    QVERIFY(compactResult.logicalEntryEquality);
    QVERIFY(compactResult.structuralVerification);
    if (QFileInfo::exists(compactedOutput))
        QVERIFY(QFileInfo(compactedOutput).size() < QFileInfo(sourcePath).size());
    else
        QCOMPARE(compactResult.status, QStringLiteral("NO_COMPATIBLE_SIZE_GAIN"));

    QFile sourceAfter(sourcePath);
    QVERIFY(sourceAfter.open(QIODevice::ReadOnly));
    QCOMPARE(sourceAfter.readAll(), originalBytes);
    const QStringList remainingFiles = QDir(dir.path()).entryList(QDir::Files);
    for (const QString &name : remainingFiles) {
        QVERIFY2(!name.contains(QStringLiteral(".compress-stage-"))
                     && !name.contains(QStringLiteral(".sc2dh-stage-"))
                     && !name.endsWith(QStringLiteral(".compact")),
                 qPrintable(QStringLiteral("Temporary archive helper was not cleaned: %1").arg(name)));
    }
#endif
}

void CoreTests::decorationXmlRoundTripAndRegionScopeIsolation()
{
    const QByteArray objects = QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        "<PlacedObjects Version=\"27\">\r\n"
        "    <ObjectDoodad Id=\"100\" Variation=\"3\" Position=\"10,10,8\" Rotation=\"1.570796\" Pitch=\"0.25\" Roll=\"-0.5\" Scale=\"1,2,3\" TintColor=\"255,128,0 1.5\" TeamColor=\"0\" Type=\"TreeVisual\"><Flag Index=\"HeightAbsolute\" Value=\"1\"/></ObjectDoodad>\r\n"
        "    <ObjectDoodad Id=\"200\" Position=\"100,100,0\" Rotation=\"0\" Scale=\"1,1,1\" Type=\"RockVisual\"/>\r\n"
        "    <ObjectDoodad Id=\"300\" Position=\"15,10,0\" Rotation=\"0\" Scale=\"1,1,1\" Type=\"EdgeVisual\"/>\r\n"
        "</PlacedObjects>\r\n");
    const QByteArray outsideRaw = QByteArrayLiteral(
        "<ObjectDoodad Id=\"200\" Position=\"100,100,0\" Rotation=\"0\" Scale=\"1,1,1\" Type=\"RockVisual\"/>");

    sc2dh::decor::DecorationStreamingPlanner planner;
    const QVector<sc2dh::decor::DoodadPlacement> parsed = planner.parseObjects(objects);
    QCOMPARE(parsed.size(), 3);
    QCOMPARE(parsed.front().pitch, 0.25);
    QCOMPARE(parsed.front().roll, -0.5);
    QCOMPARE(parsed.front().z, 8.0);
    QCOMPARE(parsed.front().placementFlags.value(QStringLiteral("HeightAbsolute")), QStringLiteral("1"));
    QVERIFY(parsed.front().dynamicCandidate);
    QString roundTripError;
    QVERIFY2(planner.verifyPlacementRoundTrip(parsed.front(), &roundTripError), qPrintable(roundTripError));

    sc2dh::decor::DecorZone zone;
    zone.id = 1;
    zone.name = QStringLiteral("Real Region");
    zone.xMin = 5.0;
    zone.yMin = 5.0;
    zone.xMax = 15.0;
    zone.yMax = 15.0;
    zone.geometry.kind = sc2dh::region::RegionShapeKind::Circle;
    zone.geometry.center = {10.0, 10.0};
    zone.geometry.radius = 5.0;
    zone.geometry.bounds = {5.0, 5.0, 15.0, 15.0, true};
    zone.geometry.supported = true;

    const sc2dh::decor::DecorationOptimizedArtifacts artifacts =
        planner.createOptimizedArtifacts(objects, {zone});
    QVERIFY2(artifacts.valid, qPrintable(artifacts.warnings.join(QStringLiteral("; "))));
    QVERIFY(artifacts.roundTripVerified);
    QVERIFY(artifacts.outsideScopePreserved);
    QCOMPARE(artifacts.removedDoodadIndices, QVector<int>{0});
    QVERIFY(!artifacts.optimizedObjectsBytes.contains("Id=\"100\""));
    QVERIFY(artifacts.optimizedObjectsBytes.contains(outsideRaw));
    QVERIFY(artifacts.optimizedObjectsBytes.contains("Id=\"300\""));
    QCOMPARE(artifacts.plan.boundaryDoodads, QVector<int>{2});
    QVERIFY(artifacts.galaxySource.contains(QStringLiteral(
        "DecorOpt_CreateActor(\"TreeVisual\", 10.0, 10.0, 8.0, 1.0, 2.0, 3.0)")));
    QVERIFY(artifacts.galaxySource.contains(QStringLiteral("libNtve_gf_SetPosition(x, y, z)")));
    QVERIFY(artifacts.galaxySource.contains(QStringLiteral("libNtve_gf_SetScale(scaleX, scaleY, scaleZ, 0.0)")));
    QVERIFY(artifacts.galaxySource.contains(QStringLiteral("libNtve_gf_SetRotation(")));
    QVERIFY(artifacts.galaxySource.contains(QStringLiteral("libNtve_gf_SetTintColor(Color(100.0, 50.196078, 0.0), 1.5, 0.0)")));
    QVERIFY(!artifacts.galaxySource.contains(QStringLiteral("SetVisibility")));
}

void CoreTests::decorationStreamingParsesZonesAndGeneratesGalaxy()
{
    const QByteArray objects = QByteArrayLiteral(
        "ObjectDoodad { Id = 1 Name = \"CrateA\" Type = \"CrateDoodad\" Position = (10, 20, 0) Rotation = 1.570796 Scale = 1.25 }\n"
        "ObjectDoodad { Id = 2 Name = \"CrateB\" Type = \"CrateDoodad\" Position = (110, 20, 0) Scale = (1, 2, 1) }\n"
        "ObjectDoodad { Id = 3 Name = \"PathBlock\" Type = \"PathingBlocker\" Position = (15, 25, 0) }\n"
        "ObjectDoodad { Id = 4 Name = \"Raised\" Type = \"RaisedVisual\" Position = (20, 25, 2) }\n");

    sc2dh::decor::DecorationStreamingPlanner planner;
    const QVector<sc2dh::decor::DecorZone> zones = {
        {1, QStringLiteral("Left"), 0.0, 0.0, 50.0, 50.0},
        {2, QStringLiteral("Right"), 100.0, 0.0, 150.0, 50.0}
    };
    QStringList warnings;
    const sc2dh::decor::DecorationStreamingPlan plan = planner.buildPlan(objects, zones, &warnings);
    QVERIFY(warnings.isEmpty());
    QCOMPARE(plan.doodads.size(), 4);
    QCOMPARE(plan.zones.size(), 2);
    QCOMPARE(plan.zones.at(0).doodadIndices.size(), 2);
    QCOMPARE(plan.zones.at(1).doodadIndices.size(), 1);
    QCOMPARE(plan.staticOnlyDoodads.size(), 1);
    QVERIFY(std::any_of(plan.staticOnlyDoodads.cbegin(), plan.staticOnlyDoodads.cend(), [&plan](int index) {
        return plan.doodads.at(index).staticOnlyReason.contains(QStringLiteral("pathing/gameplay"));
    }));

    sc2dh::decor::GalaxyGenerationOptions options;
    options.functionPrefix = QStringLiteral("NAME_OUT_FUNK");
    options.batchLimit = 8;
    const QString galaxy = planner.generateGalaxy(plan, options);
    QVERIFY(galaxy.contains(QStringLiteral("void DecorOpt_Init()")));
    QVERIFY(galaxy.contains(QStringLiteral("void DecorOpt_CreateZone(int zoneId)")));
    QVERIFY(galaxy.contains(QStringLiteral("void DecorOpt_ClearZone(int zoneId)")));
    QVERIFY(galaxy.contains(QStringLiteral("bool DecorOpt_IsZoneLoaded(int zoneId)")));
    QVERIFY(galaxy.contains(QStringLiteral("void NAME_OUT_FUNK_1()")));
    QVERIFY(galaxy.contains(QStringLiteral("void NAME_OUT_FUNK_2()")));
    QVERIFY(galaxy.contains(QStringLiteral("if (DecorOpt_Loaded[1]) { return; }")));
    QVERIFY(galaxy.contains(QStringLiteral("libNtve_gf_CreateActorAtPoint(actorType, p)")));
    QVERIFY(galaxy.contains(QStringLiteral(
        "DecorOpt_CreateActor(\"CrateDoodad\", 10.0, 20.0, 0.0, 1.25, 1.25, 1.25)")));
    QVERIFY(galaxy.contains(QStringLiteral("libNtve_gf_SetRotation(0.0, 1.0, 0.0, 0.0, 0.0, 1.0)")));
    QVERIFY(galaxy.contains(QStringLiteral(
        "DecorOpt_CreateActor(\"RaisedVisual\", 20.0, 25.0, 2.0")));
    QVERIFY(galaxy.contains(QStringLiteral("void NAME_OUT_FUNK_Create_1()")));
    QVERIFY(galaxy.contains(QStringLiteral("void NAME_OUT_FUNK_Clear_1()")));
    QVERIFY(galaxy.contains(QStringLiteral("void NAME_OUT_FUNK_CreateAll()")));
    QStringList galaxyErrors;
    QVERIFY2(planner.validateGeneratedGalaxy(galaxy, &galaxyErrors), qPrintable(galaxyErrors.join(QStringLiteral("; "))));
    QVERIFY(!galaxy.contains(QStringLiteral("PathingBlocker")));
    QVERIFY(galaxy.contains(QStringLiteral("RaisedVisual")));
    QVERIFY(!galaxy.contains(QStringLiteral("SetVisibility")));
}

void CoreTests::decorationStreamingKeepsExternallyReferencedDoodadsStatic()
{
    const QByteArray objects = QByteArrayLiteral(
        "ObjectDoodad { Id = 41 Name = \"ReferencedVisual\" Type = \"TreeVisual\" Position = (10, 10, 0) }\n"
        "ObjectDoodad { Id = 42 Name = \"FreeVisual\" Type = \"TreeVisual\" Position = (20, 10, 0) }\n");
    const QVector<sc2dh::decor::DecorZone> zones = {
        {1, QStringLiteral("ZoneA"), 0.0, 0.0, 50.0, 50.0}
    };
    sc2dh::decor::DecorationSafetyContext safety;
    safety.referenceFilesByDoodadKey.insert(QStringLiteral("referencedvisual"),
                                            {QStringLiteral("MapScript.galaxy")});

    sc2dh::decor::DecorationStreamingPlanner planner;
    const sc2dh::decor::DecorationOptimizedArtifacts artifacts =
        planner.createOptimizedArtifacts(objects, zones, safety);

    QVERIFY2(artifacts.valid, qPrintable(artifacts.warnings.join(QStringLiteral("; "))));
    QCOMPARE(artifacts.removedDoodadIndices.size(), 1);
    const QString optimizedObjects = QString::fromUtf8(artifacts.optimizedObjectsBytes);
    QVERIFY(optimizedObjects.contains(QStringLiteral("ReferencedVisual")));
    QVERIFY(!optimizedObjects.contains(QStringLiteral("FreeVisual")));
    QVERIFY(artifacts.galaxySource.contains(QStringLiteral("FreeVisual"))
            || artifacts.galaxySource.contains(QStringLiteral("TreeVisual")));
    QVERIFY(!artifacts.galaxySource.contains(QStringLiteral("ReferencedVisual")));

    const sc2dh::decor::DoodadPlacement &referenced = artifacts.plan.doodads.at(0);
    QVERIFY(!referenced.dynamicCandidate);
    QVERIFY(referenced.staticOnlyReason.contains(QStringLiteral("pathing/gameplay dependency")));
    QCOMPARE(referenced.safetyReferenceFiles, QStringList{QStringLiteral("MapScript.galaxy")});
}

void CoreTests::decorationStreamingSupportsManualAssignmentOverrides()
{
    const QByteArray objects = QByteArrayLiteral(
        "ObjectDoodad { Id = 61 Name = \"AutoLeft\" Type = \"TreeVisual\" Position = (10, 10, 0) }\n"
        "ObjectDoodad { Id = 62 Name = \"ManualRight\" Type = \"RockVisual\" Position = (15, 10, 0) }\n"
        "ObjectDoodad { Id = 63 Name = \"KeepStatic\" Type = \"GrassVisual\" Position = (110, 10, 0) }\n");
    const QVector<sc2dh::decor::DecorZone> zones = {
        {1, QStringLiteral("Left"), 0.0, 0.0, 50.0, 50.0},
        {2, QStringLiteral("Right"), 100.0, 0.0, 150.0, 50.0}
    };

    sc2dh::decor::DecorationSafetyContext safety;
    safety.forcedZoneByDoodadKey.insert(QStringLiteral("manualright"), 2);
    safety.excludedDoodadKeys.insert(QStringLiteral("keepstatic"));

    sc2dh::decor::DecorationStreamingPlanner planner;
    const sc2dh::decor::DecorationOptimizedArtifacts artifacts =
        planner.createOptimizedArtifacts(objects, zones, safety);

    QVERIFY2(artifacts.valid, qPrintable(artifacts.warnings.join(QStringLiteral("; "))));
    QCOMPARE(artifacts.plan.zones.size(), 2);
    QCOMPARE(artifacts.plan.zones.at(0).doodadIndices.size(), 1);
    QCOMPARE(artifacts.plan.zones.at(1).doodadIndices.size(), 1);
    QCOMPARE(artifacts.plan.doodads.at(artifacts.plan.zones.at(0).doodadIndices.first()).name,
             QStringLiteral("AutoLeft"));
    const int manualIndex = artifacts.plan.zones.at(1).doodadIndices.first();
    QCOMPARE(artifacts.plan.doodads.at(manualIndex).name, QStringLiteral("ManualRight"));
    QCOMPARE(artifacts.plan.doodads.at(manualIndex).forcedZoneId, 2);

    QCOMPARE(artifacts.plan.staticOnlyDoodads.size(), 1);
    const sc2dh::decor::DoodadPlacement &excluded =
        artifacts.plan.doodads.at(artifacts.plan.staticOnlyDoodads.first());
    QCOMPARE(excluded.name, QStringLiteral("KeepStatic"));
    QVERIFY(excluded.userExcluded);
    QVERIFY(excluded.staticOnlyReason.contains(QStringLiteral("excluded by user")));

    const QString optimizedObjects = QString::fromUtf8(artifacts.optimizedObjectsBytes);
    QVERIFY(!optimizedObjects.contains(QStringLiteral("AutoLeft")));
    QVERIFY(!optimizedObjects.contains(QStringLiteral("ManualRight")));
    QVERIFY(optimizedObjects.contains(QStringLiteral("KeepStatic")));
    QCOMPARE(artifacts.removedDoodadIndices.size(), 2);

    const QString galaxy = artifacts.galaxySource;
    const qsizetype zone2Start = galaxy.indexOf(QStringLiteral("void DecorOpt_CreateZone_2_Batch_1()"));
    QVERIFY(zone2Start >= 0);
    const qsizetype zone2End = galaxy.indexOf(QStringLiteral("void DecorOpt_CreateZone_2()"), zone2Start);
    QVERIFY(zone2End > zone2Start);
    const QString zone2Body = galaxy.mid(zone2Start, zone2End - zone2Start);
    QVERIFY(zone2Body.contains(QStringLiteral("\"RockVisual\"")));
    QVERIFY(!galaxy.contains(QStringLiteral("GrassVisual")));
}

void CoreTests::decorationStreamingSupportsSparseZoneIds()
{
    const QByteArray objects = QByteArrayLiteral(
        "ObjectDoodad { Id = 71 Name = \"SparseLeft\" Type = \"TreeVisual\" Position = (10, 10, 0) }\n"
        "ObjectDoodad { Id = 72 Name = \"SparseRight\" Type = \"RockVisual\" Position = (110, 10, 0) }\n");
    const QVector<sc2dh::decor::DecorZone> zones = {
        {10, QStringLiteral("Left"), 0.0, 0.0, 50.0, 50.0},
        {20, QStringLiteral("Right"), 100.0, 0.0, 150.0, 50.0}
    };

    sc2dh::decor::GalaxyGenerationOptions options;
    options.functionPrefix = QStringLiteral("NAME_OUT_FUNK");
    const sc2dh::decor::DecorationStreamingPlanner planner;
    const sc2dh::decor::DecorationStreamingPlan plan = planner.buildPlan(objects, zones);
    const QString galaxy = planner.generateGalaxy(plan, options);

    QVERIFY(galaxy.contains(QStringLiteral("const int DecorOpt_ZoneCount = 20;")));
    QVERIFY(galaxy.contains(QStringLiteral("actor[21]")));
    QVERIFY(galaxy.contains(QStringLiteral("void NAME_OUT_FUNK_10()")));
    QVERIFY(galaxy.contains(QStringLiteral("void NAME_OUT_FUNK_20()")));
    QVERIFY(galaxy.contains(QStringLiteral("if (zoneId == 20) { DecorOpt_CreateZone_20(); return; }")));
    QStringList errors;
    QVERIFY2(planner.validateGeneratedGalaxy(galaxy, &errors), qPrintable(errors.join(QStringLiteral("; "))));
}

void CoreTests::decorationStreamingRejectsInvalidGalaxyOptions()
{
    const QByteArray objects = QByteArrayLiteral(
        "ObjectDoodad { Id = 51 Name = \"Visual\" Type = \"TreeVisual\" Position = (10, 10, 0) }\n");
    sc2dh::decor::GalaxyGenerationOptions options;
    options.functionPrefix = QStringLiteral("1 Bad Prefix");
    options.batchLimit = 0;
    const QVector<sc2dh::decor::DecorZone> zones = {
        {0, QStringLiteral("Invalid"), 0.0, 0.0, 50.0, 50.0},
        {0, QStringLiteral("DuplicateInvalid"), 0.0, 0.0, 50.0, 50.0}
    };

    const sc2dh::decor::DecorationOptimizedArtifacts artifacts =
        sc2dh::decor::DecorationStreamingPlanner().createOptimizedArtifacts(objects, zones, options);

    QVERIFY(!artifacts.valid);
    const QString warnings = artifacts.warnings.join(QStringLiteral("\n"));
    QVERIFY(warnings.contains(QStringLiteral("function prefix")));
    QVERIFY(warnings.contains(QStringLiteral("batch limit")));
    QVERIFY(warnings.contains(QStringLiteral("zone id must be positive")));
    QVERIFY(warnings.contains(QStringLiteral("zone id is duplicated")));
}

void CoreTests::decorationStreamingBuildsOptimizedObjectsArtifacts()
{
    const QByteArray objects = QByteArrayLiteral(
        "HeaderLine\n"
        "ObjectDoodad { Id = 10 Name = \"VisualLeft\" Type = \"TreeVisual\" Position = (10, 10, 0) Scale = 1 }\n"
        "ObjectDoodad { Id = 11 Name = \"VisualRight\" Type = \"RockVisual\" Position = (110, 10, 0) Scale = 1 }\n"
        "ObjectDoodad { Id = 12 Name = \"GameplayBlocker\" Type = \"PathingBlocker\" Position = (12, 12, 0) }\n"
        "ObjectDoodad { Id = 13 Name = \"Unassigned\" Type = \"TreeVisual\" Position = (500, 500, 0) }\n"
        "ObjectDoodad { Id = 14 Name = \"TintedVisual\" Type = \"TreeVisual\" Position = (15, 15, 0) TintColor = (255, 0, 0) }\n"
        "FooterLine\n");
    const QVector<sc2dh::decor::DecorZone> zones = {
        {1, QStringLiteral("Left"), 0.0, 0.0, 50.0, 50.0},
        {2, QStringLiteral("Right"), 100.0, 0.0, 150.0, 50.0}
    };
    sc2dh::decor::GalaxyGenerationOptions options;
    options.functionPrefix = QStringLiteral("NAME_OUT_FUNK");
    options.batchLimit = 4;

    const sc2dh::decor::DecorationOptimizedArtifacts artifacts =
        sc2dh::decor::DecorationStreamingPlanner().createOptimizedArtifacts(objects, zones, options);

    QVERIFY2(artifacts.valid, qPrintable(artifacts.warnings.join(QStringLiteral("; "))));
    QCOMPARE(artifacts.removedDoodadIndices.size(), 3);
    const QString optimized = QString::fromUtf8(artifacts.optimizedObjectsBytes);
    QVERIFY(optimized.contains(QStringLiteral("HeaderLine")));
    QVERIFY(optimized.contains(QStringLiteral("FooterLine")));
    QVERIFY(!optimized.contains(QStringLiteral("VisualLeft")));
    QVERIFY(!optimized.contains(QStringLiteral("VisualRight")));
    QVERIFY(optimized.contains(QStringLiteral("GameplayBlocker")));
    QVERIFY(optimized.contains(QStringLiteral("Unassigned")));
    QVERIFY(!optimized.contains(QStringLiteral("TintedVisual")));
    QVERIFY(artifacts.galaxySource.contains(QStringLiteral("void NAME_OUT_FUNK_1()")));
    QVERIFY(artifacts.galaxySource.contains(QStringLiteral("void NAME_OUT_FUNK_2()")));
    QVERIFY(artifacts.galaxySource.contains(QStringLiteral("TreeVisual")));
    QVERIFY(artifacts.galaxySource.contains(QStringLiteral("RockVisual")));
    QVERIFY(!artifacts.galaxySource.contains(QStringLiteral("PathingBlocker")));
    QVERIFY(artifacts.galaxySource.contains(QStringLiteral("libNtve_gf_SetTintColor")));
}

void CoreTests::decorationStreamingInjectsGalaxyIncludeOnce()
{
    const QByteArray mapScript = QByteArrayLiteral(
        "include \"TriggerLibs/NativeLib\"\n"
        "\n"
        "void InitMap() {\n"
        "}\n");
    sc2dh::decor::DecorationStreamingPlanner planner;
    QByteArray rewritten;
    QString error;
    QVERIFY2(planner.injectGalaxyInclude(mapScript,
                                         QStringLiteral("scripts/sc2dh_decor_opt.galaxy"),
                                         &rewritten,
                                         &error),
             qPrintable(error));
    QString text = QString::fromUtf8(rewritten);
    QVERIFY(text.startsWith(QStringLiteral("include \"scripts/sc2dh_decor_opt\"")));
    QCOMPARE(text.count(QStringLiteral("include \"scripts/sc2dh_decor_opt\"")), 1);
    QVERIFY(!text.contains(QStringLiteral("sc2dh_decor_opt.galaxy.galaxy")));
    QCOMPARE(text.count(QStringLiteral("DecorOpt_Init();")), 1);
    QVERIFY(text.contains(QStringLiteral("include \"TriggerLibs/NativeLib\"")));
    QVERIFY(!text.contains(QStringLiteral("DecorOpt_CreateZone(1);")));
    QVERIFY(!text.contains(QStringLiteral("NAME_OUT_FUNK_1();")));

    QByteArray twice;
    QVERIFY2(planner.injectGalaxyInclude(rewritten,
                                         QStringLiteral("scripts/sc2dh_decor_opt.galaxy"),
                                         &twice,
                                         &error),
             qPrintable(error));
    text = QString::fromUtf8(twice);
    QCOMPARE(text.count(QStringLiteral("include \"scripts/sc2dh_decor_opt\"")), 1);
    QCOMPARE(text.count(QStringLiteral("DecorOpt_Init();")), 1);

    QVERIFY(!planner.injectGalaxyInclude(mapScript, QStringLiteral("scripts/not_galaxy.txt"), &rewritten, &error));
    QVERIFY(!planner.injectGalaxyInclude(QByteArrayLiteral("include \"TriggerLibs/NativeLib\"\n"),
                                         QStringLiteral("scripts/sc2dh_decor_opt.galaxy"),
                                         &rewritten,
                                         &error));
    QVERIFY(error.contains(QStringLiteral("InitMap")));

    QVERIFY(!planner.injectGalaxyInclude(QByteArrayLiteral(
                                                 "void DecorOpt_Init() {}\n"
                                                 "void InitMap() {}\n"),
                                             QStringLiteral("scripts/sc2dh_decor_opt.galaxy"),
                                             &rewritten,
                                             &error));
    QVERIFY(error.contains(QStringLiteral("already defines DecorOpt_Init")));
    QVERIFY(!planner.injectGalaxyInclude(QByteArrayLiteral(
                                                 "void InitMap() { DecorOpt_Init(); }\n"),
                                             QStringLiteral("scripts/sc2dh_decor_opt.galaxy"),
                                             &rewritten,
                                             &error));
    QVERIFY(error.contains(QStringLiteral("already references DecorOpt_Init")));
    QVERIFY2(planner.injectGalaxyInclude(QByteArrayLiteral(
                                          "// DecorOpt_Init() is documented here only.\n"
                                          "void InitMap() {}\n"),
                                      QStringLiteral("scripts/sc2dh_decor_opt.galaxy"),
                                      &rewritten,
                                      &error),
             qPrintable(error));
    QVERIFY(QString::fromUtf8(rewritten).contains(QStringLiteral("DecorOpt_Init();")));
}

void CoreTests::decorationStreamingPreparesArchivePatch()
{
    const QByteArray objects = QByteArrayLiteral(
        "ObjectDoodad { Id = 21 Name = \"VisualA\" Type = \"TreeVisual\" Position = (10, 10, 0) }\n"
        "ObjectDoodad { Id = 22 Name = \"StaticBlock\" Type = \"PathingBlocker\" Position = (11, 11, 0) }\n");
    const QByteArray mapScript = QByteArrayLiteral(
        "include \"TriggerLibs/NativeLib\"\n"
        "void InitMap() {\n"
        "}\n");
    const QVector<sc2dh::decor::DecorZone> zones = {
        {1, QStringLiteral("ZoneA"), 0.0, 0.0, 50.0, 50.0}
    };

    const sc2dh::decor::DecorationArchivePatch patch =
        sc2dh::decor::DecorationStreamingPlanner().prepareArchivePatch(objects, mapScript, zones);

    QVERIFY2(patch.valid, qPrintable(patch.error + QStringLiteral(" ") + patch.warnings.join(QStringLiteral("; "))));
    QVERIFY(patch.replacementEntries.contains(QStringLiteral("Objects")));
    QVERIFY(patch.replacementEntries.contains(QStringLiteral("MapScript.galaxy")));
    QVERIFY(patch.replacementEntries.contains(QStringLiteral("scripts/sc2dh_decor_opt.galaxy")));

    const QString optimizedObjects = QString::fromUtf8(patch.replacementEntries.value(QStringLiteral("Objects")));
    QVERIFY(!optimizedObjects.contains(QStringLiteral("VisualA")));
    QVERIFY(optimizedObjects.contains(QStringLiteral("StaticBlock")));

    const QString rewrittenMapScript = QString::fromUtf8(patch.replacementEntries.value(QStringLiteral("MapScript.galaxy")));
    QCOMPARE(rewrittenMapScript.count(QStringLiteral("include \"scripts/sc2dh_decor_opt\"")), 1);
    QCOMPARE(rewrittenMapScript.count(QStringLiteral("DecorOpt_Init();")), 1);
    QVERIFY(rewrittenMapScript.contains(QStringLiteral("include \"TriggerLibs/NativeLib\"")));

    const QString runtime = QString::fromUtf8(patch.replacementEntries.value(QStringLiteral("scripts/sc2dh_decor_opt.galaxy")));
    QVERIFY(runtime.contains(QStringLiteral("void NAME_OUT_FUNK_1()")));
    QVERIFY(runtime.contains(QStringLiteral("VisualA")) || runtime.contains(QStringLiteral("TreeVisual")));
    QVERIFY(!runtime.contains(QStringLiteral("StaticBlock")));
}

void CoreTests::decorationVisibilityStreamingPreservesObjectsAndGeneratesRestoreApi()
{
    const QByteArray objects = QByteArrayLiteral(
        "<PlacedObjects Version=\"27\">\n"
        "  <ObjectDoodad Id=\"101\" Variation=\"3\" Position=\"10,10,8\" Rotation=\"1\" Scale=\"1,1,1\" Type=\"RaisedVisual\">"
        "<Flag Index=\"HeightAbsolute\" Value=\"1\"/></ObjectDoodad>\n"
        "  <ObjectDoodad Id=\"102\" Position=\"11,10,0\" Type=\"PathingBlocker\"/>\n"
        "  <ObjectDoodad Id=\"103\" Name=\"ScriptedVisual\" Position=\"12,10,0\" Type=\"TreeVisual\"/>\n"
        "  <ObjectDoodad Id=\"104\" Position=\"0,10,0\" Type=\"BoundaryVisual\"/>\n"
        "</PlacedObjects>\n");
    const QByteArray mapScript = QByteArrayLiteral(
        "include \"TriggerLibs/NativeLib\"\n"
        "void InitMap() {\n}\n");
    const QVector<sc2dh::decor::DecorZone> zones = {
        {1, QStringLiteral("Visibility Zone"), 0.0, 0.0, 50.0, 50.0}
    };
    sc2dh::decor::DecorationSafetyContext safety;
    safety.referenceFilesByDoodadKey.insert(QStringLiteral("scriptedvisual"),
                                            {QStringLiteral("MapScript.galaxy")});
    sc2dh::decor::GalaxyGenerationOptions options;
    options.functionPrefix = QStringLiteral("KSPDecor");
    options.batchLimit = 8;

    sc2dh::decor::DecorationStreamingPlanner planner;
    const sc2dh::decor::DecorationVisibilityArtifacts artifacts =
        planner.createVisibilityArtifacts(objects, zones, safety, options);

    QVERIFY2(artifacts.valid, qPrintable(artifacts.warnings.join(QStringLiteral("; "))));
    QVERIFY(artifacts.objectsPreserved);
    QCOMPARE(artifacts.controlledDoodadIndices, QVector<int>{0});
    QVERIFY(artifacts.plan.doodads.at(0).visibilityCandidate);
    QVERIFY(artifacts.plan.doodads.at(0).dynamicCandidate);
    QVERIFY(artifacts.plan.doodads.at(2).visibilityStaticOnlyReason.contains(QStringLiteral("referenced by trigger/script")));
    QCOMPARE(artifacts.plan.boundaryDoodads, QVector<int>{3});

    const QString runtime = artifacts.galaxySource;
    QVERIFY(runtime.contains(QStringLiteral("DoodadFromId(101)")));
    QVERIFY(runtime.contains(QStringLiteral("SetVisibility 0")));
    QVERIFY(runtime.contains(QStringLiteral("SetVisibility 1")));
    QVERIFY(runtime.contains(QStringLiteral("void DecorOpt_HideZone(int zoneId)")));
    QVERIFY(runtime.contains(QStringLiteral("void DecorOpt_RestoreZone(int zoneId)")));
    QVERIFY(runtime.contains(QStringLiteral("void KSPDecor_Hide_1()")));
    QVERIFY(runtime.contains(QStringLiteral("void KSPDecor_Restore_1()")));
    QVERIFY(!runtime.contains(QStringLiteral("DoodadFromId(102)")));
    QVERIFY(!runtime.contains(QStringLiteral("DoodadFromId(103)")));
    QVERIFY(!runtime.contains(QStringLiteral("\"Destroy\"")));
    QStringList runtimeErrors;
    QVERIFY2(planner.validateGeneratedVisibilityGalaxy(runtime, &runtimeErrors),
             qPrintable(runtimeErrors.join(QStringLiteral("; "))));

    const sc2dh::decor::DecorationArchivePatch patch =
        planner.prepareVisibilityArchivePatch(objects, mapScript, zones, safety, options);
    QVERIFY2(patch.valid, qPrintable(patch.error + QStringLiteral(" ") + patch.warnings.join(QStringLiteral("; "))));
    QCOMPARE(patch.mode, sc2dh::decor::DecorationOptimizationMode::VisibilityOnly);
    QVERIFY(!patch.replacementEntries.contains(QStringLiteral("Objects")));
    QVERIFY(patch.replacementEntries.contains(QStringLiteral("MapScript.galaxy")));
    QVERIFY(patch.replacementEntries.contains(QStringLiteral("scripts/sc2dh_decor_opt.galaxy")));
    const QString rewrittenMapScript = QString::fromUtf8(patch.replacementEntries.value(QStringLiteral("MapScript.galaxy")));
    QCOMPARE(rewrittenMapScript.count(QStringLiteral("DecorOpt_Init();")), 1);
}

void CoreTests::decorationMapCopyServiceCreatesOptimizedArchive()
{
#ifndef SC2DH_USE_STORMLIB
    QSKIP("StormLib archive writer is unavailable.");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString sourcePath = QDir(dir.path()).absoluteFilePath(QStringLiteral("DecorSource.SC2Map"));
    const QString outputPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("DecorSource_DecorOptimized.SC2Map"));
    const QByteArray objects = QByteArrayLiteral(
        "ObjectDoodad { Id = 31 Name = \"ReferencedVisual\" Type = \"TreeVisual\" Position = (10, 10, 0) }\n"
        "ObjectDoodad { Id = 32 Name = \"StaticBlock\" Type = \"PathingBlocker\" Position = (11, 11, 0) }\n"
        "ObjectDoodad { Id = 33 Name = \"FreeVisual\" Type = \"RockVisual\" Position = (12, 12, 0) }\n"
        "ObjectDoodad { Id = 34 Name = \"ManualRight\" Type = \"CrystalVisual\" Position = (13, 12, 0) }\n"
        "ObjectDoodad { Id = 35 Name = \"KeepStatic\" Type = \"GrassVisual\" Position = (14, 12, 0) }\n"
        "ObjectDoodad { Id = 36 Name = \"FootprintFromData\" Type = \"BridgeVisual\" Position = (15, 12, 0) }\n"
        "ObjectDoodad { Id = 37 Name = \"TriggerOnlyVisual\" Type = \"TreeVisual\" Position = (16, 12, 0) }\n");
    const QByteArray componentList = QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<Components><DataComponent Type=\"gada\"/></Components>\n");
    const QByteArray mapScript = QByteArrayLiteral(
        "include \"TriggerLibs/NativeLib\"\n"
        "void InitMap() { TriggerDebugOutput(1, StringToText(\"ReferencedVisual\"), true); }\n");

    QString error;
    QVERIFY2(createTestMpqArchive(sourcePath,
                                  {
                                      {QStringLiteral("Objects"), objects},
                                      {QStringLiteral("ComponentList.SC2Components"), componentList},
                                      {QStringLiteral("MapScript.galaxy"), mapScript},
                                      {QStringLiteral("Triggers"), QByteArrayLiteral(
                                          "<TriggerData><Element Value=\"37\"/></TriggerData>")},
                                      {QStringLiteral("Base.SC2Data/GameData/UnitData.xml"),
                                       QByteArrayLiteral("<Catalog><CUnit id=\"VerificationUnit\"/><CDoodad id=\"BridgeVisual\"><Footprint value=\"Footprint2x2\"/></CDoodad></Catalog>")}
                                  },
                                  &error),
             qPrintable(error));

    sc2dh::decor::DecorOptimizedMapRequest oneZoneRequest;
    oneZoneRequest.sourceArchivePath = sourcePath;
    oneZoneRequest.outputArchivePath = QDir(dir.path()).absoluteFilePath(QStringLiteral("OneZone_DecorOptimized.SC2Map"));
    oneZoneRequest.zones = {
        {1, QStringLiteral("OnlyZone"), 0.0, 0.0, 50.0, 50.0}
    };
    const sc2dh::decor::DecorOptimizedMapResult oneZoneResult =
        sc2dh::decor::DecorationMapCopyService().createOptimizedCopy(oneZoneRequest);
    QVERIFY2(oneZoneResult.success, qPrintable(oneZoneResult.error));
    QVERIFY(QFileInfo::exists(oneZoneRequest.outputArchivePath));
    QVERIFY(oneZoneResult.removedDoodads > 0);
    Sc2Archive oneZoneOptimized;
    QVERIFY2(oneZoneOptimized.load(oneZoneRequest.outputArchivePath, &error), qPrintable(error));
    QByteArray oneZoneRuntime;
    QVERIFY2(oneZoneOptimized.readEntry(QStringLiteral("scripts/sc2dh_decor_opt.galaxy"), &oneZoneRuntime, &error), qPrintable(error));
    QVERIFY(QString::fromUtf8(oneZoneRuntime).contains(QStringLiteral("void NAME_OUT_FUNK_1()")));
    QByteArray oneZoneObjects;
    QVERIFY2(oneZoneOptimized.readEntry(QStringLiteral("Objects"), &oneZoneObjects, &error), qPrintable(error));
    QVERIFY(!QString::fromUtf8(oneZoneObjects).contains(QStringLiteral("FreeVisual")));

    sc2dh::decor::DecorOptimizedMapRequest dryRunRequest = oneZoneRequest;
    dryRunRequest.outputArchivePath = QDir(dir.path()).absoluteFilePath(QStringLiteral("DryRunMustNotExist.SC2Map"));
    dryRunRequest.dryRun = true;
    const sc2dh::decor::DecorOptimizedMapResult dryRunResult =
        sc2dh::decor::DecorationMapCopyService().createOptimizedCopy(dryRunRequest);
    QVERIFY2(dryRunResult.success, qPrintable(dryRunResult.error));
    QVERIFY(dryRunResult.dryRun);
    QVERIFY(dryRunResult.sourceUnchanged);
    QCOMPARE(dryRunResult.sourceSha256Before, dryRunResult.sourceSha256After);
    QVERIFY(!QFileInfo::exists(dryRunRequest.outputArchivePath));

    const QString visibilityOutputPath =
        QDir(dir.path()).absoluteFilePath(QStringLiteral("DecorSource_DecorVisibility.SC2Map"));
    sc2dh::decor::DecorOptimizedMapRequest visibilityRequest;
    visibilityRequest.sourceArchivePath = sourcePath;
    visibilityRequest.outputArchivePath = visibilityOutputPath;
    visibilityRequest.zones = {
        {1, QStringLiteral("VisibilityZone"), 0.0, 0.0, 50.0, 50.0}
    };
    visibilityRequest.safetyContext.excludedDoodadKeys.insert(QStringLiteral("keepstatic"));
    visibilityRequest.galaxyOptions.functionPrefix = QStringLiteral("KSPDecor");
    visibilityRequest.galaxyOptions.batchLimit = 4;
    visibilityRequest.mode = sc2dh::decor::DecorationOptimizationMode::VisibilityOnly;
    const sc2dh::decor::DecorOptimizedMapResult visibilityResult =
        sc2dh::decor::DecorationMapCopyService().createOptimizedCopy(visibilityRequest);
    QVERIFY2(visibilityResult.success,
             qPrintable(visibilityResult.error + QStringLiteral(" ") + visibilityResult.warnings.join(QStringLiteral("; "))));
    QCOMPARE(visibilityResult.removedDoodads, 0);
    QVERIFY(visibilityResult.visibilityControlledDoodads > 0);
    QVERIFY(visibilityResult.objectsPreserved);
    QVERIFY(visibilityResult.fullAnalysisVerified);

    Sc2Archive visibilityArchive;
    QVERIFY2(visibilityArchive.load(visibilityOutputPath, &error), qPrintable(error));
    QByteArray visibilityObjects;
    QVERIFY2(visibilityArchive.readEntry(QStringLiteral("Objects"), &visibilityObjects, &error), qPrintable(error));
    QCOMPARE(visibilityObjects, objects);
    QByteArray visibilityComponentList;
    QVERIFY2(visibilityArchive.readEntry(QStringLiteral("ComponentList.SC2Components"),
                                         &visibilityComponentList,
                                         &error),
             qPrintable(error));
    QCOMPARE(visibilityComponentList, componentList);
    QByteArray visibilityRuntime;
    QVERIFY2(visibilityArchive.readEntry(QStringLiteral("scripts/sc2dh_decor_opt.galaxy"), &visibilityRuntime, &error),
             qPrintable(error));
    const QString visibilityRuntimeText = QString::fromUtf8(visibilityRuntime);
    QVERIFY(visibilityRuntimeText.contains(QStringLiteral("void DecorOpt_HideZone(int zoneId)")));
    QVERIFY(visibilityRuntimeText.contains(QStringLiteral("void DecorOpt_RestoreZone(int zoneId)")));
    QVERIFY(visibilityRuntimeText.contains(QStringLiteral("void KSPDecor_Restore_1()")));
    QVERIFY(!visibilityRuntimeText.contains(QStringLiteral("DoodadFromId(31)")));
    QVERIFY(!visibilityRuntimeText.contains(QStringLiteral("DoodadFromId(32)")));
    QVERIFY(!visibilityRuntimeText.contains(QStringLiteral("DoodadFromId(35)")));
    QVERIFY(!visibilityRuntimeText.contains(QStringLiteral("DoodadFromId(36)")));
    QVERIFY(!visibilityRuntimeText.contains(QStringLiteral("DoodadFromId(37)")));

    const QString opaqueMetadataSourcePath =
        QDir(dir.path()).absoluteFilePath(QStringLiteral("VisibilityOpaqueMetadataSource.SC2Map"));
    const QString opaqueMetadataOutputPath =
        QDir(dir.path()).absoluteFilePath(QStringLiteral("VisibilityOpaqueMetadataOutput.SC2Map"));
    QVERIFY2(createTestMpqArchive(opaqueMetadataSourcePath,
                                  {
                                      {QStringLiteral("Objects"), QByteArrayLiteral(
                                          "ObjectDoodad { Id = 91 Name = \"FreeVisual\" Type = \"TreeVisual\" Position = (10, 10, 0) }\n")},
                                      {QStringLiteral("MapScript.galaxy"), QByteArrayLiteral(
                                          "include \"TriggerLibs/NativeLib\"\nvoid InitMap() {\n}\n")},
                                      {QStringLiteral("MapInfo"), QByteArray::fromHex("00010203")},
                                      {QStringLiteral("Base.SC2Data/GameData/UnitData.xml"), QByteArrayLiteral("<Catalog/>")}
                                  },
                                  &error),
             qPrintable(error));
    sc2dh::decor::DecorOptimizedMapRequest opaqueMetadataRequest;
    opaqueMetadataRequest.sourceArchivePath = opaqueMetadataSourcePath;
    opaqueMetadataRequest.outputArchivePath = opaqueMetadataOutputPath;
    opaqueMetadataRequest.zones = {{1, QStringLiteral("VisibilityZone"), 0.0, 0.0, 50.0, 50.0}};
    opaqueMetadataRequest.mode = sc2dh::decor::DecorationOptimizationMode::VisibilityOnly;
    const sc2dh::decor::DecorOptimizedMapResult opaqueMetadataResult =
        sc2dh::decor::DecorationMapCopyService().createOptimizedCopy(opaqueMetadataRequest);
    QVERIFY2(opaqueMetadataResult.success, qPrintable(opaqueMetadataResult.error));
    QVERIFY(opaqueMetadataResult.objectsPreserved);
    QVERIFY(std::any_of(opaqueMetadataResult.warnings.cbegin(), opaqueMetadataResult.warnings.cend(),
                        [](const QString &warning) {
                            return warning.contains(QStringLiteral("opaque non-executable metadata"));
                        }));

    const QString opaqueMetadataActorOutputPath =
        QDir(dir.path()).absoluteFilePath(QStringLiteral("ActorOpaqueMetadataOutput.SC2Map"));
    sc2dh::decor::DecorOptimizedMapRequest opaqueMetadataActorRequest = opaqueMetadataRequest;
    opaqueMetadataActorRequest.outputArchivePath = opaqueMetadataActorOutputPath;
    opaqueMetadataActorRequest.mode = sc2dh::decor::DecorationOptimizationMode::RecreateActors;
    const sc2dh::decor::DecorOptimizedMapResult opaqueMetadataActorResult =
        sc2dh::decor::DecorationMapCopyService().createOptimizedCopy(opaqueMetadataActorRequest);
    QVERIFY2(opaqueMetadataActorResult.success, qPrintable(opaqueMetadataActorResult.error));
    QCOMPARE(opaqueMetadataActorResult.removedDoodads, 1);
    Sc2Archive opaqueMetadataActorArchive;
    QVERIFY2(opaqueMetadataActorArchive.load(opaqueMetadataActorOutputPath, &error), qPrintable(error));
    QByteArray opaqueMetadataActorObjects;
    QVERIFY2(opaqueMetadataActorArchive.readEntry(QStringLiteral("Objects"),
                                                  &opaqueMetadataActorObjects,
                                                  &error),
             qPrintable(error));
    QVERIFY(!opaqueMetadataActorObjects.contains("FreeVisual"));

    sc2dh::decor::DecorOptimizedMapRequest request;
    request.sourceArchivePath = sourcePath;
    request.outputArchivePath = outputPath;
    request.zones = {
        {1, QStringLiteral("ZoneA"), 0.0, 0.0, 50.0, 50.0},
        {2, QStringLiteral("ZoneB"), 100.0, 0.0, 150.0, 50.0}
    };
    request.safetyContext.forcedZoneByDoodadKey.insert(QStringLiteral("manualright"), 2);
    request.safetyContext.excludedDoodadKeys.insert(QStringLiteral("keepstatic"));
    request.galaxyOptions.functionPrefix = QStringLiteral("NAME_OUT_FUNK");
    request.galaxyOptions.batchLimit = 4;

    const sc2dh::decor::DecorOptimizedMapResult result =
        sc2dh::decor::DecorationMapCopyService().createOptimizedCopy(request);
    QVERIFY2(result.success, qPrintable(result.error + QStringLiteral(" ") + result.warnings.join(QStringLiteral("; "))));
    QCOMPARE(result.outputArchivePath, outputPath);
    QCOMPARE(result.removedDoodads, 2);
    QVERIFY(result.fullAnalysisVerified);
    QVERIFY(result.verifiedScannedFiles >= 3);
    QVERIFY(result.verifiedDataNodes >= 1);
    QVERIFY(result.verificationParseErrors.isEmpty());
    QVERIFY(QFileInfo::exists(sourcePath));
    QVERIFY(QFileInfo::exists(outputPath));

    // A failed overwrite request must never pre-delete the existing output.
    QFile outputBeforeInvalidOverwrite(outputPath);
    QVERIFY(outputBeforeInvalidOverwrite.open(QIODevice::ReadOnly));
    const QByteArray outputBytesBeforeInvalidOverwrite = outputBeforeInvalidOverwrite.readAll();
    outputBeforeInvalidOverwrite.close();
    sc2dh::decor::DecorOptimizedMapRequest invalidOverwriteRequest = request;
    invalidOverwriteRequest.overwriteExisting = true;
    invalidOverwriteRequest.galaxyOptions.batchLimit = 0;
    const sc2dh::decor::DecorOptimizedMapResult invalidOverwriteResult =
        sc2dh::decor::DecorationMapCopyService().createOptimizedCopy(invalidOverwriteRequest);
    QVERIFY(!invalidOverwriteResult.success);
    QFile outputAfterInvalidOverwrite(outputPath);
    QVERIFY(outputAfterInvalidOverwrite.open(QIODevice::ReadOnly));
    QCOMPARE(outputAfterInvalidOverwrite.readAll(), outputBytesBeforeInvalidOverwrite);
    outputAfterInvalidOverwrite.close();

    // A valid overwrite stages and verifies first, then preserves a verified
    // backup of the prior copy before atomically replacing it.
    sc2dh::decor::DecorOptimizedMapRequest overwriteRequest = request;
    overwriteRequest.overwriteExisting = true;
    const sc2dh::decor::DecorOptimizedMapResult overwriteResult =
        sc2dh::decor::DecorationMapCopyService().createOptimizedCopy(overwriteRequest);
    QVERIFY2(overwriteResult.success, qPrintable(overwriteResult.error));
    QVERIFY(!overwriteResult.previousOutputBackupPath.isEmpty());
    QVERIFY(QFileInfo::exists(overwriteResult.previousOutputBackupPath));

    Sc2Archive original;
    QVERIFY2(original.load(sourcePath, &error), qPrintable(error));
    QByteArray originalObjects;
    QVERIFY2(original.readEntry(QStringLiteral("Objects"), &originalObjects, &error), qPrintable(error));
    QVERIFY(QString::fromUtf8(originalObjects).contains(QStringLiteral("ReferencedVisual")));
    QVERIFY(QString::fromUtf8(originalObjects).contains(QStringLiteral("FreeVisual")));
    QVERIFY(QString::fromUtf8(originalObjects).contains(QStringLiteral("StaticBlock")));
    QVERIFY(QString::fromUtf8(originalObjects).contains(QStringLiteral("ManualRight")));
    QVERIFY(QString::fromUtf8(originalObjects).contains(QStringLiteral("KeepStatic")));
    QVERIFY(QString::fromUtf8(originalObjects).contains(QStringLiteral("FootprintFromData")));

    Sc2Archive optimized;
    QVERIFY2(optimized.load(outputPath, &error), qPrintable(error));
    QByteArray optimizedObjects;
    QVERIFY2(optimized.readEntry(QStringLiteral("Objects"), &optimizedObjects, &error), qPrintable(error));
    const QString optimizedObjectsText = QString::fromUtf8(optimizedObjects);
    QVERIFY(optimizedObjectsText.contains(QStringLiteral("ReferencedVisual")));
    QVERIFY(!optimizedObjectsText.contains(QStringLiteral("FreeVisual")));
    QVERIFY(optimizedObjectsText.contains(QStringLiteral("StaticBlock")));
    QVERIFY(!optimizedObjectsText.contains(QStringLiteral("ManualRight")));
    QVERIFY(optimizedObjectsText.contains(QStringLiteral("KeepStatic")));
    QVERIFY(optimizedObjectsText.contains(QStringLiteral("FootprintFromData")));

    QByteArray optimizedMapScript;
    QVERIFY2(optimized.readEntry(QStringLiteral("MapScript.galaxy"), &optimizedMapScript, &error), qPrintable(error));
    const QString mapScriptText = QString::fromUtf8(optimizedMapScript);
    QCOMPARE(mapScriptText.count(QStringLiteral("include \"scripts/sc2dh_decor_opt\"")), 1);
    QCOMPARE(mapScriptText.count(QStringLiteral("DecorOpt_Init();")), 1);
    QVERIFY(mapScriptText.contains(QStringLiteral("include \"TriggerLibs/NativeLib\"")));
    QVERIFY(!mapScriptText.contains(QStringLiteral("DecorOpt_CreateZone(1);")));

    QByteArray runtime;
    QVERIFY2(optimized.readEntry(QStringLiteral("scripts/sc2dh_decor_opt.galaxy"), &runtime, &error), qPrintable(error));
    const QString runtimeText = QString::fromUtf8(runtime);
    QVERIFY(runtimeText.contains(QStringLiteral("void NAME_OUT_FUNK_1()")));
    QVERIFY(runtimeText.contains(QStringLiteral("void NAME_OUT_FUNK_2()")));
    QVERIFY(runtimeText.contains(QStringLiteral("DecorOpt_CreateZone")));
    QVERIFY(runtimeText.contains(QStringLiteral("RockVisual")));
    QVERIFY(runtimeText.contains(QStringLiteral("CrystalVisual")));
    QVERIFY(!runtimeText.contains(QStringLiteral("ReferencedVisual")));
    QVERIFY(!runtimeText.contains(QStringLiteral("StaticBlock")));
    QVERIFY(!runtimeText.contains(QStringLiteral("GrassVisual")));
    QVERIFY(!runtimeText.contains(QStringLiteral("BridgeVisual")));
    for (const QString &name : QDir(dir.path()).entryList(QDir::Files)) {
        QVERIFY2(!name.contains(QStringLiteral(".sc2dh-stage-"))
                     && !name.endsWith(QStringLiteral(".compact")),
                 qPrintable(QStringLiteral("Decoration staging helper was not cleaned: %1").arg(name)));
    }
#endif
}

void CoreTests::decorationCliCreatesOptimizedArchiveAndReport()
{
#ifndef SC2DH_USE_STORMLIB
    QSKIP("StormLib archive writer is unavailable.");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sourcePath = QDir(dir.path()).absoluteFilePath(QStringLiteral("CliDecorSource.SC2Map"));
    const QString outputPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("CliDecorOptimized.SC2Map"));
    const QString zonesPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("zones.json"));
    const QString reportPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("decor_report.json"));
    const QByteArray objects = QByteArrayLiteral(
        "ObjectDoodad { Id = 61 Name = \"LeftFree\" Type = \"TreeVisual\" Position = (10, 10, 0) }\n"
        "ObjectDoodad { Id = 62 Name = \"RightFree\" Type = \"RockVisual\" Position = (110, 10, 0) }\n"
        "ObjectDoodad { Id = 63 Name = \"StaticBlock\" Type = \"PathingBlocker\" Position = (11, 11, 0) }\n");
    QString error;
    QVERIFY2(createTestMpqArchive(sourcePath,
                                  {
                                      {QStringLiteral("Objects"), objects},
                                      {QStringLiteral("MapScript.galaxy"), QByteArrayLiteral("include \"TriggerLibs/NativeLib\"\nvoid InitMap() {\n}\n")},
                                      {QStringLiteral("Regions"), QByteArrayLiteral(
                                           "<Regions>"
                                           "<region id=\"1\"><name value=\"Left\"/><shape type=\"rect\"><quad value=\"0,0,50,50\"/></shape></region>"
                                           "<region id=\"2\"><name value=\"Right\"/><shape type=\"rect\"><quad value=\"100,0,150,50\"/></shape></region>"
                                           "</Regions>")},
                                      {QStringLiteral("Base.SC2Data/GameData/UnitData.xml"), QByteArrayLiteral("<Catalog/>")}
                                  },
                                  &error),
             qPrintable(error));
    QVERIFY(writeTextFile(zonesPath, QByteArrayLiteral(
        "["
        "{\"id\":1,\"name\":\"Left\",\"xMin\":0,\"yMin\":0,\"xMax\":50,\"yMax\":50},"
        "{\"id\":2,\"name\":\"Right\",\"xMin\":100,\"yMin\":0,\"xMax\":150,\"yMax\":50}"
        "]")));

    QString cliPath = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("SC2DecorOptimizeMap.exe"));
    if (!QFileInfo::exists(cliPath))
        cliPath = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("SC2DecorOptimizeMap"));
    QVERIFY2(QFileInfo::exists(cliPath), qPrintable(QStringLiteral("Missing CLI executable: %1").arg(cliPath)));

    QProcess process;
    process.setProgram(cliPath);
    process.setArguments({
        sourcePath,
        outputPath,
        QStringLiteral("--zones"),
        zonesPath,
        QStringLiteral("--report"),
        reportPath,
        QStringLiteral("--overwrite")
    });
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    QVERIFY2(process.waitForStarted(5000), qPrintable(process.errorString()));
    QVERIFY2(process.waitForFinished(60000), qPrintable(process.errorString()));
    const QString output = QString::fromUtf8(process.readAll());
    QVERIFY2(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
             qPrintable(output));

    QFile reportFile(reportPath);
    QVERIFY(reportFile.open(QIODevice::ReadOnly));
    const QJsonDocument reportJson = QJsonDocument::fromJson(reportFile.readAll());
    QVERIFY(reportJson.isObject());
    QCOMPARE(reportJson.object().value(QStringLiteral("removedDoodads")).toInt(), 2);
    QCOMPARE(reportJson.object().value(QStringLiteral("zones")).toArray().size(), 2);

    Sc2Archive optimized;
    QVERIFY2(optimized.load(outputPath, &error), qPrintable(error));
    QByteArray optimizedObjects;
    QVERIFY2(optimized.readEntry(QStringLiteral("Objects"), &optimizedObjects, &error), qPrintable(error));
    const QString optimizedObjectsText = QString::fromUtf8(optimizedObjects);
    QVERIFY(!optimizedObjectsText.contains(QStringLiteral("LeftFree")));
    QVERIFY(!optimizedObjectsText.contains(QStringLiteral("RightFree")));
    QVERIFY(optimizedObjectsText.contains(QStringLiteral("StaticBlock")));

    QByteArray optimizedMapScript;
    QVERIFY2(optimized.readEntry(QStringLiteral("MapScript.galaxy"), &optimizedMapScript, &error), qPrintable(error));
    const QString mapScriptText = QString::fromUtf8(optimizedMapScript);
    QCOMPARE(mapScriptText.count(QStringLiteral("DecorOpt_Init();")), 1);
    QVERIFY(!mapScriptText.contains(QStringLiteral("DecorOpt_CreateZone(1);")));
    QVERIFY(!mapScriptText.contains(QStringLiteral("NAME_OUT_FUNK_1();")));

    QByteArray runtime;
    QVERIFY2(optimized.readEntry(QStringLiteral("scripts/sc2dh_decor_opt.galaxy"), &runtime, &error), qPrintable(error));
    const QString runtimeText = QString::fromUtf8(runtime);
    QVERIFY(runtimeText.contains(QStringLiteral("void NAME_OUT_FUNK_1()")));
    QVERIFY(runtimeText.contains(QStringLiteral("void NAME_OUT_FUNK_2()")));
    QVERIFY(runtimeText.contains(QStringLiteral("if (DecorOpt_Loaded[1]) { return; }")));
    QVERIFY(runtimeText.contains(QStringLiteral("if (DecorOpt_Loaded[2]) { return; }")));

    const QString visibilityOutputPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("CliDecorVisibility.SC2Map"));
    const QString visibilityReportPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("decor_visibility_report.json"));
    QProcess visibilityProcess;
    visibilityProcess.setProgram(cliPath);
    visibilityProcess.setArguments({
        sourcePath,
        visibilityOutputPath,
        QStringLiteral("--map-regions"),
        QStringLiteral("--visibility-only"),
        QStringLiteral("--prefix"),
        QStringLiteral("KSPDecor"),
        QStringLiteral("--report"),
        visibilityReportPath,
    });
    visibilityProcess.setProcessChannelMode(QProcess::MergedChannels);
    visibilityProcess.start();
    QVERIFY2(visibilityProcess.waitForStarted(5000), qPrintable(visibilityProcess.errorString()));
    QVERIFY2(visibilityProcess.waitForFinished(60000), qPrintable(visibilityProcess.errorString()));
    const QString visibilityOutput = QString::fromUtf8(visibilityProcess.readAll());
    QVERIFY2(visibilityProcess.exitStatus() == QProcess::NormalExit && visibilityProcess.exitCode() == 0,
             qPrintable(visibilityOutput));

    QFile visibilityReportFile(visibilityReportPath);
    QVERIFY(visibilityReportFile.open(QIODevice::ReadOnly));
    const QJsonDocument visibilityReportJson = QJsonDocument::fromJson(visibilityReportFile.readAll());
    QVERIFY(visibilityReportJson.isObject());
    QCOMPARE(visibilityReportJson.object().value(QStringLiteral("mode")).toString(), QStringLiteral("visibility-only"));
    QCOMPARE(visibilityReportJson.object().value(QStringLiteral("removedDoodads")).toInt(), 0);
    QCOMPARE(visibilityReportJson.object().value(QStringLiteral("visibilityControlledDoodads")).toInt(), 2);
    QVERIFY(visibilityReportJson.object().value(QStringLiteral("objectsPreserved")).toBool());

    Sc2Archive visibilityOptimized;
    QVERIFY2(visibilityOptimized.load(visibilityOutputPath, &error), qPrintable(error));
    QByteArray visibilityObjects;
    QVERIFY2(visibilityOptimized.readEntry(QStringLiteral("Objects"), &visibilityObjects, &error), qPrintable(error));
    QCOMPARE(visibilityObjects, objects);
    QByteArray visibilityRuntime;
    QVERIFY2(visibilityOptimized.readEntry(QStringLiteral("scripts/sc2dh_decor_opt.galaxy"), &visibilityRuntime, &error),
             qPrintable(error));
    const QString visibilityRuntimeText = QString::fromUtf8(visibilityRuntime);
    QVERIFY(visibilityRuntimeText.contains(QStringLiteral("void KSPDecor_Hide_1()")));
    QVERIFY(visibilityRuntimeText.contains(QStringLiteral("void KSPDecor_Restore_2()")));
    QVERIFY(visibilityRuntimeText.contains(QStringLiteral("DoodadFromId(61)")));
    QVERIFY(visibilityRuntimeText.contains(QStringLiteral("DoodadFromId(62)")));
    QVERIFY(!visibilityRuntimeText.contains(QStringLiteral("DoodadFromId(63)")));
#endif
}

void CoreTests::deepCleanupReportsSemanticDuplicateReview()
{
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("GameData")));
    const QString xmlPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("GameData/EffectData.xml"));
    QVERIFY(writeTextFile(xmlPath, QByteArrayLiteral(
        "<Catalog>"
        "<CEffectDamage id=\"DamageA\"><Amount value=\"10\"/><Name value=\"A\"/></CEffectDamage>"
        "<CEffectDamage id=\"DamageB\"><Amount value=\"10\"/><Name value=\"B\"/></CEffectDamage>"
        "</Catalog>")));

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    QString error;
    QVERIFY2(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error), qPrintable(error));

    bool found = false;
    for (const DeepCleanupCandidate &candidate : analysis.deepCleanupCandidates) {
        if (candidate.kind == DeepCleanupKind::NearDuplicateObject) {
            found = true;
            QCOMPARE(candidate.action, DeepCleanupAction::ReportOnly);
            QCOMPARE(candidate.state, CandidateState::Risky);
        }
    }
    QVERIFY(found);
}

void CoreTests::objectFileFilterUsesFullSourcePath()
{
    ObjectTableModel model;
    ObjectFilterProxyModel proxy;
    proxy.setSourceModel(&model);

    DataNode modelNode;
    modelNode.id = QStringLiteral("MarineModel");
    modelNode.sourceFile = QStringLiteral("Base.SC2Data/GameData/ModelData.xml");
    DataNode unitNode;
    unitNode.id = QStringLiteral("Marine");
    unitNode.sourceFile = QStringLiteral("Base.SC2Data/GameData/UnitData.xml");
    model.setNodes({modelNode, unitNode});

    proxy.setSourceFileFilter(QStringLiteral("Base.SC2Data/GameData/ModelData.xml"));
    QCOMPARE(proxy.rowCount(), 1);
    QCOMPARE(proxy.index(0, ObjectTableModel::IdColumn).data().toString(), QStringLiteral("MarineModel"));
}

void CoreTests::folderScanAndAnalysis()
{
    qInfo("folderScanAndAnalysis");
    QTemporaryDir tempDir;
    QVERIFY2(tempDir.isValid(), "Temporary directory creation failed");

    QString rootFolder;
    QVERIFY2(createSampleFolder(&tempDir, &rootFolder), "Failed to create sample folder");

    FolderAnalyzer analyzer;
    AnalysisResult result;
    QString errorMessage;
    QVERIFY2(analyzer.analyzeFolder(rootFolder, QSet<QString>{}, &result, &errorMessage), qPrintable(errorMessage));

    QCOMPARE(result.totalFilesScanned(), 4);
    QCOMPARE(result.totalXmlFiles(), 3);
    QCOMPARE(result.totalDataNodes(), 5);
    QCOMPARE(result.parseErrors.size(), 0);
    QVERIFY(!result.analysisReportText.isEmpty());
    QVERIFY(result.analysisReportText.contains(QStringLiteral("Duplicate ID groups")));
}

void CoreTests::xmlParseAndLookup()
{
    qInfo("xmlParseAndLookup");
    XmlLoader loader;
    QVector<DataNode> nodes;
    const QByteArray xml = R"xml(
<Root>
  <Entries>
    <CUnit id="TestUnit">
      <Name value="Marine"/>
    </CUnit>
  </Entries>
</Root>
)xml";

    QString errorMessage;
    QVERIFY2(loader.extractNodes(QStringLiteral("GameData/Test.xml"), xml, &nodes, &errorMessage), qPrintable(errorMessage));
    QCOMPARE(nodes.size(), 1);
    QCOMPARE(nodes.front().id, QStringLiteral("TestUnit"));
    QCOMPARE(nodes.front().parentNode, QStringLiteral("Entries"));
    QCOMPARE(nodes.front().originalLocation, QStringLiteral("/Root[1]/Entries[1]/CUnit[1]"));
    QVERIFY(nodes.front().lineNumber > 0);

    const std::optional<int> index =
        loader.findNodeIndexByFileAndId(nodes, QStringLiteral("GameData/Test.xml"), QStringLiteral("TestUnit"));
    QVERIFY(index.has_value());
    QCOMPARE(*index, 0);
}

void CoreTests::duplicateIdDetection()
{
    qInfo("duplicateIdDetection");
    QTemporaryDir tempDir;
    QVERIFY2(tempDir.isValid(), "Temporary directory creation failed");

    QString rootFolder;
    QVERIFY2(createSampleFolder(&tempDir, &rootFolder), "Failed to create sample folder");

    FolderAnalyzer analyzer;
    AnalysisResult result;
    QString errorMessage;
    QVERIFY2(analyzer.analyzeFolder(rootFolder, QSet<QString>{}, &result, &errorMessage), qPrintable(errorMessage));

    bool foundLocalDup = false;
    for (const DuplicateIdGroup &group : result.duplicateIdGroups) {
        if (group.id == QStringLiteral("LocalDup")) {
            foundLocalDup = true;
            QVERIFY(group.sameFile);
            QVERIFY(!group.crossFile);
            QCOMPARE(group.nodeIndices.size(), 2);
        }
    }

    QVERIFY(foundLocalDup);
    for (const DataNode &node : result.nodes) {
        if (node.id == QStringLiteral("UnitA") || node.id == QStringLiteral("UnitB")) {
            QVERIFY(!node.duplicateId);
        }
    }
}

void CoreTests::duplicateContentDetection()
{
    qInfo("duplicateContentDetection");
    QTemporaryDir tempDir;
    QVERIFY2(tempDir.isValid(), "Temporary directory creation failed");

    QString rootFolder;
    QVERIFY2(createSampleFolder(&tempDir, &rootFolder), "Failed to create sample folder");

    FolderAnalyzer analyzer;
    AnalysisResult result;
    QString errorMessage;
    QVERIFY2(analyzer.analyzeFolder(rootFolder, QSet<QString>{}, &result, &errorMessage), qPrintable(errorMessage));

    bool foundDifferentIds = false;
    for (const DuplicateContentGroup &group : result.duplicateContentGroups) {
        QSet<QString> ids;
        for (int index : group.nodeIndices) {
            ids.insert(result.nodes[index].id);
        }
        if (ids.contains(QStringLiteral("UnitA")) && ids.contains(QStringLiteral("UnitB"))) {
            foundDifferentIds = true;
            QCOMPARE(group.elementName, QStringLiteral("CUnit"));
            QVERIFY(group.nodeIndices.size() >= 2);
            for (int index : group.nodeIndices) {
                QCOMPARE(result.nodes[index].elementName, QStringLiteral("CUnit"));
            }
        }
    }

    QVERIFY(foundDifferentIds);
}

void CoreTests::duplicateBodyRequiresSameTypeAndExactNestedBody()
{
    XmlLoader loader;
    QVector<DataNode> nodes;
    QString errorMessage;
    const QByteArray xml = R"xml(
<Catalog>
  <CUnit id="UnitA"><Link id="NestedA"/></CUnit>
  <CUnit id="UnitB"><Link id="NestedB"/></CUnit>
  <CUnit id="UnitC"><Value value="1"/></CUnit>
  <CUnit id="UnitD"><Value value="1"/></CUnit>
  <CActor id="ActorA"><Value value="1"/></CActor>
</Catalog>)xml";
    QVERIFY2(loader.extractNodes(QStringLiteral("GameData/Test.xml"), xml, &nodes, &errorMessage),
             qPrintable(errorMessage));

    const auto findNode = [&nodes](const QString &id) -> const DataNode * {
        for (const DataNode &node : nodes) {
            if (node.id == id) {
                return &node;
            }
        }
        return nullptr;
    };

    const DataNode *unitA = findNode(QStringLiteral("UnitA"));
    const DataNode *unitB = findNode(QStringLiteral("UnitB"));
    const DataNode *unitC = findNode(QStringLiteral("UnitC"));
    const DataNode *unitD = findNode(QStringLiteral("UnitD"));
    const DataNode *actorA = findNode(QStringLiteral("ActorA"));
    QVERIFY(unitA && unitB && unitC && unitD && actorA);
    QVERIFY(unitA->contentHash != unitB->contentHash);
    QCOMPARE(unitC->contentHash, unitD->contentHash);
    QVERIFY(unitC->contentHash != actorA->contentHash);
    QVERIFY(unitC->elementName != actorA->elementName);
}

void CoreTests::backupCreation()
{
    qInfo("backupCreation");
    QTemporaryDir tempDir;
    QVERIFY2(tempDir.isValid(), "Temporary directory creation failed");

    QString rootFolder;
    QVERIFY2(createSampleFolder(&tempDir, &rootFolder), "Failed to create sample folder");

    BackupManager backupManager;
    QString backupFolder;
    QString errorMessage;
    const QStringList filesToCopy = {
        QStringLiteral("GameData/A.xml"),
        QStringLiteral("GameData/B.xml")
    };

    QVERIFY2(backupManager.createFolderBackup(rootFolder,
                                              filesToCopy,
                                              QStringLiteral("analysis"),
                                              QStringLiteral("planned"),
                                              &backupFolder,
                                              &errorMessage),
             qPrintable(errorMessage));

    QVERIFY(QFileInfo(backupFolder).exists());
    QVERIFY(QFileInfo(backupFolder).fileName().startsWith(QStringLiteral("backup_")));
    QVERIFY(QFile::exists(QDir(backupFolder).absoluteFilePath(QStringLiteral("GameData/A.xml"))));
    QVERIFY(QFile::exists(QDir(backupFolder).absoluteFilePath(QStringLiteral("GameData/B.xml"))));
    QVERIFY(!QFile::exists(QDir(backupFolder).absoluteFilePath(QStringLiteral("analysis_report.txt"))));
    QVERIFY(!QFile::exists(QDir(backupFolder).absoluteFilePath(QStringLiteral("planned_changes_report.txt"))));
}

void CoreTests::folderTransactionRollsBackOnValidationFailure()
{
    QTemporaryDir tempDir;
    QVERIFY2(tempDir.isValid(), "Temporary directory creation failed");

    const QString root = tempDir.path();
    QVERIFY(QDir(root).mkpath(QStringLiteral("GameData")));
    const QString firstPath = QDir(root).absoluteFilePath(QStringLiteral("GameData/First.xml"));
    const QString removedPath = QDir(root).absoluteFilePath(QStringLiteral("GameData/Removed.xml"));
    const QString newPath = QDir(root).absoluteFilePath(QStringLiteral("GameData/New.xml"));
    const QByteArray firstOriginal = QByteArrayLiteral("<Catalog><CUnit id=\"Original\"/></Catalog>");
    const QByteArray removedOriginal = QByteArrayLiteral("<Catalog><CUnit id=\"KeepAfterRollback\"/></Catalog>");
    QVERIFY(writeTextFile(firstPath, firstOriginal));
    QVERIFY(writeTextFile(removedPath, removedOriginal));

    const QVector<TransactionalFileChange> changes{
        {QStringLiteral("GameData/First.xml"), QByteArrayLiteral("<Catalog><CUnit id=\"Changed\"/></Catalog>"), false},
        {QStringLiteral("GameData/Removed.xml"), {}, true},
        {QStringLiteral("GameData/New.xml"), QByteArrayLiteral("<Catalog><CUnit id=\"New\"/></Catalog>"), false}
    };
    const FolderSaveTransactionResult result = BackupManager().applyFolderTransaction(
        root,
        changes,
        QStringLiteral("analysis"),
        QStringLiteral("planned"),
        {},
        [](QString *errorMessage) {
            if (errorMessage)
                *errorMessage = QStringLiteral("Injected post-commit validation failure.");
            return false;
        });

    QVERIFY(!result.success);
    QCOMPARE(result.errorCode, OperationErrorCode::ValidationFailed);
    QVERIFY(result.rollbackAttempted);
    QVERIFY2(result.originalStateVerified, qPrintable(result.error));
    QFile first(firstPath);
    QVERIFY(first.open(QIODevice::ReadOnly));
    QCOMPARE(first.readAll(), firstOriginal);
    QFile removed(removedPath);
    QVERIFY(removed.open(QIODevice::ReadOnly));
    QCOMPARE(removed.readAll(), removedOriginal);
    QVERIFY(!QFileInfo::exists(newPath));
    QVERIFY(QFileInfo::exists(QDir(result.backupFolder).absoluteFilePath(QStringLiteral("GameData/First.xml"))));
    QVERIFY(QFileInfo::exists(QDir(result.backupFolder).absoluteFilePath(QStringLiteral("GameData/Removed.xml"))));
}

void CoreTests::folderTransactionRejectsStaleSourceBeforeCommit()
{
    QTemporaryDir tempDir;
    QVERIFY2(tempDir.isValid(), "Temporary directory creation failed");

    const QString root = tempDir.path();
    QVERIFY(QDir(root).mkpath(QStringLiteral("GameData")));
    const QString sourcePath = QDir(root).absoluteFilePath(QStringLiteral("GameData/Source.xml"));
    const QByteArray original = QByteArrayLiteral("<Catalog><CUnit id=\"Original\"/></Catalog>");
    const QByteArray externallyChanged = QByteArrayLiteral("<Catalog><CUnit id=\"ExternalChange\"/></Catalog>");
    QVERIFY(writeTextFile(sourcePath, original));

    const FolderSaveTransactionResult result = BackupManager().applyFolderTransaction(
        root,
        {{QStringLiteral("GameData/Source.xml"), QByteArrayLiteral("<Catalog><CUnit id=\"OurChange\"/></Catalog>"), false}},
        QStringLiteral("analysis"),
        QStringLiteral("planned"),
        [sourcePath, externallyChanged](const QString &, QString *) {
            return writeTextFile(sourcePath, externallyChanged);
        });

    QVERIFY(!result.success);
    QCOMPARE(result.errorCode, OperationErrorCode::ValidationFailed);
    QVERIFY(result.error.contains(QStringLiteral("Source changed before save commit")));
    QVERIFY(!result.rollbackAttempted);
    QVERIFY(!result.originalStateVerified);
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(source.readAll(), externallyChanged);
    QFile backup(QDir(result.backupFolder).absoluteFilePath(QStringLiteral("GameData/Source.xml")));
    QVERIFY2(backup.open(QIODevice::ReadOnly), qPrintable(result.error));
    QCOMPARE(backup.readAll(), original);
}

void CoreTests::localizationWidgetTreeRetranslatesAndPreservesDomainValue()
{
    QWidget root;
    auto *staticLabel = new QLabel(QStringLiteral("Static caption"), &root);
    auto *domainLabel = new QLabel(QStringLiteral("No object selected"), &root);
    auto *tabs = new QTabWidget(&root);
    tabs->addTab(new QWidget(tabs), QStringLiteral("Overview"));
    auto *model = new QStandardItemModel(&root);
    model->setHorizontalHeaderLabels({QStringLiteral("Object"), QStringLiteral("Status")});

    sc2dh::app::TranslationManager::captureWidgetTree(&root);
    // Simulate a loaded map after the UI was captured. This value is domain
    // data and therefore must not be translated during a language switch.
    domainLabel->setText(QStringLiteral("Marine"));

    DynamicUiTestTranslator translator;
    qApp->installTranslator(&translator);
    sc2dh::app::TranslationManager::retranslateWidgetTree(&root);
    QCOMPARE(staticLabel->text(), QStringLiteral("Localized caption"));
    QCOMPARE(tabs->tabText(0), QStringLiteral("Localized overview"));
    QCOMPARE(model->headerData(0, Qt::Horizontal).toString(), QStringLiteral("Localized object"));
    QCOMPARE(model->headerData(1, Qt::Horizontal).toString(), QStringLiteral("Localized status"));
    QCOMPARE(domainLabel->text(), QStringLiteral("Marine"));

    DataNode mapNode;
    mapNode.id = QStringLiteral("Marine");
    mapNode.candidateUnused = true;
    ObjectTableModel objectModel;
    objectModel.setNodes({mapNode});
    QCOMPARE(objectModel.headerData(ObjectTableModel::StatusColumn, Qt::Horizontal, Qt::DisplayRole).toString(),
             QStringLiteral("Localized status"));
    QCOMPARE(objectModel.data(objectModel.index(0, ObjectTableModel::StatusColumn)).toString(),
             QStringLiteral("Localized unused candidate"));
    QCOMPARE(objectModel.data(objectModel.index(0, ObjectTableModel::IdColumn)).toString(),
             QStringLiteral("Marine"));

    qApp->removeTranslator(&translator);
    sc2dh::app::TranslationManager::retranslateWidgetTree(&root);
    QCOMPARE(staticLabel->text(), QStringLiteral("Static caption"));
    QCOMPARE(tabs->tabText(0), QStringLiteral("Overview"));
    QCOMPARE(model->headerData(0, Qt::Horizontal).toString(), QStringLiteral("Object"));
    QCOMPARE(model->headerData(1, Qt::Horizontal).toString(), QStringLiteral("Status"));
    QCOMPARE(domainLabel->text(), QStringLiteral("Marine"));
    QCOMPARE(objectModel.headerData(ObjectTableModel::StatusColumn, Qt::Horizontal, Qt::DisplayRole).toString(),
             QStringLiteral("Status"));
    QCOMPARE(objectModel.data(objectModel.index(0, ObjectTableModel::StatusColumn)).toString(),
             QStringLiteral("Unused candidate"));
}

void CoreTests::dryRunGeneration()
{
    qInfo("dryRunGeneration");
    QTemporaryDir tempDir;
    QVERIFY2(tempDir.isValid(), "Temporary directory creation failed");

    QString rootFolder;
    QVERIFY2(createSampleFolder(&tempDir, &rootFolder), "Failed to create sample folder");

    FolderAnalyzer analyzer;
    AnalysisResult result;
    QString errorMessage;
    QVERIFY2(analyzer.analyzeFolder(rootFolder, QSet<QString>{}, &result, &errorMessage), qPrintable(errorMessage));

    QVector<int> selectedRows;
    selectedRows.append(2);
    selectedRows.append(4);

    const QString report = analyzer.buildDryRunReport(result, selectedRows);
    QVERIFY(report.contains(QStringLiteral("Optimization Preview")));
    QVERIFY(report.contains(QStringLiteral("Selected nodes: 2")));
    QVERIFY(report.contains(QStringLiteral("Estimated removed nodes: 2")));
    QVERIFY(report.contains(QStringLiteral("Duplicate rows affected: 0")));
    QVERIFY(report.contains(QStringLiteral("GameData/A.xml")));
    QVERIFY(report.contains(QStringLiteral("GameData/C.xml")));
}

void CoreTests::selectedNodeRemoval()
{
    qInfo("selectedNodeRemoval");
    QTemporaryDir tempDir;
    QVERIFY2(tempDir.isValid(), "Temporary directory creation failed");

    QString rootFolder;
    QVERIFY2(createSampleFolder(&tempDir, &rootFolder), "Failed to create sample folder");

    FolderAnalyzer analyzer;
    AnalysisResult result;
    QString errorMessage;
    QVERIFY2(analyzer.analyzeFolder(rootFolder, QSet<QString>{}, &result, &errorMessage), qPrintable(errorMessage));

    QVector<int> selectedRows;
    selectedRows.append(0);

    QString backupFolder;
    QStringList changedFiles;
    int removedNodes = 0;
    int skippedNodes = 0;
    QVERIFY2(analyzer.applySelectedChanges(result,
                                           selectedRows,
                                           rootFolder,
                                           QSet<QString>{},
                                           &backupFolder,
                                           &errorMessage,
                                           &changedFiles,
                                           &removedNodes,
                                           &skippedNodes),
             qPrintable(errorMessage));

    QCOMPARE(removedNodes, 1);
    QCOMPARE(skippedNodes, 0);
    QCOMPARE(changedFiles.size(), 1);
    QVERIFY(QFileInfo(backupFolder).exists());
    QVERIFY(QFile::exists(QDir(backupFolder).absoluteFilePath(QStringLiteral("GameData/A.xml"))));

    QFile rewritten(QDir(rootFolder).absoluteFilePath(QStringLiteral("GameData/A.xml")));
    QVERIFY2(rewritten.open(QIODevice::ReadOnly), "Failed to open rewritten file");
    const QByteArray rewrittenBytes = rewritten.readAll();
    QVector<DataNode> afterNodes;
    XmlLoader loader;
    QVERIFY2(loader.extractNodes(QStringLiteral("GameData/A.xml"), rewrittenBytes, &afterNodes, &errorMessage), qPrintable(errorMessage));
    QCOMPARE(afterNodes.size(), 2);
    int localDupCount = 0;
    int unitACount = 0;
    for (const DataNode &node : afterNodes) {
        if (node.id == QStringLiteral("LocalDup")) {
            ++localDupCount;
        }
        if (node.id == QStringLiteral("UnitA")) {
            ++unitACount;
        }
    }
    QCOMPARE(localDupCount, 1);
    QCOMPARE(unitACount, 1);
}

void CoreTests::saveFailureSafety()
{
    qInfo("saveFailureSafety");
    QTemporaryDir tempDir;
    QVERIFY2(tempDir.isValid(), "Temporary directory creation failed");

    QString rootFolder;
    QVERIFY2(createSampleFolder(&tempDir, &rootFolder), "Failed to create sample folder");

    const QString targetPath = QDir(rootFolder).absoluteFilePath(QStringLiteral("GameData/A.xml"));
    QFile lockedFile(targetPath);
    QVERIFY2(lockedFile.open(QIODevice::ReadOnly), "Failed to open file before locking");
    lockedFile.close();
    QVERIFY(QFile::setPermissions(targetPath, QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther));

    QFile originalFile(targetPath);
    QVERIFY2(originalFile.open(QIODevice::ReadOnly), "Failed to open original file");
    const QByteArray beforeBytes = originalFile.readAll();
    originalFile.close();

    FolderAnalyzer analyzer;
    AnalysisResult result;
    QString errorMessage;
    QVERIFY2(analyzer.analyzeFolder(rootFolder, QSet<QString>{}, &result, &errorMessage), qPrintable(errorMessage));

    QVector<int> selectedRows;
    selectedRows.append(0);

    QString backupFolder;
    QStringList changedFiles;
    int removedNodes = 0;
    int skippedNodes = 0;
    const bool success = analyzer.applySelectedChanges(result,
                                                       selectedRows,
                                                       rootFolder,
                                                       QSet<QString>{},
                                                       &backupFolder,
                                                       &errorMessage,
                                                       &changedFiles,
                                                       &removedNodes,
                                                       &skippedNodes);

    QVERIFY(!success);

    QFile afterFile(targetPath);
    QVERIFY2(afterFile.open(QIODevice::ReadOnly), "Failed to reopen original file after failure");
    const QByteArray afterBytes = afterFile.readAll();
    afterFile.close();
    QCOMPARE(afterBytes, beforeBytes);
}

void CoreTests::removeMultipleSameNameSiblingsWithoutIndexShift()
{
    const QByteArray xml = QByteArrayLiteral(
        "<Root><Actor id=\"A\"/><Actor id=\"B\"/><Actor id=\"C\"/><Actor id=\"D\"/></Root>");
    XmlLoader loader;
    QByteArray rewritten;
    QString error;
    QVERIFY2(loader.removeNodesByLocation(xml,
                                          {QStringLiteral("/Root[1]/Actor[2]"),
                                           QStringLiteral("/Root[1]/Actor[4]")},
                                          &rewritten, &error),
             qPrintable(error));
    const QString result = QString::fromUtf8(rewritten);
    QVERIFY(result.contains(QStringLiteral("id=\"A\"")));
    QVERIFY(!result.contains(QStringLiteral("id=\"B\"")));
    QVERIFY(result.contains(QStringLiteral("id=\"C\"")));
    QVERIFY(!result.contains(QStringLiteral("id=\"D\"")));
}

void CoreTests::archiveAnalysis()
{
    qInfo("archiveAnalysis");

    const QString archivePath = QStringLiteral("C:/Users/Vladimir/Downloads/Regenerate_trigger/TriggerCustom/comp/Эпические Битвы с Боссами.SC2Map");
    if (!QFileInfo::exists(archivePath)) {
        QSKIP("Sample archive is not available on this machine.");
    }

    Sc2Archive archive;
    QString errorMessage;
    QVERIFY2(archive.load(archivePath, &errorMessage), qPrintable(errorMessage));
    QVERIFY(archive.totalEntriesCount() > 0);
    QVERIFY(!archive.gameDataXmlEntries().isEmpty());

    QByteArray xmlBytes;
    QVERIFY2(archive.readEntry(archive.gameDataXmlEntries().front(), &xmlBytes, &errorMessage), qPrintable(errorMessage));
    QVERIFY(xmlBytes.startsWith("<?xml"));
}

void CoreTests::archiveRewriteRoundTrip()
{
    const QString sourcePath = QStringLiteral("C:/Users/Vladimir/Downloads/Regenerate_trigger/TriggerCustom/comp/1212_EN.SC2Map");
    if (!QFileInfo::exists(sourcePath)) QSKIP("SC2 archive rewrite fixture is not available.");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString working = QDir(directory.path()).absoluteFilePath(QStringLiteral("source.SC2Map"));
    const QString output = QDir(directory.path()).absoluteFilePath(QStringLiteral("rewritten.SC2Map"));
    QVERIFY(QFile::copy(sourcePath, working));
    Sc2Archive archive;
    QString error;
    QVERIFY2(archive.load(working, &error), qPrintable(error));
    QByteArray preload;
    QVERIFY2(archive.readEntry(QStringLiteral("Preload.xml"), &preload, &error), qPrintable(error));
    QVERIFY(!preload.isEmpty());
    QVERIFY2(archive.saveCopy(output, {{QStringLiteral("Preload.xml"), preload}}, {}, &error), qPrintable(error));
    Sc2Archive verified;
    QVERIFY2(verified.load(output, &error), qPrintable(error));
    QByteArray roundTrip;
    QVERIFY2(verified.readEntry(QStringLiteral("Preload.xml"), &roundTrip, &error), qPrintable(error));
    QCOMPARE(roundTrip, preload);
    QCOMPARE(verified.totalEntriesCount(), archive.totalEntriesCount());
}

void CoreTests::archiveDataCollectionCreatesFileAndListfile()
{
    const QString sourcePath = QStringLiteral("C:/Users/Vladimir/Downloads/Regenerate_trigger/TriggerCustom/comp/1212_EN.SC2Map");
    if (!QFileInfo::exists(sourcePath)) QSKIP("SC2 archive Data Collection fixture is not available.");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Sc2Archive archive;
    QString error;
    QVERIFY2(archive.load(sourcePath, &error), qPrintable(error));
    QByteArray listfile;
    QVERIFY2(archive.readEntry(QStringLiteral("(listfile)"), &listfile, &error), qPrintable(error));
    const QString collectionEntry = QStringLiteral("Base.SC2Data\\GameData\\DataCollectionData.xml");
    if (!listfile.endsWith('\n')) listfile.append("\r\n");
    listfile.append(collectionEntry.toUtf8() + QByteArrayLiteral("\r\n"));
    const QByteArray collectionXml = QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        "<Catalog><CDataCollectionUnit id=\"SC2DH\"><EditorCategories value=\"\"/>"
        "<DataRecord Entry=\"Unit,SC2DH@Unit\"/></CDataCollectionUnit></Catalog>\r\n");
    const QHash<QString, QByteArray> replacements{{collectionEntry, collectionXml},
                                                   {QStringLiteral("(listfile)"), listfile}};
    const QString output = QDir(directory.path()).absoluteFilePath(QStringLiteral("collection.SC2Map"));
    QVERIFY2(archive.saveCopy(output, replacements, {}, &error), qPrintable(error));
    Sc2Archive verified;
    QVERIFY2(verified.load(output, &error), qPrintable(error));
    QByteArray verifiedCollection;
    QVERIFY2(verified.readEntry(collectionEntry, &verifiedCollection, &error), qPrintable(error));
    QCOMPARE(verifiedCollection, collectionXml);
    QByteArray verifiedListfile;
    QVERIFY2(verified.readEntry(QStringLiteral("(listfile)"), &verifiedListfile, &error), qPrintable(error));
    QVERIFY(QString::fromUtf8(verifiedListfile).contains(QStringLiteral("DataCollectionData.xml")));
}

void CoreTests::archiveSaveCopyRemovesEntriesAndUpdatesListfile()
{
#ifndef SC2DH_USE_STORMLIB
    QSKIP("StormLib archive writer is unavailable.");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString source = QDir(directory.path()).absoluteFilePath(QStringLiteral("cleanup_source.SC2Map"));
    const QString output = QDir(directory.path()).absoluteFilePath(QStringLiteral("cleanup_output.SC2Map"));
    const QString unitEntry = QStringLiteral("Base.SC2Data/GameData/UnitData.xml");
    const QString addedEntry = QStringLiteral("scripts/generated_cleanup.galaxy");
    const QString removedAsset = QStringLiteral("Assets/UnusedTexture.dds");
    const QString removedTrash = QStringLiteral("debug/temp_build.log");
    const QByteArray originalXml = QByteArrayLiteral("<Catalog><CUnit id=\"Before\"/></Catalog>");
    const QByteArray rewrittenXml = QByteArrayLiteral("<Catalog><CUnit id=\"After\"/></Catalog>");
    QString error;

    QVERIFY2(createTestMpqArchive(source,
                                  {
                                      {QStringLiteral("Objects"), QByteArrayLiteral("ObjectUnit { Id = 1 }\n")},
                                      {unitEntry, originalXml},
                                      {removedAsset, QByteArrayLiteral("unused texture payload")},
                                      {removedTrash, QByteArrayLiteral("temporary debug payload")}
                                  },
                                  &error),
             qPrintable(error));

    Sc2Archive archive;
    QVERIFY2(archive.load(source, &error), qPrintable(error));
    QVERIFY2(archive.saveCopy(output,
                              {
                                  {unitEntry, rewrittenXml},
                                  {addedEntry, QByteArrayLiteral("void SC2DH_CleanupGenerated() {}\n")}
                              },
                              {removedAsset, removedTrash},
                              &error),
             qPrintable(error));

    Sc2Archive original;
    QVERIFY2(original.load(source, &error), qPrintable(error));
    QByteArray originalRoundTrip;
    QVERIFY2(original.readEntry(unitEntry, &originalRoundTrip, &error), qPrintable(error));
    QCOMPARE(originalRoundTrip, originalXml);
    QByteArray originalRemoved;
    QVERIFY2(original.readEntry(removedAsset, &originalRemoved, &error), qPrintable(error));

    Sc2Archive verified;
    QVERIFY2(verified.load(output, &error), qPrintable(error));
    QByteArray actualXml;
    QVERIFY2(verified.readEntry(unitEntry, &actualXml, &error), qPrintable(error));
    QCOMPARE(actualXml, rewrittenXml);
    QByteArray added;
    QVERIFY2(verified.readEntry(addedEntry, &added, &error), qPrintable(error));
    QVERIFY(QString::fromUtf8(added).contains(QStringLiteral("SC2DH_CleanupGenerated")));

    QByteArray removedBytes;
    QString removeReadError;
    QVERIFY(!verified.readEntry(removedAsset, &removedBytes, &removeReadError));
    QVERIFY(!verified.readEntry(removedTrash, &removedBytes, &removeReadError));

    QByteArray listfile;
    QVERIFY2(verified.readEntry(QStringLiteral("(listfile)"), &listfile, &error), qPrintable(error));
    const QString listfileText = QString::fromUtf8(listfile).replace(QLatin1Char('\\'), QLatin1Char('/'));
    QVERIFY(listfileText.contains(unitEntry));
    QVERIFY(listfileText.contains(addedEntry));
    QVERIFY(!listfileText.contains(removedAsset));
    QVERIFY(!listfileText.contains(removedTrash));
#endif
}

QTEST_MAIN(CoreTests)
#include "test_core.moc"
