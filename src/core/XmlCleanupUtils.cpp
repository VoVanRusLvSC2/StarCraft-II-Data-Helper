#include "core/XmlCleanupUtils.h"

#include <QStringList>

#include <algorithm>

namespace
{
pugi::xml_node firstElementChild(const pugi::xml_node &node)
{
    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling())
        if (child.type() == pugi::node_element)
            return child;
    return {};
}

bool isLenientEditorAttribute(const QString &name)
{
    const QString lower = name.toLower();
    return lower.contains(QStringLiteral("editor"))
        || lower == QStringLiteral("name")
        || lower == QStringLiteral("text")
        || lower == QStringLiteral("tooltip")
        || lower == QStringLiteral("description")
        || lower == QStringLiteral("icon")
        || lower == QStringLiteral("sort")
        || lower == QStringLiteral("category");
}

bool isLenientEditorNode(const QString &name)
{
    const QString lower = name.toLower();
    return lower.contains(QStringLiteral("editor"))
        || lower == QStringLiteral("name")
        || lower == QStringLiteral("text")
        || lower == QStringLiteral("tooltip")
        || lower == QStringLiteral("description")
        || lower == QStringLiteral("icon")
        || lower == QStringLiteral("editorcategories")
        || lower == QStringLiteral("editordescription");
}
}

namespace sc2dh::xmlcleanup
{
QString locationSegmentForNode(const pugi::xml_node &node)
{
    int index = 1;
    for (pugi::xml_node sibling = node.previous_sibling(node.name()); sibling; sibling = sibling.previous_sibling(node.name()))
        if (sibling.type() == pugi::node_element)
            ++index;
    return QStringLiteral("%1[%2]").arg(QString::fromUtf8(node.name())).arg(index);
}

QString canonicalNode(const pugi::xml_node &node, bool lenient, bool objectRoot)
{
    if (node.type() == pugi::node_pcdata || node.type() == pugi::node_cdata) {
        const QString text = QString::fromUtf8(node.value()).trimmed();
        return text.isEmpty() ? QString() : QStringLiteral("T%1:%2").arg(text.size()).arg(text);
    }
    if (node.type() != pugi::node_element)
        return {};

    const QString nodeName = QString::fromUtf8(node.name());
    if (lenient && !objectRoot && isLenientEditorNode(nodeName))
        return {};

    QStringList attributes;
    for (pugi::xml_attribute attribute : node.attributes()) {
        const QString name = QString::fromUtf8(attribute.name());
        if ((objectRoot && (name.compare(QStringLiteral("id"), Qt::CaseInsensitive) == 0
                            || name.compare(QStringLiteral("name"), Qt::CaseInsensitive) == 0))
            || (lenient && isLenientEditorAttribute(name))) {
            continue;
        }
        const QString value = QString::fromUtf8(attribute.value());
        attributes << QStringLiteral("A%1:%2=%3:%4").arg(name.size()).arg(name).arg(value.size()).arg(value);
    }
    std::sort(attributes.begin(), attributes.end());

    QStringList children;
    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling()) {
        const QString childKey = canonicalNode(child, lenient, false);
        if (!childKey.isEmpty())
            children << childKey;
    }
    return QStringLiteral("E%1:%2{%3}[%4]/E")
        .arg(nodeName.size())
        .arg(nodeName, attributes.join(QLatin1Char('|')), children.join(QLatin1Char('|')));
}

bool loadSerializedRoot(const QString &serializedXml, pugi::xml_document *document, pugi::xml_node *root)
{
    if (!document || !root)
        return false;
    const QByteArray bytes = serializedXml.toUtf8();
    if (!document->load_buffer(bytes.constData(), size_t(bytes.size())))
        return false;
    *root = firstElementChild(*document);
    return bool(*root);
}
}
