#include "core/MapPerformanceAnalyzer.h"

#include "core/AssetFileRules.h"

#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <pugixml.hpp>

namespace
{

QString unquote(QString value)
{
    value = value.trimmed();
    if (value.size() >= 2 && value.front() == QLatin1Char('"') && value.back() == QLatin1Char('"'))
        return value.mid(1, value.size() - 2);
    return value;
}

QString fieldValue(const QString &block, const QString &field)
{
    const QRegularExpression expression(QStringLiteral("\\b%1\\b\\s*(?:=|:)\\s*(\"[^\"]*\"|\\([^)]*\\)|\\{[^}]*\\}|[^\\s,;]+)")
                                            .arg(QRegularExpression::escape(field)),
                                        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = expression.match(block);
    return match.hasMatch() ? unquote(match.captured(1)) : QString();
}

QVector<double> numbersFrom(const QString &value)
{
    QVector<double> result;
    static const QRegularExpression number(QStringLiteral("[-+]?\\d+(?:\\.\\d+)?"));
    auto matches = number.globalMatch(value);
    while (matches.hasNext())
        result << matches.next().captured(0).toDouble();
    return result;
}

bool findBlockClose(const QString &text, qsizetype open, qsizetype *close)
{
    int depth = 0;
    for (qsizetype i = open; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('{')) {
            ++depth;
        } else if (text.at(i) == QLatin1Char('}')) {
            --depth;
            if (depth == 0) {
                if (close)
                    *close = i;
                return true;
            }
        }
    }
    return false;
}

QString placementLabel(const sc2dh::perf::MapPlacement &placement)
{
    QString label = placement.kind;
    if (!placement.name.isEmpty())
        label += QStringLiteral("(") + placement.name + QStringLiteral(")");
    else if (!placement.id.isEmpty())
        label += QStringLiteral("(") + placement.id + QStringLiteral(")");
    else if (!placement.type.isEmpty())
        label += QStringLiteral("(") + placement.type + QStringLiteral(")");
    return label;
}

QString normalizedId(const QString &value)
{
    return value.trimmed().toCaseFolded();
}

QString nodeTypeKey(const QString &elementName)
{
    QString type = elementName;
    type.remove(QRegularExpression(QStringLiteral("^C"), QRegularExpression::CaseInsensitiveOption));
    return type.toCaseFolded();
}

bool isActorOrModelNode(const DataNode &node)
{
    const QString type = node.elementName.toLower();
    return type.contains(QStringLiteral("actor")) || type.contains(QStringLiteral("model"));
}

bool isAssetRelevantToPlacement(const QString &relativeFile, const QStringList &tokens)
{
    const QString folded = relativeFile.toCaseFolded();
    for (const QString &token : tokens) {
        if (!token.isEmpty() && folded.contains(token.toCaseFolded()))
            return true;
    }
    return false;
}

QStringList uniqueSorted(QStringList values)
{
    values.removeAll(QString());
    values.removeDuplicates();
    std::sort(values.begin(), values.end(), [](const QString &left, const QString &right) {
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
    return values;
}

void appendUnique(QStringList *values, const QString &value)
{
    if (!values || value.trimmed().isEmpty())
        return;
    values->append(value.trimmed());
    values->removeDuplicates();
}

int clampIndex(int value, int maxExclusive)
{
    return std::clamp(value, 0, std::max(0, maxExclusive - 1));
}

} // namespace

namespace sc2dh::perf
{

QVector<MapPlacement> MapPerformanceAnalyzer::parseObjectsPlacements(const QByteArray &objectsBytes,
                                                                     QStringList *warnings) const
{
    if (warnings)
        warnings->clear();
    const QString text = QString::fromUtf8(objectsBytes);
    QVector<MapPlacement> placements;

    if (text.trimmed().startsWith(QLatin1Char('<'))) {
        pugi::xml_document document;
        const pugi::xml_parse_result parsed = document.load_buffer(
            objectsBytes.constData(), size_t(objectsBytes.size()), pugi::parse_default, pugi::encoding_utf8);
        const pugi::xml_node root = document.child("PlacedObjects");
        if (!parsed || !root) {
            if (warnings)
                *warnings << QStringLiteral("PlacedObjects XML parse failed at byte %1: %2")
                                 .arg(parsed.offset)
                                 .arg(QString::fromUtf8(parsed.description()));
            return placements;
        }
        for (pugi::xml_node element : root.children()) {
            const QString kind = QString::fromUtf8(element.name());
            if (kind.compare(QStringLiteral("ObjectDoodad"), Qt::CaseInsensitive) != 0
                && kind.compare(QStringLiteral("ObjectUnit"), Qt::CaseInsensitive) != 0
                && kind.compare(QStringLiteral("ObjectDestructible"), Qt::CaseInsensitive) != 0)
                continue;
            const auto attribute = [&](const char *name) {
                return QString::fromUtf8(element.attribute(name).value());
            };
            MapPlacement placement;
            placement.kind = kind;
            placement.id = attribute("Id");
            placement.name = attribute("Name");
            placement.type = kind.compare(QStringLiteral("ObjectUnit"), Qt::CaseInsensitive) == 0
                ? attribute("UnitType") : attribute("Type");
            const QVector<double> position = numbersFrom(attribute("Position"));
            if (position.size() >= 2) {
                placement.x = position.at(0);
                placement.y = position.at(1);
                placement.z = position.size() >= 3 ? position.at(2) : 0.0;
                placement.hasPosition = true;
                placements << placement;
            }
        }
        return placements;
    }

    static const QRegularExpression blockStart(QStringLiteral("\\b(ObjectDoodad|ObjectUnit|ObjectDestructible)\\b"),
                                               QRegularExpression::CaseInsensitiveOption);
    qsizetype searchFrom = 0;
    while (true) {
        const QRegularExpressionMatch match = blockStart.match(text, searchFrom);
        if (!match.hasMatch())
            break;
        const QString kind = match.captured(1);
        const qsizetype start = match.capturedStart();
        const qsizetype open = text.indexOf(QLatin1Char('{'), match.capturedEnd());
        if (open < 0) {
            if (warnings)
                *warnings << QStringLiteral("%1 at offset %2 has no opening brace.").arg(kind).arg(start);
            break;
        }

        qsizetype close = -1;
        if (!findBlockClose(text, open, &close)) {
            if (warnings)
                *warnings << QStringLiteral("%1 at offset %2 has no closing brace.").arg(kind).arg(start);
            break;
        }

        const QString block = text.mid(open + 1, close - open - 1);
        MapPlacement placement;
        placement.kind = kind;
        placement.sourceStart = start;
        placement.sourceEnd = close + 1;
        placement.id = fieldValue(block, QStringLiteral("Id"));
        placement.name = fieldValue(block, QStringLiteral("Name"));
        placement.type = fieldValue(block, QStringLiteral("Type"));
        if (placement.type.isEmpty())
            placement.type = fieldValue(block, QStringLiteral("Unit"));
        if (placement.type.isEmpty())
            placement.type = fieldValue(block, QStringLiteral("Doodad"));

        QVector<double> position = numbersFrom(fieldValue(block, QStringLiteral("Position")));
        if (position.size() < 2)
            position = numbersFrom(fieldValue(block, QStringLiteral("Pos")));
        if (position.size() >= 2) {
            placement.x = position.at(0);
            placement.y = position.at(1);
            placement.z = position.size() >= 3 ? position.at(2) : 0.0;
            placement.hasPosition = true;
        }
        if (placement.hasPosition)
            placements << placement;
        searchFrom = close + 1;
    }
    return placements;
}

MapPerformanceReport MapPerformanceAnalyzer::buildReport(const QByteArray &objectsBytes,
                                                         const AnalysisResult &analysis,
                                                         const MapPerformanceOptions &options) const
{
    MapPerformanceReport report;
    report.columns = std::max(1, options.columns);
    report.rows = std::max(1, options.rows);
    report.placements = parseObjectsPlacements(objectsBytes, &report.warnings);

    if (report.placements.isEmpty()) {
        report.warnings << QStringLiteral("No positioned ObjectDoodad/ObjectUnit/ObjectDestructible placements were found in Objects.");
        return report;
    }

    report.xMin = report.yMin = std::numeric_limits<double>::max();
    report.xMax = report.yMax = std::numeric_limits<double>::lowest();
    for (const MapPlacement &placement : report.placements) {
        report.xMin = std::min(report.xMin, placement.x);
        report.yMin = std::min(report.yMin, placement.y);
        report.xMax = std::max(report.xMax, placement.x);
        report.yMax = std::max(report.yMax, placement.y);
    }
    report.xMin -= std::max(0.0, options.padding);
    report.yMin -= std::max(0.0, options.padding);
    report.xMax += std::max(0.0, options.padding);
    report.yMax += std::max(0.0, options.padding);
    if (report.xMin == report.xMax) {
        report.xMin -= 0.5;
        report.xMax += 0.5;
    }
    if (report.yMin == report.yMax) {
        report.yMin -= 0.5;
        report.yMax += 0.5;
    }

    report.cells.reserve(report.columns * report.rows);
    const double width = (report.xMax - report.xMin) / double(report.columns);
    const double height = (report.yMax - report.yMin) / double(report.rows);
    for (int row = 0; row < report.rows; ++row) {
        for (int column = 0; column < report.columns; ++column) {
            MapPerformanceCell cell;
            cell.column = column;
            cell.row = row;
            cell.xMin = report.xMin + width * column;
            cell.xMax = column == report.columns - 1 ? report.xMax : report.xMin + width * (column + 1);
            cell.yMin = report.yMin + height * row;
            cell.yMax = row == report.rows - 1 ? report.yMax : report.yMin + height * (row + 1);
            report.cells << cell;
        }
    }

    QHash<QString, const DataNode *> nodesById;
    for (const DataNode &node : analysis.nodes) {
        if (!node.id.isEmpty() && !nodesById.contains(normalizedId(node.id)))
            nodesById.insert(normalizedId(node.id), &node);
    }

    int globalPeriodic = 0;
    int globalUnitScans = 0;
    QStringList globalTriggerLines;
    QStringList globalTriggerFiles;
    for (const DeepCleanupCandidate &candidate : analysis.deepCleanupCandidates) {
        if (candidate.kind != DeepCleanupKind::TriggerPerformance)
            continue;
        if (candidate.reason.contains(QStringLiteral("periodic"), Qt::CaseInsensitive)
            || candidate.reason.contains(QStringLiteral("timer"), Qt::CaseInsensitive))
            ++globalPeriodic;
        if (candidate.reason.contains(QStringLiteral("Unit-group"), Qt::CaseInsensitive)
            || candidate.reason.contains(QStringLiteral("region scan"), Qt::CaseInsensitive))
            ++globalUnitScans;
        appendUnique(&globalTriggerFiles, candidate.filePath);
        QString triggerLine = candidate.filePath;
        if (candidate.lineNumber > 0)
            triggerLine += QStringLiteral(":%1").arg(candidate.lineNumber);
        if (!candidate.reason.isEmpty())
            triggerLine += QStringLiteral(" | ") + candidate.reason;
        appendUnique(&globalTriggerLines, triggerLine);
    }

    QHash<int, QSet<QString>> actorModelsByCell;
    QHash<int, QSet<QString>> assetFilesByCell;
    QHash<QString, qint64> assetSizeByFile;
    for (const MapPlacement &placement : report.placements) {
        const int column = clampIndex(int(std::floor((placement.x - report.xMin) / width)), report.columns);
        const int row = clampIndex(int(std::floor((placement.y - report.yMin) / height)), report.rows);
        const int cellIndex = row * report.columns + column;
        MapPerformanceCell &cell = report.cells[cellIndex];
        const QString kindLower = placement.kind.toLower();
        if (kindLower.contains(QStringLiteral("doodad"))) {
            ++cell.doodadCount;
        } else {
            ++cell.unitCount;
            if (kindLower.contains(QStringLiteral("destruct"))
                || placement.type.contains(QStringLiteral("destruct"), Qt::CaseInsensitive)
                || placement.name.contains(QStringLiteral("destruct"), Qt::CaseInsensitive))
                ++cell.destructibleCount;
        }
        appendUnique(&cell.relatedObjects, placementLabel(placement));

        QStringList tokens{placement.type, placement.name, placement.id};
        QSet<QString> actorModelIds;
        const DataNode *node = nodesById.value(normalizedId(placement.type), nullptr);
        if (node) {
            appendUnique(&cell.relatedObjects, QStringLiteral("%1(%2)").arg(node->elementName, node->id));
            appendUnique(&cell.relatedFiles, node->sourceFile);
            for (const QString &ref : node->referencedIds) {
                const DataNode *referenced = nodesById.value(normalizedId(ref), nullptr);
                if (!referenced)
                    continue;
                appendUnique(&cell.relatedFiles, referenced->sourceFile);
                if (isActorOrModelNode(*referenced))
                    actorModelIds.insert(referenced->id);
                tokens << referenced->id;
            }
        }
        for (const QString &id : actorModelIds)
            actorModelsByCell[cellIndex].insert(id);

        for (const ScannedFileInfo &file : analysis.scannedFiles) {
            const QFileInfo info(file.filePath);
            if (!sc2dh::asset::isAssetFile(info, file.filePath))
                continue;
            if (!isAssetRelevantToPlacement(file.filePath, tokens))
                continue;
            assetFilesByCell[cellIndex].insert(file.filePath);
            assetSizeByFile.insert(file.filePath, std::max<qint64>(0, file.size));
            appendUnique(&cell.relatedFiles, file.filePath);
        }
    }

    for (int cellIndex = 0; cellIndex < report.cells.size(); ++cellIndex) {
        MapPerformanceCell &cell = report.cells[cellIndex];
        cell.uniqueActorModelCount = actorModelsByCell.value(cellIndex).size();
        cell.linkedAssetBytes = 0;
        for (const QString &file : assetFilesByCell.value(cellIndex))
            cell.linkedAssetBytes += assetSizeByFile.value(file);

        cell.periodicTimerCount = globalPeriodic;
        cell.unitGroupScanCount = globalUnitScans;
        cell.triggerLines = globalTriggerLines;
        for (const QString &file : globalTriggerFiles)
            appendUnique(&cell.relatedFiles, file);

        cell.doodadDensityScore = std::min(100.0, cell.doodadCount * 8.0);
        cell.unitsScore = std::min(100.0, (cell.unitCount + cell.destructibleCount) * 12.0);
        cell.assetsScore = std::min(100.0,
                                    cell.uniqueActorModelCount * 10.0
                                        + double(cell.linkedAssetBytes) / (256.0 * 1024.0) * 8.0);
        cell.triggerCpuRiskScore = std::min(100.0, cell.periodicTimerCount * 15.0 + cell.unitGroupScanCount * 20.0);
        cell.combinedRiskScore = std::min(100.0,
                                          cell.doodadDensityScore * 0.35
                                              + cell.unitsScore * 0.25
                                              + cell.assetsScore * 0.20
                                              + cell.triggerCpuRiskScore * 0.20);

        if (cell.doodadCount > 0)
            cell.reasons << QStringLiteral("Doodad Density: %1 doodad(s).").arg(cell.doodadCount);
        if (cell.unitCount > 0 || cell.destructibleCount > 0)
            cell.reasons << QStringLiteral("Units: %1 unit/destructible placement(s).").arg(cell.unitCount + cell.destructibleCount);
        if (cell.uniqueActorModelCount > 0 || cell.linkedAssetBytes > 0)
            cell.reasons << QStringLiteral("Assets: %1 unique Actor/Model id(s), %2 linked byte(s).")
                                .arg(cell.uniqueActorModelCount)
                                .arg(cell.linkedAssetBytes);
        if (cell.periodicTimerCount > 0 || cell.unitGroupScanCount > 0)
            cell.reasons << QStringLiteral("Trigger CPU Risk: %1 periodic timer(s), %2 unit/region scan(s); global trigger risk is not geolocated.")
                                .arg(cell.periodicTimerCount)
                                .arg(cell.unitGroupScanCount);
        if (cell.reasons.isEmpty())
            cell.reasons << QStringLiteral("No static risk indicators in this cell.");

        cell.relatedObjects = uniqueSorted(cell.relatedObjects);
        cell.relatedFiles = uniqueSorted(cell.relatedFiles);
        cell.triggerLines = uniqueSorted(cell.triggerLines);
        cell.reasons = uniqueSorted(cell.reasons);
    }

    return report;
}

} // namespace sc2dh::perf
