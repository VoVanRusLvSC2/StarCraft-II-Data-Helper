#include "core/DecorationStreamingPlanner.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringBuilder>

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

bool containsAny(const QString &value, const QStringList &needles)
{
    for (const QString &needle : needles)
        if (value.contains(needle, Qt::CaseInsensitive))
            return true;
    return false;
}

bool isGalaxyIdentifier(const QString &value)
{
    if (value.isEmpty())
        return false;
    const QChar first = value.front();
    if (!(first.isLetter() || first == QLatin1Char('_')))
        return false;
    for (const QChar ch : value) {
        if (!(ch.isLetterOrNumber() || ch == QLatin1Char('_')))
            return false;
    }
    return true;
}

struct ParsedTint
{
    bool valid = false;
    double red = 100.0;
    double green = 100.0;
    double blue = 100.0;
    double hdr = 1.0;
};

ParsedTint parseTintColor(const QString &value)
{
    ParsedTint tint;
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty())
        return tint;

    const QVector<double> values = numbersFrom(trimmed);
    if (trimmed.startsWith(QStringLiteral("NULL"), Qt::CaseInsensitive)) {
        if (values.size() > 1)
            return tint;
        tint.valid = true;
        if (!values.isEmpty())
            tint.hdr = values.front();
        return tint;
    }
    if (values.size() != 3 && values.size() != 4)
        return tint;
    for (int index = 0; index < 3; ++index) {
        if (values.at(index) < 0.0 || values.at(index) > 255.0)
            return tint;
    }
    if (values.size() == 4 && values.at(3) < 0.0)
        return tint;
    tint.valid = true;
    tint.red = values.at(0) * 100.0 / 255.0;
    tint.green = values.at(1) * 100.0 / 255.0;
    tint.blue = values.at(2) * 100.0 / 255.0;
    if (values.size() == 4)
        tint.hdr = values.at(3);
    return tint;
}

bool isNonNegativeInteger(const QString &value)
{
    if (value.trimmed().isEmpty())
        return true;
    bool ok = false;
    const int parsed = value.trimmed().toInt(&ok);
    return ok && parsed >= 0;
}

void finalizeDoodadSafety(sc2dh::decor::DoodadPlacement *doodad)
{
    if (!doodad)
        return;
    if (doodad->type.isEmpty()) {
        doodad->staticOnlyReason = QStringLiteral("Static-only: missing doodad type.");
    } else if (!doodad->hasPosition) {
        doodad->staticOnlyReason = QStringLiteral("Static-only: missing position.");
    } else if (containsAny(doodad->type + QLatin1Char(' ') + doodad->name + QLatin1Char(' ') + doodad->flags,
                           {QStringLiteral("pathing"), QStringLiteral("blocker"),
                            QStringLiteral("footprint"), QStringLiteral("destruct")})) {
        doodad->staticOnlyReason = QStringLiteral("Static-only: pathing/gameplay dependency.");
    } else if (!doodad->otherAttributes.isEmpty() || doodad->hasUnsupportedChildren) {
        doodad->staticOnlyReason = QStringLiteral("Static-only: placement has fields the runtime recreation serializer cannot preserve.");
    } else if (!isNonNegativeInteger(doodad->variation)) {
        doodad->staticOnlyReason = QStringLiteral("Static-only: malformed model variation.");
    } else if (!isNonNegativeInteger(doodad->teamColor)) {
        doodad->staticOnlyReason = QStringLiteral("Static-only: malformed team color.");
    } else if (!doodad->tintColor.isEmpty() && !parseTintColor(doodad->tintColor).valid) {
        doodad->staticOnlyReason = QStringLiteral("Static-only: malformed tint color.");
    } else {
        doodad->losslessRoundTripSupported = true;
        doodad->dynamicCandidate = true;
    }
}

QString galaxyString(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("\"%1\"").arg(value);
}

QString fixed(double value)
{
    if (std::abs(value) < 0.0000005)
        value = 0.0;
    QString out = QString::number(value, 'f', 6);
    while (out.contains(QLatin1Char('.')) && out.endsWith(QLatin1Char('0')))
        out.chop(1);
    if (out.endsWith(QLatin1Char('.')))
        out += QLatin1Char('0');
    return out;
}

struct OrientationVectors
{
    double forwardX = 1.0;
    double forwardY = 0.0;
    double forwardZ = 0.0;
    double upX = 0.0;
    double upY = 0.0;
    double upZ = 1.0;
};

OrientationVectors orientationFromEuler(const sc2dh::decor::DoodadPlacement &doodad)
{
    const double yaw = doodad.rotation;
    const double pitch = doodad.pitch;
    const double roll = doodad.roll;
    const double cy = std::cos(yaw);
    const double sy = std::sin(yaw);
    const double cp = std::cos(pitch);
    const double sp = std::sin(pitch);
    const double cr = std::cos(roll);
    const double sr = std::sin(roll);

    OrientationVectors vectors;
    vectors.forwardX = cp * cy;
    vectors.forwardY = cp * sy;
    vectors.forwardZ = sp;
    const double baseUpX = -sp * cy;
    const double baseUpY = -sp * sy;
    const double baseUpZ = cp;
    const double rightX = sy;
    const double rightY = -cy;
    vectors.upX = baseUpX * cr + rightX * sr;
    vectors.upY = baseUpY * cr + rightY * sr;
    vectors.upZ = baseUpZ * cr;
    return vectors;
}

sc2dh::region::SpatialRelation relationToZone(const sc2dh::decor::DoodadPlacement &doodad,
                                              const sc2dh::decor::DecorZone &zone)
{
    if (zone.geometry.supported)
        return zone.geometry.classify(doodad.x, doodad.y);
    const double xMin = std::min(zone.xMin, zone.xMax);
    const double xMax = std::max(zone.xMin, zone.xMax);
    const double yMin = std::min(zone.yMin, zone.yMax);
    const double yMax = std::max(zone.yMin, zone.yMax);
    if (doodad.x < xMin || doodad.x > xMax || doodad.y < yMin || doodad.y > yMax)
        return sc2dh::region::SpatialRelation::Outside;
    if (std::abs(doodad.x - xMin) <= 1e-6 || std::abs(doodad.x - xMax) <= 1e-6
        || std::abs(doodad.y - yMin) <= 1e-6 || std::abs(doodad.y - yMax) <= 1e-6)
        return sc2dh::region::SpatialRelation::Boundary;
    return sc2dh::region::SpatialRelation::Inside;
}

QString safetyKey(QString value)
{
    return value.trimmed().toCaseFolded();
}

QStringList doodadKeys(const sc2dh::decor::DoodadPlacement &doodad)
{
    QStringList keys;
    const auto append = [&](const QString &value) {
        const QString key = safetyKey(value);
        if (!key.isEmpty())
            keys << key;
    };
    append(doodad.id);
    append(doodad.name);
    return keys;
}

bool isDoodadExcluded(const sc2dh::decor::DoodadPlacement &doodad,
                      const sc2dh::decor::DecorationSafetyContext &context)
{
    for (const QString &key : doodadKeys(doodad)) {
        if (context.excludedDoodadKeys.contains(key))
            return true;
    }
    return false;
}

int forcedZoneIdFor(const sc2dh::decor::DoodadPlacement &doodad,
                    const sc2dh::decor::DecorationSafetyContext &context)
{
    for (const QString &key : doodadKeys(doodad)) {
        const auto it = context.forcedZoneByDoodadKey.constFind(key);
        if (it != context.forcedZoneByDoodadKey.cend())
            return it.value();
    }
    return 0;
}

QString typeStaticOnlyReason(const sc2dh::decor::DoodadPlacement &doodad,
                             const sc2dh::decor::DecorationSafetyContext &context)
{
    const auto it = context.staticOnlyReasonByDoodadType.constFind(safetyKey(doodad.type));
    return it == context.staticOnlyReasonByDoodadType.cend() ? QString() : it.value();
}

QStringList externalReferenceFiles(const sc2dh::decor::DoodadPlacement &doodad,
                                   const sc2dh::decor::DecorationSafetyContext &context)
{
    QStringList files;
    for (const QString &key : doodadKeys(doodad))
        files += context.referenceFilesByDoodadKey.value(key);
    files.removeAll(QString());
    files.removeDuplicates();
    std::sort(files.begin(), files.end(), [](const QString &left, const QString &right) {
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
    return files;
}

bool validDoodadActorId(const sc2dh::decor::DoodadPlacement &doodad, qint64 *id = nullptr)
{
    bool ok = false;
    const qint64 parsed = doodad.id.trimmed().toLongLong(&ok);
    if (!ok || parsed <= 0 || parsed > std::numeric_limits<int>::max())
        return false;
    if (id)
        *id = parsed;
    return true;
}

QString galaxyCodeWithoutTrivia(QString source)
{
    // Do not treat an example in a comment or a string literal as a real
    // runtime call. Replacing literals first also keeps "//" inside a string
    // from being mistaken for a line comment.
    static const QRegularExpression stringLiteral(QStringLiteral("\"(?:\\\\.|[^\"\\\\])*\""));
    static const QRegularExpression blockComment(QStringLiteral("/\\*[\\s\\S]*?\\*/"));
    static const QRegularExpression lineComment(QStringLiteral("//[^\\r\\n]*"));
    source.replace(stringLiteral, QStringLiteral("\"\""));
    source.replace(blockComment, QString());
    source.replace(lineComment, QString());
    return source;
}

bool injectMapInitCall(QString *script, QString *errorMessage)
{
    if (!script)
        return false;
    static const QRegularExpression initMapExpression(
        QStringLiteral("\\bvoid\\s+InitMap\\s*\\([^)]*\\)\\s*\\{"),
        QRegularExpression::CaseInsensitiveOption);
    if (galaxyCodeWithoutTrivia(*script).contains(QRegularExpression(QStringLiteral("\\bDecorOpt_Init\\s*\\("))))
        return true;

    const QRegularExpressionMatch match = initMapExpression.match(*script);
    if (!match.hasMatch()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("MapScript.galaxy does not contain a recognizable InitMap() function; decoration runtime was not connected.");
        return false;
    }

    script->insert(match.capturedEnd(), QStringLiteral("\n    DecorOpt_Init();"));
    return true;
}

} // namespace

namespace sc2dh::decor
{

QVector<DoodadPlacement> DecorationStreamingPlanner::parseObjects(const QByteArray &objectsBytes,
                                                                  QStringList *warnings) const
{
    if (warnings)
        warnings->clear();
    const QString text = QString::fromUtf8(objectsBytes);
    QVector<DoodadPlacement> doodads;

    if (text.trimmed().startsWith(QLatin1Char('<'))) {
        const QRegularExpression elementExpression(
            QStringLiteral("<ObjectDoodad\\b[^>]*(?:/\\s*>|>[\\s\\S]*?</ObjectDoodad\\s*>)"),
            QRegularExpression::CaseInsensitiveOption);
        auto matches = elementExpression.globalMatch(text);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            const QString raw = match.captured(0);
            pugi::xml_document document;
            const QByteArray rawBytes = raw.toUtf8();
            const pugi::xml_parse_result parsed = document.load_buffer(
                rawBytes.constData(), size_t(rawBytes.size()), pugi::parse_default, pugi::encoding_utf8);
            const pugi::xml_node element = document.child("ObjectDoodad");
            if (!parsed || !element) {
                if (warnings)
                    *warnings << QStringLiteral("Unable to parse ObjectDoodad XML at offset %1.").arg(match.capturedStart());
                continue;
            }

            DoodadPlacement doodad;
            doodad.sourceStart = match.capturedStart();
            doodad.sourceEnd = match.capturedEnd();
            doodad.rawSource = raw;
            const auto attribute = [&](const char *name) {
                return QString::fromUtf8(element.attribute(name).value());
            };
            doodad.id = attribute("Id");
            doodad.name = attribute("Name");
            doodad.type = attribute("Type");
            doodad.variation = attribute("Variation");
            doodad.tintColor = attribute("TintColor");
            doodad.teamColor = attribute("TeamColor");
            doodad.flags = attribute("Flags");
            static const QSet<QString> knownAttributes{
                QStringLiteral("Id"), QStringLiteral("Name"), QStringLiteral("Type"),
                QStringLiteral("Position"), QStringLiteral("Rotation"), QStringLiteral("Scale"),
                QStringLiteral("Pitch"), QStringLiteral("Roll"),
                QStringLiteral("Variation"), QStringLiteral("TintColor"), QStringLiteral("TeamColor"),
                QStringLiteral("Flags")
            };
            for (pugi::xml_attribute item : element.attributes()) {
                const QString key = QString::fromUtf8(item.name());
                if (!knownAttributes.contains(key))
                    doodad.otherAttributes.insert(key, QString::fromUtf8(item.value()));
            }
            for (pugi::xml_node child : element.children()) {
                if (child.type() != pugi::node_element)
                    continue;
                if (QString::fromUtf8(child.name()).compare(QStringLiteral("Flag"), Qt::CaseInsensitive) != 0) {
                    doodad.hasUnsupportedChildren = true;
                    continue;
                }
                QString index;
                QString value;
                bool unsupportedFlagField = false;
                for (pugi::xml_attribute flagAttribute : child.attributes()) {
                    const QString key = QString::fromUtf8(flagAttribute.name());
                    if (key.compare(QStringLiteral("Index"), Qt::CaseInsensitive) == 0)
                        index = QString::fromUtf8(flagAttribute.value());
                    else if (key.compare(QStringLiteral("Value"), Qt::CaseInsensitive) == 0)
                        value = QString::fromUtf8(flagAttribute.value());
                    else
                        unsupportedFlagField = true;
                }
                if (index.isEmpty() || unsupportedFlagField || child.first_child()) {
                    doodad.hasUnsupportedChildren = true;
                    continue;
                }
                doodad.placementFlags.insert(index, value);
            }

            const QVector<double> position = numbersFrom(attribute("Position"));
            if (position.size() >= 2) {
                doodad.x = position.at(0);
                doodad.y = position.at(1);
                doodad.z = position.size() >= 3 ? position.at(2) : 0.0;
                doodad.hasPosition = true;
            }
            const QVector<double> scale = numbersFrom(attribute("Scale"));
            if (scale.size() == 1) {
                doodad.scaleX = doodad.scaleY = doodad.scaleZ = scale.at(0);
            } else if (scale.size() >= 3) {
                doodad.scaleX = scale.at(0);
                doodad.scaleY = scale.at(1);
                doodad.scaleZ = scale.at(2);
            }
            const QVector<double> rotation = numbersFrom(attribute("Rotation"));
            if (!rotation.isEmpty())
                doodad.rotation = rotation.front();
            const QVector<double> pitch = numbersFrom(attribute("Pitch"));
            if (!pitch.isEmpty())
                doodad.pitch = pitch.front();
            const QVector<double> roll = numbersFrom(attribute("Roll"));
            if (!roll.isEmpty())
                doodad.roll = roll.front();
            finalizeDoodadSafety(&doodad);
            doodads << doodad;
        }
        if (doodads.isEmpty() && warnings)
            *warnings << QStringLiteral("PlacedObjects XML contains no parseable ObjectDoodad elements.");
        return doodads;
    }

    qsizetype searchFrom = 0;
    while (true) {
        const qsizetype start = text.indexOf(QStringLiteral("ObjectDoodad"), searchFrom, Qt::CaseInsensitive);
        if (start < 0)
            break;
        const qsizetype open = text.indexOf(QLatin1Char('{'), start);
        if (open < 0) {
            if (warnings)
                *warnings << QStringLiteral("ObjectDoodad at offset %1 has no opening brace.").arg(start);
            break;
        }

        int depth = 0;
        qsizetype close = -1;
        for (qsizetype i = open; i < text.size(); ++i) {
            if (text.at(i) == QLatin1Char('{'))
                ++depth;
            else if (text.at(i) == QLatin1Char('}')) {
                --depth;
                if (depth == 0) {
                    close = i;
                    break;
                }
            }
        }
        if (close < 0) {
            if (warnings)
                *warnings << QStringLiteral("ObjectDoodad at offset %1 has no closing brace.").arg(start);
            break;
        }

        const QString block = text.mid(open + 1, close - open - 1);
        DoodadPlacement doodad;
        doodad.sourceStart = start;
        doodad.sourceEnd = close + 1;
        doodad.rawSource = text.mid(doodad.sourceStart, doodad.sourceEnd - doodad.sourceStart);
        doodad.id = fieldValue(block, QStringLiteral("Id"));
        doodad.name = fieldValue(block, QStringLiteral("Name"));
        doodad.type = fieldValue(block, QStringLiteral("Type"));
        doodad.variation = fieldValue(block, QStringLiteral("Variation"));
        doodad.tintColor = fieldValue(block, QStringLiteral("TintColor"));
        doodad.teamColor = fieldValue(block, QStringLiteral("TeamColor"));
        doodad.flags = fieldValue(block, QStringLiteral("Flags"));

        QVector<double> position = numbersFrom(fieldValue(block, QStringLiteral("Position")));
        if (position.size() < 2)
            position = numbersFrom(fieldValue(block, QStringLiteral("Pos")));
        if (position.size() >= 2) {
            doodad.x = position.at(0);
            doodad.y = position.at(1);
            doodad.z = position.size() >= 3 ? position.at(2) : 0.0;
            doodad.hasPosition = true;
        }

        const QVector<double> scale = numbersFrom(fieldValue(block, QStringLiteral("Scale")));
        if (scale.size() == 1) {
            doodad.scaleX = scale.at(0);
            doodad.scaleY = scale.at(0);
            doodad.scaleZ = scale.at(0);
        } else if (scale.size() >= 3) {
            doodad.scaleX = scale.at(0);
            doodad.scaleY = scale.at(1);
            doodad.scaleZ = scale.at(2);
        }

        const QVector<double> rotation = numbersFrom(fieldValue(block, QStringLiteral("Rotation")));
        if (!rotation.isEmpty())
            doodad.rotation = rotation.at(0);
        const QVector<double> pitch = numbersFrom(fieldValue(block, QStringLiteral("Pitch")));
        if (!pitch.isEmpty())
            doodad.pitch = pitch.at(0);
        const QVector<double> roll = numbersFrom(fieldValue(block, QStringLiteral("Roll")));
        if (!roll.isEmpty())
            doodad.roll = roll.at(0);

        doodad.losslessRoundTripSupported = true;
        finalizeDoodadSafety(&doodad);

        doodads << doodad;
        searchFrom = close + 1;
    }

    return doodads;
}

QByteArray DecorationStreamingPlanner::serializePlacementLosslessly(const DoodadPlacement &placement,
                                                                    QString *errorMessage) const
{
    if (placement.rawSource.isEmpty()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Placement has no preserved source representation.");
        return {};
    }
    if (!placement.losslessRoundTripSupported) {
        if (errorMessage)
            *errorMessage = placement.staticOnlyReason.isEmpty()
                ? QStringLiteral("Placement contains unsupported semantic fields.")
                : placement.staticOnlyReason;
        return {};
    }
    return placement.rawSource.toUtf8();
}

bool DecorationStreamingPlanner::verifyPlacementRoundTrip(const DoodadPlacement &placement,
                                                           QString *errorMessage) const
{
    const QByteArray serialized = serializePlacementLosslessly(placement, errorMessage);
    if (serialized.isEmpty())
        return false;
    const bool isXml = placement.rawSource.trimmed().startsWith(QLatin1Char('<'));
    const QByteArray document = isXml
        ? QByteArrayLiteral("<PlacedObjects>") + serialized + QByteArrayLiteral("</PlacedObjects>")
        : serialized;
    QStringList warnings;
    const QVector<DoodadPlacement> reparsed = parseObjects(document, &warnings);
    if (reparsed.size() != 1 || !warnings.isEmpty()) {
        if (errorMessage)
            *errorMessage = warnings.isEmpty()
                ? QStringLiteral("Placement did not parse to exactly one object after serialization.")
                : warnings.join(QStringLiteral("; "));
        return false;
    }
    const DoodadPlacement &after = reparsed.front();
    const bool equal = placement.id == after.id && placement.name == after.name
        && placement.type == after.type && placement.variation == after.variation
        && placement.x == after.x && placement.y == after.y && placement.z == after.z
        && placement.rotation == after.rotation && placement.pitch == after.pitch
        && placement.roll == after.roll
        && placement.scaleX == after.scaleX && placement.scaleY == after.scaleY
        && placement.scaleZ == after.scaleZ && placement.tintColor == after.tintColor
        && placement.teamColor == after.teamColor && placement.flags == after.flags
        && placement.placementFlags == after.placementFlags
        && placement.otherAttributes == after.otherAttributes
        && placement.hasUnsupportedChildren == after.hasUnsupportedChildren;
    if (!equal && errorMessage)
        *errorMessage = QStringLiteral("Placement semantic fields changed during round-trip verification.");
    return equal;
}

DecorationStreamingPlan DecorationStreamingPlanner::buildPlan(const QByteArray &objectsBytes,
                                                              const QVector<DecorZone> &zones,
                                                              const DecorationSafetyContext &safetyContext,
                                                              QStringList *warnings) const
{
    DecorationStreamingPlan plan;
    plan.doodads = parseObjects(objectsBytes, &plan.warnings);
    if (warnings)
        *warnings = plan.warnings;

    for (const DecorZone &zone : zones)
    {
        if (zone.id <= 0)
            plan.warnings << QStringLiteral("Decoration zone id must be positive: %1.").arg(zone.id);
        plan.zones << ZoneAssignment{zone.id, {}};
    }
    QSet<int> zoneIds;
    QHash<int, int> zoneIndexById;
    for (int zoneIndex = 0; zoneIndex < zones.size(); ++zoneIndex) {
        const DecorZone &zone = zones.at(zoneIndex);
        if (zoneIds.contains(zone.id))
            plan.warnings << QStringLiteral("Decoration zone id is duplicated: %1.").arg(zone.id);
        zoneIds.insert(zone.id);
        if (zone.id > 0 && !zoneIndexById.contains(zone.id))
            zoneIndexById.insert(zone.id, zoneIndex);
    }

    for (int i = 0; i < plan.doodads.size(); ++i) {
        DoodadPlacement &doodad = plan.doodads[i];
        const QString label = doodad.name.isEmpty() ? (doodad.id.isEmpty() ? doodad.type : doodad.id) : doodad.name;
        doodad.userExcluded = isDoodadExcluded(doodad, safetyContext);
        doodad.forcedZoneId = forcedZoneIdFor(doodad, safetyContext);
        if (doodad.userExcluded && doodad.dynamicCandidate) {
            doodad.dynamicCandidate = false;
            doodad.staticOnlyReason = QStringLiteral("Static-only: excluded by user.");
        }
        if (doodad.dynamicCandidate) {
            const QString typeReason = typeStaticOnlyReason(doodad, safetyContext);
            if (!typeReason.isEmpty()) {
                doodad.dynamicCandidate = false;
                doodad.staticOnlyReason = typeReason;
            }
        }
        if (doodad.dynamicCandidate) {
            doodad.safetyReferenceFiles = externalReferenceFiles(doodad, safetyContext);
            if (!doodad.safetyReferenceFiles.isEmpty()) {
                doodad.dynamicCandidate = false;
                doodad.staticOnlyReason = QStringLiteral("Static-only: pathing/gameplay dependency (referenced by trigger/script text: %1).")
                                              .arg(doodad.safetyReferenceFiles.join(QStringLiteral(", ")));
            }
        }
        if (!doodad.dynamicCandidate) {
            if (doodad.forcedZoneId > 0)
                plan.warnings << QStringLiteral("Doodad %1 has a forced decoration zone %2, but it is static-only: %3")
                                     .arg(label)
                                     .arg(doodad.forcedZoneId)
                                     .arg(doodad.staticOnlyReason);
            plan.staticOnlyDoodads << i;
            continue;
        }

        int assignedZoneIndex = -1;
        if (doodad.forcedZoneId > 0) {
            assignedZoneIndex = zoneIndexById.value(doodad.forcedZoneId, -1);
            if (assignedZoneIndex < 0) {
                plan.warnings << QStringLiteral("Doodad %1 is forced to missing decoration zone %2; leaving it unassigned.")
                                     .arg(label)
                                     .arg(doodad.forcedZoneId);
                plan.unassignedDoodads << i;
                continue;
            }
        } else {
            bool touchesBoundary = false;
            for (int z = 0; z < zones.size(); ++z) {
                const sc2dh::region::SpatialRelation relation = relationToZone(doodad, zones.at(z));
                if (relation == sc2dh::region::SpatialRelation::Inside) {
                    assignedZoneIndex = z;
                    break;
                }
                touchesBoundary = touchesBoundary || relation == sc2dh::region::SpatialRelation::Boundary;
            }
            if (assignedZoneIndex < 0 && touchesBoundary) {
                plan.boundaryDoodads << i;
                doodad.dynamicCandidate = false;
                doodad.staticOnlyReason = QStringLiteral("Static-only: placement center is on an optimization-scope boundary.");
                plan.staticOnlyDoodads << i;
                continue;
            }
        }
        if (assignedZoneIndex < 0) {
            plan.unassignedDoodads << i;
            continue;
        }
        plan.zones[assignedZoneIndex].doodadIndices << i;
    }

    if (warnings)
        *warnings = plan.warnings;
    return plan;
}

DecorationStreamingPlan DecorationStreamingPlanner::buildPlan(const QByteArray &objectsBytes,
                                                              const QVector<DecorZone> &zones,
                                                              QStringList *warnings) const
{
    return buildPlan(objectsBytes, zones, DecorationSafetyContext{}, warnings);
}

DecorationVisibilityPlan DecorationStreamingPlanner::buildVisibilityPlan(
    const QByteArray &objectsBytes,
    const QVector<DecorZone> &zones,
    const DecorationSafetyContext &safetyContext,
    QStringList *warnings) const
{
    DecorationVisibilityPlan plan;
    plan.doodads = parseObjects(objectsBytes, &plan.warnings);
    if (warnings)
        *warnings = plan.warnings;

    for (const DecorZone &zone : zones) {
        if (zone.id <= 0)
            plan.warnings << QStringLiteral("Decoration zone id must be positive: %1.").arg(zone.id);
        plan.zones << ZoneAssignment{zone.id, {}};
    }

    QSet<int> zoneIds;
    QHash<int, int> zoneIndexById;
    for (int zoneIndex = 0; zoneIndex < zones.size(); ++zoneIndex) {
        const DecorZone &zone = zones.at(zoneIndex);
        if (zoneIds.contains(zone.id))
            plan.warnings << QStringLiteral("Decoration zone id is duplicated: %1.").arg(zone.id);
        zoneIds.insert(zone.id);
        if (zone.id > 0 && !zoneIndexById.contains(zone.id))
            zoneIndexById.insert(zone.id, zoneIndex);
    }

    QHash<qint64, int> doodadIdCounts;
    for (const DoodadPlacement &doodad : plan.doodads) {
        qint64 doodadId = 0;
        if (validDoodadActorId(doodad, &doodadId))
            ++doodadIdCounts[doodadId];
    }

    for (int i = 0; i < plan.doodads.size(); ++i) {
        DoodadPlacement &doodad = plan.doodads[i];
        const QString label = doodad.name.isEmpty()
            ? (doodad.id.isEmpty() ? doodad.type : doodad.id)
            : doodad.name;
        doodad.visibilityCandidate = false;
        doodad.visibilityStaticOnlyReason.clear();
        doodad.userExcluded = isDoodadExcluded(doodad, safetyContext);
        doodad.forcedZoneId = forcedZoneIdFor(doodad, safetyContext);

        qint64 doodadId = 0;
        QString staticReason;
        if (doodad.type.isEmpty()) {
            staticReason = QStringLiteral("Static-only: missing doodad type.");
        } else if (!doodad.hasPosition) {
            staticReason = QStringLiteral("Static-only: missing position.");
        } else if (!validDoodadActorId(doodad, &doodadId)) {
            staticReason = QStringLiteral("Static-only: missing or invalid positive doodad ID for actor visibility control.");
        } else if (doodadIdCounts.value(doodadId) != 1) {
            staticReason = QStringLiteral("Static-only: duplicate doodad ID cannot be controlled independently.");
        } else if (containsAny(doodad.type + QLatin1Char(' ') + doodad.name + QLatin1Char(' ') + doodad.flags,
                               {QStringLiteral("pathing"), QStringLiteral("blocker"),
                                QStringLiteral("footprint"), QStringLiteral("destruct")})) {
            staticReason = QStringLiteral("Static-only: pathing/gameplay dependency.");
        } else if (doodad.userExcluded) {
            staticReason = QStringLiteral("Static-only: excluded by user.");
        } else {
            const QString typeReason = typeStaticOnlyReason(doodad, safetyContext);
            if (!typeReason.isEmpty()) {
                staticReason = typeReason;
            } else {
                doodad.safetyReferenceFiles = externalReferenceFiles(doodad, safetyContext);
                if (!doodad.safetyReferenceFiles.isEmpty()) {
                    staticReason = QStringLiteral("Static-only: pathing/gameplay dependency (referenced by trigger/script text: %1).")
                                       .arg(doodad.safetyReferenceFiles.join(QStringLiteral(", ")));
                }
            }
        }

        if (!staticReason.isEmpty()) {
            doodad.visibilityStaticOnlyReason = staticReason;
            if (doodad.forcedZoneId > 0) {
                plan.warnings << QStringLiteral("Doodad %1 has a forced visibility zone %2, but it is static-only: %3")
                                     .arg(label)
                                     .arg(doodad.forcedZoneId)
                                     .arg(staticReason);
            }
            plan.staticOnlyDoodads << i;
            continue;
        }

        doodad.visibilityCandidate = true;
        int assignedZoneIndex = -1;
        if (doodad.forcedZoneId > 0) {
            assignedZoneIndex = zoneIndexById.value(doodad.forcedZoneId, -1);
            if (assignedZoneIndex < 0) {
                plan.warnings << QStringLiteral("Doodad %1 is forced to missing visibility zone %2; leaving it unassigned.")
                                     .arg(label)
                                     .arg(doodad.forcedZoneId);
                plan.unassignedDoodads << i;
                continue;
            }
        } else {
            bool touchesBoundary = false;
            for (int z = 0; z < zones.size(); ++z) {
                const sc2dh::region::SpatialRelation relation = relationToZone(doodad, zones.at(z));
                if (relation == sc2dh::region::SpatialRelation::Inside) {
                    // First matching zone wins. This makes overlapping scopes
                    // deterministic and prevents one actor from being hidden
                    // by one zone then restored by another.
                    assignedZoneIndex = z;
                    break;
                }
                touchesBoundary = touchesBoundary || relation == sc2dh::region::SpatialRelation::Boundary;
            }
            if (assignedZoneIndex < 0 && touchesBoundary) {
                doodad.visibilityCandidate = false;
                doodad.visibilityStaticOnlyReason =
                    QStringLiteral("Static-only: placement center is on a visibility-scope boundary.");
                plan.boundaryDoodads << i;
                plan.staticOnlyDoodads << i;
                continue;
            }
        }
        if (assignedZoneIndex < 0) {
            plan.unassignedDoodads << i;
            continue;
        }
        plan.zones[assignedZoneIndex].doodadIndices << i;
    }

    if (warnings)
        *warnings = plan.warnings;
    return plan;
}

DecorationVisibilityPlan DecorationStreamingPlanner::buildVisibilityPlan(const QByteArray &objectsBytes,
                                                                          const QVector<DecorZone> &zones,
                                                                          QStringList *warnings) const
{
    return buildVisibilityPlan(objectsBytes, zones, DecorationSafetyContext{}, warnings);
}

QString DecorationStreamingPlanner::generateGalaxy(const DecorationStreamingPlan &plan,
                                                   const GalaxyGenerationOptions &options) const
{
    const QString prefix = options.functionPrefix.isEmpty() ? QStringLiteral("NAME_OUT_FUNK") : options.functionPrefix;
    const int batchLimit = std::max(1, options.batchLimit);
    int maxActors = 1;
    int maxZoneId = 1;
    for (const ZoneAssignment &zone : plan.zones) {
        maxActors = std::max(maxActors, int(zone.doodadIndices.size()));
        maxZoneId = std::max(maxZoneId, zone.zoneId);
    }

    QString out;
    out += QStringLiteral("// Generated by SC2 Data Helper Decoration Streaming.\n");
    out += QStringLiteral("// Put this file under scripts/ and include it from the map's script coordinator.\n");
    out += QStringLiteral("const int DecorOpt_ZoneCount = %1;\n").arg(maxZoneId);
    out += QStringLiteral("const int DecorOpt_MaxActorsPerZone = %1;\n").arg(maxActors);
    out += QStringLiteral("const int DecorOpt_BatchLimit = %1;\n").arg(batchLimit);
    out += QStringLiteral("actor[%1][%2] DecorOpt_Actors;\n").arg(maxZoneId + 1).arg(maxActors + 1);
    out += QStringLiteral("int[%1] DecorOpt_ActorCount;\n").arg(maxZoneId + 1);
    out += QStringLiteral("bool[%1] DecorOpt_Loaded;\n\n").arg(maxZoneId + 1);

    out += QStringLiteral("void DecorOpt_Init() {\n");
    out += QStringLiteral("    int zoneId;\n");
    out += QStringLiteral("    zoneId = 1;\n");
    out += QStringLiteral("    while (zoneId <= DecorOpt_ZoneCount) {\n");
    out += QStringLiteral("        DecorOpt_ActorCount[zoneId] = 0;\n");
    out += QStringLiteral("        DecorOpt_Loaded[zoneId] = false;\n");
    out += QStringLiteral("        zoneId += 1;\n");
    out += QStringLiteral("    }\n");
    out += QStringLiteral("}\n\n");

    out += QStringLiteral("void DecorOpt_ClearZone(int zoneId) {\n");
    out += QStringLiteral("    int i;\n");
    out += QStringLiteral("    if (zoneId < 1 || zoneId > DecorOpt_ZoneCount) { return; }\n");
    out += QStringLiteral("    i = 0;\n");
    out += QStringLiteral("    while (i < DecorOpt_ActorCount[zoneId]) {\n");
    out += QStringLiteral("        ActorSend(DecorOpt_Actors[zoneId][i], \"Destroy\");\n");
    out += QStringLiteral("        i += 1;\n");
    out += QStringLiteral("    }\n");
    out += QStringLiteral("    DecorOpt_ActorCount[zoneId] = 0;\n");
    out += QStringLiteral("    DecorOpt_Loaded[zoneId] = false;\n");
    out += QStringLiteral("}\n\n");

    out += QStringLiteral("void DecorOpt_ClearAll() {\n");
    out += QStringLiteral("    int zoneId;\n");
    out += QStringLiteral("    zoneId = 1;\n");
    out += QStringLiteral("    while (zoneId <= DecorOpt_ZoneCount) {\n");
    out += QStringLiteral("        DecorOpt_ClearZone(zoneId);\n");
    out += QStringLiteral("        zoneId += 1;\n");
    out += QStringLiteral("    }\n");
    out += QStringLiteral("}\n\n");

    out += QStringLiteral("bool DecorOpt_IsZoneLoaded(int zoneId) {\n");
    out += QStringLiteral("    if (zoneId < 1 || zoneId > DecorOpt_ZoneCount) { return false; }\n");
    out += QStringLiteral("    return DecorOpt_Loaded[zoneId];\n");
    out += QStringLiteral("}\n\n");

    out += QStringLiteral("actor DecorOpt_CreateActor(string actorType, fixed x, fixed y, fixed z, fixed scaleX, fixed scaleY, fixed scaleZ) {\n");
    out += QStringLiteral("    point p;\n");
    out += QStringLiteral("    actor a;\n");
    out += QStringLiteral("    p = Point(x, y);\n");
    out += QStringLiteral("    libNtve_gf_CreateActorAtPoint(actorType, p);\n");
    out += QStringLiteral("    a = libNtve_gf_ActorLastCreated();\n");
    out += QStringLiteral("    ActorSend(a, libNtve_gf_SetPosition(x, y, z));\n");
    out += QStringLiteral("    ActorSend(a, libNtve_gf_SetScale(scaleX, scaleY, scaleZ, 0.0));\n");
    out += QStringLiteral("    return a;\n");
    out += QStringLiteral("}\n\n");

    out += QStringLiteral("void DecorOpt_StoreActor(int zoneId, actor a) {\n");
    out += QStringLiteral("    DecorOpt_Actors[zoneId][DecorOpt_ActorCount[zoneId]] = a;\n");
    out += QStringLiteral("    DecorOpt_ActorCount[zoneId] += 1;\n");
    out += QStringLiteral("}\n\n");

    const auto appendActorCreation = [&](QString *source, int zoneId, const DoodadPlacement &doodad) {
        *source += QStringLiteral("    a = DecorOpt_CreateActor(%1, %2, %3, %4, %5, %6, %7);\n")
                       .arg(galaxyString(doodad.type), fixed(doodad.x), fixed(doodad.y), fixed(doodad.z),
                            fixed(doodad.scaleX), fixed(doodad.scaleY), fixed(doodad.scaleZ));
        if (doodad.rotation != 0.0 || doodad.pitch != 0.0 || doodad.roll != 0.0) {
            const OrientationVectors orientation = orientationFromEuler(doodad);
            *source += QStringLiteral("    ActorSend(a, libNtve_gf_SetRotation(%1, %2, %3, %4, %5, %6));\n")
                           .arg(fixed(orientation.forwardX), fixed(orientation.forwardY),
                                fixed(orientation.forwardZ), fixed(orientation.upX),
                                fixed(orientation.upY), fixed(orientation.upZ));
        }
        if (!doodad.tintColor.isEmpty()) {
            const ParsedTint tint = parseTintColor(doodad.tintColor);
            if (tint.valid) {
                *source += QStringLiteral("    ActorSend(a, libNtve_gf_SetTintColor(Color(%1, %2, %3), %4, 0.0));\n")
                               .arg(fixed(tint.red), fixed(tint.green), fixed(tint.blue), fixed(tint.hdr));
            }
        }
        *source += QStringLiteral("    DecorOpt_StoreActor(%1, a);\n").arg(zoneId);
    };

    for (const ZoneAssignment &zone : plan.zones) {
        const int batchCount = std::max(1, (int(zone.doodadIndices.size()) + batchLimit - 1) / batchLimit);
        for (int batchIndex = 0; batchIndex < batchCount; ++batchIndex) {
            out += QStringLiteral("void DecorOpt_CreateZone_%1_Batch_%2() {\n")
                       .arg(zone.zoneId)
                       .arg(batchIndex + 1);
            out += QStringLiteral("    actor a;\n");
            const int first = batchIndex * batchLimit;
            const int last = std::min(first + batchLimit, int(zone.doodadIndices.size()));
            for (int itemIndex = first; itemIndex < last; ++itemIndex) {
                const int doodadIndex = zone.doodadIndices.at(itemIndex);
                appendActorCreation(&out, zone.zoneId, plan.doodads.at(doodadIndex));
            }
            out += QStringLiteral("}\n\n");
        }

        out += QStringLiteral("void DecorOpt_CreateZone_%1() {\n").arg(zone.zoneId);
        out += QStringLiteral("    if (DecorOpt_Loaded[%1]) { return; }\n").arg(zone.zoneId);
        for (int batchIndex = 0; batchIndex < batchCount; ++batchIndex) {
            out += QStringLiteral("    DecorOpt_CreateZone_%1_Batch_%2();\n")
                       .arg(zone.zoneId)
                       .arg(batchIndex + 1);
            if (batchIndex + 1 < batchCount)
                out += QStringLiteral("    Wait(0.0, c_timeGame);\n");
        }
        out += QStringLiteral("    DecorOpt_Loaded[%1] = true;\n").arg(zone.zoneId);
        out += QStringLiteral("}\n\n");
    }

    out += QStringLiteral("void DecorOpt_CreateZone(int zoneId) {\n");
    out += QStringLiteral("    if (zoneId < 1 || zoneId > DecorOpt_ZoneCount) { return; }\n");
    for (const ZoneAssignment &zone : plan.zones)
        out += QStringLiteral("    if (zoneId == %1) { DecorOpt_CreateZone_%1(); return; }\n").arg(zone.zoneId);
    out += QStringLiteral("}\n\n");

    out += QStringLiteral("void DecorOpt_CreateAll() {\n");
    out += QStringLiteral("    int zoneId;\n");
    out += QStringLiteral("    zoneId = 1;\n");
    out += QStringLiteral("    while (zoneId <= DecorOpt_ZoneCount) {\n");
    out += QStringLiteral("        DecorOpt_CreateZone(zoneId);\n");
    out += QStringLiteral("        zoneId += 1;\n");
    out += QStringLiteral("    }\n");
    out += QStringLiteral("}\n\n");

    for (const ZoneAssignment &zone : plan.zones)
        out += QStringLiteral("void %1_%2() { DecorOpt_CreateZone(%2); }\n"
                              "void %1_Create_%2() { DecorOpt_CreateZone(%2); }\n"
                              "void %1_Clear_%2() { DecorOpt_ClearZone(%2); }\n")
                   .arg(prefix)
                   .arg(zone.zoneId);
    out += QStringLiteral("void %1_CreateAll() { DecorOpt_CreateAll(); }\n").arg(prefix);
    out += QStringLiteral("void %1_ClearAll() { DecorOpt_ClearAll(); }\n").arg(prefix);

    return out;
}

QString DecorationStreamingPlanner::generateVisibilityGalaxy(const DecorationVisibilityPlan &plan,
                                                              const GalaxyGenerationOptions &options) const
{
    const QString prefix = options.functionPrefix.isEmpty() ? QStringLiteral("NAME_OUT_FUNK") : options.functionPrefix;
    const int batchLimit = std::max(1, options.batchLimit);
    int maxZoneId = 1;
    for (const ZoneAssignment &zone : plan.zones)
        maxZoneId = std::max(maxZoneId, zone.zoneId);

    QString out;
    out += QStringLiteral("// Generated by SC2 Data Helper Decoration Visibility Streaming.\n");
    out += QStringLiteral("// Visibility-only mode: Objects stays byte-identical; no doodad is recreated or deleted.\n");
    out += QStringLiteral("// Safe default: zones start visible. Call the public Hide/Restore API from your triggers.\n");
    out += QStringLiteral("const int DecorOpt_ZoneCount = %1;\n").arg(maxZoneId);
    out += QStringLiteral("const int DecorOpt_BatchLimit = %1;\n").arg(batchLimit);
    out += QStringLiteral("bool[%1] DecorOpt_ZoneHidden;\n\n").arg(maxZoneId + 1);

    out += QStringLiteral("void DecorOpt_Init() {\n");
    out += QStringLiteral("    int zoneId;\n");
    out += QStringLiteral("    zoneId = 1;\n");
    out += QStringLiteral("    while (zoneId <= DecorOpt_ZoneCount) {\n");
    out += QStringLiteral("        DecorOpt_ZoneHidden[zoneId] = false;\n");
    out += QStringLiteral("        zoneId += 1;\n");
    out += QStringLiteral("    }\n");
    out += QStringLiteral("}\n\n");

    for (const ZoneAssignment &zone : plan.zones) {
        out += QStringLiteral("void DecorOpt_HideZone_%1() {\n").arg(zone.zoneId);
        out += QStringLiteral("    int changed;\n");
        out += QStringLiteral("    if (DecorOpt_ZoneHidden[%1]) { return; }\n").arg(zone.zoneId);
        out += QStringLiteral("    changed = 0;\n");
        for (int doodadIndex : zone.doodadIndices) {
            const DoodadPlacement &doodad = plan.doodads.at(doodadIndex);
            qint64 doodadId = 0;
            if (!validDoodadActorId(doodad, &doodadId))
                continue;
            out += QStringLiteral("    ActorSend(ActorFromDoodad(DoodadFromId(%1)), \"SetVisibility 0\");\n")
                       .arg(doodadId);
            out += QStringLiteral("    changed += 1;\n");
            out += QStringLiteral("    if (changed >= DecorOpt_BatchLimit) { Wait(0.0, c_timeGame); changed = 0; }\n");
        }
        out += QStringLiteral("    DecorOpt_ZoneHidden[%1] = true;\n").arg(zone.zoneId);
        out += QStringLiteral("}\n\n");

        out += QStringLiteral("void DecorOpt_RestoreZone_%1() {\n").arg(zone.zoneId);
        out += QStringLiteral("    int changed;\n");
        out += QStringLiteral("    if (!DecorOpt_ZoneHidden[%1]) { return; }\n").arg(zone.zoneId);
        out += QStringLiteral("    changed = 0;\n");
        for (int doodadIndex : zone.doodadIndices) {
            const DoodadPlacement &doodad = plan.doodads.at(doodadIndex);
            qint64 doodadId = 0;
            if (!validDoodadActorId(doodad, &doodadId))
                continue;
            out += QStringLiteral("    ActorSend(ActorFromDoodad(DoodadFromId(%1)), \"SetVisibility 1\");\n")
                       .arg(doodadId);
            out += QStringLiteral("    changed += 1;\n");
            out += QStringLiteral("    if (changed >= DecorOpt_BatchLimit) { Wait(0.0, c_timeGame); changed = 0; }\n");
        }
        out += QStringLiteral("    DecorOpt_ZoneHidden[%1] = false;\n").arg(zone.zoneId);
        out += QStringLiteral("}\n\n");
    }

    out += QStringLiteral("void DecorOpt_HideZone(int zoneId) {\n");
    out += QStringLiteral("    if (zoneId < 1 || zoneId > DecorOpt_ZoneCount) { return; }\n");
    for (const ZoneAssignment &zone : plan.zones)
        out += QStringLiteral("    if (zoneId == %1) { DecorOpt_HideZone_%1(); return; }\n").arg(zone.zoneId);
    out += QStringLiteral("}\n\n");

    out += QStringLiteral("void DecorOpt_RestoreZone(int zoneId) {\n");
    out += QStringLiteral("    if (zoneId < 1 || zoneId > DecorOpt_ZoneCount) { return; }\n");
    for (const ZoneAssignment &zone : plan.zones)
        out += QStringLiteral("    if (zoneId == %1) { DecorOpt_RestoreZone_%1(); return; }\n").arg(zone.zoneId);
    out += QStringLiteral("}\n\n");

    out += QStringLiteral("void DecorOpt_HideAll() {\n");
    out += QStringLiteral("    int zoneId;\n");
    out += QStringLiteral("    zoneId = 1;\n");
    out += QStringLiteral("    while (zoneId <= DecorOpt_ZoneCount) {\n");
    out += QStringLiteral("        DecorOpt_HideZone(zoneId);\n");
    out += QStringLiteral("        zoneId += 1;\n");
    out += QStringLiteral("    }\n");
    out += QStringLiteral("}\n\n");

    out += QStringLiteral("void DecorOpt_RestoreAll() {\n");
    out += QStringLiteral("    int zoneId;\n");
    out += QStringLiteral("    zoneId = 1;\n");
    out += QStringLiteral("    while (zoneId <= DecorOpt_ZoneCount) {\n");
    out += QStringLiteral("        DecorOpt_RestoreZone(zoneId);\n");
    out += QStringLiteral("        zoneId += 1;\n");
    out += QStringLiteral("    }\n");
    out += QStringLiteral("}\n\n");

    out += QStringLiteral("bool DecorOpt_IsZoneHidden(int zoneId) {\n");
    out += QStringLiteral("    if (zoneId < 1 || zoneId > DecorOpt_ZoneCount) { return false; }\n");
    out += QStringLiteral("    return DecorOpt_ZoneHidden[zoneId];\n");
    out += QStringLiteral("}\n\n");

    for (const ZoneAssignment &zone : plan.zones) {
        out += QStringLiteral("void %1_Hide_%2() { DecorOpt_HideZone(%2); }\n").arg(prefix).arg(zone.zoneId);
        out += QStringLiteral("void %1_Restore_%2() { DecorOpt_RestoreZone(%2); }\n").arg(prefix).arg(zone.zoneId);
    }
    out += QStringLiteral("void %1_HideAll() { DecorOpt_HideAll(); }\n").arg(prefix);
    out += QStringLiteral("void %1_RestoreAll() { DecorOpt_RestoreAll(); }\n").arg(prefix);

    return out;
}

bool DecorationStreamingPlanner::validateGeneratedGalaxy(const QString &galaxySource, QStringList *errors) const
{
    if (errors)
        errors->clear();
    const auto addError = [&](const QString &error) {
        if (errors)
            *errors << error;
    };

    static const QStringList forbidden = {
        QStringLiteral("PPObjects"),
        QStringLiteral("PPLoadMap"),
        QStringLiteral("PPDoodadCreateActor"),
        QStringLiteral("a = libNtve_gf_CreateActorAtPoint")
    };
    for (const QString &token : forbidden) {
        if (galaxySource.contains(token, Qt::CaseSensitive))
            addError(QStringLiteral("Generated Galaxy uses forbidden helper %1.").arg(token));
    }

    int braces = 0;
    int parens = 0;
    bool inString = false;
    bool escaped = false;
    for (qsizetype i = 0; i < galaxySource.size(); ++i) {
        const QChar ch = galaxySource.at(i);
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == QLatin1Char('\\')) {
                escaped = true;
            } else if (ch == QLatin1Char('"')) {
                inString = false;
            }
            continue;
        }
        if (ch == QLatin1Char('"')) {
            inString = true;
            continue;
        }
        if (ch == QLatin1Char('{'))
            ++braces;
        else if (ch == QLatin1Char('}'))
            --braces;
        else if (ch == QLatin1Char('('))
            ++parens;
        else if (ch == QLatin1Char(')'))
            --parens;
        if (braces < 0 || parens < 0)
            addError(QStringLiteral("Generated Galaxy has an unmatched closing bracket near offset %1.").arg(i));
    }
    if (inString)
        addError(QStringLiteral("Generated Galaxy has an unterminated string literal."));
    if (braces != 0)
        addError(QStringLiteral("Generated Galaxy has unbalanced braces."));
    if (parens != 0)
        addError(QStringLiteral("Generated Galaxy has unbalanced parentheses."));

    static const QStringList required = {
        QStringLiteral("void DecorOpt_Init()"),
        QStringLiteral("void DecorOpt_CreateZone(int zoneId)"),
        QStringLiteral("void DecorOpt_CreateAll()"),
        QStringLiteral("void DecorOpt_ClearZone(int zoneId)"),
        QStringLiteral("void DecorOpt_ClearAll()"),
        QStringLiteral("bool DecorOpt_IsZoneLoaded(int zoneId)")
    };
    for (const QString &signature : required) {
        if (!galaxySource.contains(signature))
            addError(QStringLiteral("Generated Galaxy is missing required API: %1.").arg(signature));
    }

    static const QRegularExpression cStyleArrayDeclaration(
        QStringLiteral("\\b(?:actor|int|bool)\\s+[A-Za-z_][A-Za-z0-9_]*\\s*\\["));
    if (cStyleArrayDeclaration.match(galaxySource).hasMatch())
        addError(QStringLiteral("Generated Galaxy uses C-style array declaration syntax."));

    return !errors || errors->isEmpty();
}

bool DecorationStreamingPlanner::validateGeneratedVisibilityGalaxy(const QString &galaxySource,
                                                                    QStringList *errors) const
{
    if (errors)
        errors->clear();
    const auto addError = [&](const QString &error) {
        if (errors)
            *errors << error;
    };

    static const QStringList forbidden = {
        QStringLiteral("PPObjects"),
        QStringLiteral("PPLoadMap"),
        QStringLiteral("PPDoodadCreateActor"),
        QStringLiteral("libNtve_gf_CreateActorAtPoint"),
        QStringLiteral("\"Destroy\"")
    };
    for (const QString &token : forbidden) {
        if (galaxySource.contains(token, Qt::CaseSensitive))
            addError(QStringLiteral("Generated visibility Galaxy uses forbidden helper %1.").arg(token));
    }

    int braces = 0;
    int parens = 0;
    bool inString = false;
    bool escaped = false;
    for (qsizetype i = 0; i < galaxySource.size(); ++i) {
        const QChar ch = galaxySource.at(i);
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == QLatin1Char('\\')) {
                escaped = true;
            } else if (ch == QLatin1Char('"')) {
                inString = false;
            }
            continue;
        }
        if (ch == QLatin1Char('"')) {
            inString = true;
            continue;
        }
        if (ch == QLatin1Char('{'))
            ++braces;
        else if (ch == QLatin1Char('}'))
            --braces;
        else if (ch == QLatin1Char('('))
            ++parens;
        else if (ch == QLatin1Char(')'))
            --parens;
        if (braces < 0 || parens < 0)
            addError(QStringLiteral("Generated visibility Galaxy has an unmatched closing bracket near offset %1.").arg(i));
    }
    if (inString)
        addError(QStringLiteral("Generated visibility Galaxy has an unterminated string literal."));
    if (braces != 0)
        addError(QStringLiteral("Generated visibility Galaxy has unbalanced braces."));
    if (parens != 0)
        addError(QStringLiteral("Generated visibility Galaxy has unbalanced parentheses."));

    static const QStringList required = {
        QStringLiteral("void DecorOpt_Init()"),
        QStringLiteral("void DecorOpt_HideZone(int zoneId)"),
        QStringLiteral("void DecorOpt_RestoreZone(int zoneId)"),
        QStringLiteral("void DecorOpt_HideAll()"),
        QStringLiteral("void DecorOpt_RestoreAll()"),
        QStringLiteral("bool DecorOpt_IsZoneHidden(int zoneId)"),
        QStringLiteral("ActorFromDoodad(DoodadFromId(")
    };
    for (const QString &signature : required) {
        if (!galaxySource.contains(signature))
            addError(QStringLiteral("Generated visibility Galaxy is missing required API: %1.").arg(signature));
    }

    static const QRegularExpression cStyleArrayDeclaration(
        QStringLiteral("\\b(?:actor|int|bool)\\s+[A-Za-z_][A-Za-z0-9_]*\\s*\\["));
    if (cStyleArrayDeclaration.match(galaxySource).hasMatch())
        addError(QStringLiteral("Generated visibility Galaxy uses C-style array declaration syntax."));

    return !errors || errors->isEmpty();
}

bool DecorationStreamingPlanner::injectGalaxyInclude(const QByteArray &mapScriptBytes,
                                                     const QString &scriptEntry,
                                                     QByteArray *rewrittenMapScriptBytes,
                                                     QString *errorMessage) const
{
    if (!rewrittenMapScriptBytes) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Missing rewritten MapScript output buffer.");
        return false;
    }
    QString normalizedEntry = scriptEntry.trimmed().replace('\\', '/');
    while (normalizedEntry.startsWith(QLatin1Char('/')))
        normalizedEntry.remove(0, 1);
    if (normalizedEntry.isEmpty() || !normalizedEntry.endsWith(QStringLiteral(".galaxy"), Qt::CaseInsensitive)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Generated runtime include path must point to a .galaxy script entry.");
        return false;
    }

    QString script = QString::fromUtf8(mapScriptBytes);
    // Galaxy's map compiler treats imported script names as extensionless
    // resource identifiers and appends ".galaxy" while regenerating the
    // Created script. Supplying the physical MPQ extension here survives the
    // first raw compile, but an Editor save turns it into
    // "<name>.galaxy.galaxy". Keep the archive entry extension and inject the
    // canonical extensionless reference instead.
    QString includeReference = normalizedEntry;
    includeReference.chop(QStringLiteral(".galaxy").size());
    const QString includeLine = QStringLiteral("include \"%1\"").arg(includeReference);
    const QRegularExpression existingInclude(
        QStringLiteral("^\\s*include\\s+\"(?:%1|%2)\"\\s*;?\\s*$")
            .arg(QRegularExpression::escape(includeReference),
                 QRegularExpression::escape(normalizedEntry)),
                                             QRegularExpression::CaseInsensitiveOption
                                                 | QRegularExpression::MultilineOption);

    QString prefix;
    if (script.startsWith(QChar(0xFEFF))) {
        prefix = script.left(1);
        script.remove(0, 1);
    }

    const bool hasRuntimeInclude = existingInclude.match(script).hasMatch();
    const QString code = galaxyCodeWithoutTrivia(script);
    static const QRegularExpression runtimeDefinition(QStringLiteral("\\bvoid\\s+DecorOpt_Init\\s*\\("),
                                                       QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression runtimeReference(QStringLiteral("\\bDecorOpt_Init\\s*\\("),
                                                      QRegularExpression::CaseInsensitiveOption);
    if (runtimeDefinition.match(code).hasMatch()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("MapScript.galaxy already defines DecorOpt_Init(); refusing to overwrite a user runtime function.");
        }
        return false;
    }
    if (!hasRuntimeInclude && runtimeReference.match(code).hasMatch()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("MapScript.galaxy already references DecorOpt_Init() without the generated runtime include; refusing an ambiguous injection.");
        }
        return false;
    }

    QString rewritten = script;
    if (!hasRuntimeInclude) {
        QString withInclude = includeLine % QStringLiteral("\n");
        if (!rewritten.isEmpty() && !rewritten.startsWith(QLatin1Char('\n')) && !rewritten.startsWith(QLatin1Char('\r')))
            withInclude += QLatin1Char('\n');
        withInclude += rewritten;
        rewritten = withInclude;
    }
    if (!injectMapInitCall(&rewritten, errorMessage))
        return false;
    rewritten = prefix + rewritten;
    *rewrittenMapScriptBytes = rewritten.toUtf8();
    return true;
}

QByteArray DecorationStreamingPlanner::removeDynamicDoodadsFromObjects(const QByteArray &objectsBytes,
                                                                       const DecorationStreamingPlan &plan,
                                                                       QVector<int> *removedDoodadIndices,
                                                                       QStringList *warnings) const
{
    if (removedDoodadIndices)
        removedDoodadIndices->clear();
    if (warnings)
        warnings->clear();

    QSet<int> removeSet;
    for (const ZoneAssignment &zone : plan.zones) {
        for (int doodadIndex : zone.doodadIndices) {
            if (removeSet.contains(doodadIndex)) {
                if (warnings)
                    *warnings << QStringLiteral("Doodad index %1 is assigned to more than one dynamic zone.").arg(doodadIndex);
                continue;
            }
            removeSet.insert(doodadIndex);
        }
    }

    struct Range
    {
        qsizetype start = -1;
        qsizetype end = -1;
        int doodadIndex = -1;
    };
    QVector<Range> ranges;
    const QString text = QString::fromUtf8(objectsBytes);
    for (int doodadIndex : removeSet) {
        if (doodadIndex < 0 || doodadIndex >= plan.doodads.size()) {
            if (warnings)
                *warnings << QStringLiteral("Invalid dynamic doodad index %1.").arg(doodadIndex);
            continue;
        }
        const DoodadPlacement &doodad = plan.doodads.at(doodadIndex);
        if (!doodad.dynamicCandidate) {
            if (warnings)
                *warnings << QStringLiteral("Refused to remove static-only doodad %1 from Objects.")
                                .arg(doodad.name.isEmpty() ? doodad.type : doodad.name);
            continue;
        }
        if (doodad.sourceStart < 0 || doodad.sourceEnd <= doodad.sourceStart || doodad.sourceEnd > text.size()) {
            if (warnings)
                *warnings << QStringLiteral("Refused to remove doodad %1 because its source range is invalid.")
                                .arg(doodad.name.isEmpty() ? doodad.type : doodad.name);
            continue;
        }
        ranges << Range{doodad.sourceStart, doodad.sourceEnd, doodadIndex};
    }

    std::sort(ranges.begin(), ranges.end(), [](const Range &left, const Range &right) {
        return left.start < right.start;
    });
    qsizetype previousEnd = -1;
    QVector<Range> safeRanges;
    for (Range range : ranges) {
        if (range.start < previousEnd) {
            if (warnings)
                *warnings << QStringLiteral("Refused to remove overlapping ObjectDoodad range at offset %1.").arg(range.start);
            continue;
        }
        previousEnd = range.end;
        if (range.end < text.size()) {
            if (text.mid(range.end, 2) == QStringLiteral("\r\n"))
                range.end += 2;
            else if (text.at(range.end) == QLatin1Char('\n') || text.at(range.end) == QLatin1Char('\r'))
                range.end += 1;
        }
        safeRanges << range;
    }

    QString optimized = text;
    std::sort(safeRanges.begin(), safeRanges.end(), [](const Range &left, const Range &right) {
        return left.start > right.start;
    });
    for (const Range &range : safeRanges) {
        optimized.remove(range.start, range.end - range.start);
        if (removedDoodadIndices)
            removedDoodadIndices->prepend(range.doodadIndex);
    }

    return optimized.toUtf8();
}

DecorationOptimizedArtifacts DecorationStreamingPlanner::createOptimizedArtifacts(const QByteArray &objectsBytes,
                                                                                  const QVector<DecorZone> &zones,
                                                                                  const DecorationSafetyContext &safetyContext,
                                                                                  const GalaxyGenerationOptions &options) const
{
    DecorationOptimizedArtifacts artifacts;
    artifacts.plan = buildPlan(objectsBytes, zones, safetyContext, &artifacts.warnings);
    const QString prefix = options.functionPrefix.isEmpty() ? QStringLiteral("NAME_OUT_FUNK") : options.functionPrefix;
    if (!isGalaxyIdentifier(prefix))
        artifacts.warnings << QStringLiteral("Decoration function prefix is not a valid Galaxy identifier: %1.").arg(prefix);
    if (options.batchLimit <= 0)
        artifacts.warnings << QStringLiteral("Decoration batch limit must be positive.");

    artifacts.roundTripVerified = true;
    for (const ZoneAssignment &zone : artifacts.plan.zones) {
        for (int doodadIndex : zone.doodadIndices) {
            QString roundTripError;
            if (doodadIndex < 0 || doodadIndex >= artifacts.plan.doodads.size()
                || !verifyPlacementRoundTrip(artifacts.plan.doodads.at(doodadIndex), &roundTripError)) {
                artifacts.roundTripVerified = false;
                artifacts.warnings << QStringLiteral("Doodad round-trip blocked: %1").arg(roundTripError);
            }
        }
    }

    QStringList removalWarnings;
    artifacts.optimizedObjectsBytes = removeDynamicDoodadsFromObjects(objectsBytes,
                                                                      artifacts.plan,
                                                                      &artifacts.removedDoodadIndices,
                                                                      &removalWarnings);
    artifacts.warnings += removalWarnings;
    artifacts.outsideScopePreserved = true;
    const QSet<int> removedSet(artifacts.removedDoodadIndices.cbegin(), artifacts.removedDoodadIndices.cend());
    for (int index = 0; index < artifacts.plan.doodads.size(); ++index) {
        if (removedSet.contains(index))
            continue;
        const QByteArray raw = artifacts.plan.doodads.at(index).rawSource.toUtf8();
        if (raw.isEmpty() || !artifacts.optimizedObjectsBytes.contains(raw)) {
            artifacts.outsideScopePreserved = false;
            artifacts.warnings << QStringLiteral("Outside-scope preservation check failed for doodad index %1.").arg(index);
        }
    }
    artifacts.galaxySource = generateGalaxy(artifacts.plan, options);
    QStringList galaxyErrors;
    if (!validateGeneratedGalaxy(artifacts.galaxySource, &galaxyErrors))
        artifacts.warnings += galaxyErrors;
    artifacts.valid = artifacts.warnings.isEmpty()
        && artifacts.roundTripVerified && artifacts.outsideScopePreserved;
    return artifacts;
}

DecorationOptimizedArtifacts DecorationStreamingPlanner::createOptimizedArtifacts(const QByteArray &objectsBytes,
                                                                                  const QVector<DecorZone> &zones,
                                                                                  const GalaxyGenerationOptions &options) const
{
    return createOptimizedArtifacts(objectsBytes, zones, DecorationSafetyContext{}, options);
}

DecorationVisibilityArtifacts DecorationStreamingPlanner::createVisibilityArtifacts(
    const QByteArray &objectsBytes,
    const QVector<DecorZone> &zones,
    const DecorationSafetyContext &safetyContext,
    const GalaxyGenerationOptions &options) const
{
    DecorationVisibilityArtifacts artifacts;
    artifacts.plan = buildVisibilityPlan(objectsBytes, zones, safetyContext, &artifacts.warnings);
    const QString prefix = options.functionPrefix.isEmpty() ? QStringLiteral("NAME_OUT_FUNK") : options.functionPrefix;
    if (!isGalaxyIdentifier(prefix))
        artifacts.warnings << QStringLiteral("Decoration function prefix is not a valid Galaxy identifier: %1.").arg(prefix);
    if (options.batchLimit <= 0)
        artifacts.warnings << QStringLiteral("Decoration batch limit must be positive.");

    QSet<int> controlledSet;
    for (const ZoneAssignment &zone : artifacts.plan.zones) {
        for (int doodadIndex : zone.doodadIndices) {
            if (doodadIndex < 0 || doodadIndex >= artifacts.plan.doodads.size()) {
                artifacts.warnings << QStringLiteral("Invalid visibility doodad index %1.").arg(doodadIndex);
                continue;
            }
            const DoodadPlacement &doodad = artifacts.plan.doodads.at(doodadIndex);
            qint64 doodadId = 0;
            if (!doodad.visibilityCandidate || !validDoodadActorId(doodad, &doodadId)) {
                artifacts.warnings << QStringLiteral("Refused to generate actor visibility for static-only doodad %1.")
                                          .arg(doodad.name.isEmpty() ? doodad.type : doodad.name);
                continue;
            }
            if (controlledSet.contains(doodadIndex)) {
                artifacts.warnings << QStringLiteral("Doodad %1 is assigned to more than one visibility zone.")
                                          .arg(doodadId);
                continue;
            }
            controlledSet.insert(doodadIndex);
            artifacts.controlledDoodadIndices << doodadIndex;
        }
    }

    artifacts.objectsPreserved = true;
    artifacts.galaxySource = generateVisibilityGalaxy(artifacts.plan, options);
    QStringList galaxyErrors;
    if (!validateGeneratedVisibilityGalaxy(artifacts.galaxySource, &galaxyErrors))
        artifacts.warnings += galaxyErrors;
    if (artifacts.controlledDoodadIndices.isEmpty()) {
        artifacts.warnings << QStringLiteral("No visibility-safe visual doodads were assigned to decoration zones.");
    }
    artifacts.valid = artifacts.warnings.isEmpty() && artifacts.objectsPreserved;
    return artifacts;
}

DecorationVisibilityArtifacts DecorationStreamingPlanner::createVisibilityArtifacts(
    const QByteArray &objectsBytes,
    const QVector<DecorZone> &zones,
    const GalaxyGenerationOptions &options) const
{
    return createVisibilityArtifacts(objectsBytes, zones, DecorationSafetyContext{}, options);
}

DecorationArchivePatch DecorationStreamingPlanner::prepareArchivePatch(const QByteArray &objectsBytes,
                                                                       const QByteArray &mapScriptBytes,
                                                                       const QVector<DecorZone> &zones,
                                                                       const DecorationSafetyContext &safetyContext,
                                                                       const GalaxyGenerationOptions &options,
                                                                       const QString &objectsEntry,
                                                                       const QString &mapScriptEntry,
                                                                       const QString &runtimeEntry) const
{
    DecorationArchivePatch patch;
    patch.objectsEntry = objectsEntry.isEmpty() ? QStringLiteral("Objects") : objectsEntry;
    patch.mapScriptEntry = mapScriptEntry.isEmpty() ? QStringLiteral("MapScript.galaxy") : mapScriptEntry;
    patch.runtimeEntry = runtimeEntry.isEmpty() ? QStringLiteral("scripts/sc2dh_decor_opt.galaxy") : runtimeEntry;

    patch.artifacts = createOptimizedArtifacts(objectsBytes, zones, safetyContext, options);
    patch.warnings = patch.artifacts.warnings;
    if (!patch.artifacts.valid) {
        patch.error = QStringLiteral("Decoration optimized artifacts are not valid.");
        return patch;
    }
    if (patch.artifacts.removedDoodadIndices.isEmpty()) {
        patch.error = QStringLiteral("No dynamic visual doodads were assigned to decoration zones.");
        return patch;
    }

    QByteArray rewrittenMapScript;
    if (!injectGalaxyInclude(mapScriptBytes, patch.runtimeEntry, &rewrittenMapScript, &patch.error))
        return patch;

    patch.replacementEntries.insert(patch.objectsEntry, patch.artifacts.optimizedObjectsBytes);
    patch.replacementEntries.insert(patch.runtimeEntry, patch.artifacts.galaxySource.toUtf8());
    patch.replacementEntries.insert(patch.mapScriptEntry, rewrittenMapScript);
    patch.valid = true;
    return patch;
}

DecorationArchivePatch DecorationStreamingPlanner::prepareArchivePatch(const QByteArray &objectsBytes,
                                                                       const QByteArray &mapScriptBytes,
                                                                       const QVector<DecorZone> &zones,
                                                                       const GalaxyGenerationOptions &options,
                                                                       const QString &objectsEntry,
                                                                       const QString &mapScriptEntry,
                                                                       const QString &runtimeEntry) const
{
    return prepareArchivePatch(objectsBytes, mapScriptBytes, zones, DecorationSafetyContext{},
                               options, objectsEntry, mapScriptEntry, runtimeEntry);
}

DecorationArchivePatch DecorationStreamingPlanner::prepareVisibilityArchivePatch(
    const QByteArray &objectsBytes,
    const QByteArray &mapScriptBytes,
    const QVector<DecorZone> &zones,
    const DecorationSafetyContext &safetyContext,
    const GalaxyGenerationOptions &options,
    const QString &objectsEntry,
    const QString &mapScriptEntry,
    const QString &runtimeEntry) const
{
    DecorationArchivePatch patch;
    patch.mode = DecorationOptimizationMode::VisibilityOnly;
    patch.objectsEntry = objectsEntry.isEmpty() ? QStringLiteral("Objects") : objectsEntry;
    patch.mapScriptEntry = mapScriptEntry.isEmpty() ? QStringLiteral("MapScript.galaxy") : mapScriptEntry;
    patch.runtimeEntry = runtimeEntry.isEmpty() ? QStringLiteral("scripts/sc2dh_decor_opt.galaxy") : runtimeEntry;

    patch.visibilityArtifacts = createVisibilityArtifacts(objectsBytes, zones, safetyContext, options);
    patch.warnings = patch.visibilityArtifacts.warnings;
    if (!patch.visibilityArtifacts.valid) {
        patch.error = QStringLiteral("Decoration visibility artifacts are not valid.");
        return patch;
    }

    QByteArray rewrittenMapScript;
    if (!injectGalaxyInclude(mapScriptBytes, patch.runtimeEntry, &rewrittenMapScript, &patch.error))
        return patch;

    // Intentionally do not replace Objects: visibility mode controls existing
    // doodad actors only, so every editor placement remains byte-identical.
    patch.replacementEntries.insert(patch.runtimeEntry, patch.visibilityArtifacts.galaxySource.toUtf8());
    patch.replacementEntries.insert(patch.mapScriptEntry, rewrittenMapScript);
    if (patch.replacementEntries.contains(patch.objectsEntry)) {
        patch.error = QStringLiteral("Visibility-only decoration patch must not replace Objects.");
        return patch;
    }
    patch.valid = true;
    return patch;
}

DecorationArchivePatch DecorationStreamingPlanner::prepareVisibilityArchivePatch(
    const QByteArray &objectsBytes,
    const QByteArray &mapScriptBytes,
    const QVector<DecorZone> &zones,
    const GalaxyGenerationOptions &options,
    const QString &objectsEntry,
    const QString &mapScriptEntry,
    const QString &runtimeEntry) const
{
    return prepareVisibilityArchivePatch(objectsBytes, mapScriptBytes, zones, DecorationSafetyContext{},
                                         options, objectsEntry, mapScriptEntry, runtimeEntry);
}

} // namespace sc2dh::decor
