#include <QtTest/QtTest>

#include "core/BackupManager.h"
#include "core/FolderAnalyzer.h"
#include "core/MergeService.h"
#include "core/ReferenceRenamer.h"
#include "core/StandardNamePlanner.h"
#include "core/UnitFamilyDetector.h"
#include "core/DataCollectionAliasMapper.h"
#include "core/DataCollectionPreservation.h"
#include "core/DataCollectionUnitBuilder.h"
#include "core/DeepCleanupService.h"
#include "core/Sc2Archive.h"
#include "core/XmlLoader.h"
#include "ui/ObjectFilterProxyModel.h"
#include "ui/ObjectTableModel.h"

#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QIODevice>
#include <QSet>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QStringList>

#include <optional>
#include <pugixml.hpp>

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

} // namespace

class CoreTests : public QObject
{
    Q_OBJECT

private slots:
    void folderScanAndAnalysis();
    void xmlParseAndLookup();
    void duplicateIdDetection();
    void duplicateContentDetection();
    void duplicateBodyRequiresSameTypeAndExactNestedBody();
    void backupCreation();
    void dryRunGeneration();
    void selectedNodeRemoval();
    void removeMultipleSameNameSiblingsWithoutIndexShift();
    void saveFailureSafety();
    void archiveAnalysis();
    void archiveRewriteRoundTrip();
    void archiveDataCollectionCreatesFileAndListfile();
    void objectFileFilterUsesFullSourcePath();
    void normalizedDuplicateIgnoresOnlyRootIdentity();
    void tokenAwareReplacementVariants();
    void mergePreviewAndApplyRedirectBeforeDelete();
    void mergeRollbackOnFailure();
    void unusedSafetyClassification();
    void unusedReachabilityDistinguishesStatesAndPaths();
    void unusedDeletionRemovesWholeUnusedChain();
    void unusedDeletionSkipsPartialChainWithoutFailingBatch();
    void unusedDeletionPreservesDataCollectionLinks();
    void deepCleanupAppliesSafeCandidates();
    void unitFamilyDetectionAndStandardPlanning();
    void renamePlannerBlocksConflicts();
    void referenceRenamePreviewAndApply();
    void referenceRenameRollback();
    void dataCollectionAliasMapping();
    void dataCollectionCreatePreviewAndApply();
    void dataCollectionOffersSingleCustomUnit();
    void dataCollectionFallbackDetectsCustomFamiliesWithoutAtSign();
    void dataCollectionUnitAbilWeaponModeSplitsRoots();
    void dataCollectionTypedPreservesLegacyNonScopedAbilityRecords();
    void dataCollectionTypedSplitPreservesEveryCatalogRecord();
    void dataCollectionPreservationRestoresLossyXml();
    void dataCollectionMigrationPreservesExistingTargetRecords();
    void dataCollectionTypedSplitPreservesSharedMemberships();
    void dataCollectionMigrationRollbackRestoresAllCollections();
    void dataCollectionPatternInheritanceValidation();
    void dataCollectionEntityRootsAndConflicts();
    void gargantuaReferenceFixture();
    void gargantuaApplyFixture();
    void zombieWorldUpdate3Audit();
    void dataCollectionUpdatePreservesAndSorts();
    void dataCollectionRollback();
    void folderAnalysisStoresFullXmlSource();
    void folderAnalysisCanBeCancelled();
    void unrelatedIdenticalBodiesAreAllowed();
};

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
    QVERIFY(!group.mergeCandidate);
    QCOMPARE(group.commonIdMask, QStringLiteral("unrelated IDs"));
    for (const DataNode &node : analysis.nodes) QVERIFY(!node.duplicateContent);
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

void CoreTests::dataCollectionTypedSplitPreservesSharedMemberships()
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
    QVERIFY(contains(QStringLiteral("AbilityTwo"), QStringLiteral("SharedEffect")));
    const auto ability = std::find_if(families.cbegin(), families.cend(), [](const UnitFamily &family) {
        return family.rootId == QStringLiteral("AbilityOne");
    });
    QVERIFY(ability != families.cend());
    const auto shared = std::find_if(ability->objects.cbegin(), ability->objects.cend(), [&](const UnitFamilyObject &object) {
        return analysis.nodes[object.nodeIndex].id == QStringLiteral("SharedEffect");
    });
    QVERIFY(shared != ability->objects.cend());
    QCOMPARE(shared->role, UnitFamilyRole::Effect);
    QCOMPARE(shared->confidence, QStringLiteral("Shared"));
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
    QCOMPARE(updatedXml.count("Entry=\"Effect,SharedEffect\""), 3);
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

void CoreTests::unitFamilyDetectionAndStandardPlanning()
{
    QTemporaryDir dir;
    const QByteArray xml = QByteArrayLiteral(
        "<Catalog>"
        "<CUnit id=\"Vassel\" actor=\"ActorVassel\" button=\"ButtonVassel\" extra=\"EffectVassel\" ambiguous=\"OddFamilyThing\"/>"
        "<CActorUnit id=\"ActorVassel\" unitName=\"Vassel\" model=\"ModelVassel\" sound=\"AttackVassel\"/>"
        "<CButton id=\"ButtonVassel\"/>"
        "<CModel id=\"ModelVassel\"/>"
        "<CSound id=\"AttackVassel\"/>"
        "<CEffect id=\"EffectVassel\"/>"
        "<CActor id=\"OddFamilyThing\"/>"
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
    QCOMPARE(objects[QStringLiteral("ButtonVassel")].role, UnitFamilyRole::Button);
    QCOMPARE(objects[QStringLiteral("ModelVassel")].role, UnitFamilyRole::Model);
    QCOMPARE(objects[QStringLiteral("AttackVassel")].role, UnitFamilyRole::Attack);
    QCOMPARE(objects[QStringLiteral("EffectVassel")].role, UnitFamilyRole::Effect);
    QCOMPARE(objects[QStringLiteral("OddFamilyThing")].role, UnitFamilyRole::ManualReview);

    StandardNamePlanner planner;
    const RenamePlan plan = planner.plan(analysis, family, QStringLiteral("Vassel"));
    QHash<QString, QString> proposals;
    for (const RenamePlanItem &item : plan.items) proposals.insert(item.oldId, item.newId);
    QCOMPARE(proposals[QStringLiteral("ActorVassel")], QStringLiteral("Vassel@Actor"));
    QCOMPARE(proposals[QStringLiteral("ButtonVassel")], QStringLiteral("Vassel@Button"));
    QCOMPARE(proposals[QStringLiteral("ModelVassel")], QStringLiteral("Vassel@Model"));
    QCOMPARE(proposals[QStringLiteral("AttackVassel")], QStringLiteral("Vassel@Attack"));
    QCOMPARE(proposals[QStringLiteral("EffectVassel")], QStringLiteral("Vassel@Effect"));
    QVERIFY(!proposals.contains(QStringLiteral("Vassel"))); // already standard
    QVERIFY(!plan.manualReview.isEmpty());
}

void CoreTests::renamePlannerBlocksConflicts()
{
    QTemporaryDir dir;
    QVERIFY(writeTextFile(QDir(dir.path()).absoluteFilePath(QStringLiteral("Conflict.xml")), QByteArrayLiteral(
        "<Catalog><CUnit id=\"Vassel\" a=\"ActorVassel\" b=\"ButtonVassel\" c=\"VasselButtonAlt\"/>"
        "<CActorUnit id=\"ActorVassel\" unitName=\"Vassel\"/><CActor id=\"Vassel@Actor\"/>"
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

void CoreTests::referenceRenamePreviewAndApply()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).absoluteFilePath(QStringLiteral("Family.xml"));
    const QByteArray original = QByteArrayLiteral(
        "<Catalog><CUnit id=\"Vassel\" actor=\"ActorVassel\"/>"
        "<CActorUnit id=\"ActorVassel\" unitName=\"Vassel\"><Event>Unit,Vassel ActorVassel ActorVasselExtra</Event></CActorUnit>"
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
    QVERIFY(preview.referencesUpdated >= 4);
    QFile unchanged(path); QVERIFY(unchanged.open(QIODevice::ReadOnly)); QCOMPARE(unchanged.readAll(), original); unchanged.close();
    const RenameApplyResult applied = renamer.apply(analysis, plan, dir.path(), {});
    QVERIFY2(applied.success, qPrintable(applied.error));
    QVERIFY(QFileInfo(applied.backupFolder).exists());
    QFile rewritten(path); QVERIFY(rewritten.open(QIODevice::ReadOnly));
    const QString output = QString::fromUtf8(rewritten.readAll());
    QVERIFY(output.contains(QStringLiteral("id=\"Vessel\"")));
    QVERIFY(output.contains(QStringLiteral("id=\"Vessel@Actor\"")));
    QVERIFY(output.contains(QStringLiteral("unitName=\"Vessel\"")));
    QVERIFY(output.contains(QStringLiteral("Unit,Vessel Vessel@Actor ActorVasselExtra")));
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

void CoreTests::deepCleanupAppliesSafeCandidates()
{
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("GameData")));
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("Assets")));
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("enUS.SC2Data/LocalizedData")));
    const QString xmlPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("GameData/Data.xml"));
    const QString locPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("enUS.SC2Data/LocalizedData/GameStrings.txt"));
    const QString assetPath = QDir(dir.path()).absoluteFilePath(QStringLiteral("Assets/Unused.dds"));
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
    QVERIFY(writeTextFile(locPath, QByteArrayLiteral("Unit/Name/MissingUnit=Old name\r\nUnit/Name/ExistingFx=Keep\r\n")));
    QVERIFY(writeTextFile(assetPath, QByteArrayLiteral("unused asset bytes")));
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
    for (const DeepCleanupCandidate &candidate : analysis.deepCleanupCandidates) {
        QCOMPARE_NE(QFileInfo(candidate.filePath).fileName(), QStringLiteral("Minimap.tga"));
        QCOMPARE_NE(QFileInfo(candidate.filePath).fileName(), QStringLiteral("LightingMap.tga"));
        QCOMPARE_NE(QFileInfo(candidate.filePath).fileName(), QStringLiteral("PreloadAssetDB.txt"));
    }

    QVector<int> selected;
    for (const DeepCleanupCandidate &candidate : analysis.deepCleanupCandidates)
        if (candidate.state == CandidateState::Safe && candidate.recommended && candidate.action != DeepCleanupAction::ReportOnly)
            selected.append(candidate.index);
    QVERIFY(!selected.isEmpty());

    const DeepCleanupApplyResult applied = DeepCleanupService().apply(analysis, selected, dir.path(), true);
    QVERIFY2(applied.success, qPrintable(applied.error));
    QVERIFY(applied.filesDeleted >= 1);
    QVERIFY(applied.textLinesRemoved >= 1);
    QVERIFY(applied.xmlAttributesRemoved >= 1);
    QVERIFY(applied.xmlNodesRemoved >= 1);
    QVERIFY(!QFileInfo::exists(assetPath));
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

QTEST_MAIN(CoreTests)
#include "test_core.moc"
