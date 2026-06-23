#include "core/XmlLoader.h"

#include <algorithm>
#include <cstddef>
#include <QCryptographicHash>
#include <QSet>
#include <QStringList>

#include <pugixml.hpp>

#include <sstream>

namespace {

QString serializeNode(const pugi::xml_node &node)
{
    std::ostringstream stream;
    node.print(stream, "  ", pugi::format_default, pugi::encoding_utf8);
    return QString::fromUtf8(stream.str().c_str());
}

QString canonicalNode(const pugi::xml_node &node, bool objectRoot = false)
{
    if (node.type() == pugi::node_pcdata || node.type() == pugi::node_cdata) {
        const QString text = QString::fromUtf8(node.value());
        return text.trimmed().isEmpty() ? QString() : QStringLiteral("T%1:%2").arg(text.size()).arg(text);
    }
    if (node.type() != pugi::node_element) return {};

    QStringList attributes;
    for (const pugi::xml_attribute attribute : node.attributes()) {
        const QString name = QString::fromUtf8(attribute.name());
        if (objectRoot && (name.compare(QStringLiteral("id"), Qt::CaseInsensitive) == 0
                           || name.compare(QStringLiteral("name"), Qt::CaseInsensitive) == 0)) {
            continue;
        }
        const QString value = QString::fromUtf8(attribute.value());
        attributes << QStringLiteral("A%1:%2=%3:%4").arg(name.size()).arg(name).arg(value.size()).arg(value);
    }
    std::sort(attributes.begin(), attributes.end());
    QString output = QStringLiteral("E%1:%2{%3}").arg(qstrlen(node.name())).arg(QString::fromUtf8(node.name()), attributes.join(QLatin1Char('|')));
    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling()) {
        output += canonicalNode(child, false);
    }
    output += QStringLiteral("/E");
    return output;
}

int siblingIndexForNode(const pugi::xml_node &node)
{
    int index = 1;
    for (pugi::xml_node sibling = node.previous_sibling(node.name()); sibling; sibling = sibling.previous_sibling(node.name())) {
        if (sibling.type() == pugi::node_element) {
            ++index;
        }
    }
    return index;
}

QString locationSegmentForNode(const pugi::xml_node &node)
{
    return QStringLiteral("%1[%2]").arg(QString::fromUtf8(node.name())).arg(siblingIndexForNode(node));
}

int lineNumberFromOffset(const QByteArray &bytes, std::size_t offset)
{
    if (offset == 0 || bytes.isEmpty()) {
        return -1;
    }

    const std::size_t boundedOffset = std::min(offset, static_cast<std::size_t>(bytes.size()));
    int line = 1;
    for (std::size_t i = 0; i < boundedOffset; ++i) {
        if (bytes[static_cast<int>(i)] == '\n') {
            ++line;
        }
    }
    return line;
}

QString buildPathFromNode(const pugi::xml_node &node)
{
    QStringList segments;
    for (pugi::xml_node current = node; current && current.type() == pugi::node_element; current = current.parent()) {
        segments.prepend(locationSegmentForNode(current));
        if (!current.parent() || current.parent().type() == pugi::node_document) {
            break;
        }
    }
    return QStringLiteral("/") + segments.join(QLatin1Char('/'));
}

pugi::xml_node childByNameAndIndex(const pugi::xml_node &parent, const QString &name, int index)
{
    int count = 0;
    for (pugi::xml_node child = parent.first_child(); child; child = child.next_sibling()) {
        if (child.type() != pugi::node_element) {
            continue;
        }
        if (QString::fromUtf8(child.name()) != name) {
            continue;
        }
        ++count;
        if (count == index) {
            return child;
        }
    }
    return {};
}

QStringList splitLocation(const QString &location)
{
    QStringList segments = location.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return segments;
}

bool parseSegment(const QString &segment, QString *name, int *index)
{
    const int open = segment.lastIndexOf(QLatin1Char('['));
    const int close = segment.lastIndexOf(QLatin1Char(']'));
    if (open <= 0 || close <= open + 1) {
        return false;
    }
    bool ok = false;
    const int parsedIndex = segment.mid(open + 1, close - open - 1).toInt(&ok);
    if (!ok || parsedIndex <= 0) {
        return false;
    }
    *name = segment.left(open);
    *index = parsedIndex;
    return true;
}

pugi::xml_node findNodeByLocation(const pugi::xml_node &document, const QString &location)
{
    QStringList segments = splitLocation(location);
    if (segments.isEmpty()) {
        return {};
    }

    pugi::xml_node currentParent = document;
    pugi::xml_node currentNode;
    for (const QString &segment : segments) {
        QString name;
        int index = 0;
        if (!parseSegment(segment, &name, &index)) {
            return {};
        }
        currentNode = childByNameAndIndex(currentParent, name, index);
        if (!currentNode) {
            return {};
        }
        currentParent = currentNode;
    }
    return currentNode;
}

void collectNodes(const pugi::xml_node &node, const QByteArray &bytes, const QString &sourceFile, QVector<DataNode> *nodes)
{
    if (node.type() == pugi::node_element) {
        const auto idAttr = node.attribute("id");
        if (idAttr) {
            DataNode item;
            item.sourceFile = sourceFile;
            item.elementName = QString::fromUtf8(node.name());
            item.parentNode = node.parent() ? QString::fromUtf8(node.parent().name()) : QString();
            item.id = QString::fromUtf8(idAttr.value());
            item.originalLocation = buildPathFromNode(node);
            item.lineNumber = lineNumberFromOffset(bytes, static_cast<std::size_t>(node.offset_debug()));

            for (const auto &attribute : node.attributes()) {
                item.attributes.insert(QString::fromUtf8(attribute.name()), QString::fromUtf8(attribute.value()));
            }

            item.serializedXml = serializeNode(node);

            // Only top-level identity fields are excluded. Nested id/name values
            // remain gameplay data. Formatting and attribute order are canonicalized.
            const QString normalizedXml = canonicalNode(node, true);
            item.contentHash = QString::fromLatin1(
                QCryptographicHash::hash(normalizedXml.toUtf8(), QCryptographicHash::Sha256).toHex());
            nodes->append(std::move(item));
        }
    }

    for (const pugi::xml_node child : node.children()) {
        collectNodes(child, bytes, sourceFile, nodes);
    }
}

}

bool XmlLoader::loadDocument(const QByteArray &xmlBytes, pugi::xml_document *document, QString *errorMessage) const
{
    const pugi::xml_parse_result result = document->load_buffer(xmlBytes.constData(), static_cast<size_t>(xmlBytes.size()));
    if (!result) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("XML parse error: %1 at offset %2")
                                .arg(QString::fromUtf8(result.description()))
                                .arg(result.offset);
        }
        return false;
    }
    return true;
}

QString XmlLoader::buildNodeLocation(const pugi::xml_node &node) const
{
    return buildPathFromNode(node);
}

bool XmlLoader::extractNodes(const QString &sourceFile,
                             const QByteArray &xmlBytes,
                             QVector<DataNode> *nodes,
                             QString *errorMessage) const
{
    pugi::xml_document document;
    if (!loadDocument(xmlBytes, &document, errorMessage)) {
        return false;
    }

    collectNodes(document, xmlBytes, sourceFile, nodes);
    return true;
}

std::optional<int> XmlLoader::findNodeIndexByFileAndId(const QVector<DataNode> &nodes,
                                                       const QString &sourceFile,
                                                       const QString &id) const
{
    for (int i = 0; i < nodes.size(); ++i) {
        if (nodes[i].sourceFile == sourceFile && nodes[i].id == id) {
            return i;
        }
    }
    return std::nullopt;
}

bool XmlLoader::removeNodesByLocation(const QByteArray &xmlBytes,
                                      const QSet<QString> &locations,
                                      QByteArray *rewrittenXml,
                                      QString *errorMessage) const
{
    pugi::xml_document document;
    if (!loadDocument(xmlBytes, &document, errorMessage)) {
        return false;
    }

    QStringList orderedLocations;
    for (const QString &location : locations) {
        orderedLocations.append(location);
    }
    std::sort(orderedLocations.begin(), orderedLocations.end(), [](const QString &left, const QString &right) {
        const int leftDepth = splitLocation(left).size();
        const int rightDepth = splitLocation(right).size();
        if (leftDepth != rightDepth) {
            return leftDepth > rightDepth;
        }
        return left > right;
    });

    // Resolve every location before mutating the tree. Removing an earlier
    // same-name sibling changes the numeric path of all following siblings.
    QVector<pugi::xml_node> nodesToRemove;
    nodesToRemove.reserve(orderedLocations.size());
    for (const QString &location : orderedLocations) {
        pugi::xml_node node = findNodeByLocation(document, location);
        if (!node) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Unable to locate XML node at %1").arg(location);
            }
            return false;
        }
        nodesToRemove.append(node);
    }
    for (const pugi::xml_node &node : nodesToRemove) {
        if (!node.parent() || !node.parent().remove_child(node)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Unable to remove resolved XML node.");
            }
            return false;
        }
    }

    std::ostringstream stream;
    document.save(stream, "  ", pugi::format_default, pugi::encoding_utf8);
    *rewrittenXml = QByteArray::fromStdString(stream.str());
    return true;
}
