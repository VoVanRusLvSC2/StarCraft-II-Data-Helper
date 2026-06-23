#include "core/DataCollectionUnitBuilder.h"

#include "core/BackupManager.h"
#include "core/FolderAnalyzer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

#include <pugixml.hpp>

#include <algorithm>
#include <sstream>

namespace {

bool readBytes(const QString &path, QByteArray *bytes, QString *error);

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
        const int leftOrder = recordOrder(left), rightOrder = recordOrder(right);
        if (leftOrder != rightOrder) return leftOrder < rightOrder;
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
}

pugi::xml_node findByTypeAndId(pugi::xml_document &doc, const QString &type, const QString &id)
{
    for (const pugi::xpath_node &match : doc.select_nodes("//*[@id]")) {
        const pugi::xml_node node = match.node();
        if (QString::fromUtf8(node.name()).compare(type, Qt::CaseInsensitive) == 0
            && QString::fromUtf8(node.attribute("id").value()) == id) return node;
    }
    return {};
}

QStringList existingEntries(const pugi::xml_node &collection, QStringList *duplicates = nullptr)
{
    QStringList entries;
    QSet<QString> seen;
    for (pugi::xml_node record : collection.children("DataRecord")) {
        const QString entry = QString::fromUtf8(record.attribute("Entry").value());
        if (entry.isEmpty()) continue;
        if (seen.contains(entry)) { if (duplicates) duplicates->append(entry); }
        else { seen.insert(entry); entries.append(entry); }
    }
    return entries;
}

void setAttribute(pugi::xml_node node, const char *name, const QString &value)
{
    pugi::xml_attribute attribute = node.attribute(name);
    if (!attribute) attribute = node.append_attribute(name);
    attribute.set_value(value.toUtf8().constData());
}

void updateCollection(pugi::xml_node collection, const QString &parent, const QString &categories,
                       const QStringList &records)
{
    if (!parent.isEmpty()) setAttribute(collection, "parent", parent);
    pugi::xml_node category = collection.child("EditorCategories");
    if (!category) category = collection.prepend_child("EditorCategories");
    setAttribute(category, "value", categories);
    QSet<QString> existing;
    for (pugi::xml_node record = collection.child("DataRecord"); record;) {
        pugi::xml_node next = record.next_sibling("DataRecord");
        const QString entry = QString::fromUtf8(record.attribute("Entry").value());
        if (!entry.isEmpty() && existing.contains(entry)) {
            collection.remove_child(record);
        } else if (!entry.isEmpty()) {
            existing.insert(entry);
        }
        record = next;
    }
    for (const QString &entry : records) {
        if (existing.contains(entry)) continue;
        pugi::xml_node record = collection.append_child("DataRecord");
        setAttribute(record, "Entry", entry);
        existing.insert(entry);
    }
}

QString serializeNode(const pugi::xml_node &node)
{
    std::ostringstream stream;
    node.print(stream, "  ", pugi::format_default, pugi::encoding_utf8);
    return QString::fromUtf8(stream.str());
}

QString buildReport(const DataCollectionPreviewReport &preview, const QString &finalResult)
{
    QString report = QStringLiteral("Data Collection Preview\nSelected family: %1\nCurrent real unit ID: %1\nRequested unit name: %2\nCollection ID: %1\nParent: %3\nEditor categories: %4\nTarget file: %5\nArchive entry: %6\nListfile: %7\nListfile update required: %8\n\nReal object IDs / DataRecord entries\n")
                         .arg(preview.request.family.rootId,
                              preview.request.requestedUnitId.isEmpty() ? preview.request.family.rootId : preview.request.requestedUnitId,
                              preview.request.parent, preview.request.editorCategories, preview.targetFile,
                              preview.archiveEntry, preview.listfilePath,
                              preview.listfileNeedsUpdate ? QStringLiteral("yes") : QStringLiteral("no"));
    for (const DataCollectionEntryProposal &entry : preview.entries)
        report += QStringLiteral("- %1 | %2 | %3 | %4\n").arg(entry.realId, entry.alias, unitFamilyRoleName(entry.role), entry.status);
    const auto section = [&report](const QString &title, const QStringList &values) {
        report += QStringLiteral("\n%1\n").arg(title);
        report += values.isEmpty() ? QStringLiteral("- none\n") : QStringLiteral("- ") + values.join(QStringLiteral("\n- ")) + QLatin1Char('\n');
    };
    section(QStringLiteral("Dynamically discovered objects"), [&]() {
        QStringList values; for (const auto &entry : preview.entries) if (entry.role == UnitFamilyRole::Other
            || entry.role == UnitFamilyRole::Weapon || entry.role == UnitFamilyRole::Ability
            || entry.role == UnitFamilyRole::Effect || entry.role == UnitFamilyRole::Behavior
            || entry.role == UnitFamilyRole::Validator || entry.role == UnitFamilyRole::Requirement
            || entry.role == UnitFamilyRole::Upgrade) values << entry.realId; return values; }());
    section(QStringLiteral("Manual review objects"), preview.manualReviewObjects);
    section(QStringLiteral("Missing expected real objects"), preview.missingExpectedObjects);
    section(QStringLiteral("Existing records preserved"), preview.existingRecordsPreserved);
    section(QStringLiteral("Records to add"), preview.recordsToAdd);
    section(QStringLiteral("Duplicate records skipped"), preview.duplicateRecordsSkipped);
    section(QStringLiteral("Warnings"), preview.warnings);
    report += QStringLiteral("\nGenerated XML\n%1\nFinal result: %2\n").arg(preview.generatedXml, finalResult);
    return report;
}

QString collectionFilePath(const QString &rootFile)
{
    return QDir(QFileInfo(rootFile).absolutePath()).absoluteFilePath(QStringLiteral("DataCollectionData.xml"));
}

QString listfileEntry(const QString &rootFolder, const QString &targetFile)
{
    return QDir(rootFolder).relativeFilePath(targetFile).replace('/', '\\');
}

bool listfileContains(const QString &path, const QString &entry)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    const QString normalizedEntry = QDir::cleanPath(entry).replace('/', '\\');
    for (const QString &line : QString::fromUtf8(file.readAll()).split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts))
        if (QDir::cleanPath(line.trimmed()).replace('/', '\\').compare(normalizedEntry, Qt::CaseInsensitive) == 0) return true;
    return false;
}

QByteArray buildCollectionDocument(const QString &targetFile, const QString &elementName, const QString &id, const QString &parent,
                                   const QString &categories, const QStringList &records, QString *error)
{
    pugi::xml_document doc;
    if (QFileInfo::exists(targetFile)) {
        QByteArray bytes;
        if (!readBytes(targetFile, &bytes, error)) return {};
        const auto parsed = doc.load_buffer(bytes.constData(), size_t(bytes.size()));
        if (!parsed) { *error = QStringLiteral("Cannot parse DataCollectionData.xml: %1").arg(parsed.description()); return {}; }
    } else {
        auto declaration = doc.append_child(pugi::node_declaration);
        declaration.append_attribute("version") = "1.0";
        declaration.append_attribute("encoding") = "utf-8";
        doc.append_child("Catalog");
    }
    pugi::xml_node catalog = doc.child("Catalog");
    if (!catalog) catalog = doc.append_child("Catalog");
    pugi::xml_node collection = findByTypeAndId(doc, elementName, id);
    if (!collection) {
        collection = catalog.append_child(elementName.toUtf8().constData());
        setAttribute(collection, "id", id);
    }
    updateCollection(collection, parent, categories, records);
    std::ostringstream stream;
    doc.save(stream, "    ", pugi::format_default, pugi::encoding_utf8);
    return QByteArray::fromStdString(stream.str());
}

bool readBytes(const QString &path, QByteArray *bytes, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { *error = QStringLiteral("Unable to read %1").arg(path); return false; }
    *bytes = file.readAll(); return true;
}

bool restore(const QString &root, const QString &backup, const QString &relative, QString *error)
{
    const QString target = QDir(root).absoluteFilePath(relative);
    QFile::remove(target);
    if (!QFile::copy(QDir(backup).absoluteFilePath(relative), target)) {
        *error += QStringLiteral(" Rollback failed for %1.").arg(target); return false;
    }
    return true;
}

} // namespace

DataCollectionPreviewReport DataCollectionUnitBuilder::preview(const AnalysisResult &analysis,
                                                               const DataCollectionBuildRequest &request) const
{
    DataCollectionPreviewReport result;
    result.request = request;
    if (request.family.rootNodeIndex < 0 || request.family.rootNodeIndex >= analysis.nodes.size()) {
        result.warnings << QStringLiteral("Select a unit family."); result.reportText = buildReport(result, QStringLiteral("blocked")); return result;
    }
    const QString root = request.family.rootId;
    const QString requestedCollectionId = request.requestedUnitId.trimmed().isEmpty() ? root : request.requestedUnitId.trimmed();
    const bool nameMatchesCollection = requestedCollectionId == root;
    if (!nameMatchesCollection)
        result.warnings << QStringLiteral("The requested Collection ID differs from the detected CollectionID@ child prefix. Rename the real XML IDs first.");
    const DataNode &rootNode = analysis.nodes[request.family.rootNodeIndex];
    const DataNode *existingCollectionNode = nullptr;
    for (const DataNode &node : analysis.nodes)
        if (node.elementName.startsWith(QStringLiteral("CDataCollection"), Qt::CaseInsensitive) && node.id == root)
            existingCollectionNode = &node;
    result.existingCollection = existingCollectionNode != nullptr;
    result.targetFile = existingCollectionNode ? existingCollectionNode->sourceFile : collectionFilePath(rootNode.sourceFile);
    result.targetFileExists = QFileInfo::exists(result.targetFile);
    result.listfilePath = QDir(analysis.rootFolder).absoluteFilePath(QStringLiteral("(listfile)"));
    result.archiveEntry = listfileEntry(analysis.rootFolder, result.targetFile);
    result.listfileNeedsUpdate = !listfileContains(result.listfilePath, result.archiveEntry);

    DataCollectionAliasMapper mapper;
    QSet<QString> knownAliases;
    QStringList existing;
    if (existingCollectionNode) {
        pugi::xml_document fragment;
        if (fragment.load_string(existingCollectionNode->serializedXml.toUtf8().constData())) {
            existing = existingEntries(fragment.first_child(), &result.duplicateRecordsSkipped);
            result.existingRecordsPreserved = existing;
            knownAliases = QSet<QString>(existing.cbegin(), existing.cend());
        }
    }
    QSet<int> included = request.includedNodeIndices;
    if (included.isEmpty()) for (const UnitFamilyObject &object : request.family.objects)
        if (object.role != UnitFamilyRole::ManualReview) included.insert(object.nodeIndex);
    QHash<int, UnitFamilyObject> familyObjects;
    for (const UnitFamilyObject &object : request.family.objects) familyObjects.insert(object.nodeIndex, object);
    bool standardized = true;
    int linkedObjectCount = 0;
    for (const UnitFamilyObject &object : request.family.objects) {
        const DataNode &node = analysis.nodes[object.nodeIndex];
        DataCollectionEntryProposal proposal;
        proposal.nodeIndex = object.nodeIndex; proposal.realType = node.elementName; proposal.realId = node.id;
        proposal.role = object.role; proposal.confidence = object.confidence; proposal.included = included.contains(object.nodeIndex);
        const bool validCollectionId = node.id.compare(root, Qt::CaseInsensitive) == 0
            || node.id.startsWith(root + QLatin1Char('@'), Qt::CaseInsensitive);
        proposal.alias = mapper.aliasFor(node, root, object.role);
        if (proposal.alias.isEmpty()) {
            proposal.status = QStringLiteral("Manual Review: unsupported catalog type");
            result.manualReviewObjects << node.id;
            standardized = false;
        } else if (object.role == UnitFamilyRole::ManualReview) {
            result.manualReviewObjects << node.id;
            if (proposal.included && request.confirmNonStandard) {
                if (knownAliases.contains(proposal.alias)) proposal.status = QStringLiteral("Already exists (manually confirmed)");
                else { proposal.status = QStringLiteral("Will add (manually confirmed)"); result.recordsToAdd << proposal.alias; ++linkedObjectCount; }
            } else proposal.status = QStringLiteral("Manual Review");
        } else if (knownAliases.contains(proposal.alias)) {
            proposal.status = validCollectionId ? QStringLiteral("Already exists")
                                                : QStringLiteral("Already exists (non-standard ID)");
            ++linkedObjectCount;
        } else if (proposal.included) {
            proposal.status = validCollectionId ? QStringLiteral("Will add")
                                                : QStringLiteral("Will add (non-standard ID)");
            result.recordsToAdd << proposal.alias;
            ++linkedObjectCount;
        }
        else proposal.status = QStringLiteral("Excluded");
        result.entries << proposal;
        if (!validCollectionId)
            standardized = false;
    }
    result.familyStandardized = standardized;
    if (!standardized)
        result.warnings << QStringLiteral("Family contains existing real IDs outside CollectionID / CollectionID@Child format. They can still be linked, but naming is non-standard.");
    if (!result.existingCollection && linkedObjectCount < 2)
        result.warnings << QStringLiteral("A new collection requires at least two existing related objects; a single standard override is not a collection candidate.");

    QStringList allRecords = existing;
    for (const QString &entry : result.recordsToAdd) if (!allRecords.contains(entry)) allRecords << entry;
    sortEntries(&allRecords);
    QString generatedError;
    const QString elementName = existingCollectionNode ? existingCollectionNode->elementName : request.family.collectionElementName;
    const QByteArray generated = buildCollectionDocument(result.targetFile, elementName, root, request.parent,
                                                         request.editorCategories, allRecords, &generatedError);
    result.generatedXml = QString::fromUtf8(generated);
    if (!generatedError.isEmpty()) result.warnings << generatedError;
    result.valid = nameMatchesCollection && generatedError.isEmpty()
        && (result.existingCollection || linkedObjectCount >= 2);
    result.reportText = buildReport(result, QStringLiteral("Preview only; no files modified"));
    return result;
}

DataCollectionApplyResult DataCollectionUnitBuilder::apply(const AnalysisResult &analysis,
                                                           const DataCollectionBuildRequest &request,
                                                           const QString &rootFolder,
                                                           const QSet<QString> &whitelistIds) const
{
    DataCollectionApplyResult result;
    const DataCollectionPreviewReport plan = preview(analysis, request);
    if (!plan.valid) { result.error = plan.warnings.join(QStringLiteral("; ")); return result; }
    const QByteArray staged = plan.generatedXml.toUtf8();
    const QString relative = QDir(rootFolder).relativeFilePath(plan.targetFile);
    const QString listRelative = QDir(rootFolder).relativeFilePath(plan.listfilePath);
    const bool targetExisted = QFileInfo::exists(plan.targetFile);
    const bool listfileExisted = QFileInfo::exists(plan.listfilePath);
    QByteArray listfileBytes;
    if (listfileExisted && !readBytes(plan.listfilePath, &listfileBytes, &result.error)) return result;
    if (plan.listfileNeedsUpdate) {
        if (!listfileBytes.isEmpty() && !listfileBytes.endsWith('\n')) listfileBytes.append("\r\n");
        listfileBytes.append(plan.archiveEntry.toUtf8());
        listfileBytes.append("\r\n");
    }
    QStringList existingForBackup;
    if (targetExisted) existingForBackup << relative;
    if (listfileExisted) existingForBackup << listRelative;
    BackupManager backup;
    if (!backup.createFolderBackup(rootFolder, existingForBackup, analysis.analysisReportText, plan.reportText,
                                   &result.backupFolder, &result.error)) return result;
    if (m_failureInjectionStep == QStringLiteral("after-backup")) { result.error = QStringLiteral("Injected failure after backup."); return result; }
    const auto rollback = [&]() {
        if (targetExisted) restore(rootFolder, result.backupFolder, relative, &result.error); else QFile::remove(plan.targetFile);
        if (listfileExisted) restore(rootFolder, result.backupFolder, listRelative, &result.error); else QFile::remove(plan.listfilePath);
    };
    QDir().mkpath(QFileInfo(plan.targetFile).absolutePath());
    QSaveFile output(plan.targetFile);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || output.write(staged) != staged.size() || !output.commit()) {
        result.error = QStringLiteral("Unable to commit Data Collection XML."); return result;
    }
    if (plan.listfileNeedsUpdate || !listfileExisted) {
        QSaveFile listfile(plan.listfilePath);
        if (!listfile.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || listfile.write(listfileBytes) != listfileBytes.size() || !listfile.commit()) {
            result.error = QStringLiteral("Unable to update (listfile)."); rollback(); return result;
        }
    }
    if (m_failureInjectionStep == QStringLiteral("after-commit")) {
        result.error = QStringLiteral("Injected failure after commit."); rollback(); return result;
    }
    Q_UNUSED(whitelistIds);
    QByteArray verifiedBytes;
    QString verifyError;
    if (!readBytes(plan.targetFile, &verifiedBytes, &verifyError)) {
        result.error = QStringLiteral("Collection verification failed: %1").arg(verifyError); rollback(); return result;
    }
    pugi::xml_document verifiedDocument;
    const auto parsed = verifiedDocument.load_buffer(verifiedBytes.constData(), size_t(verifiedBytes.size()));
    if (!parsed) {
        result.error = QStringLiteral("Collection verification failed: %1").arg(parsed.description()); rollback(); return result;
    }
    const pugi::xml_node verified = findByTypeAndId(verifiedDocument, request.family.collectionElementName, request.family.rootId);
    if (!verified) { result.error = QStringLiteral("Collection verification failed: object missing."); rollback(); return result; }
    QStringList verifyDuplicates; const QStringList verifiedEntries = existingEntries(verified, &verifyDuplicates);
    for (const QString &entry : plan.recordsToAdd) if (!verifiedEntries.contains(entry)) {
        result.error = QStringLiteral("Collection verification failed: %1 missing.").arg(entry); rollback(); return result;
    }
    if (!verifyDuplicates.isEmpty()) { result.error = QStringLiteral("Collection verification failed: duplicate records remain."); rollback(); return result; }
    if (!listfileContains(plan.listfilePath, plan.archiveEntry)) {
        result.error = QStringLiteral("Collection verification failed: (listfile) entry is missing."); rollback(); return result;
    }
    result.success = true; result.changedFile = relative; result.recordsAdded = plan.recordsToAdd.size();
    result.changedFiles << relative;
    if (plan.listfileNeedsUpdate || !listfileExisted) result.changedFiles << listRelative;
    result.duplicatesSkipped = plan.duplicateRecordsSkipped.size();
    result.finalReport = plan.reportText + QStringLiteral("\nFinal result after apply: success\nBackup: %1\n").arg(result.backupFolder);
    return result;
}
