#pragma once

#include "core/CatalogProtection.h"
#include "core/GuiTypeBindings.h"
#include "core/ScannedFileReader.h"
#include "core/XmlParsePolicy.h"
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QVector>
#include <algorithm>
#include <functional>
#include <memory>
#include <pugixml.hpp>

namespace sc2dh::gui {

inline bool isGuiPath(const QString &path)
{
    const auto name = QFileInfo(path).fileName().toLower();
    const auto suffix = QFileInfo(path).suffix().toLower();
    return name == QStringLiteral("triggers") || suffix == QStringLiteral("sc2lib")
        || suffix == QStringLiteral("triggerlib") || suffix == QStringLiteral("sc2triggers")
        || suffix == QStringLiteral("trigger");
}

struct Reference {
    QString file, catalog, value, fieldPath, reason;
    qsizetype start = -1, length = 0;
    bool rewritable = false;
};

struct Definition {
    QString kind, catalog;
    bool operator==(const Definition &other) const { return kind == other.kind && catalog == other.catalog; }
};

class Registry {
public:
    void addFile(const QString &path, const QByteArray &bytes, bool required)
    {
        auto source = std::make_shared<Source>();
        source->path = path; source->bytes = bytes;
        const auto parsed = source->document.load_buffer(bytes.constData(), size_t(bytes.size()), xmlParseFlags);
        const auto root = source->document.document_element();
        if (!parsed || QString::fromUtf8(root.name()) != QStringLiteral("TriggerData")) {
            if (required || QString::fromUtf8(root.name()).endsWith(QStringLiteral(":TriggerData"))) {
                m_guiFiles.insert(path);
                m_issues << path + QStringLiteral(": GUI document is malformed or has unsupported root/namespace");
            }
            return;
        }
        source->utf8 = !bytes.startsWith(QByteArray::fromHex("fffe"))
            && !bytes.startsWith(QByteArray::fromHex("feff")) && !bytes.contains('\0');
        m_guiFiles.insert(path); m_sources.append(source);
    }

    void finalize()
    {
        for (const auto &source : m_sources) {
            visit(source->document.document_element(), [&](pugi::xml_node element, const QString &library) {
                const auto type = attribute(element, "Type");
                const auto id = identity(library, attribute(element, "Id"));
                if (type == QStringLiteral("ParamDef")) {
                    const auto definition = readType(element.child("ParameterType"));
                    insertDefinition(id, definition);
                    const auto fallback = element.child("Default");
                    if (fallback && attribute(fallback, "Type") == QStringLiteral("Param"))
                        m_contextTypes[referenceIdentity(fallback, library)].append(definition);
                } else if (type == QStringLiteral("Variable")) {
                    const auto initial = element.child("InitialValue");
                    if (initial && attribute(initial, "Type") == QStringLiteral("Param"))
                        m_contextTypes[referenceIdentity(initial, library)].append(readType(element.child("VariableType")));
                } else if (type == QStringLiteral("FunctionDef")) {
                    QSet<QString> parameters;
                    for (auto parameter : element.children("Parameter"))
                        if (attribute(parameter, "Type") == QStringLiteral("ParamDef"))
                            parameters.insert(referenceIdentity(parameter, library));
                    if (m_functions.contains(id) && m_functions.value(id) != parameters) m_ambiguousFunctions.insert(id);
                    else m_functions.insert(id, parameters);
                }
            });
        }
        populateNativeBindings();
        for (const auto &source : m_sources) {
            visit(source->document.document_element(), [&](pugi::xml_node element, const QString &library) {
                if (attribute(element, "Type") != QStringLiteral("FunctionCall")) return;
                const auto function = element.child("FunctionDef");
                const auto functionKey = referenceIdentity(function, library);
                for (auto parameter : element.children("Parameter")) {
                    if (attribute(parameter, "Type") != QStringLiteral("Param")) continue;
                    const auto key = referenceIdentity(parameter, library);
                    if (!m_functions.contains(functionKey) || m_ambiguousFunctions.contains(functionKey))
                        m_unresolvedCallParams.insert(key);
                    else m_callParameters[key].append(m_functions.value(functionKey));
                }
            });
        }
        for (const auto &source : m_sources) {
            visit(source->document.document_element(), [&](pugi::xml_node element, const QString &library) {
                if (attribute(element, "Type") == QStringLiteral("Param")) analyzeValue(*source, element, library);
            });
        }
    }

    bool isGuiFile(const QString &file) const { return m_guiFiles.contains(file); }
    QStringList files() const { return m_guiFiles.values(); }
    const QStringList &issues() const { return m_issues; }
    QVector<Reference> references(const QString &file) const { return m_references.value(file); }

    bool hasUnresolvedReference(const QString &catalog, const QString &id) const
    {
        for (auto file = m_references.cbegin(); file != m_references.cend(); ++file)
            for (const auto &reference : file.value()) {
                if (reference.rewritable) continue;
                if (!reference.catalog.isEmpty() && reference.catalog != catalog) continue;
                if (reference.value.isEmpty() || reference.value.compare(id, Qt::CaseInsensitive) == 0) return true;
            }
        return false;
    }

    bool rewrite(const QString &file, const QByteArray &bytes,
                 const std::function<QString(const QString &, const QString &)> &replacement,
                 QByteArray *output, int *count) const
    {
        *output = bytes; *count = 0;
        const Source *source = nullptr;
        for (const auto &candidate : m_sources) if (candidate->path == file) { source = candidate.get(); break; }
        if (!source || source->bytes != bytes) return false;
        auto references = m_references.value(file);
        std::sort(references.begin(), references.end(), [](const Reference &a, const Reference &b) { return a.start > b.start; });
        qsizetype boundary = bytes.size();
        for (const auto &reference : references) {
            if (!reference.rewritable || reference.start < 0 || reference.start + reference.length > boundary) continue;
            const auto next = replacement(reference.catalog, reference.value);
            if (next.isEmpty() || next == reference.value) continue;
            // Automatic object IDs exclude XML metacharacters; still escape defensively.
            auto escaped = next; escaped.replace('&', QStringLiteral("&amp;")); escaped.replace('<', QStringLiteral("&lt;"));
            escaped.replace('>', QStringLiteral("&gt;"));
            output->replace(reference.start, reference.length, escaped.toUtf8());
            boundary = reference.start; ++*count;
        }
        return true;
    }

    void addIssue(const QString &issue) { m_issues.append(issue); }

private:
    struct Source { QString path; QByteArray bytes; pugi::xml_document document; bool utf8 = true; };
    QVector<std::shared_ptr<Source>> m_sources;
    QSet<QString> m_guiFiles, m_ambiguousDefinitions, m_ambiguousFunctions, m_unresolvedCallParams;
    QHash<QString, Definition> m_definitions;
    QHash<QString, QSet<QString>> m_functions;
    QHash<QString, QVector<QSet<QString>>> m_callParameters;
    QHash<QString, QVector<Definition>> m_contextTypes;
    QHash<QString, QVector<Reference>> m_references;
    QStringList m_issues;

    static QString attribute(pugi::xml_node node, const char *name) { return QString::fromUtf8(node.attribute(name).value()); }
    // GUI declaration identifiers use exact spelling and are distinct from catalog IDs.
    static QString identity(const QString &library, const QString &id) { return library + QLatin1Char('|') + id; }
    static QString referenceIdentity(pugi::xml_node node, const QString &library)
    {
        const auto explicitLibrary = attribute(node, "Library");
        return identity(explicitLibrary.isEmpty() ? library : explicitLibrary, attribute(node, "Id"));
    }
    static Definition readType(pugi::xml_node container)
    {
        Definition result;
        result.kind = attribute(container.child("Type"), "Value");
        if (result.kind == QStringLiteral("gamelink")) {
            const auto name = attribute(container.child("GameType"), "Value");
            if (!name.isEmpty()) result.catalog = structuralCatalogIdentityScope(QLatin1Char('C') + name);
        }
        return result;
    }
    QSet<QString> m_nativeDefinitionKeys;
    QString m_nativeProvenance;
    void populateNativeBindings()
    {
        if (m_sources.isEmpty()) return;
        const auto stock = nativeBindings();
        if (!stock->valid) return;
        m_nativeProvenance = stock->provenance;
        for (const auto &source : m_sources) {
            visit(source->document.document_element(), [&](pugi::xml_node element, const QString &library) {
                if (attribute(element, "Type") == QStringLiteral("Param")) {
                    const auto key = referenceIdentity(element.child("ParameterDef"), library);
                    if (!stock->parameters.contains(key)) return;
                    const auto type = stock->parameters.value(key);
                    const auto catalog = type.kind == QStringLiteral("gamelink") && !type.gameType.isEmpty()
                        ? structuralCatalogIdentityScope(QLatin1Char('C') + type.gameType) : QString();
                    insertDefinition(key, {type.kind, catalog});
                    m_nativeDefinitionKeys.insert(key);
                } else if (attribute(element, "Type") == QStringLiteral("FunctionCall")) {
                    const auto key = referenceIdentity(element.child("FunctionDef"), library);
                    if (!stock->functions.contains(key)) return;
                    const auto parameters = stock->functions.value(key);
                    if (m_functions.contains(key) && m_functions.value(key) != parameters)
                        m_ambiguousFunctions.insert(key);
                    else m_functions.insert(key, parameters);
                }
            });
        }
    }
    void insertDefinition(const QString &id, const Definition &definition)
    {
        if (m_definitions.contains(id) && !(m_definitions.value(id) == definition)) m_ambiguousDefinitions.insert(id);
        else m_definitions.insert(id, definition);
    }
    template<class Callback> static void visit(pugi::xml_node root, Callback callback)
    {
        const auto standard = attribute(root.child("Standard"), "Id");
        QVector<QPair<pugi::xml_node, QString>> containers{{root, standard}};
        for (qsizetype i = 0; i < containers.size(); ++i) {
            const auto container = containers[i];
            for (auto node : container.first.children()) {
                if (QString::fromUtf8(node.name()) == QStringLiteral("Library"))
                    containers.append({node, attribute(node, "Id")});
                else if (QString::fromUtf8(node.name()) == QStringLiteral("Element")) callback(node, container.second);
            }
        }
    }
    void analyzeValue(const Source &source, pugi::xml_node element, const QString &library)
    {
        const auto paramKey = identity(library, attribute(element, "Id"));
        const auto parameter = element.child("ParameterDef");
        const auto definitionKey = referenceIdentity(parameter, library);
        const auto hintType = attribute(element.child("ValueType"), "Type");
        const auto gameHint = attribute(element.child("ValueGameType"), "Type");
        const auto hintCatalog = gameHint.isEmpty() ? QString() : structuralCatalogIdentityScope(QLatin1Char('C') + gameHint);
        Definition type; bool established = false;
        if (parameter && m_definitions.contains(definitionKey) && !m_ambiguousDefinitions.contains(definitionKey)) {
            type = m_definitions.value(definitionKey); established = !type.kind.isEmpty();
        } else if (!parameter && m_contextTypes.contains(paramKey)) {
            const auto contexts = m_contextTypes.value(paramKey); type = contexts.front(); established = !type.kind.isEmpty();
            for (const auto &context : contexts) if (!(context == type)) established = false;
        } else if (!parameter && !hintType.isEmpty()) {
            type = {hintType, hintCatalog}; established = true;
        }
        bool conflict = m_unresolvedCallParams.contains(paramKey)
            || (parameter && m_ambiguousDefinitions.contains(definitionKey));
        for (const auto &allowed : m_callParameters.value(paramKey))
            if (!parameter || !allowed.contains(definitionKey)) conflict = true;
        if (established && !hintType.isEmpty() && hintType != type.kind) conflict = true;
        if (established && !hintCatalog.isEmpty() && !type.catalog.isEmpty() && hintCatalog != type.catalog) conflict = true;
        // These GUI types can carry catalog identities or commands even without GameType.
        // Keep them protected until their composite/context grammar is independently supported.
        const QSet<QString> uncertainKinds = {QStringLiteral("catalogentry"), QStringLiteral("abilcmd"),
            QStringLiteral("soundlink"), QStringLiteral("sameas"), QStringLiteral("sameasparent"),
            QStringLiteral("anyvariable"), QStringLiteral("actormsg"), QStringLiteral("userinstance"), QStringLiteral("userfield")};
        const bool uncertainType = established && uncertainKinds.contains(type.kind);
        if (established && !conflict && type.kind != QStringLiteral("gamelink")
            && type.kind != QStringLiteral("anygamelink") && !uncertainType) return;
        if (uncertainType && type.kind == QStringLiteral("abilcmd")) type.catalog = QStringLiteral("cabil");
        if (uncertainType && type.kind == QStringLiteral("soundlink")) type.catalog = QStringLiteral("csound");

        const auto valueNode = element.child("Value");
        const auto value = QString::fromUtf8(valueNode.text().get());
        if (!established && !parameter && hintType.isEmpty() && !valueNode) return;
        Reference reference; reference.file = source.path;
        reference.catalog = conflict ? QString() : established ? type.catalog : hintCatalog;
        reference.value = value;
        if (uncertainType && (!isSafeAutomaticObjectId(value) || type.kind == QStringLiteral("actormsg")
            || type.kind == QStringLiteral("userinstance") || type.kind == QStringLiteral("userfield"))) reference.value.clear();
        reference.fieldPath = library + QStringLiteral("/Param[@Id='%1']/Value").arg(attribute(element, "Id"));
        reference.reason = established && !conflict ? QStringLiteral("GUI gamelink from parameter/value type")
                                                    : QStringLiteral("GUI parameter type unresolved or inconsistent");
        if (established && !conflict && m_nativeDefinitionKeys.contains(definitionKey))
            reference.reason += QStringLiteral("; ") + m_nativeProvenance;
        if (uncertainType) reference.reason = QStringLiteral("GUI catalog-facing type %1 requires unresolved command/context grammar").arg(type.kind);
        const auto text = valueNode.first_child();
        if (source.utf8 && established && !conflict && !uncertainType && !reference.catalog.isEmpty()
            && valueNode && text.type() == pugi::node_pcdata && !text.next_sibling()
            && value == value.trimmed() && isSafeAutomaticObjectId(value)) {
            const auto offset = text.offset_debug();
            const auto end = offset >= 0 ? source.bytes.indexOf('<', offset) : -1;
            if (end >= offset && offset >= 0) {
                reference.start = offset; reference.length = end - offset; reference.rewritable = true;
            }
        }
        m_references[source.path].append(reference);
    }
};

inline Registry projectRegistry(const AnalysisResult &analysis)
{
    Registry registry; ScannedFileReader reader(analysis);
    for (const auto &file : analysis.scannedFiles) {
        const auto relative = ScannedFileReader::relativePath(analysis.rootFolder, file.filePath).replace('\\', '/');
        const bool required = isGuiPath(relative);
        if (!required && !file.isXml) continue;
        QByteArray bytes;
        if (!reader.readBytes(file, 16ll * 1024ll * 1024ll, &bytes)) {
            if (required) registry.addIssue(relative + QStringLiteral(": GUI source unreadable or exceeds 16 MiB"));
            continue;
        }
        if (!required && !bytes.contains("TriggerData")) continue;
        registry.addFile(relative, bytes, required);
    }
    registry.finalize(); return registry;
}

} // namespace sc2dh::gui
