#include "core/DecorationMapCopyService.h"
#include "core/DecorationStreamingPlanner.h"
#include "core/MapRegionRepository.h"
#include "core/Sc2Archive.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>

#include <algorithm>
#include <limits>

using sc2dh::decor::DecorOptimizedMapRequest;
using sc2dh::decor::DecorOptimizedMapResult;
using sc2dh::decor::DecorationMapCopyService;
using sc2dh::decor::DecorationOptimizationMode;
using sc2dh::decor::DecorationStreamingPlan;
using sc2dh::decor::DecorationStreamingPlanner;
using sc2dh::decor::DecorationVisibilityPlan;
using sc2dh::decor::DecorZone;
using sc2dh::decor::DoodadPlacement;
using sc2dh::decor::GalaxyGenerationOptions;
using sc2dh::decor::ZoneAssignment;
using sc2dh::region::MapRegionRepository;
using sc2dh::region::RegionReadResult;

namespace
{

bool readFile(const QString &path, QByteArray *bytes, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Unable to read %1").arg(QDir::toNativeSeparators(path));
        return false;
    }
    if (bytes)
        *bytes = file.readAll();
    return true;
}

bool writeJsonFile(const QString &path, const QJsonObject &object, QString *error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Unable to write %1").arg(QDir::toNativeSeparators(path));
        return false;
    }
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size() || !file.commit()) {
        if (error)
            *error = QStringLiteral("Unable to commit %1").arg(QDir::toNativeSeparators(path));
        return false;
    }
    return true;
}

QJsonArray stringArray(QStringList values, bool sorted = false)
{
    if (sorted)
        values.sort(Qt::CaseInsensitive);
    QJsonArray array;
    for (const QString &value : values)
        array.append(value);
    return array;
}

QJsonValue firstValue(const QJsonObject &object, std::initializer_list<const char *> keys)
{
    for (const char *key : keys) {
        const QJsonValue value = object.value(QLatin1String(key));
        if (!value.isUndefined() && !value.isNull())
            return value;
    }
    return {};
}

bool valueToDouble(const QJsonValue &value, double *out)
{
    if (value.isDouble()) {
        *out = value.toDouble();
        return true;
    }
    if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().toDouble(&ok);
        if (ok) {
            *out = parsed;
            return true;
        }
    }
    return false;
}

bool readRequiredDouble(const QJsonObject &object,
                        std::initializer_list<const char *> keys,
                        double *out,
                        QString *error)
{
    const QJsonValue value = firstValue(object, keys);
    if (valueToDouble(value, out))
        return true;

    QStringList names;
    for (const char *key : keys)
        names << QString::fromLatin1(key);
    if (error)
        *error = QStringLiteral("Zone is missing numeric field: %1").arg(names.join(QStringLiteral("/")));
    return false;
}

bool loadZonesJson(const QString &path, QVector<DecorZone> *zones, QString *error)
{
    QByteArray bytes;
    if (!readFile(path, &bytes, error))
        return false;

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error)
            *error = QStringLiteral("Invalid zones JSON at offset %1: %2")
                         .arg(parseError.offset)
                         .arg(parseError.errorString());
        return false;
    }

    QJsonArray array;
    if (document.isArray()) {
        array = document.array();
    } else if (document.isObject()) {
        const QJsonValue zonesValue = document.object().value(QStringLiteral("zones"));
        if (!zonesValue.isArray()) {
            if (error)
                *error = QStringLiteral("Zones JSON must be an array or an object with a zones array.");
            return false;
        }
        array = zonesValue.toArray();
    } else {
        if (error)
            *error = QStringLiteral("Zones JSON root must be an object or array.");
        return false;
    }

    QVector<DecorZone> parsed;
    QSet<int> ids;
    for (int i = 0; i < array.size(); ++i) {
        if (!array.at(i).isObject()) {
            if (error)
                *error = QStringLiteral("Zone %1 must be an object.").arg(i + 1);
            return false;
        }
        const QJsonObject object = array.at(i).toObject();
        DecorZone zone;
        zone.id = object.value(QStringLiteral("id")).toInt(i + 1);
        zone.name = object.value(QStringLiteral("name")).toString(QStringLiteral("Zone_%1").arg(zone.id));
        if (zone.id <= 0) {
            if (error)
                *error = QStringLiteral("Zone %1 has invalid id %2.").arg(i + 1).arg(zone.id);
            return false;
        }
        if (ids.contains(zone.id)) {
            if (error)
                *error = QStringLiteral("Duplicate zone id: %1").arg(zone.id);
            return false;
        }
        ids.insert(zone.id);

        if (!readRequiredDouble(object, {"xMin", "minX", "x1", "left"}, &zone.xMin, error)
            || !readRequiredDouble(object, {"yMin", "minY", "y1", "bottom"}, &zone.yMin, error)
            || !readRequiredDouble(object, {"xMax", "maxX", "x2", "right"}, &zone.xMax, error)
            || !readRequiredDouble(object, {"yMax", "maxY", "y2", "top"}, &zone.yMax, error)) {
            if (error)
                *error = QStringLiteral("Zone %1: %2").arg(i + 1).arg(*error);
            return false;
        }

        parsed << zone;
    }

    if (parsed.isEmpty()) {
        if (error)
            *error = QStringLiteral("Zones JSON contains no zones.");
        return false;
    }
    if (zones)
        *zones = parsed;
    return true;
}

bool parseGridSpec(const QString &spec, int *columns, int *rows)
{
    const QString normalized = spec.trimmed().toLower();
    const QStringList parts = normalized.split(QLatin1Char('x'));
    if (parts.size() != 2)
        return false;
    bool okColumns = false;
    bool okRows = false;
    const int parsedColumns = parts.at(0).toInt(&okColumns);
    const int parsedRows = parts.at(1).toInt(&okRows);
    if (!okColumns || !okRows || parsedColumns <= 0 || parsedRows <= 0)
        return false;
    if (columns)
        *columns = parsedColumns;
    if (rows)
        *rows = parsedRows;
    return true;
}

bool buildAutoGridZones(const QString &sourceArchivePath,
                        int columns,
                        int rows,
                        double padding,
                        QVector<DecorZone> *zones,
                        QJsonObject *autoGridReport,
                        QString *error)
{
    Sc2Archive archive;
    if (!archive.load(sourceArchivePath, error))
        return false;

    QByteArray objectsBytes;
    if (!archive.readEntry(QStringLiteral("Objects"), &objectsBytes, error))
        return false;

    DecorationStreamingPlanner planner;
    QStringList warnings;
    const QVector<DoodadPlacement> doodads = planner.parseObjects(objectsBytes, &warnings);

    double xMin = std::numeric_limits<double>::max();
    double yMin = std::numeric_limits<double>::max();
    double xMax = std::numeric_limits<double>::lowest();
    double yMax = std::numeric_limits<double>::lowest();
    int dynamicCandidates = 0;
    int staticOnly = 0;
    for (const DoodadPlacement &doodad : doodads) {
        if (!doodad.dynamicCandidate) {
            ++staticOnly;
            continue;
        }
        ++dynamicCandidates;
        xMin = std::min(xMin, doodad.x);
        yMin = std::min(yMin, doodad.y);
        xMax = std::max(xMax, doodad.x);
        yMax = std::max(yMax, doodad.y);
    }

    if (dynamicCandidates == 0) {
        if (error)
            *error = QStringLiteral("Auto-grid found no runtime-safe visual doodads in Objects.");
        return false;
    }

    xMin -= padding;
    yMin -= padding;
    xMax += padding;
    yMax += padding;
    if (xMin == xMax) {
        xMin -= 0.5;
        xMax += 0.5;
    }
    if (yMin == yMax) {
        yMin -= 0.5;
        yMax += 0.5;
    }

    QVector<DecorZone> generated;
    const double width = (xMax - xMin) / double(columns);
    const double height = (yMax - yMin) / double(rows);
    int id = 1;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            DecorZone zone;
            zone.id = id++;
            zone.name = QStringLiteral("Auto_R%1_C%2").arg(row + 1).arg(column + 1);
            zone.xMin = xMin + width * column;
            zone.xMax = column == columns - 1 ? xMax : xMin + width * (column + 1);
            zone.yMin = yMin + height * row;
            zone.yMax = row == rows - 1 ? yMax : yMin + height * (row + 1);
            generated << zone;
        }
    }

    if (zones)
        *zones = generated;
    if (autoGridReport) {
        autoGridReport->insert(QStringLiteral("columns"), columns);
        autoGridReport->insert(QStringLiteral("rows"), rows);
        autoGridReport->insert(QStringLiteral("padding"), padding);
        autoGridReport->insert(QStringLiteral("sourceDynamicCandidates"), dynamicCandidates);
        autoGridReport->insert(QStringLiteral("sourceStaticOnlyDoodads"), staticOnly);
        autoGridReport->insert(QStringLiteral("parseWarnings"), stringArray(warnings));
        autoGridReport->insert(QStringLiteral("bounds"), QJsonObject{
            {QStringLiteral("xMin"), xMin},
            {QStringLiteral("yMin"), yMin},
            {QStringLiteral("xMax"), xMax},
            {QStringLiteral("yMax"), yMax}
        });
    }
    return true;
}

bool loadMapRegionZones(const QString &sourceArchivePath,
                        QVector<DecorZone> *zones,
                        QString *error)
{
    Sc2Archive archive;
    if (!archive.load(sourceArchivePath, error))
        return false;

    QByteArray regionsBytes;
    if (!archive.readEntry(QStringLiteral("Regions"), &regionsBytes, error))
        return false;

    const RegionReadResult parsed = MapRegionRepository().parse(regionsBytes, QStringLiteral("Regions"));
    if (!parsed.success || !parsed.complete) {
        if (error) {
            *error = QStringLiteral("Exact map Regions could not be read completely: %1")
                         .arg((parsed.errors + parsed.warnings).join(QStringLiteral("; ")));
        }
        return false;
    }

    QVector<DecorZone> result;
    QSet<int> usedIds;
    int generatedId = 1;
    for (const auto &region : parsed.regions) {
        if (!region.geometry.supported)
            continue;
        DecorZone zone;
        bool idOk = false;
        zone.id = region.id.toInt(&idOk);
        if (!idOk)
            zone.id = 0;
        if (zone.id <= 0 || usedIds.contains(zone.id)) {
            while (usedIds.contains(generatedId))
                ++generatedId;
            zone.id = generatedId++;
        }
        usedIds.insert(zone.id);
        zone.name = region.name.isEmpty() ? QStringLiteral("Region_%1").arg(zone.id) : region.name;
        zone.xMin = region.geometry.bounds.xMin;
        zone.yMin = region.geometry.bounds.yMin;
        zone.xMax = region.geometry.bounds.xMax;
        zone.yMax = region.geometry.bounds.yMax;
        zone.geometry = region.geometry;
        result << zone;
    }
    if (result.isEmpty()) {
        if (error)
            *error = QStringLiteral("The map contains no supported exact Regions.");
        return false;
    }
    if (zones)
        *zones = result;
    return true;
}

QJsonObject zoneJson(const DecorZone &zone)
{
    return QJsonObject{
        {QStringLiteral("id"), zone.id},
        {QStringLiteral("name"), zone.name},
        {QStringLiteral("xMin"), zone.xMin},
        {QStringLiteral("yMin"), zone.yMin},
        {QStringLiteral("xMax"), zone.xMax},
        {QStringLiteral("yMax"), zone.yMax}
    };
}

QJsonArray zonesJson(const QVector<DecorZone> &zones,
                     const QVector<ZoneAssignment> &assignments,
                     const QString &countKey)
{
    QHash<int, int> dynamicCounts;
    for (const ZoneAssignment &assignment : assignments)
        dynamicCounts.insert(assignment.zoneId, assignment.doodadIndices.size());

    QJsonArray array;
    for (const DecorZone &zone : zones) {
        QJsonObject object = zoneJson(zone);
        object.insert(countKey, dynamicCounts.value(zone.id));
        array.append(object);
    }
    return array;
}

QJsonObject visibilityStaticReasonCounts(const DecorationVisibilityPlan &plan)
{
    QMap<QString, int> counts;
    for (int index : plan.staticOnlyDoodads) {
        if (index < 0 || index >= plan.doodads.size())
            continue;
        const QString reason = plan.doodads.at(index).visibilityStaticOnlyReason.trimmed();
        ++counts[reason.isEmpty() ? QStringLiteral("Static-only: unspecified safety reason.") : reason];
    }

    QJsonObject object;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it)
        object.insert(it.key(), it.value());
    return object;
}

QJsonObject reportObject(const QString &sourcePath,
                         const QVector<DecorZone> &zones,
                         const DecorOptimizedMapRequest &request,
                         const DecorOptimizedMapResult &result,
                         const QJsonObject &autoGridReport = {})
{
    QJsonObject report;
    report.insert(QStringLiteral("success"), result.success);
    report.insert(QStringLiteral("source"), QDir::toNativeSeparators(sourcePath));
    report.insert(QStringLiteral("output"), QDir::toNativeSeparators(result.outputArchivePath));
    report.insert(QStringLiteral("removedDoodads"), result.removedDoodads);
    report.insert(QStringLiteral("visibilityControlledDoodads"), result.visibilityControlledDoodads);
    report.insert(QStringLiteral("objectsPreserved"), result.objectsPreserved);
    report.insert(QStringLiteral("mode"), request.mode == DecorationOptimizationMode::VisibilityOnly
                                        ? QStringLiteral("visibility-only")
                                        : QStringLiteral("recreate-actors"));
    report.insert(QStringLiteral("warnings"), stringArray(result.warnings));
    report.insert(QStringLiteral("functionPrefix"), request.galaxyOptions.functionPrefix);
    report.insert(QStringLiteral("objectsEntry"), request.objectsEntry);
    report.insert(QStringLiteral("mapScriptEntry"), request.mapScriptEntry);
    report.insert(QStringLiteral("runtimeEntry"), request.runtimeEntry);
    if (!autoGridReport.isEmpty())
        report.insert(QStringLiteral("autoGrid"), autoGridReport);

    // A visibility plan is available even when safety validation blocks the
    // write. Include its immutable diagnostics so a caller can see why a
    // fail-closed request did not select any actors.
    if (request.mode == DecorationOptimizationMode::VisibilityOnly
        && !result.patch.visibilityArtifacts.plan.doodads.isEmpty()) {
        const DecorationVisibilityPlan &plan = result.patch.visibilityArtifacts.plan;
        report.insert(QStringLiteral("totalDoodads"), plan.doodads.size());
        report.insert(QStringLiteral("staticOnlyDoodads"), plan.staticOnlyDoodads.size());
        report.insert(QStringLiteral("unassignedVisibilityDoodads"), plan.unassignedDoodads.size());
        report.insert(QStringLiteral("visibilitySafetyReasons"), visibilityStaticReasonCounts(plan));
        report.insert(QStringLiteral("zones"), zonesJson(zones, plan.zones,
                                                           QStringLiteral("visibilityControlledDoodads")));
    }

    if (result.success) {
        if (request.mode != DecorationOptimizationMode::VisibilityOnly) {
            const DecorationStreamingPlan &plan = result.patch.artifacts.plan;
            report.insert(QStringLiteral("totalDoodads"), plan.doodads.size());
            report.insert(QStringLiteral("staticOnlyDoodads"), plan.staticOnlyDoodads.size());
            report.insert(QStringLiteral("unassignedDynamicDoodads"), plan.unassignedDoodads.size());
            report.insert(QStringLiteral("zones"), zonesJson(zones, plan.zones,
                                                               QStringLiteral("dynamicDoodads")));
        }
        QStringList entries = result.patch.replacementEntries.keys();
        report.insert(QStringLiteral("patchedEntries"), stringArray(entries, true));
    } else {
        report.insert(QStringLiteral("error"), result.error);
        QJsonArray zonesArray;
        for (const DecorZone &zone : zones)
            zonesArray.append(zoneJson(zone));
        report.insert(QStringLiteral("zones"), zonesArray);
    }
    return report;
}

struct CliOptions
{
    bool help = false;
    bool version = false;
    bool overwrite = false;
    bool visibilityOnly = false;
    bool mapRegions = false;
    QString sourcePath;
    QString outputPath;
    QString zonesPath;
    QString autoGridSpec;
    QString prefix = QStringLiteral("NAME_OUT_FUNK");
    QString padding = QStringLiteral("0");
    QString reportPath;
    QString objectsEntry = QStringLiteral("Objects");
    QString mapScriptEntry = QStringLiteral("MapScript.galaxy");
    QString runtimeEntry = QStringLiteral("scripts/sc2dh_decor_opt.galaxy");
};

QString usageText()
{
    return QStringLiteral(
        "Usage:\n"
        "  SC2DecorOptimizeMap <source.SC2Map> <output.SC2Map> --zones <zones.json> [options]\n"
        "  SC2DecorOptimizeMap <source.SC2Map> <output.SC2Map> --auto-grid <COLSxROWS> [options]\n"
        "  SC2DecorOptimizeMap <source.SC2Map> <output.SC2Map> --map-regions [options]\n"
        "\n"
        "Options:\n"
        "  --zones <file>              JSON array/object with zones: id/name/xMin/yMin/xMax/yMax.\n"
        "  --auto-grid <COLSxROWS>     Build zones from current runtime-safe doodad bounds, e.g. 4x3.\n"
        "  --map-regions              Use every supported exact Region from the map.\n"
        "  --visibility-only            Keep Objects unchanged; add hide/restore actor visibility API (requires --zones).\n"
        "  --padding <tiles>           Non-negative auto-grid padding. Default: 0.\n"
        "  --prefix <name>             Generated public function prefix. Default: NAME_OUT_FUNK.\n"
        "  --report <file>             Write JSON report.\n"
        "  --overwrite                 Replace existing output archive.\n"
        "  --objects-entry <entry>     Placement entry. Default: Objects.\n"
        "  --mapscript-entry <entry>   Map script entry. Default: MapScript.galaxy.\n"
        "  --runtime-entry <entry>     Generated Galaxy entry. Default: scripts/sc2dh_decor_opt.galaxy.\n"
        "  --help                      Show this help.\n"
        "  --version                   Show version.\n");
}

bool consumeOptionValue(const QStringList &args, int *index, const QString &currentValue, QString *target, QString *error)
{
    if (!currentValue.isNull()) {
        *target = currentValue;
        return true;
    }
    if (*index + 1 >= args.size()) {
        if (error)
            *error = QStringLiteral("Missing value for %1.").arg(args.at(*index));
        return false;
    }
    if (args.at(*index + 1).startsWith(QStringLiteral("--"))) {
        if (error)
            *error = QStringLiteral("Missing value for %1.").arg(args.at(*index));
        return false;
    }
    ++(*index);
    *target = args.at(*index);
    return true;
}

bool parseCli(const QStringList &args, CliOptions *options, QString *error)
{
    QStringList positional;
    for (int i = 1; i < args.size(); ++i) {
        const QString argument = args.at(i);
        if (argument == QStringLiteral("--")) {
            for (++i; i < args.size(); ++i)
                positional << args.at(i);
            break;
        }
        if (argument == QStringLiteral("--help") || argument == QStringLiteral("-h")) {
            options->help = true;
            continue;
        }
        if (argument == QStringLiteral("--version")) {
            options->version = true;
            continue;
        }
        if (!argument.startsWith(QStringLiteral("--"))) {
            positional << argument;
            continue;
        }

        QString name = argument;
        QString suppliedValue;
        const int equals = argument.indexOf(QLatin1Char('='));
        if (equals >= 0) {
            name = argument.left(equals);
            suppliedValue = argument.mid(equals + 1);
        }

        if (name == QStringLiteral("--overwrite") || name == QStringLiteral("--visibility-only")
            || name == QStringLiteral("--map-regions")) {
            if (equals >= 0) {
                if (error)
                    *error = QStringLiteral("%1 does not accept a value.").arg(name);
                return false;
            }
            if (name == QStringLiteral("--overwrite"))
                options->overwrite = true;
            else if (name == QStringLiteral("--map-regions"))
                options->mapRegions = true;
            else
                options->visibilityOnly = true;
        } else if (name == QStringLiteral("--zones")) {
            if (!consumeOptionValue(args, &i, suppliedValue, &options->zonesPath, error))
                return false;
        } else if (name == QStringLiteral("--auto-grid")) {
            if (!consumeOptionValue(args, &i, suppliedValue, &options->autoGridSpec, error))
                return false;
        } else if (name == QStringLiteral("--padding")) {
            if (!consumeOptionValue(args, &i, suppliedValue, &options->padding, error))
                return false;
        } else if (name == QStringLiteral("--prefix")) {
            if (!consumeOptionValue(args, &i, suppliedValue, &options->prefix, error))
                return false;
        } else if (name == QStringLiteral("--report")) {
            if (!consumeOptionValue(args, &i, suppliedValue, &options->reportPath, error))
                return false;
        } else if (name == QStringLiteral("--objects-entry")) {
            if (!consumeOptionValue(args, &i, suppliedValue, &options->objectsEntry, error))
                return false;
        } else if (name == QStringLiteral("--mapscript-entry")) {
            if (!consumeOptionValue(args, &i, suppliedValue, &options->mapScriptEntry, error))
                return false;
        } else if (name == QStringLiteral("--runtime-entry")) {
            if (!consumeOptionValue(args, &i, suppliedValue, &options->runtimeEntry, error))
                return false;
        } else {
            if (error)
                *error = QStringLiteral("Unknown option: %1").arg(name);
            return false;
        }
    }

    if (options->help || options->version)
        return true;
    if (positional.size() != 2) {
        if (error)
            *error = QStringLiteral("Expected exactly two positional arguments: source and output.");
        return false;
    }
    const int zoneSources = int(!options->zonesPath.isEmpty())
        + int(!options->autoGridSpec.isEmpty())
        + int(options->mapRegions);
    if (zoneSources != 1) {
        if (error)
            *error = QStringLiteral("Specify exactly one of --zones, --auto-grid, or --map-regions.");
        return false;
    }
    if (options->visibilityOnly && !options->autoGridSpec.isEmpty()) {
        if (error)
            *error = QStringLiteral("--visibility-only requires --zones or --map-regions; --auto-grid is for actor recreation only.");
        return false;
    }
    options->sourcePath = positional.at(0);
    options->outputPath = positional.at(1);
    return true;
}

int fail(const QString &message, int code)
{
    QTextStream(stderr) << message << '\n';
    return code;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("SC2DecorOptimizeMap"));
    QCoreApplication::setApplicationVersion(QStringLiteral(SC2DH_VERSION_NUMBER));

    CliOptions cli;
    QString error;
    if (!parseCli(app.arguments(), &cli, &error))
        return fail(error + QStringLiteral("\n\n") + usageText(), 2);
    if (cli.help) {
        QTextStream(stdout) << usageText();
        return 0;
    }
    if (cli.version) {
        QTextStream(stdout) << QStringLiteral("SC2DecorOptimizeMap %1 (%2)\n")
                                  .arg(QStringLiteral(SC2DH_VERSION_LABEL),
                                       QStringLiteral(SC2DH_VERSION_NUMBER));
        return 0;
    }

    bool ok = false;

    const QString sourcePath = QFileInfo(cli.sourcePath).absoluteFilePath();
    const QString outputPath = QFileInfo(cli.outputPath).absoluteFilePath();

    QVector<DecorZone> zones;
    QJsonObject autoGridReport;
    if (!cli.zonesPath.isEmpty()) {
        if (!loadZonesJson(cli.zonesPath, &zones, &error))
            return fail(error, 3);
    } else if (cli.mapRegions) {
        if (!loadMapRegionZones(sourcePath, &zones, &error))
            return fail(QStringLiteral("Map Regions failed: %1").arg(error), 3);
    } else {
        int columns = 0;
        int rows = 0;
        if (!parseGridSpec(cli.autoGridSpec, &columns, &rows))
            return fail(QStringLiteral("--auto-grid must use COLSxROWS, for example 4x3."), 2);
        const double padding = cli.padding.toDouble(&ok);
        if (!ok || padding < 0.0)
            return fail(QStringLiteral("--padding must be a non-negative number."), 2);
        if (!buildAutoGridZones(sourcePath, columns, rows, padding, &zones, &autoGridReport, &error))
            return fail(QStringLiteral("Auto-grid failed: %1").arg(error), 3);
    }

    DecorOptimizedMapRequest request;
    request.sourceArchivePath = sourcePath;
    request.outputArchivePath = outputPath;
    request.zones = zones;
    request.galaxyOptions = GalaxyGenerationOptions{cli.prefix, 64};
    request.objectsEntry = cli.objectsEntry;
    request.mapScriptEntry = cli.mapScriptEntry;
    request.runtimeEntry = cli.runtimeEntry;
    request.mode = cli.visibilityOnly ? DecorationOptimizationMode::VisibilityOnly
                                      : DecorationOptimizationMode::RecreateActors;
    request.overwriteExisting = cli.overwrite;

    const DecorOptimizedMapResult result = DecorationMapCopyService().createOptimizedCopy(request);
    const QJsonObject report = reportObject(sourcePath, zones, request, result, autoGridReport);

    if (!cli.reportPath.isEmpty()) {
        if (!writeJsonFile(cli.reportPath, report, &error))
            return fail(error, 5);
    }

    QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Compact) << '\n';
    if (!result.success)
        return fail(result.error, 4);
    return 0;
}
