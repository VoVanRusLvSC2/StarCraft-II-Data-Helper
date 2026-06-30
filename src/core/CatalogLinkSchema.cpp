#include "core/CatalogLinkSchema.h"

#include "core/CatalogProtection.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QStringList>

#include <pugixml.hpp>

#include <algorithm>

namespace
{

enum class RuleKind
{
    Link,
    Struct
};

struct FieldRule
{
    RuleKind kind = RuleKind::Struct;
    QString targetType;
};

struct Schema
{
    QHash<QString, QHash<QString, FieldRule>> fieldsByOwner;
};

QString key(const QString &value)
{
    return value.trimmed().toCaseFolded();
}

bool readAll(const QString &path, QByteArray *bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    *bytes = file.readAll();
    return true;
}

QByteArray loadSchemaBytes()
{
    QByteArray bytes;
    const QStringList candidates = {
        QStringLiteral(":/catalog_link_schema.tsv"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/resources/catalog_link_schema.tsv"),
        QDir::current().absoluteFilePath(QStringLiteral("resources/catalog_link_schema.tsv")),
        QDir::current().absoluteFilePath(QStringLiteral("../resources/catalog_link_schema.tsv")),
        QDir::current().absoluteFilePath(QStringLiteral("../../resources/catalog_link_schema.tsv"))
    };
    for (const QString &candidate : candidates) {
        if (readAll(candidate, &bytes))
            return bytes;
    }
    return {};
}

const Schema &schema()
{
    static const Schema loaded = [] {
        Schema output;
        const QByteArray bytes = loadSchemaBytes();
        const QList<QByteArray> lines = bytes.split('\n');
        for (QByteArray lineBytes : lines) {
            lineBytes = lineBytes.trimmed();
            if (lineBytes.isEmpty() || lineBytes.startsWith('#'))
                continue;

            const QList<QByteArray> columns = lineBytes.split('\t');
            if (columns.size() < 4)
                continue;

            const QString owner = QString::fromUtf8(columns.at(0));
            const QString field = QString::fromUtf8(columns.at(1));
            const QString kindText = QString::fromUtf8(columns.at(2));
            const QString target = QString::fromUtf8(columns.at(3));

            FieldRule rule;
            rule.kind = kindText.compare(QStringLiteral("link"), Qt::CaseInsensitive) == 0
                ? RuleKind::Link
                : RuleKind::Struct;
            rule.targetType = target;
            output.fieldsByOwner[key(owner)].insert(key(field), rule);
        }
        return output;
    }();
    return loaded;
}

const FieldRule *findRule(const QString &ownerType, const QString &fieldName)
{
    const auto ownerIt = schema().fieldsByOwner.constFind(key(ownerType));
    if (ownerIt == schema().fieldsByOwner.cend())
        return nullptr;
    const auto fieldIt = ownerIt.value().constFind(key(fieldName));
    if (fieldIt == ownerIt.value().cend())
        return nullptr;
    return &fieldIt.value();
}

QStringList candidateReferenceTokens(const QString &value)
{
    const QString normalized = value.trimmed();
    if (normalized.isEmpty())
        return {};
    if (normalized.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
        || normalized.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)
        || normalized.contains(QStringLiteral("://"))) {
        return {};
    }

    QStringList tokens = normalized.split(QRegularExpression(QStringLiteral("[\\s,;|:/\\\\]+")), Qt::SkipEmptyParts);
    if (normalized.contains(QLatin1Char(','))) {
        const QString tail = normalized.section(QLatin1Char(','), -1).trimmed();
        if (!tail.isEmpty())
            tokens.append(tail);
    }
    if (normalized.contains(QLatin1Char(' '))) {
        const QString tail = normalized.section(QLatin1Char(' '), -1).trimmed();
        if (!tail.isEmpty())
            tokens.append(tail);
    }
    tokens.removeDuplicates();
    return tokens;
}

void addReferenceValue(const QString &value, const QString &selfId, QSet<QString> *references)
{
    for (const QString &token : candidateReferenceTokens(value)) {
        if (token.isEmpty() || token == selfId)
            continue;
        if (!sc2dh::isSafeAutomaticObjectId(token) || sc2dh::isReservedCatalogToken(token))
            continue;
        references->insert(token);
    }
}

bool isMetaAttribute(const QString &name)
{
    const QString field = key(name);
    return field == QStringLiteral("id")
        || field == QStringLiteral("index")
        || field == QStringLiteral("removed")
        || field == QStringLiteral("default");
}

bool isPlacedUnitField(const QString &schemaType, const QString &fieldName)
{
    return schemaType.compare(QStringLiteral("CPlacedUnit"), Qt::CaseInsensitive) == 0
        && fieldName.compare(QStringLiteral("Unit"), Qt::CaseInsensitive) == 0;
}

void addDataRecordEntry(const QString &entry, const QString &selfId, QSet<QString> *references)
{
    const QString trimmed = entry.trimmed();
    if (trimmed.isEmpty())
        return;

    const int comma = trimmed.indexOf(QLatin1Char(','));
    if (comma >= 0) {
        addReferenceValue(trimmed.mid(comma + 1).trimmed(), selfId, references);
        return;
    }

    addReferenceValue(trimmed, selfId, references);
}

void addLinkNodeValues(const pugi::xml_node &node, const QString &selfId, QSet<QString> *references)
{
    bool collectedPreferred = false;
    for (const char *preferred : {"value", "Link", "link", "Face", "face"}) {
        const pugi::xml_attribute attribute = node.attribute(preferred);
        if (attribute) {
            addReferenceValue(QString::fromUtf8(attribute.value()), selfId, references);
            collectedPreferred = true;
        }
    }
    if (!collectedPreferred) {
        for (const pugi::xml_attribute attribute : node.attributes()) {
            if (isMetaAttribute(QString::fromUtf8(attribute.name())))
                continue;
            addReferenceValue(QString::fromUtf8(attribute.value()), selfId, references);
        }
    }
    const QString text = QString::fromUtf8(node.child_value()).trimmed();
    if (!text.isEmpty())
        addReferenceValue(text, selfId, references);
}

void traverseSchemaNode(const pugi::xml_node &xmlNode,
                        const QString &schemaType,
                        const QString &selfId,
                        QSet<QString> *references,
                        int depth = 0)
{
    if (!xmlNode || schemaType.isEmpty() || depth > 16)
        return;

    for (const pugi::xml_attribute attribute : xmlNode.attributes()) {
        const QString field = QString::fromUtf8(attribute.name());
        if (isPlacedUnitField(schemaType, field)) {
            addReferenceValue(QString::fromUtf8(attribute.value()), selfId, references);
            continue;
        }
        if (field.compare(QStringLiteral("parent"), Qt::CaseInsensitive) == 0) {
            addReferenceValue(QString::fromUtf8(attribute.value()), selfId, references);
            continue;
        }
        const FieldRule *rule = findRule(schemaType, field);
        if (!rule || rule->kind != RuleKind::Link)
            continue;
        addReferenceValue(QString::fromUtf8(attribute.value()), selfId, references);
    }

    for (pugi::xml_node child = xmlNode.first_child(); child; child = child.next_sibling()) {
        if (child.type() != pugi::node_element)
            continue;
        const QString field = QString::fromUtf8(child.name());
        if (field.compare(QStringLiteral("DataRecord"), Qt::CaseInsensitive) == 0) {
            const pugi::xml_attribute entry = child.attribute("Entry");
            if (entry)
                addDataRecordEntry(QString::fromUtf8(entry.value()), selfId, references);
        }
        const FieldRule *rule = findRule(schemaType, field);
        if (!rule)
            continue;
        if (rule->kind == RuleKind::Link) {
            addLinkNodeValues(child, selfId, references);
        } else {
            traverseSchemaNode(child, rule->targetType, selfId, references, depth + 1);
        }
    }
}

} // namespace

namespace sc2dh
{

QSet<QString> extractCatalogLinkReferences(const DataNode &node)
{
    QSet<QString> references;
    if (node.serializedXml.isEmpty() || node.elementName.isEmpty() || schema().fieldsByOwner.isEmpty())
        return references;

    pugi::xml_document document;
    const QByteArray bytes = node.serializedXml.toUtf8();
    const pugi::xml_parse_result parsed = document.load_buffer(bytes.constData(), size_t(bytes.size()));
    if (!parsed)
        return references;

    pugi::xml_node root = document.first_child();
    while (root && root.type() != pugi::node_element)
        root = root.next_sibling();
    if (!root)
        return references;

    traverseSchemaNode(root, QString::fromUtf8(root.name()), node.id, &references);

    // Unit tests can enable a compact fake field for small fixtures. Production
    // SC2 graphs stay schema-driven and do not scan arbitrary XML attributes.
    if (qEnvironmentVariableIsSet("SC2DH_ENABLE_TEST_REFS")) {
        const auto legacyRefs = node.attributes.constFind(QStringLiteral("refs"));
        if (legacyRefs != node.attributes.cend())
            addReferenceValue(legacyRefs.value(), node.id, &references);
    }

    return references;
}

} // namespace sc2dh
