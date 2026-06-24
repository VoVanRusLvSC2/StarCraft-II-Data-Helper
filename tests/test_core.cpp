#include <QtTest/QtTest>

#include "core/BackupManager.h"
#include "core/FolderAnalyzer.h"
#include "core/MergeService.h"
#include "core/ReferenceRenamer.h"
#include "core/StandardNamePlanner.h"
#include "core/UnitFamilyDetector.h"
#include "core/DataCollectionAliasMapper.h"
#include "core/DataCollectionUnitBuilder.h"
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
    void unusedDeletionRemovesDataCollectionLinks();
    void unitFamilyDetectionAndStandardPlanning();
    void renamePlannerBlocksConflicts();
    void referenceRenamePreviewAndApply();
    void referenceRenameRollback();
    void dataCollectionAliasMapping();
    void dataCollectionCreatePreviewAndApply();
    void dataCollectionOffersSingleCustomUnit();
    void dataCollectionFallbackDetectsCustomFamiliesWithoutAtSign();
    void dataCollectionUnitAbilWeaponModeSplitsRoots();
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
    const QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(analysis);
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

    const QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(analysis);
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
    QCOMPARE(unit->recommendedParent, QStringLiteral("UnitBase"));
    QCOMPARE(ability->recommendedParent, QStringLiteral("AbilityBase"));
    QCOMPARE(weapon->recommendedParent, QStringLiteral("WeaponBase"));
    DataCollectionBuildRequest request; request.family = *unit;
    const DataCollectionPreviewReport preview = DataCollectionUnitBuilder().preview(analysis, request);
    QVERIFY(preview.recordsToRemove.contains(QStringLiteral("Abil,Gargantua_Jump")));
    QVERIFY(preview.recordsToRemove.contains(QStringLiteral("Weapon,Gargantua_Weapon")));
    const DataCollectionApplyResult applied = DataCollectionUnitBuilder().apply(analysis, request, dir.path(), {});
    QVERIFY2(applied.success, qPrintable(applied.error));
    QCOMPARE(applied.recordsRemoved, 2);
    QFile updated(path); QVERIFY(updated.open(QIODevice::ReadOnly)); const QByteArray updatedXml = updated.readAll();
    QVERIFY(!updatedXml.contains("Entry=\"Abil,Gargantua_Jump\""));
    QVERIFY(!updatedXml.contains("Entry=\"Weapon,Gargantua_Weapon\""));
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
        "<CDataCollectionUnit id=\"OtherFamily\"><DataRecord Entry=\"Other,Untouched\" custom=\"yes\"/></CDataCollectionUnit>"
        "<CDataCollectionUnit id=\"Vassel\" parent=\"CustomParent\" custom=\"keep\"><EditorCategories value=\"ExistingCategories\"/><Metadata value=\"PreserveNode\"/>"
        "<DataRecord Entry=\"Actor,Vassel@Actor\" custom=\"preserve-attribute\"/><DataRecord Entry=\"Other,PreserveMe\"/><DataRecord Entry=\"Actor,Vassel@Actor\"/></CDataCollectionUnit></Catalog>")));
    FolderAnalyzer analyzer; AnalysisResult analysis; QString error; QVERIFY(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error));
    DataCollectionBuildRequest request; request.family = UnitFamilyDetector().detectCollectionFamilies(analysis).front();
    request.parent = QStringLiteral("CustomParent"); request.editorCategories = QStringLiteral("ExistingCategories");
    DataCollectionUnitBuilder builder; const DataCollectionPreviewReport preview = builder.preview(analysis, request);
    QVERIFY(preview.existingCollection);
    QVERIFY(preview.existingRecordsPreserved.contains(QStringLiteral("Other,PreserveMe")));
    QVERIFY(preview.duplicateRecordsSkipped.contains(QStringLiteral("Actor,Vassel@Actor")));
    const DataCollectionApplyResult applied = builder.apply(analysis, request, dir.path(), {});
    QVERIFY2(applied.success, qPrintable(applied.error));
    QFile output(path); QVERIFY(output.open(QIODevice::ReadOnly)); const QString xml = QString::fromUtf8(output.readAll());
    QCOMPARE(xml.count(QStringLiteral("Actor,Vassel@Actor")), 1);
    QCOMPARE(xml.count(QStringLiteral("Other,PreserveMe")), 1);
    QVERIFY(xml.contains(QStringLiteral("parent=\"CustomParent\"")));
    QVERIFY(xml.contains(QStringLiteral("value=\"ExistingCategories\"")));
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
    QCOMPARE(proposals[QStringLiteral("ActorVassel")], QStringLiteral("VasselActor"));
    QCOMPARE(proposals[QStringLiteral("ButtonVassel")], QStringLiteral("VasselButton"));
    QCOMPARE(proposals[QStringLiteral("ModelVassel")], QStringLiteral("VasselModel"));
    QCOMPARE(proposals[QStringLiteral("AttackVassel")], QStringLiteral("VasselAttack"));
    QCOMPARE(proposals[QStringLiteral("EffectVassel")], QStringLiteral("VasselEffect"));
    QVERIFY(!proposals.contains(QStringLiteral("Vassel"))); // already standard
    QVERIFY(!plan.manualReview.isEmpty());
}

void CoreTests::renamePlannerBlocksConflicts()
{
    QTemporaryDir dir;
    QVERIFY(writeTextFile(QDir(dir.path()).absoluteFilePath(QStringLiteral("Conflict.xml")), QByteArrayLiteral(
        "<Catalog><CUnit id=\"Vassel\" a=\"ActorVassel\" b=\"ButtonVassel\" c=\"VasselButtonAlt\"/>"
        "<CActorUnit id=\"ActorVassel\" unitName=\"Vassel\"/><CActor id=\"VasselActor\"/>"
        "<CButton id=\"ButtonVassel\"/><CButton id=\"VasselButtonAlt\"/></Catalog>")));
    FolderAnalyzer analyzer; AnalysisResult analysis; QString error;
    QVERIFY(analyzer.analyzeFolder(dir.path(), {}, &analysis, &error));
    const UnitFamily family = UnitFamilyDetector().detect(analysis).front();
    const RenamePlan plan = StandardNamePlanner().plan(analysis, family, QStringLiteral("Vassel"));
    QVERIFY(!plan.valid);
    QVERIFY(!plan.conflicts.isEmpty());
    bool targetConflict = false, duplicateConflict = false;
    for (const QString &conflict : plan.conflicts) {
        targetConflict |= conflict.contains(QStringLiteral("already exists"));
        duplicateConflict |= conflict.contains(QStringLiteral("Duplicate proposed"));
    }
    QVERIFY(targetConflict);
    QVERIFY(duplicateConflict);
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
    QVERIFY(output.contains(QStringLiteral("id=\"VesselActor\"")));
    QVERIFY(output.contains(QStringLiteral("unitName=\"Vessel\"")));
    QVERIFY(output.contains(QStringLiteral("Unit,Vessel VesselActor ActorVasselExtra")));
    QVERIFY(!output.contains(QLatin1Char('@')));
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

void CoreTests::unusedDeletionRemovesDataCollectionLinks()
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
    QVERIFY(!xml.contains("Unit,UnusedUnit"));
    QCOMPARE(removed, 1);
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
    QVERIFY(QFile::exists(QDir(backupFolder).absoluteFilePath(QStringLiteral("analysis_report.txt"))));
    QVERIFY(QFile::exists(QDir(backupFolder).absoluteFilePath(QStringLiteral("planned_changes_report.txt"))));
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
