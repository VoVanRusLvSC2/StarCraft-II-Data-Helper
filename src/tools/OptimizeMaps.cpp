#include "core/BackupManager.h"
#include "core/ArchiveReferenceRewriter.h"
#include "core/CatalogEnumRepair.h"
#include "core/DataCollectionAliasMapper.h"
#include "core/DataCollectionPreservation.h"
#include "core/DataCollectionUnitBuilder.h"
#include "core/DeepCleanupService.h"
#include "core/FolderAnalyzer.h"
#include "core/ReferenceRenamer.h"
#include "core/Sc2Archive.h"
#include "core/StandardNamePlanner.h"
#include "core/UnitFamilyDetector.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QTextStream>

#include <pugixml.hpp>

#include <algorithm>
#include <sstream>

namespace {

constexpr int kRenamePassLimit = 1;

struct MaterializeStats
{
    int archiveEntries = 0;
    int xmlMaterialized = 0;
    int dataCollectionXmlSkipped = 0;
};

struct ArchiveReferenceStats
{
    bool complete = true;
    int entriesScanned = 0;
    int idsFound = 0;
    QSet<QString> referencedIds;
};

struct CollectionStats
{
    int files = 0;
    int collections = 0;
    int directRecords = 0;
    QSet<QString> pairs;
    QSet<QString> entries;
};

struct MapReport
{
    QString mapPath;
    QString backupPath;
    bool success = false;
    QString error;
    qint64 beforeBytes = 0;
    qint64 afterBytes = 0;
    int archiveEntriesBefore = 0;
    int archiveEntriesAfter = 0;
    int xmlMaterialized = 0;
    int dataCollectionXmlSkipped = 0;
    int archiveReferenceEntriesScanned = 0;
    int archiveReferenceIdsFound = 0;
    int objectsBefore = 0;
    int objectsAfter = 0;
    int safeUnusedBefore = 0;
    int unusedDeleted = 0;
    int unusedSkipped = 0;
    int deepCleanupCandidates = 0;
    int deepCleanupChanges = 0;
    int deepCleanupSkipped = 0;
    int renamePlansApplied = 0;
    int idsRenamed = 0;
    int referencesUpdated = 0;
    int renameSkippedArchiveRefs = 0;
    int collectionFamiliesDetected = 0;
    int collectionFamiliesApplied = 0;
    int collectionFamiliesSkipped = 0;
    int rootTypeConflicts = 0;
    int collectionRecordsAdded = 0;
    int collectionRecordsReorganized = 0;
    int finalCollectionFiles = 0;
    int finalCollections = 0;
    int finalCollectionRecords = 0;
    int finalCollectionUniquePairs = 0;
    int finalCollectionUniqueEntries = 0;
    int expectedCollectionPairs = 0;
    int missingExpectedCollectionPairs = 0;
    int extraCollectionPairs = 0;
    int finalSafeUnused = 0;
    int finalDuplicateMergeCandidates = 0;
};

bool writeBytes(const QString &path, const QByteArray &bytes, QString *error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(bytes) != bytes.size()
        || !file.commit()) {
        if (error)
            *error = QStringLiteral("Unable to write %1").arg(path);
        return false;
    }
    return true;
}

void logProgress(const QString &mapPath, const QString &stage, const QString &detail = {})
{
    QTextStream err(stderr);
    err << "progress map=" << QDir::toNativeSeparators(mapPath)
        << " stage=" << stage;
    if (!detail.isEmpty())
        err << " " << detail;
    err << '\n';
    err.flush();
}

bool readBytes(const QString &path, QByteArray *bytes, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Unable to read %1").arg(path);
        return false;
    }
    if (bytes)
        *bytes = file.readAll();
    return true;
}

QString firstExistingFile(const QStringList &candidates)
{
    for (const QString &candidate : candidates) {
        const QString cleaned = QDir::cleanPath(candidate);
        if (QFileInfo::exists(cleaned) && QFileInfo(cleaned).isFile())
            return cleaned;
    }
    return {};
}

QString schemaValidatorScriptPath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    return firstExistingFile({
        QDir(appDir).absoluteFilePath(QStringLiteral("scripts/validate_sc2_catalogs.py")),
        QDir(appDir).absoluteFilePath(QStringLiteral("../scripts/validate_sc2_catalogs.py")),
        QDir(appDir).absoluteFilePath(QStringLiteral("../../scripts/validate_sc2_catalogs.py")),
        QDir::current().absoluteFilePath(QStringLiteral("scripts/validate_sc2_catalogs.py"))
    });
}

QString catalogXsdPath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    return firstExistingFile({
        QDir(appDir).absoluteFilePath(QStringLiteral("resources/catalogsData.xsd")),
        QDir(appDir).absoluteFilePath(QStringLiteral("../resources/catalogsData.xsd")),
        QDir(appDir).absoluteFilePath(QStringLiteral("../../resources/catalogsData.xsd")),
        QDir::current().absoluteFilePath(QStringLiteral("resources/catalogsData.xsd"))
    });
}

QString compactProcessOutput(QString text, int maxChars = 2400)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    text = text.trimmed();
    if (text.size() > maxChars)
        text = text.left(maxChars) + QStringLiteral("\n...");
    return text;
}

bool validateArchiveCatalogSchema(const QString &archivePath, QString *error)
{
    const QString scriptPath = schemaValidatorScriptPath();
    const QString xsdPath = catalogXsdPath();
    if (scriptPath.isEmpty() || xsdPath.isEmpty())
        return true;

    QProcess process;
    process.setProgram(QStringLiteral("python"));
    process.setArguments({
        scriptPath,
        archivePath,
        QStringLiteral("--xsd"),
        xsdPath,
        QStringLiteral("--max-errors"),
        QStringLiteral("16")
    });
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForStarted(5000))
        return true;
    if (!process.waitForFinished(180000)) {
        process.kill();
        process.waitForFinished(5000);
        if (error)
            *error = QStringLiteral("XSD catalog validation timed out before archive save.");
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error) {
            const QString output = compactProcessOutput(QString::fromUtf8(process.readAll()));
            *error = QStringLiteral("XSD catalog validation failed before archive save:\n%1").arg(output);
        }
        return false;
    }
    return true;
}

QString normalizedArchiveName(QString name)
{
    return QDir::cleanPath(name).replace('\\', '/').toCaseFolded();
}

QString replacementKeyForArchiveEntry(const QHash<QString, QByteArray> &replacements, const QString &archiveEntry)
{
    const QString wanted = normalizedArchiveName(archiveEntry);
    for (auto it = replacements.cbegin(); it != replacements.cend(); ++it) {
        if (normalizedArchiveName(it.key()) == wanted)
            return it.key();
    }
    return {};
}

bool addCatalogEnumRepairs(const Sc2Archive &archive,
                           QHash<QString, QByteArray> *replacements,
                           int *repairCount,
                           QString *error)
{
    if (repairCount)
        *repairCount = 0;
    if (!replacements)
        return true;

    int total = 0;
    for (const QString &entry : archive.gameDataXmlEntries()) {
        const QString existingKey = replacementKeyForArchiveEntry(*replacements, entry);
        QByteArray bytes;
        const QString replacementKey = existingKey.isEmpty() ? entry : existingKey;
        if (existingKey.isEmpty()) {
            if (!archive.readEntry(entry, &bytes, error))
                return false;
        } else {
            bytes = replacements->value(existingKey);
        }

        int changes = 0;
        if (!sc2dh::repairKnownCatalogEnumDamage(&bytes, &changes, error))
            return false;
        if (changes <= 0)
            continue;
        replacements->insert(replacementKey, bytes);
        total += changes;
    }

    if (repairCount)
        *repairCount = total;
    return true;
}

bool isDataCollectionXmlEntry(const QString &entry)
{
    return QDir::cleanPath(entry).replace('\\', '/').endsWith(
        QStringLiteral("DataCollectionData.xml"), Qt::CaseInsensitive);
}

bool isDataCollectionName(const QString &name)
{
    return name.startsWith(QStringLiteral("CDataCollection"), Qt::CaseInsensitive)
        && !name.startsWith(QStringLiteral("CDataCollectionPattern"), Qt::CaseInsensitive);
}

int recordOrder(const QString &entry)
{
    static const QStringList order{QStringLiteral("Button"), QStringLiteral("Unit"), QStringLiteral("Actor"),
        QStringLiteral("Model"), QStringLiteral("Sound"), QStringLiteral("Weapon"), QStringLiteral("Abil"),
        QStringLiteral("Effect"), QStringLiteral("Behavior"), QStringLiteral("Validator"),
        QStringLiteral("Requirement"), QStringLiteral("Upgrade")};
    const QString catalog = entry.section(QLatin1Char(','), 0, 0);
    const int index = order.indexOf(catalog);
    return index < 0 ? order.size() : index;
}

void sortEntries(QStringList *entries)
{
    std::sort(entries->begin(), entries->end(), [](const QString &left, const QString &right) {
        const int leftOrder = recordOrder(left);
        const int rightOrder = recordOrder(right);
        if (leftOrder != rightOrder)
            return leftOrder < rightOrder;
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
}

void setXmlAttribute(pugi::xml_node node, const char *name, const QString &value)
{
    pugi::xml_attribute attribute = node.attribute(name);
    if (!attribute)
        attribute = node.append_attribute(name);
    attribute.set_value(value.toUtf8().constData());
}

pugi::xml_node findCollectionByTypeAndId(pugi::xml_node catalog, const QString &type, const QString &id)
{
    for (pugi::xml_node node = catalog.first_child(); node; node = node.next_sibling()) {
        if (QString::fromUtf8(node.name()).compare(type, Qt::CaseInsensitive) == 0
            && QString::fromUtf8(node.attribute("id").value()).compare(id, Qt::CaseInsensitive) == 0)
            return node;
    }
    return {};
}

pugi::xml_node findCollectionById(pugi::xml_node catalog, const QString &id)
{
    for (pugi::xml_node node = catalog.first_child(); node; node = node.next_sibling()) {
        const QString type = QString::fromUtf8(node.name());
        if (isDataCollectionName(type)
            && QString::fromUtf8(node.attribute("id").value()).compare(id, Qt::CaseInsensitive) == 0)
            return node;
    }
    return {};
}

pugi::xml_node retagCollection(pugi::xml_node catalog, pugi::xml_node oldCollection, const QString &newType)
{
    pugi::xml_node replacement = catalog.insert_child_before(newType.toUtf8().constData(), oldCollection);
    for (pugi::xml_attribute attribute : oldCollection.attributes())
        replacement.append_attribute(attribute.name()).set_value(attribute.value());
    for (pugi::xml_node child : oldCollection.children())
        replacement.append_copy(child);
    oldCollection.parent().remove_child(oldCollection);
    return replacement;
}

pugi::xml_node ensureCollectionNode(pugi::xml_node catalog, const QString &type, const QString &id)
{
    pugi::xml_node collection = findCollectionByTypeAndId(catalog, type, id);
    if (collection)
        return collection;
    pugi::xml_node typedDifferently = findCollectionById(catalog, id);
    if (typedDifferently)
        return retagCollection(catalog, typedDifferently, type);
    collection = catalog.append_child(type.toUtf8().constData());
    setXmlAttribute(collection, "id", id);
    return collection;
}

void ensureTemplate(pugi::xml_node catalog, const QString &type, const QString &id, const QString &parent = {})
{
    pugi::xml_node node = ensureCollectionNode(catalog, type, id);
    setXmlAttribute(node, "default", QStringLiteral("1"));
    if (!parent.isEmpty())
        setXmlAttribute(node, "parent", parent);
}

void ensureBasicDataCollectionSupport(pugi::xml_node catalog)
{
    ensureTemplate(catalog, QStringLiteral("CDataCollectionUnit"), QStringLiteral("UnitBase"));
    ensureTemplate(catalog, QStringLiteral("CDataCollectionUnit"), QStringLiteral("UnitGround"), QStringLiteral("UnitBase"));
    ensureTemplate(catalog, QStringLiteral("CDataCollectionUnit"), QStringLiteral("UnitAir"), QStringLiteral("UnitBase"));
    ensureTemplate(catalog, QStringLiteral("CDataCollectionAbil"), QStringLiteral("AbilityBase"));
    ensureTemplate(catalog, QStringLiteral("CDataCollectionAbil"), QStringLiteral("AbilityMisssile"), QStringLiteral("AbilityBase"));
    ensureTemplate(catalog, QStringLiteral("CDataCollection"), QStringLiteral("WeaponBase"));
    ensureTemplate(catalog, QStringLiteral("CDataCollection"), QStringLiteral("Weapon_Instant"), QStringLiteral("WeaponBase"));
    ensureTemplate(catalog, QStringLiteral("CDataCollection"), QStringLiteral("Weapon_Missile"), QStringLiteral("WeaponBase"));
}

QString defaultCategoriesForFamily(const UnitFamily &family)
{
    if (family.entityType == DataCollectionEntityType::Ability)
        return QStringLiteral("DataGroup:Ability,ObjectType:Other");
    if (family.entityType == DataCollectionEntityType::Weapon)
        return QStringLiteral("DataGroup:Weapon,ObjectType:Other");
    return QStringLiteral("DataGroup:Unit,ObjectType:Unit");
}

bool listfileContainsEntry(const QString &path, const QString &entry)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    const QString normalizedEntry = QDir::cleanPath(entry).replace('/', '\\');
    for (const QString &line : QString::fromUtf8(file.readAll()).split(
             QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts)) {
        if (QDir::cleanPath(line.trimmed()).replace('/', '\\').compare(normalizedEntry, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

bool isArchiveReferenceEntry(const QString &entry)
{
    const QString normalized = QDir::cleanPath(entry).replace('\\', '/').toLower();
    const QString name = normalized.section('/', -1);
    static const QSet<QString> exactNames = {
        QStringLiteral("objects"), QStringLiteral("units"), QStringLiteral("regions"),
        QStringLiteral("triggers"), QStringLiteral("mapinfo"), QStringLiteral("documentinfo"),
        QStringLiteral("preload.xml"), QStringLiteral("componentlist.sc2components")};
    if (exactNames.contains(name))
        return true;
    static const QSet<QString> extensions = {
        QStringLiteral("galaxy"), QStringLiteral("txt"), QStringLiteral("ini"),
        QStringLiteral("json"), QStringLiteral("yaml"), QStringLiteral("yml"),
        QStringLiteral("version"), QStringLiteral("sc2components")};
    return extensions.contains(QFileInfo(name).suffix().toLower());
}

void collectKnownIdTokens(const QByteArray &bytes, const QSet<QString> &knownIds, QSet<QString> *found)
{
    if (!found)
        return;
    const auto isIdChar = [](uchar value) {
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')
            || (value >= '0' && value <= '9') || value == '_' || value == '@';
    };
    for (qsizetype start = 0; start < bytes.size();) {
        while (start < bytes.size() && !isIdChar(uchar(bytes[start])))
            ++start;
        qsizetype end = start;
        while (end < bytes.size() && isIdChar(uchar(bytes[end])))
            ++end;
        if (end > start) {
            const QString token = QString::fromLatin1(bytes.constData() + start, end - start);
            if (knownIds.contains(token))
                found->insert(token);
        }
        start = qMax(end, start + 1);
    }
    for (qsizetype start = 0; start + 1 < bytes.size();) {
        while (start + 1 < bytes.size()
               && (!isIdChar(uchar(bytes[start])) || bytes[start + 1] != '\0'))
            ++start;
        qsizetype end = start;
        QByteArray tokenBytes;
        while (end + 1 < bytes.size()
               && isIdChar(uchar(bytes[end])) && bytes[end + 1] == '\0') {
            tokenBytes.append(bytes[end]);
            end += 2;
        }
        if (!tokenBytes.isEmpty()) {
            const QString token = QString::fromLatin1(tokenBytes);
            if (knownIds.contains(token))
                found->insert(token);
        }
        start = qMax(end, start + 1);
    }
}

QStringList xmlEntries(const Sc2Archive &archive)
{
    QStringList entries;
    for (const QString &entry : archive.allEntries()) {
        if (entry.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive))
            entries.append(entry);
    }
    entries.removeDuplicates();
    return entries;
}

bool materializeArchiveXml(const Sc2Archive &archive, const QString &tempRoot,
                           bool skipDataCollectionXml, MaterializeStats *stats, QString *error)
{
    if (stats)
        stats->archiveEntries = archive.totalEntriesCount();
    for (const QString &entry : xmlEntries(archive)) {
        if (skipDataCollectionXml && isDataCollectionXmlEntry(entry)) {
            if (stats)
                ++stats->dataCollectionXmlSkipped;
            continue;
        }
        QByteArray bytes;
        if (!archive.readEntry(entry, &bytes, error)) {
            if (error)
                *error = QStringLiteral("%1: %2").arg(entry, *error);
            return false;
        }
        QString relative = QDir::cleanPath(entry).replace('\\', '/');
        if (relative.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(relative)) {
            if (error)
                *error = QStringLiteral("Unsafe archive entry path: %1").arg(entry);
            return false;
        }
        if (!writeBytes(QDir(tempRoot).absoluteFilePath(relative), bytes, error))
            return false;
        if (stats)
            ++stats->xmlMaterialized;
    }

    QByteArray listfileBytes;
    QString listfileError;
    if (!archive.readEntry(QStringLiteral("(listfile)"), &listfileBytes, &listfileError)) {
        QStringList entries;
        for (QString entry : archive.allEntries())
            entries << entry.replace('/', '\\');
        listfileBytes = entries.join(QStringLiteral("\r\n")).toUtf8() + QByteArrayLiteral("\r\n");
    }
    return writeBytes(QDir(tempRoot).absoluteFilePath(QStringLiteral("(listfile)")), listfileBytes, error);
}

QStringList materializeArchiveReferenceEntries(const Sc2Archive &archive, const QString &tempRoot, QString *error)
{
    QStringList materialized;
    for (const QString &entry : archive.allEntries()) {
        if (entry.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)
            || !isArchiveReferenceEntry(entry))
            continue;
        QByteArray bytes;
        if (!archive.readEntry(entry, &bytes, error)) {
            if (error)
                *error = QStringLiteral("%1: %2").arg(entry, *error);
            return {};
        }
        const QString relative = QDir::cleanPath(entry).replace('\\', '/');
        if (relative.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(relative)) {
            if (error)
                *error = QStringLiteral("Unsafe archive entry path: %1").arg(entry);
            return {};
        }
        if (!writeBytes(QDir(tempRoot).absoluteFilePath(relative), bytes, error))
            return {};
        materialized << relative;
    }
    materialized.removeDuplicates();
    return materialized;
}

bool analyzeWorkspace(const QString &workspace, AnalysisResult *analysis, QString *error)
{
    FolderAnalyzer analyzer;
    return analyzer.analyzeFolder(workspace, {}, analysis, error);
}

ArchiveReferenceStats scanArchiveReferences(const Sc2Archive &archive, const AnalysisResult &analysis)
{
    ArchiveReferenceStats stats;
    QSet<QString> knownIds;
    for (const DataNode &node : analysis.nodes) {
        if (!node.id.isEmpty())
            knownIds.insert(node.id);
    }
    for (const QString &entry : archive.allEntries()) {
        if (entry.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)
            || !isArchiveReferenceEntry(entry))
            continue;
        QByteArray bytes;
        QString error;
        if (!archive.readEntry(entry, &bytes, &error)) {
            stats.complete = false;
            continue;
        }
        collectKnownIdTokens(bytes, knownIds, &stats.referencedIds);
        ++stats.entriesScanned;
    }
    stats.idsFound = stats.referencedIds.size();
    return stats;
}

void applyArchiveReferenceSafety(AnalysisResult *analysis, const ArchiveReferenceStats &referenceStats)
{
    if (!analysis)
        return;
    analysis->possibleUnusedNodeIndices.clear();
    for (UnusedCandidateInfo &candidate : analysis->unusedCandidates) {
        if (candidate.state != CandidateState::Safe)
            continue;
        const bool externallyReferenced = candidate.nodeIndex >= 0
            && candidate.nodeIndex < analysis->nodes.size()
            && referenceStats.referencedIds.contains(analysis->nodes[candidate.nodeIndex].id);
        if (!referenceStats.complete || externallyReferenced) {
            candidate.state = CandidateState::Blocked;
            candidate.usageState = UsageState::Blocked;
            candidate.protectedObject = true;
            candidate.reason = !referenceStats.complete
                ? QStringLiteral("Archive reference scan was incomplete")
                : QStringLiteral("Referenced by archive placement, trigger, or script data");
            candidate.riskLevel = QStringLiteral("high");
            if (candidate.nodeIndex >= 0 && candidate.nodeIndex < analysis->nodes.size())
                analysis->nodes[candidate.nodeIndex].candidateUnused = false;
        } else {
            analysis->possibleUnusedNodeIndices.append(candidate.nodeIndex);
        }
    }
}

bool isSafeUnusedRemovalCandidate(const UnusedCandidateInfo &candidate)
{
    return candidate.state == CandidateState::Safe
        && (candidate.usageState == UsageState::Disconnected
            || candidate.usageState == UsageState::UnusedSubgraph);
}

int safeUnusedRemovalCount(const AnalysisResult &analysis)
{
    int count = 0;
    for (const UnusedCandidateInfo &candidate : analysis.unusedCandidates) {
        if (isSafeUnusedRemovalCandidate(candidate))
            ++count;
    }
    return count;
}

QVector<int> safeUnusedRemovalRows(const AnalysisResult &analysis)
{
    QVector<int> rows;
    for (const UnusedCandidateInfo &candidate : analysis.unusedCandidates) {
        if (isSafeUnusedRemovalCandidate(candidate))
            rows.append(candidate.nodeIndex);
    }
    return rows;
}

int duplicateMergeCandidateCount(const AnalysisResult &analysis)
{
    return int(std::count_if(analysis.duplicateContentGroups.cbegin(),
                             analysis.duplicateContentGroups.cend(),
                             [](const DuplicateContentGroup &group) {
                                 return group.mergeCandidate && group.nodeIndices.size() > 1;
                             }));
}

bool hasArchiveReferencedRenameItem(const RenamePlan &plan, const QSet<QString> &archiveReferencedIds)
{
    for (const RenamePlanItem &item : plan.items) {
        if (item.selected && !item.blocked && archiveReferencedIds.contains(item.oldId))
            return true;
    }
    return false;
}

int usableRenameItemCount(const RenamePlan &plan)
{
    return int(std::count_if(plan.items.cbegin(), plan.items.cend(), [](const RenamePlanItem &item) {
        return item.selected && !item.blocked;
    }));
}

bool applyRenamePlans(QString workspace, AnalysisResult *analysis,
                      const ArchiveReferenceStats &referenceStats,
                      const QStringList &archiveReferenceEntries,
                      MapReport *report,
                      QStringList *changedFiles, QString *error)
{
    if (!analysis || !report || !changedFiles)
        return false;

    QSet<QString> skippedArchiveRoots;
    bool appliedAnyRenamePass = false;
    for (int pass = 0; pass < kRenamePassLimit; ++pass) {
        const QVector<UnitFamily> families = UnitFamilyDetector().detect(*analysis);
        RenamePlan combined;
        combined.valid = true;
        combined.targetRootId = QStringLiteral("Batch");
        QSet<int> selectedNodes;
        QSet<QString> selectedNewIds;
        int batchFamilyPlans = 0;

        for (const UnitFamily &family : families) {
            if (family.rootId.isEmpty())
                continue;
            RenamePlan plan = StandardNamePlanner().plan(*analysis, family, family.rootId);
            if (!plan.valid || usableRenameItemCount(plan) == 0)
                continue;
            if (!referenceStats.complete && hasArchiveReferencedRenameItem(plan, referenceStats.referencedIds)) {
                if (!skippedArchiveRoots.contains(family.rootId)) {
                    skippedArchiveRoots.insert(family.rootId);
                    ++report->renameSkippedArchiveRefs;
                }
                continue;
            }

            QVector<RenamePlanItem> acceptedItems;
            for (const RenamePlanItem &item : plan.items) {
                if (!item.selected || item.blocked || item.oldId == item.newId)
                    continue;
                const QString newKey = item.newId.toLower();
                if (selectedNodes.contains(item.nodeIndex) || selectedNewIds.contains(newKey)) {
                    acceptedItems.clear();
                    break;
                }
                acceptedItems.append(item);
            }
            if (acceptedItems.isEmpty())
                continue;

            if (combined.items.isEmpty())
                combined.family = plan.family;
            for (const RenamePlanItem &item : acceptedItems) {
                combined.items.append(item);
                selectedNodes.insert(item.nodeIndex);
                selectedNewIds.insert(item.newId.toLower());
            }
            ++batchFamilyPlans;
        }

        if (combined.items.isEmpty())
            return true;

        const RenameApplyResult result = ReferenceRenamer().apply(*analysis, combined, workspace, {});
        if (!result.success) {
            if (error)
                *error = result.error;
            return false;
        }
        appliedAnyRenamePass = true;
        report->renamePlansApplied += batchFamilyPlans;
        report->idsRenamed += result.identitiesRenamed;
        report->referencesUpdated += result.referencesUpdated;
        changedFiles->append(result.changedFiles);
        changedFiles->removeDuplicates();

        sc2dh::ArchiveReferenceRewriteReport archiveRewrite;
        if (!sc2dh::rewriteArchiveReferenceFiles(workspace, archiveReferenceEntries,
                                                 result.appliedRenames, &archiveRewrite, error)) {
            return false;
        }
        if (archiveRewrite.replacements > 0) {
            report->referencesUpdated += archiveRewrite.replacements;
            changedFiles->append(archiveRewrite.changedFiles);
            changedFiles->removeDuplicates();
        }

        if (!analyzeWorkspace(workspace, analysis, error))
            return false;
        applyArchiveReferenceSafety(analysis, referenceStats);
    }

    if (appliedAnyRenamePass)
        return true;
    if (error)
        *error = QStringLiteral("Rename safety limit exceeded.");
    return false;
}

void collectStats(const QString &rootFolder, CollectionStats *stats)
{
    if (!stats)
        return;
    QDirIterator it(rootFolder, QStringList{QStringLiteral("DataCollectionData.xml")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        ++stats->files;
        QByteArray bytes;
        QString ignored;
        if (!readBytes(path, &bytes, &ignored))
            continue;
        pugi::xml_document document;
        if (!document.load_buffer(bytes.constData(), size_t(bytes.size())))
            continue;
        const pugi::xml_node catalog = document.child("Catalog")
            ? document.child("Catalog") : document.document_element();
        for (pugi::xml_node collection = catalog.first_child(); collection;
             collection = collection.next_sibling()) {
            const QString type = QString::fromUtf8(collection.name());
            if (!isDataCollectionName(type))
                continue;
            const QString id = QString::fromUtf8(collection.attribute("id").value());
            if (id.isEmpty())
                continue;
            ++stats->collections;
            for (pugi::xml_node record : collection.children("DataRecord")) {
                const QString entry = QString::fromUtf8(record.attribute("Entry").value());
                if (entry.isEmpty())
                    continue;
                ++stats->directRecords;
                stats->entries.insert(entry);
                stats->pairs.insert(type + QChar(0x1f) + id + QChar(0x1f) + entry);
            }
        }
    }
}

QSet<QString> expectedCollectionPairs(const AnalysisResult &analysis,
                                      const QVector<UnitFamily> &families,
                                      int *rootTypeConflicts)
{
    DataCollectionAliasMapper mapper;
    QSet<QString> expected;
    if (rootTypeConflicts)
        *rootTypeConflicts = 0;
    for (const UnitFamily &family : families) {
        if (family.rootTypeConflict) {
            if (rootTypeConflicts)
                ++(*rootTypeConflicts);
            continue;
        }
        for (const UnitFamilyObject &object : family.objects) {
            if (object.nodeIndex < 0 || object.nodeIndex >= analysis.nodes.size())
                continue;
            const QString alias = mapper.aliasFor(analysis.nodes[object.nodeIndex],
                                                  family.rootId, object.role);
            if (alias.isEmpty())
                continue;
            expected.insert(family.collectionElementName + QChar(0x1f)
                            + family.rootId + QChar(0x1f) + alias);
        }
    }
    return expected;
}

bool applyDataCollections(const QString &mapPath, QString workspace, const AnalysisResult &analysis,
                          MapReport *report, QStringList *changedFiles, QString *error)
{
    const QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(
        analysis, DataCollectionMode::UnitAbilWeapon);
    report->collectionFamiliesDetected = families.size();
    qsizetype totalMemberships = 0;
    int maxFamilyObjects = 0;
    QString maxFamilyRoot;
    for (const UnitFamily &family : families) {
        totalMemberships += family.objects.size();
        if (family.objects.size() > maxFamilyObjects) {
            maxFamilyObjects = family.objects.size();
            maxFamilyRoot = family.rootId;
        }
    }
    logProgress(mapPath, QStringLiteral("collection_detected"),
                QStringLiteral("families=%1 memberships=%2 max_family=%3 max_root=%4")
                    .arg(families.size())
                    .arg(totalMemberships)
                    .arg(maxFamilyObjects)
                    .arg(maxFamilyRoot));

    QHash<QString, QVector<int>> familiesByTargetFile;
    for (int familyIndex = 0; familyIndex < families.size(); ++familyIndex) {
        const UnitFamily &family = families.at(familyIndex);
        if (family.rootTypeConflict) {
            ++report->collectionFamiliesSkipped;
            ++report->rootTypeConflicts;
            continue;
        }
        if (family.rootNodeIndex < 0 || family.rootNodeIndex >= analysis.nodes.size()) {
            ++report->collectionFamiliesSkipped;
            continue;
        }
        const QString targetFile = QDir(QFileInfo(analysis.nodes[family.rootNodeIndex].sourceFile).absolutePath())
            .absoluteFilePath(QStringLiteral("DataCollectionData.xml"));
        familiesByTargetFile[targetFile].append(familyIndex);
    }

    DataCollectionAliasMapper mapper;
    QStringList targetFiles = familiesByTargetFile.keys();
    std::sort(targetFiles.begin(), targetFiles.end(), [](const QString &left, const QString &right) {
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });

    int processedFamilies = 0;
    for (const QString &targetFile : targetFiles) {
        pugi::xml_document document;
        QByteArray existingBytes;
        if (QFileInfo::exists(targetFile)) {
            if (!readBytes(targetFile, &existingBytes, error))
                return false;
            const auto parsed = document.load_buffer(existingBytes.constData(), size_t(existingBytes.size()));
            if (!parsed) {
                if (error)
                    *error = QStringLiteral("Cannot parse DataCollectionData.xml: %1").arg(parsed.description());
                return false;
            }
        } else {
            auto declaration = document.append_child(pugi::node_declaration);
            declaration.append_attribute("version") = "1.0";
            declaration.append_attribute("encoding") = "utf-8";
            document.append_child("Catalog");
        }
        pugi::xml_node catalog = document.child("Catalog");
        if (!catalog)
            catalog = document.append_child("Catalog");
        ensureBasicDataCollectionSupport(catalog);

        QVector<int> group = familiesByTargetFile.value(targetFile);
        std::sort(group.begin(), group.end());
        for (int familyIndex : group) {
            const UnitFamily &family = families.at(familyIndex);
            ++processedFamilies;
            if (processedFamilies == 1 || processedFamilies % 100 == 0 || processedFamilies == families.size()) {
                logProgress(mapPath, QStringLiteral("collection_batch"),
                            QStringLiteral("family=%1/%2 root=%3 objects=%4")
                                .arg(processedFamilies)
                                .arg(families.size())
                                .arg(family.rootId)
                                .arg(family.objects.size()));
            }

            QStringList records;
            QSet<QString> seenRecords;
            for (const UnitFamilyObject &object : family.objects) {
                if (object.nodeIndex < 0 || object.nodeIndex >= analysis.nodes.size())
                    continue;
                const QString alias = mapper.aliasFor(analysis.nodes[object.nodeIndex], family.rootId, object.role);
                if (alias.isEmpty() || seenRecords.contains(alias))
                    continue;
                seenRecords.insert(alias);
                records.append(alias);
            }
            sortEntries(&records);

            pugi::xml_node collection = ensureCollectionNode(catalog, family.collectionElementName, family.rootId);
            if (!family.recommendedParent.isEmpty())
                setXmlAttribute(collection, "parent", family.recommendedParent);
            pugi::xml_node category = collection.child("EditorCategories");
            if (!category)
                category = collection.prepend_child("EditorCategories");
            setXmlAttribute(category, "value", defaultCategoriesForFamily(family));

            QSet<QString> existingRecords;
            for (pugi::xml_node record : collection.children("DataRecord")) {
                const QString entry = QString::fromUtf8(record.attribute("Entry").value());
                if (!entry.isEmpty())
                    existingRecords.insert(entry);
            }
            for (const QString &entry : records) {
                if (existingRecords.contains(entry))
                    continue;
                pugi::xml_node record = collection.append_child("DataRecord");
                setXmlAttribute(record, "Entry", entry);
                existingRecords.insert(entry);
                ++report->collectionRecordsAdded;
            }
            ++report->collectionFamiliesApplied;
        }

        std::ostringstream stream;
        document.save(stream, "    ", pugi::format_default, pugi::encoding_utf8);
        const QByteArray output = QByteArray::fromStdString(stream.str());
        if (!writeBytes(targetFile, output, error))
            return false;

        const QString relative = QDir(workspace).relativeFilePath(targetFile);
        changedFiles->append(relative);

        const QString listfilePath = QDir(workspace).absoluteFilePath(QStringLiteral("(listfile)"));
        const QString archiveEntry = QDir(workspace).relativeFilePath(targetFile).replace('/', '\\');
        if (!listfileContainsEntry(listfilePath, archiveEntry)) {
            QByteArray listfileBytes;
            if (QFileInfo::exists(listfilePath) && !readBytes(listfilePath, &listfileBytes, error))
                return false;
            if (!listfileBytes.isEmpty() && !listfileBytes.endsWith('\n'))
                listfileBytes.append("\r\n");
            listfileBytes.append(archiveEntry.toUtf8());
            listfileBytes.append("\r\n");
            if (!writeBytes(listfilePath, listfileBytes, error))
                return false;
            changedFiles->append(QStringLiteral("(listfile)"));
        }
    }
    changedFiles->removeDuplicates();
    Q_UNUSED(error);
    return true;
}

bool archiveEntryNameForFile(const Sc2Archive &archive, const QString &relativeFile, QString *archiveName)
{
    const QString normalized = QDir::cleanPath(relativeFile).replace('\\', '/');
    for (const QString &entry : archive.allEntries()) {
        if (QDir::cleanPath(entry).replace('\\', '/').compare(normalized, Qt::CaseInsensitive) == 0) {
            if (archiveName)
                *archiveName = entry;
            return true;
        }
    }
    if (archiveName)
        *archiveName = QString(normalized).replace('/', '\\');
    return false;
}

bool commitArchiveChanges(const QString &archivePath, const QString &tempRoot,
                          const QStringList &changedFiles, QString *backupPath,
                          QString *error)
{
    if (changedFiles.isEmpty())
        return true;

    Sc2Archive archive;
    if (!archive.load(archivePath, error))
        return false;

    QHash<QString, QByteArray> replacements;
    for (const QString &relativeFile : changedFiles) {
        const QString normalized = QDir::cleanPath(relativeFile).replace('\\', '/');
        QString archiveName;
        const bool existed = archiveEntryNameForFile(archive, normalized, &archiveName);
        QByteArray replacementBytes;
        if (!readBytes(QDir(tempRoot).absoluteFilePath(normalized), &replacementBytes, error))
            return false;
        if (normalized.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive) && existed) {
            QByteArray originalBytes;
            QString readError;
            if (archive.readEntry(archiveName, &originalBytes, &readError)) {
                DataCollectionPreservationReport preservationReport;
                if (!restoreMissingDataCollectionRecords(originalBytes, &replacementBytes,
                                                         &preservationReport, error))
                    return false;
            }
        }
        replacements.insert(archiveName, replacementBytes);
    }

    int enumRepairCount = 0;
    if (!addCatalogEnumRepairs(archive, &replacements, &enumRepairCount, error))
        return false;
    Q_UNUSED(enumRepairCount);

    BackupManager backupManager;
    QString backup;
    if (!backupManager.createBackup(archivePath, &backup, error))
        return false;

    const QString pending = archivePath + QStringLiteral(".sc2dh.pending");
    QFile::remove(pending);
    if (!archive.saveCopy(pending, replacements, {}, error)) {
        QFile::remove(pending);
        return false;
    }
    if (!validateArchiveCatalogSchema(pending, error)) {
        QFile::remove(pending);
        return false;
    }

    QByteArray archiveBytes;
    if (!readBytes(pending, &archiveBytes, error)) {
        QFile::remove(pending);
        return false;
    }

    QSaveFile destination(archivePath);
    if (!destination.open(QIODevice::WriteOnly)
        || destination.write(archiveBytes) != archiveBytes.size()
        || !destination.commit()) {
        if (error)
            *error = QStringLiteral("Unable to atomically replace archive: %1").arg(archivePath);
        QFile::remove(pending);
        return false;
    }
    QFile::remove(pending);
    if (backupPath)
        *backupPath = backup;
    return true;
}

bool verifyOptimizedMap(const QString &archivePath, MapReport *report, QString *error)
{
    Sc2Archive archive;
    if (!archive.load(archivePath, error))
        return false;
    report->archiveEntriesAfter = archive.totalEntriesCount();

    QTemporaryDir workspace;
    if (!workspace.isValid()) {
        if (error)
            *error = QStringLiteral("Unable to create verification workspace.");
        return false;
    }

    MaterializeStats stats;
    if (!materializeArchiveXml(archive, workspace.path(), false, &stats, error))
        return false;

    AnalysisResult analysis;
    if (!analyzeWorkspace(workspace.path(), &analysis, error))
        return false;
    const ArchiveReferenceStats referenceStats = scanArchiveReferences(archive, analysis);
    applyArchiveReferenceSafety(&analysis, referenceStats);

    report->objectsAfter = analysis.totalDataNodes();
    report->finalSafeUnused = safeUnusedRemovalCount(analysis);
    report->finalDuplicateMergeCandidates = duplicateMergeCandidateCount(analysis);

    const QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(
        analysis, DataCollectionMode::UnitAbilWeapon);
    int rootConflicts = 0;
    const QSet<QString> expected = expectedCollectionPairs(analysis, families, &rootConflicts);
    report->expectedCollectionPairs = expected.size();
    report->rootTypeConflicts = qMax(report->rootTypeConflicts, rootConflicts);

    CollectionStats collectionStats;
    collectStats(workspace.path(), &collectionStats);
    QSet<QString> missing = expected;
    missing.subtract(collectionStats.pairs);
    QSet<QString> extra = collectionStats.pairs;
    extra.subtract(expected);

    report->finalCollectionFiles = collectionStats.files;
    report->finalCollections = collectionStats.collections;
    report->finalCollectionRecords = collectionStats.directRecords;
    report->finalCollectionUniquePairs = collectionStats.pairs.size();
    report->finalCollectionUniqueEntries = collectionStats.entries.size();
    report->missingExpectedCollectionPairs = missing.size();
    report->extraCollectionPairs = extra.size();
    return missing.isEmpty();
}

bool optimizeMap(const QString &mapPath, MapReport *report)
{
    report->mapPath = QDir::fromNativeSeparators(mapPath);
    report->beforeBytes = QFileInfo(report->mapPath).size();

    QString error;
    logProgress(report->mapPath, QStringLiteral("load"));
    Sc2Archive archive;
    if (!archive.load(report->mapPath, &error)) {
        report->error = error;
        return false;
    }
    report->archiveEntriesBefore = archive.totalEntriesCount();

    QTemporaryDir workspace;
    if (!workspace.isValid()) {
        report->error = QStringLiteral("Unable to create optimization workspace.");
        return false;
    }

    MaterializeStats materializeStats;
    logProgress(report->mapPath, QStringLiteral("materialize_start"));
    if (!materializeArchiveXml(archive, workspace.path(), true, &materializeStats, &error)) {
        report->error = error;
        return false;
    }
    logProgress(report->mapPath, QStringLiteral("materialize_done"),
                QStringLiteral("xml=%1 skipped_dc=%2")
                    .arg(materializeStats.xmlMaterialized)
                    .arg(materializeStats.dataCollectionXmlSkipped));
    report->xmlMaterialized = materializeStats.xmlMaterialized;
    report->dataCollectionXmlSkipped = materializeStats.dataCollectionXmlSkipped;

    AnalysisResult analysis;
    logProgress(report->mapPath, QStringLiteral("analysis_start"));
    if (!analyzeWorkspace(workspace.path(), &analysis, &error)) {
        report->error = error;
        return false;
    }
    logProgress(report->mapPath, QStringLiteral("analysis_done"),
                QStringLiteral("objects=%1").arg(analysis.totalDataNodes()));
    logProgress(report->mapPath, QStringLiteral("archive_reference_scan_start"));
    ArchiveReferenceStats referenceStats = scanArchiveReferences(archive, analysis);
    logProgress(report->mapPath, QStringLiteral("archive_reference_scan_done"),
                QStringLiteral("entries=%1 ids=%2 complete=%3")
                    .arg(referenceStats.entriesScanned)
                    .arg(referenceStats.idsFound)
                    .arg(referenceStats.complete ? 1 : 0));
    report->archiveReferenceEntriesScanned = referenceStats.entriesScanned;
    report->archiveReferenceIdsFound = referenceStats.idsFound;
    error.clear();
    QStringList archiveReferenceEntries = materializeArchiveReferenceEntries(archive, workspace.path(), &error);
    if (!error.isEmpty() && archiveReferenceEntries.isEmpty()) {
        report->error = error;
        return false;
    }
    applyArchiveReferenceSafety(&analysis, referenceStats);
    report->objectsBefore = analysis.totalDataNodes();
    report->safeUnusedBefore = safeUnusedRemovalCount(analysis);

    QStringList changedFiles;
    QVector<int> deepCleanupRows;
    for (const DeepCleanupCandidate &candidate : analysis.deepCleanupCandidates) {
        if (candidate.state == CandidateState::Safe
            && candidate.recommended
            && candidate.action != DeepCleanupAction::ReportOnly)
            deepCleanupRows.append(candidate.index);
    }
    report->deepCleanupCandidates = deepCleanupRows.size();
    if (!deepCleanupRows.isEmpty()) {
        logProgress(report->mapPath, QStringLiteral("deep_cleanup_start"),
                    QStringLiteral("rows=%1").arg(deepCleanupRows.size()));
        const DeepCleanupApplyResult cleanup = DeepCleanupService().apply(analysis, deepCleanupRows, workspace.path(), false);
        if (!cleanup.success) {
            report->error = cleanup.error;
            return false;
        }
        report->deepCleanupChanges = cleanup.filesDeleted + cleanup.textLinesRemoved
            + cleanup.xmlNodesRemoved + cleanup.xmlAttributesRemoved;
        report->deepCleanupSkipped = cleanup.reportOnlySkipped;
        changedFiles.append(cleanup.changedFiles);
        changedFiles.removeDuplicates();
        logProgress(report->mapPath, QStringLiteral("deep_cleanup_done"),
                    QStringLiteral("changes=%1 skipped=%2")
                        .arg(report->deepCleanupChanges)
                        .arg(report->deepCleanupSkipped));
        if (!analyzeWorkspace(workspace.path(), &analysis, &error)) {
            report->error = error;
            return false;
        }
        applyArchiveReferenceSafety(&analysis, referenceStats);
    }

    const QVector<int> unusedRows = safeUnusedRemovalRows(analysis);
    if (!unusedRows.isEmpty()) {
        logProgress(report->mapPath, QStringLiteral("unused_delete_start"),
                    QStringLiteral("rows=%1").arg(unusedRows.size()));
        QString unusedBackup;
        QStringList unusedChangedFiles;
        int removed = 0;
        int skipped = 0;
        if (!FolderAnalyzer().applySelectedChanges(analysis, unusedRows, workspace.path(), {},
                                                   &unusedBackup, &error, &unusedChangedFiles,
                                                   &removed, &skipped)) {
            report->error = error;
            return false;
        }
        report->unusedDeleted += removed;
        report->unusedSkipped += skipped;
        changedFiles.append(unusedChangedFiles);
        changedFiles.removeDuplicates();
        logProgress(report->mapPath, QStringLiteral("unused_delete_done"),
                    QStringLiteral("removed=%1 skipped=%2").arg(removed).arg(skipped));
        if (!analyzeWorkspace(workspace.path(), &analysis, &error)) {
            report->error = error;
            return false;
        }
        applyArchiveReferenceSafety(&analysis, referenceStats);
    }

    logProgress(report->mapPath, QStringLiteral("rename_start"));
    if (!applyRenamePlans(workspace.path(), &analysis, referenceStats, archiveReferenceEntries, report,
                          &changedFiles, &error)) {
        report->error = error;
        return false;
    }
    logProgress(report->mapPath, QStringLiteral("rename_done"),
                QStringLiteral("ids=%1 refs=%2 skipped_archive=%3")
                    .arg(report->idsRenamed)
                    .arg(report->referencesUpdated)
                    .arg(report->renameSkippedArchiveRefs));

    logProgress(report->mapPath, QStringLiteral("collection_start"));
    if (!applyDataCollections(report->mapPath, workspace.path(), analysis, report, &changedFiles, &error)) {
        report->error = error;
        return false;
    }
    logProgress(report->mapPath, QStringLiteral("collection_done"),
                QStringLiteral("applied=%1 skipped=%2 records=%3")
                    .arg(report->collectionFamiliesApplied)
                    .arg(report->collectionFamiliesSkipped)
                    .arg(report->collectionRecordsAdded));

    if (!changedFiles.isEmpty()) {
        logProgress(report->mapPath, QStringLiteral("commit_start"),
                    QStringLiteral("changed_files=%1").arg(changedFiles.size()));
        if (!commitArchiveChanges(report->mapPath, workspace.path(), changedFiles,
                                  &report->backupPath, &error)) {
            report->error = error;
            return false;
        }
        logProgress(report->mapPath, QStringLiteral("commit_done"));
    }

    report->afterBytes = QFileInfo(report->mapPath).size();
    logProgress(report->mapPath, QStringLiteral("verify_start"));
    if (!verifyOptimizedMap(report->mapPath, report, &error)) {
        report->error = error.isEmpty()
            ? QStringLiteral("Verification failed: Data Collection is missing expected pairs.")
            : error;
        return false;
    }
    logProgress(report->mapPath, QStringLiteral("verify_done"),
                QStringLiteral("missing=%1").arg(report->missingExpectedCollectionPairs));

    report->success = true;
    return true;
}

bool verifyOnlyMap(const QString &mapPath, MapReport *report)
{
    report->mapPath = QDir::fromNativeSeparators(mapPath);
    report->beforeBytes = QFileInfo(report->mapPath).size();
    report->afterBytes = report->beforeBytes;

    QString error;
    Sc2Archive archive;
    if (!archive.load(report->mapPath, &error)) {
        report->error = error;
        return false;
    }
    report->archiveEntriesBefore = archive.totalEntriesCount();

    if (!verifyOptimizedMap(report->mapPath, report, &error)) {
        report->error = error.isEmpty()
            ? QStringLiteral("Verification failed: Data Collection is missing expected pairs.")
            : error;
        return false;
    }
    report->success = true;
    return true;
}

void printReport(const MapReport &report, QTextStream &out)
{
    const qint64 delta = report.afterBytes - report.beforeBytes;
    const double reducedPercent = report.beforeBytes > 0
        ? (double(report.beforeBytes - report.afterBytes) * 100.0 / double(report.beforeBytes))
        : 0.0;
    out << "map=" << QDir::toNativeSeparators(report.mapPath) << '\n';
    out << "status=" << (report.success ? "ok" : "failed") << '\n';
    if (!report.error.isEmpty())
        out << "error=" << report.error << '\n';
    if (!report.backupPath.isEmpty())
        out << "backup=" << QDir::toNativeSeparators(report.backupPath) << '\n';
    out << "before_bytes=" << report.beforeBytes << '\n';
    out << "after_bytes=" << report.afterBytes << '\n';
    out << "size_delta_bytes=" << delta << '\n';
    out << "size_reduced_percent=" << QString::number(reducedPercent, 'f', 2) << '\n';
    out << "archive_entries_before=" << report.archiveEntriesBefore << '\n';
    out << "archive_entries_after=" << report.archiveEntriesAfter << '\n';
    out << "xml_materialized=" << report.xmlMaterialized << '\n';
    out << "existing_datacollection_xml_skipped_for_zero_generation="
        << report.dataCollectionXmlSkipped << '\n';
    out << "archive_reference_entries_scanned=" << report.archiveReferenceEntriesScanned << '\n';
    out << "archive_reference_ids_found=" << report.archiveReferenceIdsFound << '\n';
    out << "objects_before=" << report.objectsBefore << '\n';
    out << "objects_after=" << report.objectsAfter << '\n';
    out << "safe_unused_before=" << report.safeUnusedBefore << '\n';
    out << "unused_deleted=" << report.unusedDeleted << '\n';
    out << "unused_skipped=" << report.unusedSkipped << '\n';
    out << "deep_cleanup_candidates=" << report.deepCleanupCandidates << '\n';
    out << "deep_cleanup_changes=" << report.deepCleanupChanges << '\n';
    out << "deep_cleanup_skipped=" << report.deepCleanupSkipped << '\n';
    out << "rename_plans_applied=" << report.renamePlansApplied << '\n';
    out << "ids_renamed=" << report.idsRenamed << '\n';
    out << "references_updated=" << report.referencesUpdated << '\n';
    out << "rename_skipped_archive_refs=" << report.renameSkippedArchiveRefs << '\n';
    out << "collection_families_detected=" << report.collectionFamiliesDetected << '\n';
    out << "collection_families_applied=" << report.collectionFamiliesApplied << '\n';
    out << "collection_families_skipped=" << report.collectionFamiliesSkipped << '\n';
    out << "root_type_conflicts=" << report.rootTypeConflicts << '\n';
    out << "collection_records_added=" << report.collectionRecordsAdded << '\n';
    out << "collection_records_reorganized=" << report.collectionRecordsReorganized << '\n';
    out << "final_collection_files=" << report.finalCollectionFiles << '\n';
    out << "final_collections=" << report.finalCollections << '\n';
    out << "final_collection_records=" << report.finalCollectionRecords << '\n';
    out << "final_collection_unique_pairs=" << report.finalCollectionUniquePairs << '\n';
    out << "final_collection_unique_entries=" << report.finalCollectionUniqueEntries << '\n';
    out << "expected_collection_pairs=" << report.expectedCollectionPairs << '\n';
    out << "missing_expected_collection_pairs=" << report.missingExpectedCollectionPairs << '\n';
    out << "extra_collection_pairs=" << report.extraCollectionPairs << '\n';
    out << "final_safe_unused=" << report.finalSafeUnused << '\n';
    out << "final_duplicate_merge_candidates=" << report.finalDuplicateMergeCandidates << '\n';
    out << "----\n";
    out.flush();
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);
    const QStringList args = app.arguments();
    if (args.size() < 2) {
        err << "Usage: SC2OptimizeMaps [--verify-only] <map1.SC2Map> [map2.SC2Map ...]\n";
        return 2;
    }

    const bool verifyOnly = args.size() > 2 && args.at(1) == QStringLiteral("--verify-only");
    const int firstMapArg = verifyOnly ? 2 : 1;
    bool allOk = true;
    for (int i = firstMapArg; i < args.size(); ++i) {
        MapReport report;
        const bool ok = verifyOnly ? verifyOnlyMap(args.at(i), &report)
                                   : optimizeMap(args.at(i), &report);
        if (!ok)
            allOk = false;
        printReport(report, out);
    }
    return allOk ? 0 : 1;
}
