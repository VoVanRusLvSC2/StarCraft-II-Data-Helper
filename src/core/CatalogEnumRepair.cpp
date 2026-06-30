#include "core/CatalogEnumRepair.h"

#include <pugixml.hpp>

#include <sstream>

namespace
{

QString nodeName(const pugi::xml_node &node)
{
    return QString::fromUtf8(node.name()).toLower();
}

bool isSoundNode(const pugi::xml_node &node)
{
    return nodeName(node).startsWith(QStringLiteral("csound"));
}

bool isFilterField(const QString &field)
{
    const QString lower = field.toLower();
    return lower.contains(QStringLiteral("targetfilters"))
        || lower.contains(QStringLiteral("searchfilters"))
        || lower.contains(QStringLiteral("autocastfilters"));
}

bool isVitalIndexNode(const pugi::xml_node &node)
{
    const QString name = nodeName(node);
    return name.contains(QStringLiteral("statuscolors"))
        || name.contains(QStringLiteral("vitalcolors"))
        || name.contains(QStringLiteral("vitalnames"))
        || name.contains(QStringLiteral("vitalmax"))
        || name.contains(QStringLiteral("vitalregen"));
}

bool isNegativeInteger(const QString &value)
{
    bool ok = false;
    const int parsed = value.toInt(&ok);
    return ok && parsed < 0;
}

bool replaceAll(QString *value, const QString &from, const QString &to)
{
    if (!value || !value->contains(from))
        return false;
    value->replace(from, to);
    return true;
}

int repairNode(pugi::xml_node node)
{
    int changes = 0;
    if (node.type() == pugi::node_element) {
        const QString currentName = nodeName(node);
        for (pugi::xml_attribute attribute : node.attributes()) {
            const QString field = QString::fromUtf8(attribute.name());
            QString value = QString::fromUtf8(attribute.value());
            const QString before = value;

            if (isFilterField(field) || isFilterField(currentName)) {
                replaceAll(&value, QStringLiteral("Marine2@Behavior10"), QStringLiteral("Dead"));
            } else if (field.compare(QStringLiteral("Send"), Qt::CaseInsensitive) == 0
                       || field.compare(QStringLiteral("Terms"), Qt::CaseInsensitive) == 0) {
                replaceAll(&value, QStringLiteral("Marine2@Behavior4"), QStringLiteral("Death"));
            } else if (field.compare(QStringLiteral("Anim"), Qt::CaseInsensitive) == 0
                       || currentName == QStringLiteral("anim")) {
                replaceAll(&value, QStringLiteral("Marine2@Behavior4"), QStringLiteral("Death"));
            } else if (field.compare(QStringLiteral("parent"), Qt::CaseInsensitive) == 0
                       && isSoundNode(node)
                       && value.compare(QStringLiteral("Marine2@Behavior4"), Qt::CaseInsensitive) == 0) {
                value = QStringLiteral("Death");
            } else if (field.compare(QStringLiteral("index"), Qt::CaseInsensitive) == 0
                       && isVitalIndexNode(node)
                       && value.compare(QStringLiteral("Marine2@Requirement4"), Qt::CaseInsensitive) == 0) {
                value = QStringLiteral("Shields");
            } else if (field.compare(QStringLiteral("Reference"), Qt::CaseInsensitive) == 0) {
                replaceAll(&value, QStringLiteral("[Marine2@Requirement4]"), QStringLiteral("[Shields]"));
            } else if (field.compare(QStringLiteral("value"), Qt::CaseInsensitive) == 0
                       && currentName == QStringLiteral("alliedpushpriority")
                       && isNegativeInteger(value)) {
                value = QStringLiteral("0");
            }

            if (value != before) {
                attribute.set_value(value.toUtf8().constData());
                ++changes;
            }
        }
    }

    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling())
        changes += repairNode(child);
    return changes;
}

QByteArray saveXml(const pugi::xml_document &document)
{
    std::ostringstream stream;
    document.save(stream, "    ", pugi::format_default, pugi::encoding_utf8);
    return QByteArray::fromStdString(stream.str());
}

} // namespace

namespace sc2dh
{

bool repairKnownCatalogEnumDamage(QByteArray *xmlBytes, int *changes, QString *errorMessage)
{
    if (changes)
        *changes = 0;
    if (!xmlBytes || xmlBytes->isEmpty())
        return true;

    pugi::xml_document document;
    const pugi::xml_parse_result parsed = document.load_buffer(xmlBytes->constData(), size_t(xmlBytes->size()));
    if (!parsed) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Unable to parse XML for catalog enum repair: %1").arg(QString::fromUtf8(parsed.description()));
        return false;
    }

    const int repaired = repairNode(document);
    if (repaired <= 0)
        return true;

    *xmlBytes = saveXml(document);
    if (changes)
        *changes = repaired;
    return true;
}

} // namespace sc2dh
