#include "core/DecorationStreamingPlanner.h"

#include <QRegularExpression>
#include <QMap>
#include <QSet>
#include <QStringBuilder>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <utility>
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
    QString tuple = value.trimmed();
    if (tuple.startsWith(QLatin1Char('(')) && tuple.endsWith(QLatin1Char(')')))
        tuple = tuple.mid(1, tuple.size() - 2).trimmed();
    static const QString numberPattern = QStringLiteral("[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?");
    static const QRegularExpression tupleExpression(
        QStringLiteral("\\A%1(?:(?:\\s*,\\s*|\\s+)%1)*\\z").arg(numberPattern));
    if (!tupleExpression.match(tuple).hasMatch())
        return {};
    QVector<double> result;
    static const QRegularExpression number(numberPattern);
    auto matches = number.globalMatch(tuple);
    while (matches.hasNext()) {
        bool ok = false;
        const double parsed = matches.next().captured(0).toDouble(&ok);
        if (!ok || !std::isfinite(parsed))
            return {};
        result << parsed;
    }
    return result;
}

bool validNumericTuple(const QString &value, std::initializer_list<int> allowedCounts)
{
    const QVector<double> numbers = numbersFrom(value);
    return std::find(allowedCounts.begin(), allowedCounts.end(), numbers.size()) != allowedCounts.end();
}

bool hasUnsupportedLegacyFields(const QString &block, const QSet<QString> &known)
{
    // Consume the whole record: unknown/nested or duplicate fields must not
    // disappear merely because fieldValue found the familiar properties.
    static const QRegularExpression field(QStringLiteral(
        "\\s*[,;]?\\s*([A-Za-z_][A-Za-z_0-9]*)\\s*(?:=|:)\\s*(\"[^\"]*\"|\\([^(){}]*\\)|[^\\s,;{}]+)"));
    QSet<QString> seen;
    qsizetype offset = 0;
    while (offset < block.size()) {
        const QString remainder = block.mid(offset).trimmed();
        if (remainder.isEmpty() || remainder == QStringLiteral(";") || remainder == QStringLiteral(","))
            return false;
        const auto match = field.match(block, offset, QRegularExpression::NormalMatch,
                                      QRegularExpression::AnchorAtOffsetMatchOption);
        if (!match.hasMatch())
            return true;
        const QString key = match.captured(1).toLower();
        if (!known.contains(key) || seen.contains(key) || match.captured(2) == QStringLiteral("\"\""))
            return true;
        seen.insert(key);
        offset = match.capturedEnd();
    }
    return false;
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
    bool nullColor = false;
    double red = 100.0;
    double green = 100.0;
    double blue = 100.0;
    double hdr = 1.0;
};

bool fitsGalaxyFixed(double value)
{
    // Galaxy fixed is signed 32-bit with 12 fractional bits. Check the
    // emitted six-decimal literal too: rounding must not cross the limit.
    constexpr double maximum = double(std::numeric_limits<int>::max()) / 4096.0;
    return std::isfinite(value) && std::abs(value) <= maximum
        && std::abs(QString::number(value, 'f', 6).toDouble()) <= maximum;
}

ParsedTint parseTintColor(const QString &value)
{
    ParsedTint tint;
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty())
        return tint;

    const QVector<double> values = numbersFrom(trimmed);
    if (trimmed.startsWith(QStringLiteral("NULL"), Qt::CaseInsensitive)) {
        // CColorHDR stores a nullable packed color separately from HDR.
        // NULL is not explicit white, especially for weather/light actors.
        static const QRegularExpression nullTintExpression(
            QStringLiteral("^NULL(?:\\s+([+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?))?$"),
            QRegularExpression::CaseInsensitiveOption);
        const auto match = nullTintExpression.match(trimmed);
        if (!match.hasMatch())
            return tint;
        if (!match.captured(1).isEmpty()) {
            bool ok = false;
            tint.hdr = match.captured(1).toDouble(&ok);
            if (!ok || !std::isfinite(tint.hdr) || tint.hdr < 0.0
                || tint.hdr > double(std::numeric_limits<float>::max()))
                return tint;
        }
        tint.valid = true;
        tint.nullColor = true;
        return tint;
    }
    if (values.size() != 3 && values.size() != 4)
        return tint;
    for (int index = 0; index < 3; ++index) {
        if (values.at(index) < 0.0 || values.at(index) > 255.0)
            return tint;
    }
    if (values.size() == 4 && (values.at(3) < 0.0 || !fitsGalaxyFixed(values.at(3))))
        return tint;
    tint.valid = true;
    tint.red = values.at(0) * 100.0 / 255.0;
    tint.green = values.at(1) * 100.0 / 255.0;
    tint.blue = values.at(2) * 100.0 / 255.0;
    if (values.size() == 4)
        tint.hdr = values.at(3);
    return tint;
}

bool isOptionalIntegerInRange(const QString &value, int maximum)
{
    if (value.trimmed().isEmpty())
        return true;
    bool ok = false;
    const int parsed = value.trimmed().toInt(&ok);
    return ok && parsed >= 0 && parsed <= maximum;
}

bool hasUnsupportedPlacementFlags(const QHash<QString, QString> &flags)
{
    for (auto it = flags.cbegin(); it != flags.cend(); ++it) {
        if (it.value() != QStringLiteral("1")
            || (it.key() != QStringLiteral("HeightAbsolute")
                && it.key() != QStringLiteral("HeightOffset")
                && it.key() != QStringLiteral("NoDoodadFootprint")
                && it.key() != QStringLiteral("ForcePlacement")))
            return true;
    }
    return false;
}

void finalizeDoodadSafety(sc2dh::decor::DoodadPlacement *doodad)
{
    if (!doodad)
        return;
    if (doodad->type.isEmpty()) {
        doodad->staticOnlyReason = QStringLiteral("Static-only: missing doodad type.");
    } else if (!doodad->hasPosition) {
        doodad->staticOnlyReason = QStringLiteral("Static-only: missing position.");
    } else if (doodad->hasInvalidTransform) {
        doodad->staticOnlyReason = QStringLiteral("Static-only: malformed position, rotation, or scale.");
    } else if (!fitsGalaxyFixed(doodad->x) || !fitsGalaxyFixed(doodad->y)
               || !fitsGalaxyFixed(doodad->scaleX) || !fitsGalaxyFixed(doodad->scaleY)
               || !fitsGalaxyFixed(doodad->scaleZ)
               || std::abs(doodad->z) > double(std::numeric_limits<float>::max())) {
        doodad->staticOnlyReason = QStringLiteral("Static-only: transform exceeds runtime numeric range.");
    } else if (!doodad->otherAttributes.isEmpty() || doodad->hasUnsupportedChildren) {
        doodad->staticOnlyReason = QStringLiteral("Static-only: placement has fields the runtime recreation serializer cannot preserve.");
    } else if (!isOptionalIntegerInRange(doodad->variation, 254)) {
        // The native model variation is a byte; 255 means random selection.
        doodad->staticOnlyReason = QStringLiteral("Static-only: malformed model variation.");
    } else if (!isOptionalIntegerInRange(doodad->teamColor, 15)) {
        doodad->staticOnlyReason = QStringLiteral("Static-only: malformed team color.");
    } else if (!doodad->flags.isEmpty() || hasUnsupportedPlacementFlags(doodad->placementFlags)) {
        doodad->staticOnlyReason = QStringLiteral("Static-only: unsupported placement or footprint flag.");
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

QString preciseNumber(double value)
{
    QString result = QString::number(value == 0.0 ? 0.0 : value, 'g', std::numeric_limits<double>::max_digits10);
    if (!result.contains(QLatin1Char('.')) && !result.contains(QLatin1Char('e'), Qt::CaseInsensitive))
        result += QStringLiteral(".0");
    return result;
}

QString sourceAngles(const QString &raw, std::initializer_list<std::pair<const char *, double>> angles)
{
    // Parse a placement once for all three axes; preserve its original literals.
    pugi::xml_document doc;
    const bool isXml = raw.trimmed().startsWith(QLatin1Char('<'));
    if (isXml) {
        const auto bytes = raw.toUtf8();
        doc.load_buffer(bytes.constData(), size_t(bytes.size()));
    }
    QStringList literals;
    literals.reserve(qsizetype(angles.size()));
    for (const auto &[field, value] : angles) {
        QString literal = isXml
            ? QString::fromUtf8(doc.document_element().attribute(field).value()).trimmed()
            : fieldValue(raw, QString::fromLatin1(field)).trimmed();
        bool ok = false;
        const double original = literal.toDouble(&ok);
        if (!raw.isEmpty() && literal.isEmpty() && value == 0.0)
            literals << QStringLiteral("~");
        else if (ok && original == value
                 && std::none_of(literal.cbegin(), literal.cend(), [](QChar c) { return c.isSpace(); }))
            literals << literal;
        else
            literals << preciseNumber(value);
    }
    return literals.join(QLatin1Char(','));
}

QString actorRotation(const sc2dh::decor::DoodadPlacement &doodad)
{
    // Native Objects loader: XML Pitch goes to placement offset 0x34 (X),
    // XML Roll to 0x38 (Y). Compose Rz(yaw)*Rx(pitch)*Ry(roll), in that order.
    // SetRotation's forward/up parser uses local -Y as forward, not local +X.
    // Sending MakeModelFaceAngle's (cos(yaw),sin(yaw),0) would therefore add
    // 90 degrees to the model transform stored in Objects. Rotate local -Y
    // and +Z by the placement transform instead, including for zero rotation.
    const double cy = std::cos(doodad.rotation);
    const double sy = std::sin(doodad.rotation);
    const double cp = std::cos(doodad.pitch);
    const double sp = std::sin(doodad.pitch);
    const double cr = std::cos(doodad.roll);
    const double sr = std::sin(doodad.roll);
    return QStringLiteral("libNtve_gf_SetRotation(%1, %2, %3, %4, %5, %6)")
        .arg(preciseNumber(sy * cp), preciseNumber(-cy * cp), preciseNumber(-sp),
             preciseNumber(cy * sr + sy * sp * cr), preciseNumber(sy * sr - cy * sp * cr), preciseNumber(cp * cr));
}

// Only identity and editor flags live in this annotation. Every transform,
// model variant, player and tint is reconstructed from executable statements.
QString placementIdentity(const QString &raw, const char *kind, const QString &id, const QString &name)
{
    pugi::xml_document doc;
    const QByteArray bytes = raw.toUtf8();
    doc.load_buffer(bytes.constData(), size_t(bytes.size()));
    auto node = doc.document_element();
    if (!node) node = doc.append_child(kind);
    for (const char *field : {"Type", "UnitType", "Player", "Position", "Rotation", "Pitch", "Roll", "Scale", "Variation", "TeamColor", "TintColor"})
        node.remove_attribute(field);
    if (!node.attribute("Id")) node.append_attribute("Id") = id.toUtf8().constData();
    if (!name.isEmpty() && !node.attribute("Name")) node.append_attribute("Name") = name.toUtf8().constData();
    struct Writer : pugi::xml_writer {
        QByteArray bytes;
        void write(const void *data, size_t size) override { bytes.append(static_cast<const char *>(data), qsizetype(size)); }
    } writer;
    node.print(writer, "", pugi::format_raw);
    return QString::fromLatin1(writer.bytes.toBase64());
}

sc2dh::region::SpatialRelation relationToZone(double x, double y,
                                              const sc2dh::decor::DecorZone &zone)
{
    if (zone.geometry.supported)
        return zone.geometry.classify(x, y);
    const double xMin = std::min(zone.xMin, zone.xMax);
    const double xMax = std::max(zone.xMin, zone.xMax);
    const double yMin = std::min(zone.yMin, zone.yMax);
    const double yMax = std::max(zone.yMin, zone.yMax);
    if (x < xMin || x > xMax || y < yMin || y > yMax)
        return sc2dh::region::SpatialRelation::Outside;
    if (std::abs(x - xMin) <= 1e-6 || std::abs(x - xMax) <= 1e-6
        || std::abs(y - yMin) <= 1e-6 || std::abs(y - yMax) <= 1e-6)
        return sc2dh::region::SpatialRelation::Boundary;
    return sc2dh::region::SpatialRelation::Inside;
}

sc2dh::region::SpatialRelation relationToZone(const sc2dh::decor::DoodadPlacement &doodad,
                                              const sc2dh::decor::DecorZone &zone)
{
    return relationToZone(doodad.x, doodad.y, zone);
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

QStringList unitKeys(const sc2dh::decor::UnitPlacement &unit)
{
    QStringList keys;
    for (const QString &value : {unit.id, unit.name}) {
        const QString key = safetyKey(value);
        if (!key.isEmpty())
            keys << key;
    }
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
    if (context.keepReferencedTypes)
        files += context.referenceFilesByDoodadType.value(safetyKey(doodad.type));
    files.removeAll(QString());
    files.removeDuplicates();
    std::sort(files.begin(), files.end(), [](const QString &left, const QString &right) {
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
    return files;
}

QStringList externalReferenceFiles(const sc2dh::decor::UnitPlacement &unit,
                                   const sc2dh::decor::DecorationSafetyContext &context)
{
    QStringList files;
    for (const QString &key : unitKeys(unit))
        files += context.referenceFilesByDoodadKey.value(key);
    files.removeAll(QString());
    files.removeDuplicates();
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

QString galaxyRegionFunctionStem(const DecorationStreamingPlan &plan,
                                 const ZoneAssignment &zone,
                                 const GalaxyGenerationOptions &options)
{
    const QString prefix = options.functionPrefix.isEmpty() ? QStringLiteral("Game_area") : options.functionPrefix;
    if (options.publicFunctionNames.contains(zone.zoneId))
        return options.publicFunctionNames.value(zone.zoneId);
    if (!options.useRegionNames)
        return QStringLiteral("%1_%2").arg(prefix).arg(zone.zoneId);
    auto identifier = [](QString name) {
        name = name.trimmed().replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_]+")), QStringLiteral("_"));
        while (name.startsWith(QLatin1Char('_')))
            name.remove(0, 1);
        while (name.endsWith(QLatin1Char('_')))
            name.chop(1);
        return name;
    };
    const QString name = identifier(zone.zoneName);
    if (name.isEmpty())
        return QStringLiteral("%1_%2").arg(prefix).arg(zone.zoneId);
    // Create_Entire_Map is the global entry, not a per-region loader.
    if (name.compare(QStringLiteral("Entire_Map"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("%1_R%2").arg(name).arg(zone.zoneId);
    for (const ZoneAssignment &other : plan.zones) {
        if (other.zoneId != zone.zoneId && identifier(other.zoneName).compare(name, Qt::CaseInsensitive) == 0)
            return QStringLiteral("%1_R%2").arg(name).arg(zone.zoneId);
    }
    return name;
}

QVector<DoodadPlacement> DecorationStreamingPlanner::parseObjects(const QByteArray &objectsBytes,
                                                                  QStringList *warnings) const
{
    if (warnings)
        warnings->clear();
    const QString text = QString::fromUtf8(objectsBytes);
    QVector<DoodadPlacement> doodads;

    if (text.trimmed().startsWith(QLatin1Char('<'))) {
        const QRegularExpression elementExpression(
            QStringLiteral("<ObjectDoodad\\b[^>]*?/>|<ObjectDoodad\\b[^>]*>[\\s\\S]*?</ObjectDoodad\\s*>"),
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
            QSet<QString> seenAttributes;
            for (pugi::xml_attribute item : element.attributes()) {
                const QString key = QString::fromUtf8(item.name());
                if (!knownAttributes.contains(key) || seenAttributes.contains(key)
                    || (key != QStringLiteral("Name") && QString::fromUtf8(item.value()).trimmed().isEmpty()))
                    doodad.otherAttributes.insert(key, QString::fromUtf8(item.value()));
                seenAttributes.insert(key);
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
                if (index.isEmpty() || unsupportedFlagField || child.first_child()
                    || std::distance(child.attributes_begin(), child.attributes_end()) != 2
                    || doodad.placementFlags.contains(index)) {
                    doodad.hasUnsupportedChildren = true;
                    continue;
                }
                doodad.placementFlags.insert(index, value);
            }

            const QVector<double> position = numbersFrom(attribute("Position"));
            doodad.hasInvalidTransform = !validNumericTuple(attribute("Position"), {2, 3})
                || (!attribute("Scale").isEmpty() && !validNumericTuple(attribute("Scale"), {1, 3}))
                || (!attribute("Rotation").isEmpty() && !validNumericTuple(attribute("Rotation"), {1}))
                || (!attribute("Pitch").isEmpty() && !validNumericTuple(attribute("Pitch"), {1}))
                || (!attribute("Roll").isEmpty() && !validNumericTuple(attribute("Roll"), {1}));
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
        static const QSet<QString> knownFields{
            QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("type"),
            QStringLiteral("position"), QStringLiteral("pos"), QStringLiteral("rotation"),
            QStringLiteral("scale"), QStringLiteral("pitch"), QStringLiteral("roll"),
            QStringLiteral("variation"), QStringLiteral("tintcolor"), QStringLiteral("teamcolor"),
            QStringLiteral("flags")};
        doodad.hasUnsupportedChildren = hasUnsupportedLegacyFields(block, knownFields)
            || (!fieldValue(block, QStringLiteral("Position")).isEmpty()
                && !fieldValue(block, QStringLiteral("Pos")).isEmpty());
        const auto invalidOptional = [&](const QString &key, std::initializer_list<int> counts) {
            const QString value = fieldValue(block, key);
            return !value.isEmpty() && !validNumericTuple(value, counts);
        };
        QString positionText = fieldValue(block, QStringLiteral("Position"));
        if (positionText.isEmpty())
            positionText = fieldValue(block, QStringLiteral("Pos"));
        doodad.hasInvalidTransform = !validNumericTuple(positionText, {2, 3})
            || invalidOptional(QStringLiteral("Scale"), {1, 3})
            || invalidOptional(QStringLiteral("Rotation"), {1})
            || invalidOptional(QStringLiteral("Pitch"), {1})
            || invalidOptional(QStringLiteral("Roll"), {1});

        const QVector<double> position = numbersFrom(positionText);
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

        finalizeDoodadSafety(&doodad);

        doodads << doodad;
        searchFrom = close + 1;
    }

    return doodads;
}

QVector<UnitPlacement> DecorationStreamingPlanner::parseUnits(const QByteArray &objectsBytes,
                                                              QStringList *warnings) const
{
    if (warnings)
        warnings->clear();
    const QString text = QString::fromUtf8(objectsBytes);
    QVector<UnitPlacement> units;
    const auto finalize = [](UnitPlacement *unit, const QString &position, const QString &rotation,
                             const QString &scale, const QString &player, bool hasExtraFields) {
        const QVector<double> coordinates = numbersFrom(position);
        QVector<double> scales = scale.isEmpty() ? QVector<double>{1.0, 1.0, 1.0} : numbersFrom(scale);
        if (scales.size() == 1) scales = QVector<double>{scales[0], scales[0], scales[0]};
        const QVector<double> rotations = rotation.isEmpty() ? QVector<double>{0.0} : numbersFrom(rotation);
        bool playerOk = false;
        unit->player = player.toInt(&playerOk);
        if (player.isEmpty()) { unit->player = 0; playerOk = true; }
        if (coordinates.size() >= 2) {
            unit->x = coordinates.at(0);
            unit->y = coordinates.at(1);
            unit->z = coordinates.size() == 3 ? coordinates.at(2) : 0.0;
        }
        if (unit->type.isEmpty())
            unit->staticOnlyReason = QStringLiteral("Missing unit type.");
        else if (coordinates.size() < 2 || coordinates.size() > 3
                 || !fitsGalaxyFixed(coordinates.at(0)) || !fitsGalaxyFixed(coordinates.at(1)) ||
                 (coordinates.size() == 3 && !fitsGalaxyFixed(coordinates.at(2))))
            unit->staticOnlyReason = QStringLiteral("Unit position cannot be recreated exactly.");
        else if (!playerOk || unit->player < 0 || unit->player > 15)
            unit->staticOnlyReason = QStringLiteral("Unit owner is missing or invalid.");
        else if (rotations.size() != 1 || !fitsGalaxyFixed(rotations.front() * 180.0 / 3.14159265358979323846))
            unit->staticOnlyReason = QStringLiteral("Unit facing cannot be recreated exactly.");
        else if (scales.size() != 3 || std::any_of(scales.cbegin(), scales.cend(), [](double v) { return v <= 0 || !fitsGalaxyFixed(v * 100.0); }))
            unit->staticOnlyReason = QStringLiteral("Unit scale cannot be recreated exactly.");
        else if (hasExtraFields)
            unit->staticOnlyReason = QStringLiteral("Unit has placement fields or children that cannot be recreated.");
        else {
            unit->x = coordinates.at(0);
            unit->y = coordinates.at(1);
            unit->rotation = rotations.front();
            unit->scaleX = scales[0]; unit->scaleY = scales[1]; unit->scaleZ = scales[2];
            unit->dynamicCandidate = true;
        }
    };

    if (text.trimmed().startsWith(QLatin1Char('<'))) {
        const QRegularExpression expression(
            QStringLiteral("<ObjectUnit\\b[^>]*?/>|<ObjectUnit\\b[^>]*>[\\s\\S]*?</ObjectUnit\\s*>"),
            QRegularExpression::CaseInsensitiveOption);
        auto matches = expression.globalMatch(text);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            const QString raw = match.captured(0);
            const QByteArray rawBytes = raw.toUtf8();
            pugi::xml_document document;
            if (!document.load_buffer(rawBytes.constData(), size_t(rawBytes.size()),
                                      pugi::parse_default, pugi::encoding_utf8)) {
                if (warnings)
                    *warnings << QStringLiteral("Unable to parse ObjectUnit XML at offset %1.").arg(match.capturedStart());
                continue;
            }
            const pugi::xml_node node = document.child("ObjectUnit");
            if (!node)
                continue;
            const auto attribute = [&](const char *name) { return QString::fromUtf8(node.attribute(name).value()); };
            UnitPlacement unit;
            unit.sourceStart = match.capturedStart();
            unit.sourceEnd = match.capturedEnd();
            unit.rawSource = raw;
            unit.id = attribute("Id");
            unit.name = attribute("Name");
            unit.type = attribute("UnitType");
            const QSet<QString> known{QStringLiteral("Id"), QStringLiteral("Name"),
                                      QStringLiteral("UnitType"), QStringLiteral("Position"),
                                      QStringLiteral("Rotation"), QStringLiteral("Scale"),
                                      QStringLiteral("Player")};
            QSet<QString> supported = known;
            supported.insert(QStringLiteral("Variation"));
            supported.insert(QStringLiteral("Resources"));
            unit.attributePresence = (node.attribute("Player") ? 1 : 0)
                | (node.attribute("Scale") ? 2 : 0) | (node.attribute("Variation") ? 4 : 0)
                | (node.attribute("Resources") ? 8 : 0);
            bool optionalOk = true;
            if (node.attribute("Variation")) {
                unit.variation = attribute("Variation").toInt(&optionalOk);
                optionalOk = optionalOk && unit.variation >= 0 && unit.variation <= 254;
            }
            if (node.attribute("Resources")) {
                bool ok = false; unit.resources = attribute("Resources").toInt(&ok);
                optionalOk = optionalOk && ok && unit.resources >= 0;
            }
            bool extra = !optionalOk || !validNumericTuple(attribute("Position"), {2,3})
                || (node.attribute("Rotation") && !validNumericTuple(attribute("Rotation"), {1}))
                || (node.attribute("Scale") && !validNumericTuple(attribute("Scale"), {1,3}));
            bool seenAiUsable = false;
            for (pugi::xml_node child : node.children()) {
                if (child.type() != pugi::node_element)
                    continue;
                const bool forcePlacementFlag = QString::fromUtf8(child.name()) == QStringLiteral("Flag")
                    && QString::fromUtf8(child.attribute("Index").value()) == QStringLiteral("ForcePlacement")
                    && QString::fromUtf8(child.attribute("Value").value()) == QStringLiteral("1")
                    && std::distance(child.attributes_begin(), child.attributes_end()) == 2
                    && !child.first_child();
                const QString index = QString::fromUtf8(child.attribute("Index").value());
                const bool stateFlag = QString::fromUtf8(child.name()) == QStringLiteral("Flag")
                    && QString::fromUtf8(child.attribute("Value").value()) == QStringLiteral("1")
                    && std::distance(child.attributes_begin(), child.attributes_end()) == 2 && !child.first_child();
                if (forcePlacementFlag)
                    unit.forcePlacement = true;
                else if (stateFlag && index == QStringLiteral("UnitNoCreate")) unit.stateFlags |= 1;
                else if (stateFlag && index == QStringLiteral("UnitHidden")) unit.stateFlags |= 2;
                else if (stateFlag && index == QStringLiteral("HeightOffset")) unit.stateFlags |= 4;
                else if (stateFlag && index == QStringLiteral("DeferModelLoad")) unit.stateFlags |= 8;
                else if (QString::fromUtf8(child.name()) == QStringLiteral("AIFlag")
                         && index == QStringLiteral("IsUseable")
                         && (QString::fromUtf8(child.attribute("Value").value()) == QStringLiteral("0")
                             || QString::fromUtf8(child.attribute("Value").value()) == QStringLiteral("1"))
                         && std::distance(child.attributes_begin(), child.attributes_end()) == 2 && !child.first_child()) {
                    extra = extra || seenAiUsable;
                    seenAiUsable = true;
                    if (QString::fromUtf8(child.attribute("Value").value()) == QStringLiteral("0")) unit.stateFlags |= 16;
                }
                else if (QString::fromUtf8(child.name()) == QStringLiteral("Texture")
                         && QString::fromUtf8(child.attribute("TexSlot").value()) == QStringLiteral("main")
                         && QString::fromUtf8(child.attribute("TexLink").value()).isEmpty()
                         && unit.textureProps.isEmpty() && !child.first_child()
                         && std::distance(child.attributes_begin(), child.attributes_end()) == 3
                         && QRegularExpression(QStringLiteral("^[A-Za-z0-9_,]+$")).match(QString::fromUtf8(child.attribute("TexProps").value())).hasMatch())
                    unit.textureProps = QString::fromUtf8(child.attribute("TexProps").value());
                else
                    extra = true;
            }
            QSet<QString> seenAttributes;
            for (pugi::xml_attribute item : node.attributes()) {
                const QString key = QString::fromUtf8(item.name());
                extra = extra || !supported.contains(key) || seenAttributes.contains(key)
                    || (key != QStringLiteral("Name") && QString::fromUtf8(item.value()).trimmed().isEmpty());
                seenAttributes.insert(key);
            }
            finalize(&unit, attribute("Position"), attribute("Rotation"), attribute("Scale"),
                     attribute("Player"), extra);
            if (node.attribute("BoostexCliffLevel"))
                unit.staticOnlyReason = QStringLiteral("Per-placement BoostexCliffLevel cannot be recreated by the runtime unit API.");
            if ((unit.stateFlags & (8 | 16)) != 0 && (unit.stateFlags & 1) == 0) {
                unit.dynamicCandidate = false;
                unit.staticOnlyReason = QStringLiteral("Active unit has deferred-model or AI placement options.");
            }
            unit.extendedPlacement = !node.attribute("Player") || node.attribute("Variation")
                || node.attribute("Resources") || unit.stateFlags != 0 || unit.z != 0
                || unit.scaleX != 1 || unit.scaleY != 1 || unit.scaleZ != 1;
            unit.extendedPlacement = unit.extendedPlacement || !unit.textureProps.isEmpty();
            units << unit;
        }
        return units;
    }

    const QRegularExpression expression(QStringLiteral("\\bObjectUnit\\b"), QRegularExpression::CaseInsensitiveOption);
    qsizetype searchFrom = 0;
    while (true) {
        const QRegularExpressionMatch match = expression.match(text, searchFrom);
        if (!match.hasMatch())
            break;
        const qsizetype open = text.indexOf(QLatin1Char('{'), match.capturedEnd());
        if (open < 0)
            break;
        int depth = 0;
        qsizetype close = -1;
        for (qsizetype i = open; i < text.size(); ++i) {
            if (text.at(i) == QLatin1Char('{')) ++depth;
            else if (text.at(i) == QLatin1Char('}') && --depth == 0) { close = i; break; }
        }
        if (close < 0)
            break;
        const QString block = text.mid(open + 1, close - open - 1);
        UnitPlacement unit;
        unit.sourceStart = match.capturedStart();
        unit.sourceEnd = close + 1;
        unit.rawSource = text.mid(unit.sourceStart, unit.sourceEnd - unit.sourceStart);
        unit.id = fieldValue(block, QStringLiteral("Id"));
        unit.name = fieldValue(block, QStringLiteral("Name"));
        unit.type = fieldValue(block, QStringLiteral("UnitType"));
        if (unit.type.isEmpty()) unit.type = fieldValue(block, QStringLiteral("Type"));
        if (unit.type.isEmpty()) unit.type = fieldValue(block, QStringLiteral("Unit"));
        const QSet<QString> known{QStringLiteral("id"), QStringLiteral("name"),
                                  QStringLiteral("unittype"), QStringLiteral("type"), QStringLiteral("unit"),
                                  QStringLiteral("position"), QStringLiteral("pos"),
                                  QStringLiteral("rotation"), QStringLiteral("scale"), QStringLiteral("player")};
        const int typeFieldCount = int(!fieldValue(block, QStringLiteral("UnitType")).isEmpty())
            + int(!fieldValue(block, QStringLiteral("Type")).isEmpty())
            + int(!fieldValue(block, QStringLiteral("Unit")).isEmpty());
        const bool extra = hasUnsupportedLegacyFields(block, known) || typeFieldCount > 1
            || (!fieldValue(block, QStringLiteral("Position")).isEmpty()
                && !fieldValue(block, QStringLiteral("Pos")).isEmpty());
        QString position = fieldValue(block, QStringLiteral("Position"));
        if (position.isEmpty()) position = fieldValue(block, QStringLiteral("Pos"));
        finalize(&unit, position, fieldValue(block, QStringLiteral("Rotation")),
                 fieldValue(block, QStringLiteral("Scale")), fieldValue(block, QStringLiteral("Player")), extra);
        units << unit;
        searchFrom = close + 1;
    }
    return units;
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
        && placement.hasUnsupportedChildren == after.hasUnsupportedChildren
        && placement.hasInvalidTransform == after.hasInvalidTransform;
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
        plan.zones << ZoneAssignment{zone.id, {}, {}, zone.name};
        plan.decorZones << zone;
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
            double smallestArea = std::numeric_limits<double>::infinity();
            for (int z = 0; z < zones.size(); ++z) {
                const sc2dh::region::SpatialRelation relation = relationToZone(doodad, zones.at(z));
                if (relation == sc2dh::region::SpatialRelation::Inside) {
                    const DecorZone &candidate = zones.at(z);
                    const double area = std::abs((candidate.xMax - candidate.xMin)
                                                 * (candidate.yMax - candidate.yMin));
                    if (area < smallestArea || (area == smallestArea &&
                        (assignedZoneIndex < 0 || candidate.id < zones.at(assignedZoneIndex).id))) {
                        smallestArea = area;
                        assignedZoneIndex = z;
                    }
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

    plan.units = parseUnits(objectsBytes);
    if (!plan.units.isEmpty())
        plan.warnings.removeAll(QStringLiteral("PlacedObjects XML contains no parseable ObjectDoodad elements."));
    for (int index = 0; index < plan.units.size(); ++index) {
        UnitPlacement &unit = plan.units[index];
        if (safetyContext.keepAiTemplates && (unit.stateFlags & 3) != 0 && (unit.stateFlags & 16) == 0) {
            unit.dynamicCandidate = false;
            unit.staticOnlyReason = QStringLiteral("Initially hidden placement available to AI stays native.");
        }
        if (safetyContext.keepAiBuildings && safetyContext.aiPlayerSlots.contains(unit.player)
            && safetyContext.structureUnitTypes.contains(unit.type.toCaseFolded())) {
            unit.dynamicCandidate = false;
            unit.staticOnlyReason = QStringLiteral("AI-owned building stays in native placement.");
        }
        if (safetyContext.excludedUnitTypeKeys.contains(unit.type.trimmed().toCaseFolded())) {
            unit.dynamicCandidate = false;
            unit.staticOnlyReason = QStringLiteral("Unit type excluded by user.");
        }
        for (const QString &key : unitKeys(unit)) {
            if (safetyContext.excludedUnitKeys.contains(key)) {
                unit.dynamicCandidate = false;
                unit.staticOnlyReason = QStringLiteral("Excluded by user.");
                break;
            }
        }
        if (unit.dynamicCandidate) {
            unit.safetyReferenceFiles = safetyContext.keepReferencedTypes
                ? safetyContext.referenceFilesByUnitType.value(unit.type.trimmed().toCaseFolded()) : QStringList{};
            if (!unit.safetyReferenceFiles.isEmpty()) {
                unit.safetyReferenceFiles.removeDuplicates();
                std::sort(unit.safetyReferenceFiles.begin(), unit.safetyReferenceFiles.end());
                unit.dynamicCandidate = false;
                unit.staticOnlyReason = QStringLiteral("Unit type referenced by trigger/script in %1.")
                                            .arg(unit.safetyReferenceFiles.join(QStringLiteral(", ")));
            }
        }
        if (unit.dynamicCandidate) {
            unit.safetyReferenceFiles = externalReferenceFiles(unit, safetyContext);
            if (!unit.safetyReferenceFiles.isEmpty()) {
                unit.dynamicCandidate = false;
                unit.staticOnlyReason = QStringLiteral("Unit ID/name referenced by %1.")
                                            .arg(unit.safetyReferenceFiles.join(QStringLiteral(", ")));
            }
        }
        if (!unit.dynamicCandidate) {
            plan.staticOnlyUnits << index;
            continue;
        }
        int assigned = -1;
        bool boundary = false;
        double smallestArea = std::numeric_limits<double>::infinity();
        for (int zoneIndex = 0; zoneIndex < zones.size(); ++zoneIndex) {
            const auto relation = relationToZone(unit.x, unit.y, zones.at(zoneIndex));
            if (relation == sc2dh::region::SpatialRelation::Inside) {
                const DecorZone &candidate = zones.at(zoneIndex);
                const double area = std::abs((candidate.xMax - candidate.xMin)
                                             * (candidate.yMax - candidate.yMin));
                if (area < smallestArea || (area == smallestArea &&
                    (assigned < 0 || candidate.id < zones.at(assigned).id))) {
                    smallestArea = area;
                    assigned = zoneIndex;
                }
            }
            boundary = boundary || relation == sc2dh::region::SpatialRelation::Boundary;
        }
        if (assigned >= 0)
            plan.zones[assigned].unitIndices << index;
        else if (boundary) {
            unit.dynamicCandidate = false;
            unit.staticOnlyReason = QStringLiteral("Unit lies on a Region boundary.");
            plan.boundaryUnits << index;
            plan.staticOnlyUnits << index;
        } else
            plan.unassignedUnits << index;
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
        } else if (doodad.type.startsWith(QStringLiteral("TerrainObject"), Qt::CaseInsensitive)
                   || containsAny(doodad.type + QLatin1Char(' ') + doodad.name + QLatin1Char(' ') + doodad.flags,
                                  {QStringLiteral("pathing"), QStringLiteral("blocker"), QStringLiteral("footprint"), QStringLiteral("destruct")})) {
            staticReason = QStringLiteral("Static-only: native physical objects stay visible in visibility-only mode.");
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

bool DecorationStreamingPlanner::generateRuntimeActorCatalog(const DecorationStreamingPlan &plan,
                                                             const QByteArray &originalCatalog,
                                                             QByteArray *catalog,
                                                             QString *errorMessage) const
{
    const auto fail = [&](const QString &message) {
        if (errorMessage) *errorMessage = message;
        return false;
    };
    if (!catalog)
        return fail(QStringLiteral("No output for runtime actor catalog."));
    pugi::xml_document document;
    pugi::xml_node root;
    if (originalCatalog.isEmpty()) {
        root = document.append_child("Catalog");
    } else {
        const auto parsed = document.load_buffer(originalCatalog.constData(), size_t(originalCatalog.size()),
                                                 pugi::parse_full);
        root = document.child("Catalog");
        if (!parsed || !root)
            return fail(QStringLiteral("ActorData.xml is not a valid Catalog; no output was changed."));
    }
    QStringList types;
    for (const auto &zone : plan.zones) {
        for (int index : zone.doodadIndices) {
            const QString type = plan.doodads.at(index).type;
            if (!types.contains(type)) types << type;
        }
    }
    if (types.isEmpty()) {
        *catalog = originalCatalog;
        return true;
    }
    types.sort();
    for (const auto &node : root.children()) {
        if (QString::fromUtf8(node.attribute("id").value()) == QStringLiteral("SC2DH_DecorPlacementSource"))
            return fail(QStringLiteral("Actor catalog already contains a reserved SC2DH placement profile; restore its conversion first."));
    }
    auto site = root.append_child("CActorSite");
    site.append_attribute("id") = "SC2DH_DecorPlacementSource";
    site.append_child("Host").append_attribute("Subject") = "";
    for (const auto &type : types) {
        // Preserve the original ID: inherited ##id## tokens and actor-name
        // terms must still identify the original sounds/models/events.
        auto profile = root.find_child_by_attribute("id", type.toUtf8().constData());
        if (!profile) {
            // SC2 merges this map layer with the dependency's original entry.
            profile = root.append_child("CActorDoodad");
            profile.append_attribute("id") = type.toUtf8().constData();
        }
        auto copySource = profile.child("CopySource");
        if (!copySource) copySource = profile.append_child("CopySource");
        if (!copySource.attribute("value")) copySource.append_attribute("value");
        copySource.attribute("value") = "SC2DH_DecorPlacementSource";
        for (const char *transfer : {"Position", "Rotation"}) {
            auto field = profile.find_child_by_attribute("AcceptedTransfers", "index", transfer);
            if (!field) {
                field = profile.append_child("AcceptedTransfers");
                field.append_attribute("index") = transfer;
            }
            if (!field.attribute("value")) field.append_attribute("value");
            field.attribute("value") = "1";
        }
    }
    struct Writer : pugi::xml_writer {
        QByteArray bytes;
        void write(const void *data, size_t size) override { bytes.append(static_cast<const char *>(data), qsizetype(size)); }
    } writer;
    document.save(writer, "    ", pugi::format_default, pugi::encoding_utf8);
    *catalog = writer.bytes;
    return true;
}

bool DecorationStreamingPlanner::generateRuntimeModelCatalog(const DecorationStreamingPlan &plan,
                                                              const QByteArray &actorCatalog,
                                                              const QByteArray &originalCatalog,
                                                              QByteArray *catalog, QStringList *modelIds,
                                                              QString *errorMessage) const
{
    const auto fail = [&](const QString &message) { if (errorMessage) *errorMessage = message; return false; };
    if (!catalog || !modelIds) return fail(QStringLiteral("Missing runtime model catalog output."));
    pugi::xml_document actors, models;
    if (!actorCatalog.isEmpty() && !actors.load_buffer(actorCatalog.constData(), size_t(actorCatalog.size())))
        return fail(QStringLiteral("Invalid original actor catalog."));
    auto root = models.append_child("Catalog");
    if (!originalCatalog.isEmpty()) {
        if (!models.load_buffer(originalCatalog.constData(), size_t(originalCatalog.size()), pugi::parse_full)
            || !(root = models.child("Catalog"))) return fail(QStringLiteral("Invalid original model catalog."));
    }
    QSet<QString> types;
    for (const auto &zone : plan.zones) {
        for (int index : zone.doodadIndices) {
            const auto &doodad = plan.doodads[index];
            if (doodad.variation.toInt() > 0) types.insert(doodad.type);
        }
        for (int index : zone.unitIndices) {
            const auto &unit = plan.units[index];
            if (unit.variation > 0 && (unit.stateFlags & 1) == 0) types.insert(unit.type);
        }
    }
    QSet<QString> links;
    for (const auto &type : types) {
        QString link = type, parent = type;
        QSet<QString> visited;
        while (!parent.isEmpty() && !visited.contains(parent)) {
            visited.insert(parent);
            auto actor = actors.child("Catalog").find_child_by_attribute("id", parent.toUtf8().constData());
            if (!actor) break;
            if (actor.child("Model")) { link = QString::fromUtf8(actor.child("Model").attribute("value").value()); break; }
            parent = QString::fromUtf8(actor.attribute("parent").value());
        }
        link.replace(QStringLiteral("##id##"), type).replace(QStringLiteral("##unitName##"), type);
        if (!link.isEmpty() && !link.contains(QStringLiteral("##"))) links.insert(link);
    }
    *modelIds = links.values(); modelIds->sort();
    if (modelIds->isEmpty()) { *catalog = originalCatalog; return true; }
    for (const auto &link : *modelIds) {
        auto model = root.find_child_by_attribute("id", link.toUtf8().constData());
        if (!model) { model = root.append_child("CModel"); model.append_attribute("id") = link.toUtf8().constData(); }
        // Engine's VariationFallback (bit 25) replaces a requested variant with
        // zero if its asset is not ready. Preload variants before construction,
        // and never silently substitute a different model for a saved index.
        for (const auto &flag : {qMakePair("VariationPreload", "1"), qMakePair("VariationFallback", "0")}) {
            auto field = model.find_child_by_attribute("Flags", "index", flag.first);
            if (!field) { field = model.append_child("Flags"); field.append_attribute("index") = flag.first; }
            if (!field.attribute("value")) field.append_attribute("value");
            field.attribute("value") = flag.second;
        }
    }
    struct Writer : pugi::xml_writer {
        QByteArray bytes;
        void write(const void *data, size_t size) override { bytes.append(static_cast<const char *>(data), qsizetype(size)); }
    } writer;
    models.save(writer, "    ", pugi::format_default, pugi::encoding_utf8);
    *catalog = writer.bytes;
    return true;
}

bool DecorationStreamingPlanner::generateRuntimePreload(const DecorationStreamingPlan &plan,
                                                        const QByteArray &originalPreload,
                                                        QByteArray *preload,
                                                        QString *errorMessage) const
{
    if (!preload) return false;
    pugi::xml_document document;
    auto root = document.append_child("Preload");
    if (!originalPreload.isEmpty()) {
        const auto parsed = document.load_buffer(originalPreload.constData(), size_t(originalPreload.size()), pugi::parse_full);
        root = document.child("Preload");
        if (!parsed || !root) {
            if (errorMessage) *errorMessage = QStringLiteral("Preload.xml is invalid; no output was changed.");
            return false;
        }
    }
    QMap<QString, QSet<int>> variations;
    for (const auto &zone : plan.zones) {
        for (int index : zone.doodadIndices) {
            const auto &doodad = plan.doodads.at(index);
            // Objects and Preload.xml both use native zero-based indices.
            variations[doodad.type].insert(doodad.variation.isEmpty() ? 0 : doodad.variation.toInt());
        }
        for (int index : zone.unitIndices) {
            const auto &unit = plan.units[index];
            if (unit.variation >= 0 && (unit.stateFlags & 1) == 0)
                variations[unit.type].insert(unit.variation);
        }
    }
    if (variations.isEmpty()) { *preload = originalPreload; return true; }
    for (auto it = variations.cbegin(); it != variations.cend(); ++it) {
        auto indices = it.value();
        pugi::xml_node target;
        for (auto node : root.children("Actor")) {
            if (QString::fromUtf8(node.attribute("id").value()) != it.key()) continue;
            if (!target) target = node;
            for (const auto &value : QString::fromUtf8(node.attribute("Variations").value()).split(',', Qt::SkipEmptyParts)) {
                bool valid = false;
                const int index = value.trimmed().toInt(&valid);
                if (!valid || index < 0 || index > 255) {
                    if (errorMessage) *errorMessage = QStringLiteral("Unsupported Preload.xml variation for actor %1.").arg(it.key());
                    return false;
                }
                indices.insert(index);
            }
        }
        if (!target) {
            target = root.append_child("Actor");
            target.append_attribute("id") = it.key().toUtf8().constData();
        }
        auto sorted = indices.values();
        std::sort(sorted.begin(), sorted.end());
        QStringList values;
        for (int index : sorted) values << QString::number(index);
        if (!target.attribute("Variations")) target.append_attribute("Variations");
        target.attribute("Variations") = values.join(',').toUtf8().constData();
    }
    struct Writer : pugi::xml_writer {
        QByteArray bytes;
        void write(const void *data, size_t size) override { bytes.append(static_cast<const char *>(data), qsizetype(size)); }
    } writer;
    document.save(writer, "    ", pugi::format_default, pugi::encoding_utf8);
    *preload = writer.bytes;
    return true;
}

QString DecorationStreamingPlanner::generateGalaxy(const DecorationStreamingPlan &plan,
                                                   const GalaxyGenerationOptions &options) const
{
    const QString prefix = options.functionPrefix.isEmpty() ? QStringLiteral("Game_area") : options.functionPrefix;
    int maxActors = 1;
    int maxZoneId = 1;
    for (const ZoneAssignment &zone : plan.zones) {
        maxActors = std::max(maxActors, int(zone.doodadIndices.size()));
        maxZoneId = std::max(maxZoneId, zone.zoneId);
    }

    QString out;
    out += QStringLiteral("// Generated by SC2 Data Helper Decoration Streaming.\n");
    int exportedDoodads = 0, exportedUnits = 0;
    for (const auto &zone : plan.zones) { exportedDoodads += int(zone.doodadIndices.size()); exportedUnits += int(zone.unitIndices.size()); }
    out += QStringLiteral("// SC2DH import counts %1 %2\n").arg(exportedDoodads).arg(exportedUnits);
    out += QStringLiteral("// Put this file under scripts/ and include it from the map's script coordinator.\n");
    out += QStringLiteral("// Requires the placement source and actor fields added to ActorData.xml by map conversion.\n");
    out += QStringLiteral("// Preload.xml must include the exported actor variants before map startup.\n");
    out += QStringLiteral("const int DecorOpt_ZoneCount = %1;\n").arg(maxZoneId);
    out += QStringLiteral("const int DecorOpt_MaxActorsPerZone = %1;\n").arg(maxActors);
    out += QStringLiteral("actorscope[%1] DecorOpt_Scopes;\n").arg(exportedDoodads + 1);
    out += QStringLiteral("actor[%1] DecorOpt_Actors;\n").arg(exportedDoodads + 1);
    out += QStringLiteral("point[%1] DecorOpt_Positions;\n").arg(exportedDoodads + 1);
    out += QStringLiteral("int[%1] DecorOpt_ActorCount;\n").arg(maxZoneId + 1);
    out += QStringLiteral("unit[%1] DecorOpt_Units;\n").arg(exportedUnits + 1);
    out += QStringLiteral("bool[%1] DecorOpt_UnitInitiallyHidden;\n").arg(exportedUnits + 1);
    out += QStringLiteral("int[%1] DecorOpt_UnitCount;\n").arg(maxZoneId + 1);
    out += QStringLiteral("bool[%1] DecorOpt_Loaded;\n\n").arg(maxZoneId + 1);

    // Sparse region IDs must not multiply storage by the largest region's
    // capacity. Give each region a disjoint slice of the actual placements.
    for (bool actors : {true, false}) {
        out += QStringLiteral("int DecorOpt_%1Offset(int zoneId) {\n")
                   .arg(actors ? QStringLiteral("Actor") : QStringLiteral("Unit"));
        int offset = 0;
        for (const auto &zone : plan.zones) {
            out += QStringLiteral("    if (zoneId == %1) { return %2; }\n").arg(zone.zoneId).arg(offset);
            offset += actors ? int(zone.doodadIndices.size()) : int(zone.unitIndices.size());
        }
        out += QStringLiteral("    return 0;\n}\n\n");
    }

    out += QStringLiteral("void DecorOpt_Init() {\n");
    out += QStringLiteral("    int zoneId;\n");
    out += QStringLiteral("    zoneId = 1;\n");
    out += QStringLiteral("    while (zoneId <= DecorOpt_ZoneCount) {\n");
    out += QStringLiteral("        DecorOpt_ActorCount[zoneId] = 0;\n");
    out += QStringLiteral("        DecorOpt_UnitCount[zoneId] = 0;\n");
    out += QStringLiteral("        DecorOpt_Loaded[zoneId] = false;\n");
    out += QStringLiteral("        zoneId += 1;\n");
    out += QStringLiteral("    }\n");
    out += QStringLiteral("}\n\n");

    out += QStringLiteral("void DecorOpt_ShowZone(int zoneId) {\n");
    out += QStringLiteral("    int i;\n    i = 0;\n");
    out += QStringLiteral("    while (i < DecorOpt_ActorCount[zoneId]) {\n");
    out += QStringLiteral("        ActorSend(DecorOpt_Actors[DecorOpt_ActorOffset(zoneId) + i], \"SetVisibility 1\");\n");
    out += QStringLiteral("        i += 1;\n    }\n}\n\n");
    out += QStringLiteral("void DecorOpt_ShowRegion(region target) {\n");
    out += QStringLiteral("    int zoneId;\n    int i;\n    int slot;\n    zoneId = 1;\n");
    out += QStringLiteral("    while (zoneId <= DecorOpt_ZoneCount) {\n");
    out += QStringLiteral("        i = 0;\n        while (i < DecorOpt_ActorCount[zoneId]) {\n");
    out += QStringLiteral("            slot = DecorOpt_ActorOffset(zoneId) + i;\n");
    out += QStringLiteral("            if (target == null || RegionContainsPoint(target, DecorOpt_Positions[slot])) {\n");
    out += QStringLiteral("                ActorSend(DecorOpt_Actors[slot], \"SetVisibility 1\");\n");
    out += QStringLiteral("            } else {\n                ActorSend(DecorOpt_Actors[slot], \"SetVisibility 0\");\n            }\n");
    out += QStringLiteral("            i += 1;\n        }\n        zoneId += 1;\n    }\n}\n\n");
    out += QStringLiteral("void DecorOpt_ShowAllUnits() {\n");
    out += QStringLiteral("    int zoneId;\n    int i;\n    unit u;\n    zoneId = 1;\n");
    out += QStringLiteral("    while (zoneId <= DecorOpt_ZoneCount) {\n        i = 0;\n");
    out += QStringLiteral("        while (i < DecorOpt_UnitCount[zoneId]) {\n");
    out += QStringLiteral("            u = DecorOpt_Units[DecorOpt_UnitOffset(zoneId) + i];\n");
    out += QStringLiteral("            if (u != null) { libNtve_gf_ShowHideUnit(u, !DecorOpt_UnitInitiallyHidden[DecorOpt_UnitOffset(zoneId) + i]); libNtve_gf_PauseUnit(u, false); }\n");
    out += QStringLiteral("            i += 1;\n        }\n        zoneId += 1;\n    }\n}\n\n");
    out += QStringLiteral("void DecorOpt_ClearZone(int zoneId) {\n");
    out += QStringLiteral("    int i;\n");
    out += QStringLiteral("    if (zoneId < 1 || zoneId > DecorOpt_ZoneCount) { return; }\n");
    out += QStringLiteral("    i = 0;\n");
    out += QStringLiteral("    while (i < DecorOpt_ActorCount[zoneId]) {\n");
    out += QStringLiteral("        ActorScopeKill(DecorOpt_Scopes[DecorOpt_ActorOffset(zoneId) + i]);\n");
    out += QStringLiteral("        DecorOpt_Scopes[DecorOpt_ActorOffset(zoneId) + i] = null;\n");
    out += QStringLiteral("        DecorOpt_Actors[DecorOpt_ActorOffset(zoneId) + i] = null;\n");
    out += QStringLiteral("        DecorOpt_Positions[DecorOpt_ActorOffset(zoneId) + i] = null;\n");
    out += QStringLiteral("        i += 1;\n");
    out += QStringLiteral("    }\n");
    out += QStringLiteral("    DecorOpt_ActorCount[zoneId] = 0;\n");
    out += QStringLiteral("    i = 0;\n");
    out += QStringLiteral("    while (i < DecorOpt_UnitCount[zoneId]) {\n");
    out += QStringLiteral("        UnitRemove(DecorOpt_Units[DecorOpt_UnitOffset(zoneId) + i]);\n");
    out += QStringLiteral("        DecorOpt_Units[DecorOpt_UnitOffset(zoneId) + i] = null;\n");
    out += QStringLiteral("        DecorOpt_UnitInitiallyHidden[DecorOpt_UnitOffset(zoneId) + i] = false;\n");
    out += QStringLiteral("        i += 1;\n");
    out += QStringLiteral("    }\n");
    out += QStringLiteral("    DecorOpt_UnitCount[zoneId] = 0;\n");
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

    out += QStringLiteral("void DecorOpt_SetTransform(actor target, string transform) {\n");
    out += QStringLiteral("    ActorSend(target, libNtve_gf_ActorMsg1(\"SetPosition\", StringWord(transform, 1)));\n");
    out += QStringLiteral("    ActorSend(target, libNtve_gf_ActorMsg1(\"SetRotation\", StringWord(transform, 2) + \" \" + StringWord(transform, 3)));\n");
    out += QStringLiteral("}\n\n");
    out += QStringLiteral("actor DecorOpt_CreateActor(int zoneId, string actorType, fixed scaleX, fixed scaleY, fixed scaleZ, int variation, int teamColor, string transform) {\n");
    out += QStringLiteral("    actor a;\n");
    out += QStringLiteral("    actor placement;\n");
    out += QStringLiteral("    actorscope scope;\n");
    out += QStringLiteral("    string modelLink;\n");
    out += QStringLiteral("    int modelToken;\n    int variationCount;\n");
    out += QStringLiteral("    string coordinates;\n    string rest;\n    int comma;\n    fixed x;\n    fixed y;\n    int slot;\n");
    // A doodad's fog bounds are cached during construction, not SetPosition.
    // Supply initial bearings through native CopySource before that happens.
    // Keep the original actor ID and event/token identity. CopySource only
    // resolves the positioned site inside this fresh scope; placed doodads
    // outside it still receive their native initial placement context.
    out += QStringLiteral("    scope = ActorScopeCreate(null);\n");
    out += QStringLiteral("    placement = ActorCreate(scope, \"SC2DH_DecorPlacementSource\", null, null, null);\n");
    out += QStringLiteral("    if (placement == null) { ActorScopeKill(scope); return null; }\n");
    out += QStringLiteral("    DecorOpt_SetTransform(placement, transform);\n");
    // Catalog Model can inherit the literal ##id## template. Resolve it before
    // using the value as a model catalog key or actor creation content.
    // Explicit custom model links still win over the actor's ID.
    out += QStringLiteral("    modelLink = CatalogFieldValueGet(c_gameCatalogActor, actorType, \"Model\", 0);\n");
    out += QStringLiteral("    modelToken = StringFind(modelLink, \"##id##\", true);\n");
    out += QStringLiteral("    while (modelToken > 0) {\n");
    out += QStringLiteral("        modelLink = StringReplace(modelLink, actorType, modelToken, modelToken + 5);\n");
    out += QStringLiteral("        modelToken = StringFind(modelLink, \"##id##\", true);\n");
    out += QStringLiteral("    }\n");
    out += QStringLiteral("    if (modelLink == \"\" && CatalogEntryIsValid(c_gameCatalogModel, actorType)) { modelLink = actorType; }\n");
    // Variants are declared in Preload.xml before map startup, not loaded
    // through a computed PreloadModel argument during actor construction.
    out += QStringLiteral("    variationCount = 0;\n");
    out += QStringLiteral("    if (modelLink != \"\" && CatalogEntryIsValid(c_gameCatalogModel, modelLink)) { variationCount = StringToInt(CatalogFieldValueGet(c_gameCatalogModel, modelLink, \"VariationCount\", 0)); }\n");
    out += QStringLiteral("    if (modelLink != \"\" && variation >= 0) {\n");
    out += QStringLiteral("        a = ActorCreate(scope, actorType, modelLink, IntToString(variation), null);\n");
    out += QStringLiteral("    } else {\n");
    out += QStringLiteral("        a = ActorCreate(scope, actorType, null, null, null);\n");
    out += QStringLiteral("    }\n");
    out += QStringLiteral("    if (a == null) { ActorScopeKill(scope); return null; }\n");
    // A saved nonzero variation is already evidence of a variant placement.
    // Never let a missing/empty VariationCount lookup silently discard it.
    // Leave ordinary single-model weather (variation zero) untouched.
    out += QStringLiteral("    if (modelLink != \"\" && variation >= 0 && (variation > 0 || variationCount > 1)) {\n");
    out += QStringLiteral("        ActorSend(a, libNtve_gf_ModelSwap(modelLink, variation));\n");
    out += QStringLiteral("    }\n");
    // Host site operations run after the model reads its explicit bearings.
    // Remove placement adaptation on this instance before pinning the saved
    // world transform. Do not change the original catalog's HostSiteOps: the
    // same type can also have native, trigger-referenced placements.
    // ActorCreate has no unit-style c_unitCreateIgnorePlacement flag.
    out += QStringLiteral("    // Ignore host placement adaptation; saved world XYZ and rotation are authoritative.\n");
    out += QStringLiteral("    ActorSend(a, libNtve_gf_HostSiteOpsSet(\"::Host\", \"\", 1, 1));\n");
    // CopySource supplies initial bearings for construction/fog bounds, but
    // does not establish the model's explicit position override. CActorDoodad
    // otherwise reads its scope position plus a height offset, rather than
    // the copied world position. Our script-created scope has no native
    // placed-object bearings: pin all three coordinates on the actual actor
    // after construction as well, including negative absolute heights.
    // Use the native position and rotation message contracts independently.
    // The combined SetBearings message is rejected by the running game.
    out += QStringLiteral("    DecorOpt_SetTransform(a, transform);\n");
    // Placed scale is authoritative, independent of scope/host scale.
    out += QStringLiteral("    ActorSend(a, libNtve_gf_SetScaleAbsolute(scaleX, scaleY, scaleZ, 0.0));\n");
    out += QStringLiteral("    ActorSend(a, \"SetVisibility 1\");\n");
    // The game resolves ActorCreation, event macros, model flags and animation
    // brackets against the map's complete catalog (including dependency mods).
    // EditorAnim is only a preview selector: playing it here overwrites the
    // actor's own runtime Stand block.
    // Never replay creation events either: those can spawn sounds/child actors.
    out += QStringLiteral("    // Catalog events own animations and particles.\n");
    // Objects.TeamColor is a player slot (as shown by the editor), not a
    // palette index. Honor that player's assigned color, including map/lobby
    // overrides, and use the separate diffuse and emissive palette channels.
    out += QStringLiteral("    if (teamColor >= 0) {\n");
    out += QStringLiteral("        ActorSend(a, libNtve_gf_SetTeamColor(ColorFromIndex(PlayerGetColorIndex(teamColor, false), c_teamColorDiffuse), ColorFromIndex(PlayerGetColorIndex(teamColor, false), c_teamColorEmissive)));\n");
    out += QStringLiteral("    }\n");
    // Decode only XY for visibility membership; placement still consumes the
    // original literal message, without rounding its XYZ or rotation.
    out += QStringLiteral("    coordinates = StringWord(transform, 1);\n");
    out += QStringLiteral("    comma = StringFind(coordinates, \",\", true);\n");
    out += QStringLiteral("    x = StringToFixed(StringSub(coordinates, 1, comma - 1));\n");
    out += QStringLiteral("    rest = StringSub(coordinates, comma + 1, StringLength(coordinates));\n");
    out += QStringLiteral("    comma = StringFind(rest, \",\", true);\n");
    out += QStringLiteral("    y = StringToFixed(StringSub(rest, 1, comma - 1));\n");
    out += QStringLiteral("    slot = DecorOpt_ActorOffset(zoneId) + DecorOpt_ActorCount[zoneId];\n");
    out += QStringLiteral("    DecorOpt_Scopes[slot] = scope;\n    DecorOpt_Actors[slot] = a;\n    DecorOpt_Positions[slot] = Point(x, y);\n");
    out += QStringLiteral("    DecorOpt_ActorCount[zoneId] += 1;\n");
    out += QStringLiteral("    return a;\n");
    out += QStringLiteral("}\n\n");

    out += QStringLiteral("void DecorOpt_CreateUnit(int zoneId, string unitType, int createFlags, int player, fixed x, fixed y, string orientation) {\n");
    out += QStringLiteral("    unit u;\n");
    out += QStringLiteral("    UnitCreate(1, unitType, createFlags | c_unitCreateIgnorePlacement, player, Point(x, y), StringToFixed(StringWord(orientation, 1)));\n");
    out += QStringLiteral("    u = UnitLastCreated();\n    if (u == null) { return; }\n");
    out += QStringLiteral("    DecorOpt_Units[DecorOpt_UnitOffset(zoneId) + DecorOpt_UnitCount[zoneId]] = u;\n");
    out += QStringLiteral("    DecorOpt_UnitInitiallyHidden[DecorOpt_UnitOffset(zoneId) + DecorOpt_UnitCount[zoneId]] = false;\n");
    out += QStringLiteral("    DecorOpt_UnitCount[zoneId] += 1;\n");
    out += QStringLiteral("}\n\n");

    out += QStringLiteral("void DecorOpt_CreateUnitV2(int zoneId, string unitType, int createFlags, int player, fixed x, fixed y, string properties) {\n");
    out += QStringLiteral("    unit u; actor a; string model; int states; int variation; int resources;\n");
    out += QStringLiteral("    states = StringToInt(StringWord(properties, 9));\n");
    out += QStringLiteral("    if ((states & 1) != 0) { return; }\n");
    out += QStringLiteral("    UnitCreate(1, unitType, createFlags | c_unitCreateIgnorePlacement, player, Point(x, y), StringToFixed(StringWord(properties, 1)));\n");
    out += QStringLiteral("    u = UnitLastCreated(); if (u == null) { return; }\n");
    out += QStringLiteral("    if ((states & 4) != 0 || StringToFixed(StringWord(properties, 3)) != 0.0) { UnitSetHeight(u, StringToFixed(StringWord(properties, 3)), 0.0); }\n");
    out += QStringLiteral("    UnitSetScale(u, 100.0 * StringToFixed(StringWord(properties, 4)), 100.0 * StringToFixed(StringWord(properties, 5)), 100.0 * StringToFixed(StringWord(properties, 6)));\n");
    out += QStringLiteral("    resources = StringToInt(StringWord(properties, 8)); if (resources >= 0) { UnitSetPropertyInt(u, c_unitPropResources, resources); }\n");
    out += QStringLiteral("    variation = StringToInt(StringWord(properties, 7));\n");
    out += QStringLiteral("    if (variation >= 0 && CatalogEntryIsValid(c_gameCatalogActor, unitType)) {\n");
    out += QStringLiteral("        model = CatalogFieldValueGet(c_gameCatalogActor, unitType, \"Model\", player);\n");
    out += QStringLiteral("        if (StringFind(model, \"##id##\", true) > 0) { model = StringReplace(model, unitType, StringFind(model, \"##id##\", true), StringFind(model, \"##id##\", true) + 5); }\n");
    out += QStringLiteral("        if (StringFind(model, \"##unitName##\", true) > 0) { model = StringReplace(model, unitType, StringFind(model, \"##unitName##\", true), StringFind(model, \"##unitName##\", true) + 11); }\n");
    out += QStringLiteral("        a = ActorFromScope(ActorScopeFromUnit(u), \"::Main\");\n");
    out += QStringLiteral("        if (a != null && model != \"\" && CatalogEntryIsValid(c_gameCatalogModel, model)) { ActorSend(a, libNtve_gf_ModelSwap(model, variation)); }\n");
    out += QStringLiteral("    }\n");
    out += QStringLiteral("    if (StringWord(properties, 11) != \"-\") { a = ActorFromScope(ActorScopeFromUnit(u), \"::Main\"); if (a != null) { ActorSend(a, libNtve_gf_TextureGroupApply(StringWord(properties, 11))); } }\n");
    out += QStringLiteral("    if ((states & 2) != 0) { libNtve_gf_ShowHideUnit(u, false); }\n");
    out += QStringLiteral("    DecorOpt_UnitInitiallyHidden[DecorOpt_UnitOffset(zoneId) + DecorOpt_UnitCount[zoneId]] = (states & 2) != 0;\n");
    out += QStringLiteral("    DecorOpt_Units[DecorOpt_UnitOffset(zoneId) + DecorOpt_UnitCount[zoneId]] = u; DecorOpt_UnitCount[zoneId] += 1;\n}\n\n");

    QByteArray identities;
    const auto appendActorCreation = [&](QString *source, int zoneId, const DoodadPlacement &doodad) {
        identities += placementIdentity(doodad.rawSource, "ObjectDoodad", doodad.id, doodad.name).toLatin1() + '\n';
        QString basis = actorRotation(doodad);
        basis = basis.mid(basis.indexOf(QLatin1Char('(')) + 1);
        basis.chop(1);
        const QStringList components = basis.split(QStringLiteral(", "));
        // Keep original Euler literals in the executable placement payload.
        // The first three words are the native transform; the fourth is its
        // lossless source representation, checked against the basis on import.
        const QString bearingsMessage = QStringLiteral("%1,%2,%3 %4,%5,%6 %7,%8,%9")
                       .arg(fixed(doodad.x), fixed(doodad.y), fixed(doodad.z),
                            components.at(0), components.at(1), components.at(2),
                            components.at(3), components.at(4), components.at(5))
                       + QLatin1Char(' ') + sourceAngles(doodad.rawSource,
                           {{"Rotation", doodad.rotation}, {"Pitch", doodad.pitch}, {"Roll", doodad.roll}});
        *source += QStringLiteral("    a = DecorOpt_CreateActor(%1, %2, %3, %4, %5, %6, %7, %8);\n")
                       .arg(zoneId).arg(galaxyString(doodad.type), fixed(doodad.scaleX), fixed(doodad.scaleY), fixed(doodad.scaleZ))
                       .arg(doodad.variation.trimmed().isEmpty() ? 0 : doodad.variation.toInt())
                       .arg(doodad.teamColor.trimmed().isEmpty() ? -1 : doodad.teamColor.toInt())
                       .arg(galaxyString(bearingsMessage));
        if (!doodad.tintColor.isEmpty()) {
            const ParsedTint tint = parseTintColor(doodad.tintColor);
            if (tint.valid && tint.nullColor) {
                // NULL and HDR together form one CColorHDR message argument.
                *source += QStringLiteral("    ActorSend(a, \"SetTintColor {NULL %1} {0.0}\");\n")
                               .arg(fixed(tint.hdr));
            } else if (tint.valid) {
                *source += QStringLiteral("    ActorSend(a, libNtve_gf_SetTintColor(Color(%1, %2, %3), %4, 0.0));\n")
                               .arg(fixed(tint.red), fixed(tint.green), fixed(tint.blue), fixed(tint.hdr));
            }
        }
    };

    constexpr int placementsPerHelper = 24;
    for (const ZoneAssignment &zone : plan.zones) {
        const QString stem = galaxyRegionFunctionStem(plan, zone, options);
        int helperCount = 0;
        int placementsInHelper = 0;
        const auto beginHelper = [&]() {
            out += QStringLiteral("void DecorOpt_CreatePart_%1_%2() {\n")
                       .arg(zone.zoneId).arg(helperCount);
            out += QStringLiteral("    actor a;\n");
        };
        const auto finishPlacement = [&]() {
            ++placementsInHelper;
            if (placementsInHelper == placementsPerHelper) {
                out += QStringLiteral("}\n\n");
                ++helperCount;
                placementsInHelper = 0;
            }
        };
        for (int doodadIndex : zone.doodadIndices) {
            if (placementsInHelper == 0)
                beginHelper();
            appendActorCreation(&out, zone.zoneId, plan.doodads.at(doodadIndex));
            finishPlacement();
        }
        for (int unitIndex : zone.unitIndices) {
            if (placementsInHelper == 0)
                beginHelper();
            const UnitPlacement &unit = plan.units.at(unitIndex);
            identities += placementIdentity(unit.rawSource, "ObjectUnit", unit.id, unit.name).toLatin1() + '\n';
            const QString unitProperties = unit.extendedPlacement
                ? QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8 %9 %10 %11")
                    .arg(fixed(unit.rotation * 180.0 / 3.14159265358979323846), sourceAngles(unit.rawSource, {{"Rotation", unit.rotation}}),
                         fixed(unit.z), fixed(unit.scaleX), fixed(unit.scaleY), fixed(unit.scaleZ))
                    .arg(unit.variation).arg(unit.resources).arg(unit.stateFlags).arg(unit.attributePresence)
                    .arg(unit.textureProps.isEmpty() ? QStringLiteral("-") : unit.textureProps)
                : fixed(unit.rotation * 180.0 / 3.14159265358979323846) + QLatin1Char(' ')
                    + sourceAngles(unit.rawSource, {{"Rotation", unit.rotation}});
            out += QStringLiteral("    %8(%1, %2, %3, %4, %5, %6, %7);\n")
                       .arg(zone.zoneId)
                       .arg(galaxyString(unit.type))
                       .arg(unit.forcePlacement ? QStringLiteral("c_unitCreateIgnorePlacement") : QStringLiteral("0"))
                       .arg(unit.player)
                       .arg(fixed(unit.x)).arg(fixed(unit.y))
                       .arg(galaxyString(unitProperties))
                       .arg(unit.extendedPlacement ? QStringLiteral("DecorOpt_CreateUnitV2") : QStringLiteral("DecorOpt_CreateUnit"));
            finishPlacement();
        }
        if (placementsInHelper != 0) {
            out += QStringLiteral("}\n\n");
            ++helperCount;
        }
        out += QStringLiteral("void DecorOpt_Load_%1() {\n").arg(stem);
        out += QStringLiteral("    if (DecorOpt_Loaded[%1]) { DecorOpt_ShowZone(%1); return; }\n").arg(zone.zoneId);
        for (int part = 0; part < helperCount; ++part)
            out += QStringLiteral("    DecorOpt_CreatePart_%1_%2();\n").arg(zone.zoneId).arg(part);
        out += QStringLiteral("    DecorOpt_Loaded[%1] = true;\n").arg(zone.zoneId);
        out += QStringLiteral("    DecorOpt_ShowZone(%1);\n").arg(zone.zoneId);
        out += QStringLiteral("}\n\n");
        out += QStringLiteral("void Clear_%1() { DecorOpt_ClearZone(%2); }\n\n")
                   .arg(stem).arg(zone.zoneId);
    }

    out += QStringLiteral("void DecorOpt_CreateZone(int zoneId) {\n");
    out += QStringLiteral("    if (zoneId < 1 || zoneId > DecorOpt_ZoneCount) { return; }\n");
    for (const ZoneAssignment &zone : plan.zones)
        out += QStringLiteral("    if (zoneId == %1) { DecorOpt_Load_%2(); return; }\n")
                   .arg(zone.zoneId).arg(galaxyRegionFunctionStem(plan, zone, options));
    out += QStringLiteral("}\n\n");

    out += QStringLiteral("void DecorOpt_CreateAll() {\n");
    out += QStringLiteral("    int zoneId;\n");
    out += QStringLiteral("    zoneId = 1;\n");
    out += QStringLiteral("    while (zoneId <= DecorOpt_ZoneCount) {\n");
    out += QStringLiteral("        DecorOpt_CreateZone(zoneId);\n");
    out += QStringLiteral("        zoneId += 1;\n");
    out += QStringLiteral("    }\n");
    out += QStringLiteral("}\n\n");

    out += QStringLiteral("void Create_%1_All() { DecorOpt_CreateAll(); DecorOpt_ShowRegion(null); DecorOpt_ShowAllUnits(); }\n").arg(prefix);
    out += QStringLiteral("void Clear_%1_All() { DecorOpt_ClearAll(); }\n").arg(prefix);

    out += QStringLiteral("void Create_Entire_Map() { DecorOpt_CreateAll(); DecorOpt_ShowRegion(null); DecorOpt_ShowAllUnits(); }\n\n");
    for (const auto &target : plan.zones) {
        const auto targetZone = std::find_if(plan.decorZones.cbegin(), plan.decorZones.cend(), [&](const auto &z) { return z.id == target.zoneId; });
        const QString stem = galaxyRegionFunctionStem(plan, target, options);
        out += QStringLiteral("void Create_%1() {\n").arg(stem);
        if (target.zoneId == options.entireMapEntryZoneId) {
            out += QStringLiteral("    Create_Entire_Map();\n}\n\n");
            continue;
        }
        if (targetZone == plan.decorZones.cend()) {
            out += QStringLiteral("    DecorOpt_Load_%1();\n}\n\n").arg(stem);
            continue;
        }
        for (const auto &source : plan.zones) {
            bool overlaps = source.zoneId == target.zoneId;
            for (int i : source.doodadIndices)
                overlaps = overlaps || relationToZone(plan.doodads[i].x, plan.doodads[i].y, *targetZone) != sc2dh::region::SpatialRelation::Outside;
            for (int i : source.unitIndices)
                overlaps = overlaps || relationToZone(plan.units[i].x, plan.units[i].y, *targetZone) != sc2dh::region::SpatialRelation::Outside;
            // Visibility switching must preserve cached actors and unit state.
            // Only load missing partitions here; ShowRegion hides the rest.
            if (overlaps)
                out += QStringLiteral("    DecorOpt_Load_%1();\n").arg(galaxyRegionFunctionStem(plan, source, options));
        }
        out += QStringLiteral("    DecorOpt_ShowRegion(RegionFromId(%1));\n}\n\n").arg(target.zoneId);
    }
    // Preserve maps generated with the short-lived Enter API as well.
    out += QStringLiteral("void Enter_Entire_Map() { Create_Entire_Map(); }\n");
    for (const auto &target : plan.zones) {
        const QString stem = galaxyRegionFunctionStem(plan, target, options);
        out += QStringLiteral("void Enter_%1() { Create_%1(); }\n").arg(stem);
        GalaxyGenerationOptions originalNames = options;
        originalNames.publicFunctionNames.clear();
        const QString originalStem = galaxyRegionFunctionStem(plan, target, originalNames);
        if (originalStem != stem) {
            out += QStringLiteral("void Create_%1() { Create_%2(); }\n").arg(originalStem, stem);
            out += QStringLiteral("void Clear_%1() { Clear_%2(); }\n").arg(originalStem, stem);
        }
    }

    // One bounded header block, not one comment for every executable actor.
    const QByteArray encoded = qCompress(identities, 9).toBase64();
    QString metadata = QStringLiteral("// SC2DH identities v2\n");
    for (qsizetype i = 0; i < encoded.size(); i += 200)
        metadata += QStringLiteral("// SC2DH identity-data %1\n").arg(QString::fromLatin1(encoded.mid(i, 200)));
    const qsizetype headerEnd = out.indexOf(QLatin1Char('\n')) + 1;
    out.insert(headerEnd, metadata);
    return out;
}

bool DecorationStreamingPlanner::restoreObjectsFromGalaxy(const QByteArray &objects, const QString &galaxy,
                                                           QByteArray *restored, int *doodads, int *units,
                                                           QString *error) const
{
    const auto fail = [&](const QString &why) { if (error) *error = why; return false; };
    if (!restored || !galaxy.startsWith(QStringLiteral("// Generated by SC2 Data Helper Decoration Streaming.")))
        return fail(QStringLiteral("Only SC2DataHelper recreation scripts can be imported."));
    pugi::xml_document doc;
    if (!doc.load_buffer(objects.constData(), size_t(objects.size()), pugi::parse_full))
        return fail(QStringLiteral("Objects XML is invalid."));
    auto root = doc.document_element();
    QSet<QString> ids;
    for (auto node : root.children()) ids.insert(QString::fromUtf8(node.attribute("Id").value()));
    pugi::xml_document identity;
    pugi::xml_node pending;
    const bool compactIdentities = galaxy.contains(QStringLiteral("// SC2DH identities v2\n"));
    QList<QByteArray> identityList;
    qsizetype identityIndex = 0;
    if (compactIdentities) {
        QByteArray encoded;
        const QRegularExpression part(QStringLiteral("^// SC2DH identity-data ([A-Za-z0-9+/=]+)$"), QRegularExpression::MultilineOption);
        auto matches = part.globalMatch(galaxy);
        while (matches.hasNext()) encoded += matches.next().captured(1).toLatin1();
        const QByteArray compressed = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
        if (compressed.size() < 4) return fail(QStringLiteral("Missing compact placement identities."));
        const quint32 size = (quint32(quint8(compressed[0])) << 24) | (quint32(quint8(compressed[1])) << 16)
                           | (quint32(quint8(compressed[2])) << 8) | quint32(quint8(compressed[3]));
        if (size > 16 * 1024 * 1024) return fail(QStringLiteral("Placement identity block is too large."));
        const QByteArray decoded = qUncompress(compressed);
        if (quint32(decoded.size()) != size) return fail(QStringLiteral("Invalid compressed placement identities."));
        identityList = decoded.split('\n');
        if (!identityList.isEmpty() && identityList.back().isEmpty()) identityList.removeLast();
    }
    bool inPart = false, created = false, bearings = false;
    int countDoodads = 0, countUnits = 0;
    auto attr = [](pugi::xml_node node, const char *key, const QString &value) {
        auto field = node.attribute(key);
        if (!field) field = node.append_attribute(key);
        field = value.toUtf8().constData();
    };
    auto tuple = [](const QVector<double> &v, int start, int count) {
        QStringList parts;
        for (int i = start; i < start + count; ++i) parts << QString::number(v.at(i), 'g', 16);
        return parts.join(QLatin1Char(','));
    };
    auto numeric = [](const QString &value, int count, QVector<double> *v) {
        static const QRegularExpression number(QStringLiteral("^[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?$"));
        const auto parts = value.split(QLatin1Char(','));
        if (parts.size() != count) return false;
        v->clear();
        for (const auto &part : parts) {
            bool ok = false;
            const double x = part.trimmed().toDouble(&ok);
            if (!ok || !std::isfinite(x) || !number.match(part.trimmed()).hasMatch()) return false;
            v->append(x);
        }
        return true;
    };
    auto angles = [&](pugi::xml_node node, const QVector<double> &v, int offset) {
        // Compatibility path for older exports without original Euler literals.
        double fx = v.at(offset), fy = v.at(offset+1), fz = v.at(offset+2);
        double ux = v.at(offset+3), uy = v.at(offset+4), uz = v.at(offset+5);
        const double fn = std::sqrt(fx*fx+fy*fy+fz*fz), un = std::sqrt(ux*ux+uy*uy+uz*uz);
        if (fn < 1e-9 || un < 1e-9 || std::abs((fx*ux+fy*uy+fz*uz)/(fn*un)) > 1e-5) return false;
        fx /= fn; fy /= fn; fz /= fn; ux /= un; uy /= un; uz /= un;
        const double xx = -fy*uz+fz*uy, xy = -fz*ux+fx*uz, xz = -fx*uy+fy*ux;
        const double pitch = std::asin(std::clamp(-fz, -1.0, 1.0));
        const bool singular = std::hypot(fx,fy) < 1e-7;
        const double yaw = singular ? std::atan2(xy,xx) : std::atan2(fx,-fy);
        const double roll = singular ? 0.0 : std::atan2(-xz,uz);
        constexpr double tau = 6.28318530717958647692;
        for (const auto &field : {qMakePair("Rotation",yaw), qMakePair("Pitch",pitch), qMakePair("Roll",roll)})
            attr(node,field.first,QString::number(std::fmod(field.second+tau,tau),'g',16));
        return true;
    };
    auto angleLiteral = [&](const QString &literal, double *value) {
        if (literal == QStringLiteral("~")) { *value = 0.0; return true; }
        QVector<double> parsed;
        if (!numeric(literal, 1, &parsed)) return false;
        *value = parsed.front();
        return true;
    };
    auto setAngleLiteral = [&](pugi::xml_node node, const char *field, const QString &literal) {
        if (literal == QStringLiteral("~")) node.remove_attribute(field);
        else attr(node, field, literal);
    };
    auto exactAngles = [&](pugi::xml_node node, const QString &literal, const QVector<double> &world) {
        const QStringList parts = literal.split(QLatin1Char(','));
        DoodadPlacement placement;
        if (parts.size() != 3 || !angleLiteral(parts[0], &placement.rotation)
            || !angleLiteral(parts[1], &placement.pitch) || !angleLiteral(parts[2], &placement.roll)) return false;
        QString basis = actorRotation(placement);
        basis = basis.mid(basis.indexOf(QLatin1Char('(')) + 1);
        basis.chop(1);
        QVector<double> expected;
        if (!numeric(basis, 6, &expected)) return false;
        for (int i = 0; i < 6; ++i)
            if (std::abs(expected[i] - world[i + 3]) > 2e-15) return false;
        const char *fields[] = {"Rotation", "Pitch", "Roll"};
        for (int i = 0; i < 3; ++i) setAngleLiteral(node, fields[i], parts[i]);
        return true;
    };
    auto finish = [&]() {
        if (!pending) return true;
        if (!created || (QString::fromUtf8(pending.name()) == QStringLiteral("ObjectDoodad") && !bearings)) return false;
        root.append_copy(pending);
        pending = {}; identity.reset(); created = false; bearings = false;
        return true;
    };
    auto loadIdentity = [&](const QByteArray &encoded) {
        const auto bytes = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
        if (bytes.isEmpty() || !identity.load_buffer(bytes.constData(), size_t(bytes.size()))) return false;
        pending = identity.document_element();
        const QString kind = QString::fromUtf8(pending.name());
        const QString id = QString::fromUtf8(pending.attribute("Id").value());
        bool ok = false;
        const auto value = id.toULongLong(&ok);
        if ((kind != QStringLiteral("ObjectDoodad") && kind != QStringLiteral("ObjectUnit")) || !ok || !value || ids.contains(id)) return false;
        for (const char *key : {"Type","UnitType","Position","Rotation","Pitch","Roll","Scale","Variation","Player","TeamColor","TintColor"})
            if (pending.attribute(key)) return false;
        ids.insert(id);
        return true;
    };
    const QRegularExpression actorCall(QStringLiteral(R"re(^a = DecorOpt_CreateActor\((\d+), "((?:\\.|[^"\\])*)", (.*), libNtve_gf_SetRotation\((.*)\)\);$)re"));
    const QRegularExpression compactActorCall(QStringLiteral(R"re(^a = DecorOpt_CreateActor\((\d+), "((?:\\.|[^"\\])*)", (.*), (?:"SetBearings \{'([^']+)'\}"|"([^"{}']+)")\);$)re"));
    const QRegularExpression unitCall(QStringLiteral(R"re(^DecorOpt_CreateUnit\((\d+), "((?:\\.|[^"\\])*)", (0|c_unitCreateIgnorePlacement), (.*)\);$)re"));
    const QRegularExpression bearingCall(QStringLiteral(R"re(^ActorSend\(a, "SetBearings \{'([^']+)'\}"\);$)re"));
    const QRegularExpression tintNull(QStringLiteral(R"re(^ActorSend\(a, "SetTintColor \{NULL ([^ ]+)\} \{0\.0\}"\);$)re"));
    const QRegularExpression tintNullLegacy(QStringLiteral(R"re(^ActorSend\(a, "SetTintColor NULL ([^ ]+) 0\.0"\);$)re"));
    const QRegularExpression tintColor(QStringLiteral(R"re(^ActorSend\(a, libNtve_gf_SetTintColor\(Color\((.*)\), ([^,]+), 0\.0\)\);$)re"));
    const QRegularExpression unitV2Call(QStringLiteral(R"re(^DecorOpt_CreateUnitV2\((\d+), "((?:\\.|[^"\\])*)", (0|c_unitCreateIgnorePlacement), ([^,]+), ([^,]+), ([^,]+), "([^"\\]+)"\);$)re"));
    for (const QString &rawLine : galaxy.split(QLatin1Char('\n'))) {
        const QString line = rawLine.trimmed();
        if (line.startsWith(QStringLiteral("void DecorOpt_CreatePart_"))) {
            if (inPart || pending) return fail(QStringLiteral("Malformed placement function."));
            inPart = true; continue;
        }
        if (!inPart) continue;
        if (line == QStringLiteral("}")) {
            if (!finish()) return fail(QStringLiteral("Incomplete placement: missing creation or absolute bearings."));
            inPart = false; continue;
        }
        if (line.isEmpty() || line == QStringLiteral("actor a;")) continue;
        if (line.startsWith(QStringLiteral("// SC2DH identity "))) {
            if (compactIdentities) return fail(QStringLiteral("Mixed placement identity formats."));
            if (!finish()) return fail(QStringLiteral("Incomplete placement before next identity."));
            if (!loadIdentity(line.mid(18).toLatin1())) return fail(QStringLiteral("Invalid or duplicate script placement identity."));
            continue;
        }
        if (line.startsWith(QStringLiteral("//"))) continue;
        if (compactIdentities && (line.startsWith(QStringLiteral("a = DecorOpt_CreateActor(")) || line.startsWith(QStringLiteral("DecorOpt_CreateUnit("))
                                 || line.startsWith(QStringLiteral("DecorOpt_CreateUnitV2(")))) {
            if (!finish() || identityIndex >= identityList.size() || !loadIdentity(identityList.at(identityIndex++)))
                return fail(QStringLiteral("Missing, invalid or duplicate compact placement identity."));
        }
        if (!pending) return fail(QStringLiteral("Placement has no script identity; re-export it with this version."));
        auto match = compactActorCall.match(line);
        if (match.hasMatch()) {
            QVector<double> args, world;
            QString values = match.captured(4).isEmpty() ? match.captured(5) : match.captured(4);
            const QStringList words = values.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (words.size() != 3 && words.size() != 4) return fail(QStringLiteral("Invalid compact doodad transform."));
            values = words.mid(0,3).join(QLatin1Char(','));
            if (created || QString::fromUtf8(pending.name()) != QStringLiteral("ObjectDoodad")
                || !numeric(match.captured(3),5,&args) || !numeric(values,9,&world)
                || args[3] < 0 || args[3] != std::floor(args[3]) || args[4] != std::floor(args[4])
                || !(words.size() == 4 ? exactAngles(pending,words[3],world) : angles(pending,world,3)))
                return fail(QStringLiteral("Invalid compact doodad call or inconsistent exact angles."));
            QString type = match.captured(2); type.replace(QStringLiteral("\\\""),QStringLiteral("\"")); type.replace(QStringLiteral("\\\\"),QStringLiteral("\\"));
            attr(pending,"Type",type); attr(pending,"Position",tuple(world,0,3)); attr(pending,"Scale",tuple(args,0,3));
            attr(pending,"Variation",QString::number(int(args[3])));
            if (args[4] >= 0) attr(pending,"TeamColor",QString::number(int(args[4])));
            created = bearings = true; ++countDoodads; continue;
        }
        match = actorCall.match(line);
        if (match.hasMatch()) {
            QVector<double> args, basis;
            if (created || QString::fromUtf8(pending.name()) != QStringLiteral("ObjectDoodad")
                || !numeric(match.captured(3),8,&args) || !numeric(match.captured(4),6,&basis)
                || args[6] < 0 || args[6] != std::floor(args[6]) || args[7] != std::floor(args[7])
                || !angles(pending,basis,0)) return fail(QStringLiteral("Invalid doodad call."));
            QString type = match.captured(2); type.replace(QStringLiteral("\\\""),QStringLiteral("\"")); type.replace(QStringLiteral("\\\\"),QStringLiteral("\\"));
            attr(pending,"Type",type); attr(pending,"Position",tuple(args,0,3)); attr(pending,"Scale",tuple(args,3,3));
            attr(pending,"Variation",QString::number(int(args[6])));
            if (args[7] >= 0) attr(pending,"TeamColor",QString::number(int(args[7])));
            created = true; ++countDoodads; continue;
        }
        match = unitV2Call.match(line);
        if (match.hasMatch()) {
            const auto words = match.captured(7).split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (created || QString::fromUtf8(pending.name()) != QStringLiteral("ObjectUnit") || words.size() != 11)
                return fail(QStringLiteral("Invalid extended unit placement."));
            QVector<double> xy, properties;
            double radians = 0;
            if (!numeric(match.captured(4) + ',' + match.captured(5) + ',' + match.captured(6), 3, &xy)
                || !numeric(words[0] + ',' + words.mid(2,8).join(','), 9, &properties)
                || !angleLiteral(words[1], &radians) || xy[0] < 0 || xy[0] > 15 || xy[0] != std::floor(xy[0])
                || properties[0] != fixed(radians * 180.0 / 3.14159265358979323846).toDouble())
                return fail(QStringLiteral("Invalid extended unit values."));
            if (properties[5] < -1 || properties[5] > 254 || properties[6] < -1 || properties[6] > std::numeric_limits<int>::max()
                || properties[7] < 0 || properties[7] > 31 || properties[8] < 0 || properties[8] > 15
                || properties[2] <= 0 || properties[3] <= 0 || properties[4] <= 0)
                return fail(QStringLiteral("Extended unit options exceed supported ranges."));
            const int variation = int(properties[5]), resources = int(properties[6]);
            const int states = int(properties[7]), presence = int(properties[8]);
            if (variation < -1 || variation > 254 || resources < -1 || states < 0 || states > 31 || presence < 0 || presence > 15
                || properties[5] != variation || properties[6] != resources || properties[7] != states || properties[8] != presence)
                return fail(QStringLiteral("Invalid extended unit options."));
            attr(pending, "UnitType", match.captured(2));
            if ((presence & 1) != 0) attr(pending, "Player", QString::number(int(xy[0])));
            else if (xy[0] != 0) return fail(QStringLiteral("An omitted player must be neutral."));
            attr(pending, "Position", tuple(xy, 1, 2) + ',' + QString::number(properties[1], 'g', 16));
            setAngleLiteral(pending, "Rotation", words[1]);
            if ((presence & 2) != 0) attr(pending, "Scale", tuple(properties, 2, 3));
            if ((presence & 4) != 0) attr(pending, "Variation", QString::number(variation));
            if ((presence & 8) != 0) attr(pending, "Resources", QString::number(resources));
            else pending.remove_attribute("Resources");
            auto texture = pending.child("Texture");
            if (words[10] != QStringLiteral("-")) {
                if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_,]+$")).match(words[10]).hasMatch())
                    return fail(QStringLiteral("Invalid unit texture group."));
                if (!texture) texture = pending.append_child("Texture");
                attr(texture, "TexSlot", QStringLiteral("main")); attr(texture, "TexLink", QString());
                attr(texture, "TexProps", words[10]);
            } else if (texture) pending.remove_child(texture);
            created = true; ++countUnits; continue;
        }
        match = unitCall.match(line);
        if (match.hasMatch()) {
            QVector<double> args;
            QString unitArgs = match.captured(4);
            QString exactRotation;
            static const QRegularExpression exactUnit(QStringLiteral(R"re(^(.*), "([^" ]+) ([^" ]+)"$)re"));
            const auto orientation = exactUnit.match(unitArgs);
            if (orientation.hasMatch()) {
                double radians = 0.0;
                QVector<double> degrees;
                exactRotation = orientation.captured(3);
                if (!angleLiteral(exactRotation, &radians) || !numeric(orientation.captured(2),1,&degrees)
                    || !fitsGalaxyFixed(radians * 180.0 / 3.14159265358979323846)
                    || degrees[0] != fixed(radians * 180.0 / 3.14159265358979323846).toDouble())
                    return fail(QStringLiteral("Invalid unit orientation or inconsistent exact angle."));
                unitArgs = orientation.captured(1) + QLatin1Char(',') + orientation.captured(2);
            }
            if (created || QString::fromUtf8(pending.name()) != QStringLiteral("ObjectUnit") || !numeric(unitArgs,4,&args)
                || args[0] < 0 || args[0] != std::floor(args[0])) return fail(QStringLiteral("Invalid unit call."));
            attr(pending,"UnitType",match.captured(2)); attr(pending,"Player",QString::number(int(args[0])));
            attr(pending,"Position",tuple(args,1,2)+QStringLiteral(",0"));
            if (orientation.hasMatch()) setAngleLiteral(pending,"Rotation",exactRotation);
            else attr(pending,"Rotation",QString::number(args[3]*3.14159265358979323846/180.0,'g',16));
            auto flag = pending.find_child_by_attribute("Flag","Index","ForcePlacement");
            if (match.captured(3) == QStringLiteral("c_unitCreateIgnorePlacement")) {
                if (!flag) { flag = pending.append_child("Flag"); flag.append_attribute("Index") = "ForcePlacement"; }
                attr(flag,"Value",QStringLiteral("1"));
            } else if (flag) pending.remove_child(flag);
            created = true; ++countUnits; continue;
        }
        match = bearingCall.match(line);
        if (match.hasMatch()) {
            QString values = match.captured(1); values.replace(QLatin1Char(' '),QLatin1Char(','));
            QVector<double> v;
            if (!created || bearings || QString::fromUtf8(pending.name()) != QStringLiteral("ObjectDoodad")
                || !numeric(values,9,&v) || !angles(pending,v,3)) return fail(QStringLiteral("Invalid absolute-world bearings."));
            attr(pending,"Position",tuple(v,0,3)); bearings = true; continue;
        }
        match = tintNull.match(line);
        if (!match.hasMatch()) match = tintNullLegacy.match(line);
        if (match.hasMatch()) {
            QVector<double> v;
            if (!created || !bearings || !numeric(match.captured(1),1,&v) || v[0] < 0) return fail(QStringLiteral("Invalid tint."));
            attr(pending,"TintColor",QStringLiteral("NULL ")+tuple(v,0,1)); continue;
        }
        match = tintColor.match(line);
        if (match.hasMatch()) {
            QVector<double> v;
            if (!created || !bearings || !numeric(match.captured(1)+QLatin1Char(',')+match.captured(2),4,&v)
                || v[3] < 0) return fail(QStringLiteral("Invalid tint."));
            for (int i=0;i<3;++i) { if (v[i]<0 || v[i]>100) return fail(QStringLiteral("Invalid tint component.")); v[i] = std::round(v[i]*2.55); }
            attr(pending,"TintColor",tuple(v,0,3)+QLatin1Char(' ')+tuple(v,3,1)); continue;
        }
        return fail(QStringLiteral("Unsupported placement statement: %1").arg(line));
    }
    if (inPart || pending || countDoodads+countUnits == 0) return fail(QStringLiteral("Incomplete or empty Galaxy placement export."));
    if (compactIdentities && identityIndex != identityList.size()) return fail(QStringLiteral("Unused placement identities; incomplete Galaxy export."));
    const QRegularExpression totals(QStringLiteral("^// SC2DH import counts ([0-9]+) ([0-9]+)$"),QRegularExpression::MultilineOption);
    const auto total = totals.match(galaxy);
    if (!total.hasMatch() || total.captured(1).toInt() != countDoodads || total.captured(2).toInt() != countUnits)
        return fail(QStringLiteral("Galaxy placement export is truncated or its placement totals changed."));
    struct Writer : pugi::xml_writer {
        QByteArray bytes;
        void write(const void *data, size_t size) override { bytes.append(static_cast<const char *>(data), qsizetype(size)); }
    } writer;
    doc.save(writer,"    ",pugi::format_default,pugi::encoding_utf8);
    *restored = writer.bytes;
    if (doodads) *doodads = countDoodads;
    if (units) *units = countUnits;
    return true;
}

QString DecorationStreamingPlanner::staticPlacementGalaxy(const QString &galaxy) const
{
    QString result = QStringLiteral("// Static placements imported from SC2DataHelper Galaxy. Runtime creation disabled.\n");
    const QRegularExpression declaration(QStringLiteral("^(void|bool) ([A-Za-z_][A-Za-z0-9_]*)\\(([^)]*)\\) \\{"),QRegularExpression::MultilineOption);
    auto matches = declaration.globalMatch(galaxy);
    while (matches.hasNext()) {
        const auto m = matches.next();
        const QString name = m.captured(2);
        if (name.startsWith(QStringLiteral("DecorOpt_CreatePart_")) || name == QStringLiteral("DecorOpt_CreateUnit")) continue;
        result += QStringLiteral("%1 %2(%3) { %4 }\n").arg(m.captured(1),name,m.captured(3),
                    m.captured(1) == QStringLiteral("bool") ? QStringLiteral("return true;") : QString());
    }
    return result;
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
        QStringLiteral("libNtve_gf_CreateActorAtPoint")
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
                                                                       QStringList *warnings,
                                                                       QVector<int> *removedUnitIndices) const
{
    if (removedDoodadIndices)
        removedDoodadIndices->clear();
    if (removedUnitIndices)
        removedUnitIndices->clear();
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
        int index = -1;
        bool unit = false;
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
        ranges << Range{doodad.sourceStart, doodad.sourceEnd, doodadIndex, false};
    }

    for (const ZoneAssignment &zone : plan.zones) {
        for (int unitIndex : zone.unitIndices) {
            if (unitIndex < 0 || unitIndex >= plan.units.size()) {
                if (warnings)
                    *warnings << QStringLiteral("Invalid unit index %1.").arg(unitIndex);
                continue;
            }
            const UnitPlacement &unit = plan.units.at(unitIndex);
            if (!unit.dynamicCandidate || unit.sourceStart < 0 || unit.sourceEnd <= unit.sourceStart
                || unit.sourceEnd > text.size()) {
                if (warnings)
                    *warnings << QStringLiteral("Refused to remove unsafe unit %1.").arg(unit.id);
                continue;
            }
            ranges << Range{unit.sourceStart, unit.sourceEnd, unitIndex, true};
        }
    }

    std::sort(ranges.begin(), ranges.end(), [](const Range &left, const Range &right) {
        return left.start < right.start;
    });
    qsizetype previousEnd = -1;
    QVector<Range> safeRanges;
    for (Range range : ranges) {
        if (range.start < previousEnd) {
            if (warnings)
            *warnings << QStringLiteral("Refused to remove overlapping placement at offset %1.").arg(range.start);
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
        if (range.unit) {
            if (removedUnitIndices)
                removedUnitIndices->prepend(range.index);
        } else if (removedDoodadIndices)
            removedDoodadIndices->prepend(range.index);
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
    if (options.entireMapEntryZoneId > 0) {
        auto fallback = std::find_if(artifacts.plan.zones.begin(), artifacts.plan.zones.end(),
            [&](const auto &zone) { return zone.zoneId == options.entireMapEntryZoneId; });
        if (fallback != artifacts.plan.zones.end()) {
            fallback->doodadIndices += artifacts.plan.unassignedDoodads;
            fallback->unitIndices += artifacts.plan.unassignedUnits;
            artifacts.plan.unassignedDoodads.clear();
            artifacts.plan.unassignedUnits.clear();
        }
    }
    const QString prefix = options.functionPrefix.isEmpty() ? QStringLiteral("Game_area") : options.functionPrefix;
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
                                                                      &removalWarnings,
                                                                      &artifacts.removedUnitIndices);
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
    const QSet<int> removedUnits(artifacts.removedUnitIndices.cbegin(), artifacts.removedUnitIndices.cend());
    for (int index = 0; index < artifacts.plan.units.size(); ++index) {
        if (removedUnits.contains(index))
            continue;
        const QByteArray raw = artifacts.plan.units.at(index).rawSource.toUtf8();
        if (raw.isEmpty() || !artifacts.optimizedObjectsBytes.contains(raw)) {
            artifacts.outsideScopePreserved = false;
            artifacts.warnings << QStringLiteral("Preservation check failed for unit index %1.").arg(index);
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
    if (patch.artifacts.removedDoodadIndices.isEmpty() && patch.artifacts.removedUnitIndices.isEmpty()) {
        patch.error = QStringLiteral("No safe doodads or units were assigned to selected Regions.");
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
