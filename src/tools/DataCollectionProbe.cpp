#include "core/DataCollectionAliasMapper.h"
#include "core/DataCollectionUnitBuilder.h"
#include "core/FolderAnalyzer.h"
#include "core/Sc2Archive.h"
#include "core/UnitFamilyDetector.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTextStream>

#include <pugixml.hpp>

namespace {

bool writeBytes(const QString &path, const QByteArray &bytes, QString *error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = QStringLiteral("Unable to write %1").arg(path);
        return false;
    }
    return true;
}

bool isDataCollectionName(const QString &name)
{
    return name.startsWith(QStringLiteral("CDataCollection"), Qt::CaseInsensitive)
        && !name.startsWith(QStringLiteral("CDataCollectionPattern"), Qt::CaseInsensitive);
}

struct CollectionStats
{
    int files = 0;
    int collections = 0;
    int directRecords = 0;
    QSet<QString> pairs;
    QSet<QString> entries;
};

void collectStats(const QString &rootFolder, CollectionStats *stats)
{
    if (!stats)
        return;
    const QStringList candidates = QDir(rootFolder).entryList();
    Q_UNUSED(candidates);
    QDirIterator it(rootFolder, QStringList{QStringLiteral("DataCollectionData.xml")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        ++stats->files;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QByteArray bytes = file.readAll();
        pugi::xml_document document;
        if (!document.load_buffer(bytes.constData(), size_t(bytes.size())))
            continue;
        const pugi::xml_node catalog = document.child("Catalog") ? document.child("Catalog") : document.document_element();
        for (pugi::xml_node collection = catalog.first_child(); collection; collection = collection.next_sibling()) {
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

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);
    const QStringList args = app.arguments();
    if (args.size() != 2) {
        err << "Usage: SC2DataCollectionProbe <map.SC2Map>\n";
        return 2;
    }

    const QString archivePath = QDir::fromNativeSeparators(args.at(1));
    Sc2Archive archive;
    QString error;
    if (!archive.load(archivePath, &error)) {
        err << "archive_load_failed=" << error << '\n';
        return 3;
    }

    QTemporaryDir workspace;
    if (!workspace.isValid()) {
        err << "temp_workspace_failed\n";
        return 4;
    }

    int skippedExistingCollections = 0;
    int materializedXml = 0;
    for (const QString &entry : archive.gameDataXmlEntries()) {
        if (entry.endsWith(QStringLiteral("DataCollectionData.xml"), Qt::CaseInsensitive)) {
            ++skippedExistingCollections;
            continue;
        }
        QByteArray bytes;
        if (!archive.readEntry(entry, &bytes, &error)) {
            err << "archive_read_failed=" << entry << " | " << error << '\n';
            return 5;
        }
        QString relative = entry;
        relative.replace('\\', '/');
        if (!writeBytes(QDir(workspace.path()).absoluteFilePath(relative), bytes, &error)) {
            err << error << '\n';
            return 6;
        }
        ++materializedXml;
    }
    QByteArray listfileBytes;
    if (!archive.readEntry(QStringLiteral("(listfile)"), &listfileBytes, &error)) {
        QStringList entries;
        for (QString entry : archive.allEntries())
            entries << entry.replace('/', '\\');
        listfileBytes = entries.join(QStringLiteral("\r\n")).toUtf8() + QByteArrayLiteral("\r\n");
    }
    if (!writeBytes(QDir(workspace.path()).absoluteFilePath(QStringLiteral("(listfile)")), listfileBytes, &error)) {
        err << error << '\n';
        return 7;
    }

    FolderAnalyzer analyzer;
    AnalysisResult analysis;
    if (!analyzer.analyzeFolder(workspace.path(), {}, &analysis, &error)) {
        err << "analysis_failed=" << error << '\n';
        return 8;
    }

    const QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(
        analysis, DataCollectionMode::UnitAbilWeapon);
    DataCollectionAliasMapper mapper;
    QSet<QString> expectedPairs;
    int expectedDirect = 0;
    int skippedConflicts = 0;
    for (const UnitFamily &family : families) {
        if (family.rootTypeConflict) {
            ++skippedConflicts;
            continue;
        }
        for (const UnitFamilyObject &object : family.objects) {
            if (object.nodeIndex < 0 || object.nodeIndex >= analysis.nodes.size())
                continue;
            const QString alias = mapper.aliasFor(analysis.nodes[object.nodeIndex], family.rootId, object.role);
            if (alias.isEmpty())
                continue;
            ++expectedDirect;
            expectedPairs.insert(family.collectionElementName + QChar(0x1f) + family.rootId + QChar(0x1f) + alias);
        }
    }

    DataCollectionUnitBuilder builder;
    int applied = 0;
    int skippedInvalid = 0;
    int recordsAdded = 0;
    for (const UnitFamily &family : families) {
        if (family.rootTypeConflict) {
            ++skippedInvalid;
            continue;
        }
        DataCollectionBuildRequest request;
        request.family = family;
        request.requestedUnitId = family.rootId;
        request.confirmNonStandard = true;
        const DataCollectionApplyResult result = builder.apply(
            analysis, request, workspace.path(), {}, false, &families, true);
        if (!result.success) {
            ++skippedInvalid;
            continue;
        }
        ++applied;
        recordsAdded += result.recordsAdded;
    }

    CollectionStats stats;
    collectStats(workspace.path(), &stats);
    QSet<QString> missing = expectedPairs;
    missing.subtract(stats.pairs);
    QSet<QString> extra = stats.pairs;
    extra.subtract(expectedPairs);

    out << "archive=" << archivePath << '\n';
    out << "archive_entries=" << archive.totalEntriesCount() << '\n';
    out << "game_data_xml_materialized=" << materializedXml << '\n';
    out << "existing_datacollection_entries_skipped_for_zero_generation=" << skippedExistingCollections << '\n';
    out << "families_detected=" << families.size() << '\n';
    out << "families_applied=" << applied << '\n';
    out << "families_skipped_invalid=" << skippedInvalid << '\n';
    out << "root_type_conflicts=" << skippedConflicts << '\n';
    out << "records_added_reported=" << recordsAdded << '\n';
    out << "expected_direct_from_graph=" << expectedDirect << '\n';
    out << "expected_unique_pairs=" << expectedPairs.size() << '\n';
    out << "generated_collection_files=" << stats.files << '\n';
    out << "generated_collections=" << stats.collections << '\n';
    out << "generated_direct_records=" << stats.directRecords << '\n';
    out << "generated_unique_pairs=" << stats.pairs.size() << '\n';
    out << "generated_unique_entries=" << stats.entries.size() << '\n';
    out << "missing_expected_pairs=" << missing.size() << '\n';
    out << "extra_pairs=" << extra.size() << '\n';
    return missing.isEmpty() ? 0 : 9;
}
