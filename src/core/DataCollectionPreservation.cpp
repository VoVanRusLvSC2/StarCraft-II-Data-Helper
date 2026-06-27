#include "core/DataCollectionPreservation.h"

#include <QHash>
#include <QSet>

#include <pugixml.hpp>

#include <algorithm>
#include <sstream>

namespace {

constexpr int kSampleLimit = 30;

bool isDataCollectionNodeName(const QString &type)
{
    return type.startsWith(QStringLiteral("CDataCollection"), Qt::CaseInsensitive)
        && !type.startsWith(QStringLiteral("CDataCollectionPattern"), Qt::CaseInsensitive);
}

QString collectionKey(const QString &type, const QString &id)
{
    Q_UNUSED(type);
    return id.toLower();
}

QString recordKey(const QString &type, const QString &id, const QString &entry)
{
    return collectionKey(type, id) + QChar(0x1f) + entry;
}

QString sampleText(const QString &type, const QString &id, const QString &entry)
{
    return QStringLiteral("%1:%2:%3").arg(type, id, entry);
}

void addSample(QStringList *samples, const QString &value)
{
    if (samples && samples->size() < kSampleLimit)
        samples->append(value);
}

struct CollectionSnapshot
{
    QHash<QString, int> recordCounts;
    QSet<QString> collections;
    int recordTotal = 0;
};

CollectionSnapshot collectSnapshot(const pugi::xml_document &document)
{
    CollectionSnapshot snapshot;
    const pugi::xml_node root = document.child("Catalog") ? document.child("Catalog") : document.document_element();
    for (pugi::xml_node collection = root.first_child(); collection; collection = collection.next_sibling()) {
        const QString type = QString::fromUtf8(collection.name());
        if (!isDataCollectionNodeName(type))
            continue;
        const QString id = QString::fromUtf8(collection.attribute("id").value());
        if (id.isEmpty())
            continue;
        snapshot.collections.insert(collectionKey(type, id));
        for (pugi::xml_node record : collection.children("DataRecord")) {
            const QString entry = QString::fromUtf8(record.attribute("Entry").value());
            if (entry.isEmpty())
                continue;
            snapshot.recordCounts[recordKey(type, id, entry)] += 1;
            ++snapshot.recordTotal;
        }
    }
    return snapshot;
}

pugi::xml_node findCollectionByTypeAndId(pugi::xml_document &document, const QString &type, const QString &id)
{
    const pugi::xml_node root = document.child("Catalog") ? document.child("Catalog") : document.document_element();
    for (pugi::xml_node collection = root.first_child(); collection; collection = collection.next_sibling()) {
        if (QString::fromUtf8(collection.name()).compare(type, Qt::CaseInsensitive) == 0
            && QString::fromUtf8(collection.attribute("id").value()).compare(id, Qt::CaseInsensitive) == 0)
            return collection;
    }
    return {};
}

pugi::xml_node findCollectionById(pugi::xml_document &document, const QString &id)
{
    const pugi::xml_node root = document.child("Catalog") ? document.child("Catalog") : document.document_element();
    for (pugi::xml_node collection = root.first_child(); collection; collection = collection.next_sibling()) {
        const QString type = QString::fromUtf8(collection.name());
        if (isDataCollectionNodeName(type)
            && QString::fromUtf8(collection.attribute("id").value()).compare(id, Qt::CaseInsensitive) == 0)
            return collection;
    }
    return {};
}

void compareSnapshots(const CollectionSnapshot &baseline, const CollectionSnapshot &staged,
                      int *missing, QStringList *missingSamples,
                      int *added, QStringList *addedSamples)
{
    int missingCount = 0;
    for (auto it = baseline.recordCounts.cbegin(); it != baseline.recordCounts.cend(); ++it) {
        const int delta = it.value() - staged.recordCounts.value(it.key());
        if (delta <= 0)
            continue;
        missingCount += delta;
        const QStringList parts = it.key().split(QChar(0x1f));
        if (parts.size() >= 2)
            addSample(missingSamples, QStringLiteral("%1:%2").arg(parts[0], parts[1]));
    }
    if (missing)
        *missing = missingCount;

    int addedCount = 0;
    for (auto it = staged.recordCounts.cbegin(); it != staged.recordCounts.cend(); ++it) {
        const int delta = it.value() - baseline.recordCounts.value(it.key());
        if (delta <= 0)
            continue;
        addedCount += delta;
        const QStringList parts = it.key().split(QChar(0x1f));
        if (parts.size() >= 2)
            addSample(addedSamples, QStringLiteral("%1:%2").arg(parts[0], parts[1]));
    }
    if (added)
        *added = addedCount;
}

} // namespace

QString DataCollectionPreservationReport::summaryText() const
{
    return QStringLiteral("Data Collection preservation: baseline collections=%1, staged collections=%2, "
                          "baseline records=%3, staged records=%4, missing before restore=%5, "
                          "restored=%6, missing after restore=%7, added=%8")
        .arg(baselineCollections)
        .arg(stagedCollections)
        .arg(baselineRecords)
        .arg(stagedRecords)
        .arg(missingBeforeRestore)
        .arg(restoredRecords)
        .arg(missingAfterRestore)
        .arg(addedRecords);
}

bool restoreMissingDataCollectionRecords(const QByteArray &baselineBytes, QByteArray *stagedBytes,
                                         DataCollectionPreservationReport *report, QString *error)
{
    if (!stagedBytes)
        return true;

    pugi::xml_document baselineDocument;
    const auto baselineParsed = baselineDocument.load_buffer(baselineBytes.constData(), size_t(baselineBytes.size()));
    if (!baselineParsed) {
        if (error)
            *error = QStringLiteral("Cannot parse baseline DataCollection XML: %1").arg(baselineParsed.description());
        return false;
    }

    pugi::xml_document stagedDocument;
    const auto stagedParsed = stagedDocument.load_buffer(stagedBytes->constData(), size_t(stagedBytes->size()));
    if (!stagedParsed) {
        if (error)
            *error = QStringLiteral("Cannot parse staged DataCollection XML: %1").arg(stagedParsed.description());
        return false;
    }

    const CollectionSnapshot baselineBefore = collectSnapshot(baselineDocument);
    CollectionSnapshot stagedBefore = collectSnapshot(stagedDocument);
    DataCollectionPreservationReport local;
    local.baselineCollections = baselineBefore.collections.size();
    local.stagedCollections = stagedBefore.collections.size();
    local.baselineRecords = baselineBefore.recordTotal;
    local.stagedRecords = stagedBefore.recordTotal;
    compareSnapshots(baselineBefore, stagedBefore, &local.missingBeforeRestore, &local.missingSamples,
                     &local.addedRecords, &local.addedSamples);

    if (local.missingBeforeRestore > 0) {
        pugi::xml_node stagedCatalog = stagedDocument.child("Catalog");
        if (!stagedCatalog)
            stagedCatalog = stagedDocument.append_child("Catalog");

        const pugi::xml_node baselineRoot = baselineDocument.child("Catalog")
            ? baselineDocument.child("Catalog") : baselineDocument.document_element();
        for (pugi::xml_node baselineCollection = baselineRoot.first_child(); baselineCollection;
             baselineCollection = baselineCollection.next_sibling()) {
            const QString type = QString::fromUtf8(baselineCollection.name());
            if (!isDataCollectionNodeName(type))
                continue;
            const QString id = QString::fromUtf8(baselineCollection.attribute("id").value());
            if (id.isEmpty())
                continue;

            pugi::xml_node stagedCollection = findCollectionByTypeAndId(stagedDocument, type, id);
            if (!stagedCollection)
                stagedCollection = findCollectionById(stagedDocument, id);
            if (!stagedCollection) {
                stagedCatalog.append_copy(baselineCollection);
                for (pugi::xml_node record : baselineCollection.children("DataRecord")) {
                    const QString entry = QString::fromUtf8(record.attribute("Entry").value());
                    if (!entry.isEmpty()) {
                        ++local.restoredRecords;
                        addSample(&local.restoredSamples, sampleText(type, id, entry));
                    }
                }
                continue;
            }

            QHash<QString, int> stagedCounts;
            for (pugi::xml_node record : stagedCollection.children("DataRecord")) {
                const QString entry = QString::fromUtf8(record.attribute("Entry").value());
                if (!entry.isEmpty())
                    stagedCounts[entry] += 1;
            }
            QHash<QString, int> baselineSeen;
            for (pugi::xml_node record : baselineCollection.children("DataRecord")) {
                const QString entry = QString::fromUtf8(record.attribute("Entry").value());
                if (entry.isEmpty())
                    continue;
                baselineSeen[entry] += 1;
                if (stagedCounts.value(entry) >= baselineSeen.value(entry))
                    continue;
                stagedCollection.append_copy(record);
                stagedCounts[entry] += 1;
                ++local.restoredRecords;
                addSample(&local.restoredSamples, sampleText(type, id, entry));
            }
        }

        std::ostringstream stream;
        stagedDocument.save(stream, "    ", pugi::format_default, pugi::encoding_utf8);
        *stagedBytes = QByteArray::fromStdString(stream.str());

        pugi::xml_document verifiedDocument;
        const auto verifiedParsed = verifiedDocument.load_buffer(stagedBytes->constData(), size_t(stagedBytes->size()));
        if (!verifiedParsed) {
            if (error)
                *error = QStringLiteral("Cannot parse preserved DataCollection XML: %1").arg(verifiedParsed.description());
            return false;
        }
        CollectionSnapshot stagedAfter = collectSnapshot(verifiedDocument);
        local.stagedCollections = stagedAfter.collections.size();
        local.stagedRecords = stagedAfter.recordTotal;
        QStringList missingAfterSamples;
        compareSnapshots(baselineBefore, stagedAfter, &local.missingAfterRestore, &missingAfterSamples,
                         &local.addedRecords, &local.addedSamples);
    }

    if (report)
        *report = local;
    if (local.missingAfterRestore > 0) {
        if (error)
            *error = QStringLiteral("Data Collection preservation failed: %1 records are still missing.")
                         .arg(local.missingAfterRestore);
        return false;
    }
    return true;
}
