#include "core/XmlParsePolicy.h"
#include "core/CatalogLinkSchema.h"

#include "core/CatalogProtection.h"
#include "core/GuiTypeBindings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QStringList>

#include <pugixml.hpp>

#include <algorithm>
#include <memory>
#include <mutex>

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
    QString targetCatalog;
    bool repeated = false;
};

struct Schema
{
    QHash<QString, QHash<QString, FieldRule>> fieldsByOwner;
    QHash<QString, QString> baseByType;
    bool valid = false;
    QHash<QString, QString> catalogByClass;
    QSet<QString> simpleTypes;
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
        QStringLiteral(":/catalog_type_index.json"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/resources/catalog_type_index.json"),
        QDir::current().absoluteFilePath(QStringLiteral("resources/catalog_type_index.json")),
        QDir::current().absoluteFilePath(QStringLiteral("../resources/catalog_type_index.json")),
        QDir::current().absoluteFilePath(QStringLiteral("../../resources/catalog_type_index.json")),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../resources/catalog_type_index.json")
    };
    for (const QString &candidate : candidates) {
        if (readAll(candidate, &bytes))
            return bytes;
    }
    return {};
}


QByteArray loadSchemaSourceBytes()
{
    QByteArray bytes;
    const QStringList paths = {
        QCoreApplication::applicationDirPath() + QStringLiteral("/resources/catalogsData.xsd"),
        QDir::current().absoluteFilePath(QStringLiteral("resources/catalogsData.xsd")),
        QDir::current().absoluteFilePath(QStringLiteral("../resources/catalogsData.xsd")),
        QDir::current().absoluteFilePath(QStringLiteral("../../resources/catalogsData.xsd")),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../resources/catalogsData.xsd")
    };
    for (const QString &path : paths) if (readAll(path, &bytes)) return bytes;
    return {};
}

QString linkCatalog(QString type, const QHash<QString, QString> &bases, const QSet<QString> &catalogs)
{
    QSet<QString> seen;
    while (!type.isEmpty() && !seen.contains(type)) {
        seen.insert(type);
        QString plain = type;
        if (plain.startsWith(QStringLiteral("simple_"))) plain.remove(0, 7);

        if (plain.startsWith(QLatin1Char('C')) && plain.endsWith(QStringLiteral("Link"))) {
            const QString domain = plain.mid(1, plain.size() - 5);
            if (catalogs.contains(domain)) return domain;
        }
        type = bases.value(type);
    }
    return {};
}

const Schema &schema(bool refresh = false)
{
    const auto build = [](const QByteArray &bytes, const QByteArray &source) {
        Schema output;
        const QJsonObject document = QJsonDocument::fromJson(bytes).object();
        if (document.value(QStringLiteral("format")).toString() != QStringLiteral("sc2dh.catalog-index.v1")) return output;
        if (source.isEmpty() || document.value(QStringLiteral("sourceSha256")).toString().toLatin1()
            != QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex()) return output;
        const QJsonObject types = document.value(QStringLiteral("runtimeTypes")).toObject();
        const QJsonObject catalogClasses=document.value(QStringLiteral("catalogTypes")).toObject();
        QSet<QString> catalogs;
        for(auto it=catalogClasses.begin();it!=catalogClasses.end();++it) {
            output.catalogByClass.insert(key(it.key()),it.value().toString());
            catalogs.insert(it.value().toString());
        }
        const auto structuralTypes=document.value(QStringLiteral("types")).toObject();
        for(auto it=structuralTypes.begin();it!=structuralTypes.end();++it)
            if(it.value().toObject().value(QStringLiteral("kind")).toString()==QStringLiteral("simpleType")) output.simpleTypes.insert(it.key());

        for (auto it = types.begin(); it != types.end(); ++it)
            output.baseByType.insert(it.key(), it.value().toObject().value(QStringLiteral("base")).toString());
        for (auto it = types.begin(); it != types.end(); ++it) {
            QString current = it.key();
            QSet<QString> seen;
            auto &fields = output.fieldsByOwner[key(current)];
            while (!current.isEmpty() && !seen.contains(current)) {
                seen.insert(current);
                const QJsonArray entries = types.value(current).toObject().value(QStringLiteral("fields")).toArray();
                for (const QJsonValue &entry : entries) {
                    const QJsonObject field = entry.toObject();
                    const QString name = field.value(QStringLiteral("name")).toString();
                    const QString target = field.value(QStringLiteral("type")).toString();
                    if (fields.contains(key(name))) continue;
                    FieldRule rule;
                    rule.targetType = target;
                    rule.targetCatalog = linkCatalog(target, output.baseByType, catalogs);
                    rule.kind = rule.targetCatalog.isEmpty() ? RuleKind::Struct : RuleKind::Link;
                    rule.repeated = field.value(QStringLiteral("maxOccurs")).toString() == QStringLiteral("unbounded");
                    fields.insert(key(name), rule);
                }
                current = output.baseByType.value(current);
            }
        }
        output.valid = !types.isEmpty() && !catalogClasses.isEmpty();
        return output;
    };
    static std::mutex mutex;
    const std::lock_guard<std::mutex> lock(mutex);
    // Retain immutable generations so readers' field pointers stay valid.
    static QVector<std::shared_ptr<const Schema>> generations;
    static QByteArray previousBytes, previousSource;
    if (generations.isEmpty() || refresh) {
        const QByteArray bytes = loadSchemaBytes();
        const QByteArray source = loadSchemaSourceBytes();
        if (generations.isEmpty() || bytes != previousBytes || source != previousSource) {
            generations.append(std::make_shared<const Schema>(build(bytes, source)));
            previousBytes = bytes;
            previousSource = source;
        }
    }
    return *generations.last();
}

const FieldRule *findRule(const QString &ownerType, const QString &fieldName)
{
    const auto &data = schema();
    const auto ownerIt = data.fieldsByOwner.constFind(key(ownerType));
    if (ownerIt == data.fieldsByOwner.cend())
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

void addActorEventReferenceValue(const QString &value, const QString &selfId, QSet<QString> *references)
{
    addReferenceValue(value, selfId, references);

    const QStringList tokens = value.split(QRegularExpression(QStringLiteral("[\\s,;|:/\\\\]+")), Qt::SkipEmptyParts);
    static const QRegularExpression scopedSeparator(QStringLiteral("[@.]"));
    for (const QString &token : tokens) {
        if (!token.contains(QLatin1Char('@')) && !token.contains(QLatin1Char('.')))
            continue;
        const QStringList parts = token.split(scopedSeparator, Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            if (part != token)
                addReferenceValue(part, selfId, references);
        }
    }
}

void traverseSchemaNode(const pugi::xml_node &xmlNode,
                        const QString &schemaType,
                        const QString &selfId,
                        QSet<QString> *references,
                        int depth = 0)
{
    if (!xmlNode || schemaType.isEmpty() || depth > 16)
        return;

    if (schemaType.compare(QStringLiteral("SActorEvent"), Qt::CaseInsensitive) == 0) {
        for (const pugi::xml_attribute attribute : xmlNode.attributes()) {
            if (isMetaAttribute(QString::fromUtf8(attribute.name())))
                continue;
            addActorEventReferenceValue(QString::fromUtf8(attribute.value()), selfId, references);
        }
        const QString text = QString::fromUtf8(xmlNode.child_value()).trimmed();
        if (!text.isEmpty())
            addActorEventReferenceValue(text, selfId, references);
    }

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

bool isActorCatalogElement(const QString &name)
{
    return name.startsWith(QStringLiteral("CActor"), Qt::CaseInsensitive);
}

bool isActorEventElement(const QString &name)
{
    return name.compare(QStringLiteral("On"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("Remove"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("Do"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("Event"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("Term"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("Terms"), Qt::CaseInsensitive) == 0;
}

void addActorEventReferences(const pugi::xml_node &xmlNode,
                             const QString &selfId,
                             QSet<QString> *references,
                             int depth = 0)
{
    if (!xmlNode || depth > 24)
        return;

    if (xmlNode.type() == pugi::node_element
        && isActorEventElement(QString::fromUtf8(xmlNode.name()))) {
        for (const pugi::xml_attribute attribute : xmlNode.attributes()) {
            if (isMetaAttribute(QString::fromUtf8(attribute.name())))
                continue;
            addActorEventReferenceValue(QString::fromUtf8(attribute.value()), selfId, references);
        }
        const QString text = QString::fromUtf8(xmlNode.child_value()).trimmed();
        if (!text.isEmpty())
            addActorEventReferenceValue(text, selfId, references);
    }

    for (pugi::xml_node child = xmlNode.first_child(); child; child = child.next_sibling()) {
        if (child.type() == pugi::node_element)
            addActorEventReferences(child, selfId, references, depth + 1);
    }
}

const FieldRule *declaredCatalogFieldRule(pugi::xml_node node, const QString &attribute)
{
    QStringList path;
    pugi::xml_node owner = node;
    while (owner && owner.parent().type() == pugi::node_element
           && QString::fromUtf8(owner.parent().name()) != QStringLiteral("Catalog")
           && QString::fromUtf8(owner.parent().name()) != QStringLiteral("Entries")) {
        path.prepend(QString::fromUtf8(owner.name()));
        owner = owner.parent();
    }
    if (!owner) return nullptr;
    QString type = QString::fromUtf8(owner.name());
    const FieldRule *last = nullptr;
    for (const QString &field : path) {
        last = findRule(type, field);
        if (!last) return nullptr;
        type = last->targetType;
    }
    if (!attribute.isEmpty() && (attribute != QStringLiteral("value") || !last || last->kind != RuleKind::Link))
        last = findRule(type, attribute);
    else if (path.isEmpty())
        last = findRule(type, attribute);
    return last;
}

} // namespace

namespace sc2dh
{


QString structuralCatalogIdentityScope(const QString &elementName)
{
    const QString catalog=schema().catalogByClass.value(key(elementName));
    return catalog.isEmpty()?QString():QLatin1Char('c')+catalog.toCaseFolded();
}

bool scalarCatalogField(const QString &owner, const QString &field, bool attribute)
{
    const FieldRule *rule=findRule(owner,field);
    if(!rule || rule->repeated) return false;
    if(attribute && (schema().simpleTypes.contains(rule->targetType) || rule->targetType.startsWith(QStringLiteral("{http://www.w3.org/2001/XMLSchema}")))) return true;
    const auto fields=schema().fieldsByOwner.value(key(rule->targetType));
    return fields.size()==1 && fields.contains(QStringLiteral("value"));
}

bool repeatedCatalogField(const QString &owner, const QString &field)
{
    const FieldRule *rule=findRule(owner,field);
    return rule && rule->repeated;
}

bool repeatedCatalogElement(pugi::xml_node element)
{
    const FieldRule *rule=declaredCatalogFieldRule(element,{});
    return rule && rule->repeated;
}

QStringList possibleCatalogReferenceScopes(const QString &owner)
{
    const auto &data=schema();
    QSet<QString> seen, domains;
    QVector<QString> pending{owner};
    while(!pending.isEmpty()) {
        const QString current=key(pending.takeLast());
        if(seen.contains(current)) continue;
        seen.insert(current);
        const auto fields=data.fieldsByOwner.value(current);
        for(const auto &rule:fields) {
            if(!rule.targetCatalog.isEmpty()) domains.insert(QLatin1Char('c')+rule.targetCatalog.toCaseFolded());
            else if(data.fieldsByOwner.contains(key(rule.targetType))) pending.append(rule.targetType);
        }
    }
    return domains.values();
}

void refreshCatalogSchema() { schema(true); }

bool catalogSchemaAvailable() { return schema().valid; }

QByteArray catalogSchemaFingerprint()
{
    return QCryptographicHash::hash(loadSchemaBytes() + loadSchemaSourceBytes() + sc2dh::gui::nativeBindingBytes(), QCryptographicHash::Sha256);
}

bool catalogFieldDeclared(pugi::xml_node node, const QString &attribute)
{
    // Placement Unit is an explicit analyzer carrier outside catalogsData.xsd.
    if (QString::fromUtf8(node.name()) == QStringLiteral("CPlacedUnit") && attribute == QStringLiteral("Unit"))
        return true;
    return declaredCatalogFieldRule(node, attribute) != nullptr;
}

QString catalogReferencePrefix(pugi::xml_node node, const QString &attribute)
{
    // Data Collection Entry uses a catalog prefix in its value rather than an
    // XSD Link type. Keep the explicit comma grammar scoped to collection rows.
    if (QString::fromUtf8(node.name()) == QStringLiteral("DataRecord") && attribute == QStringLiteral("Entry")) {
        for (pugi::xml_node owner = node.parent(); owner && owner.type() == pugi::node_element; owner = owner.parent()) {
            if (!QString::fromUtf8(owner.name()).startsWith(QStringLiteral("CDataCollection"))) continue;
            const QString entry = QString::fromUtf8(node.attribute("Entry").value());
            const int comma = entry.indexOf(QLatin1Char(','));
            if (comma > 0) {
                const QString domain = entry.left(comma).trimmed().toCaseFolded();
                static const QSet<QString> catalogs = {
                    QStringLiteral("unit"), QStringLiteral("abil"), QStringLiteral("weapon"),
                    QStringLiteral("effect"), QStringLiteral("actor"), QStringLiteral("behavior"),
                    QStringLiteral("model"), QStringLiteral("button")};
                if (catalogs.contains(domain)) return QLatin1Char('C') + domain;
            }
            break;
        }
    }
    const FieldRule *last = declaredCatalogFieldRule(node, attribute);
    if (last && !last->targetCatalog.isEmpty()) return QLatin1Char('C') + last->targetCatalog;
    return {};
}

QSet<QString> extractCatalogLinkReferences(const DataNode &node)
{
    QSet<QString> references;
    if (node.serializedXml.isEmpty() || node.elementName.isEmpty() || schema().fieldsByOwner.isEmpty())
        return references;

    pugi::xml_document document;
    const QByteArray bytes = node.serializedXml.toUtf8();
    const pugi::xml_parse_result parsed = document.load_buffer(bytes.constData(), size_t(bytes.size()), sc2dh::xmlParseFlags);
    if (!parsed)
        return references;

    pugi::xml_node root = document.first_child();
    while (root && root.type() != pugi::node_element)
        root = root.next_sibling();
    if (!root)
        return references;

    traverseSchemaNode(root, QString::fromUtf8(root.name()), node.id, &references);
    if (isActorCatalogElement(QString::fromUtf8(root.name())))
        addActorEventReferences(root, node.id, &references);

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
