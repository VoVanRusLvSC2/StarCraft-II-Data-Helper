#include "core/DecorationGuiActionBuilder.h"
#include "core/GalaxyReferenceSpans.h"

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSet>
#include <pugixml.hpp>
#include <sstream>

namespace sc2dh::decor
{
namespace
{
pugi::xml_node reference(pugi::xml_node parent, const char *tag, const char *type,
                         const QString &id, const char *library = nullptr)
{
    auto node = parent.append_child(tag);
    node.append_attribute("Type") = type;
    node.append_attribute("Id") = id.toUtf8().constData();
    if (library) node.append_attribute("Library") = library;
    return node;
}
void text(pugi::xml_node parent, const char *tag, const QString &value)
{
    parent.append_child(tag).text().set(value.toUtf8().constData());
}
void scriptCode(pugi::xml_node parent, const QString &value)
{
    // Match the opening/closing framing lines in SC2's native ScriptCode XML.
    // Inline text appeared empty when the generated actions were imported.
    text(parent, "ScriptCode", QStringLiteral("\n            %1\n        ").arg(value.trimmed()));
}
bool displayNameValid(const QString &value)
{
    return !value.trimmed().isEmpty() && value.size() <= 256
        && !value.contains(QLatin1Char('\n')) && !value.contains(QLatin1Char('\r'));
}
bool isOurInclude(const QString &value, const QString &path)
{
    QString normalized = value;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (normalized.endsWith(QStringLiteral(".galaxy"), Qt::CaseInsensitive))
        normalized.chop(7);
    return normalized.compare(path, Qt::CaseInsensitive) == 0;
}
// Only real include tokens are inspected; an example in a comment is not a connection.
QString canonicalIncludes(QString script, const QString &path, bool *found, bool *complete)
{
    const auto tokens = sc2dh::galaxy::lex(script, complete);
    *found = false;
    for (int i = tokens.size() - 2; i >= 0; --i) {
        if (tokens[i].raw != QStringLiteral("include") || !tokens[i + 1].literal
            || !isOurInclude(tokens[i + 1].value, path))
            continue;
        *found = true;
        script.replace(tokens[i + 1].start, tokens[i + 1].length,
                       QStringLiteral("\"%1\"").arg(path));
    }
    return script;
}
}

DecorGuiActionArtifacts buildDecorGuiAction(const QByteArray &triggers,
                                            const QByteArray &mapScript,
                                            const QString &runtimeEntry,
                                            const DecorationStreamingPlan &plan,
                                            const GalaxyGenerationOptions &galaxyOptions,
                                            const DecorGuiActionOptions &guiOptions)
{
    DecorGuiActionArtifacts result;
    if (!displayNameValid(guiOptions.actionName) || !displayNameValid(guiOptions.presetName)
        || plan.zones.isEmpty()) {
        result.error = QStringLiteral("GUI action/preset names and at least one Region are required.");
        return result;
    }
    QString includePath = runtimeEntry;
    includePath.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (includePath.endsWith(QStringLiteral(".galaxy"), Qt::CaseInsensitive)) includePath.chop(7);
    if (includePath.isEmpty() || includePath.contains(QLatin1Char('"')) || includePath.contains(QLatin1Char('\n'))) {
        result.error = QStringLiteral("Invalid Galaxy include path.");
        return result;
    }
    pugi::xml_document document;
    if (triggers.isEmpty()) document.append_child("TriggerData");
    else if (!document.load_buffer(triggers.constData(), triggers.size())) {
        result.error = QStringLiteral("Cannot parse GUI Triggers; no map copy created.");
        return result;
    }
    auto root = document.child("TriggerData");
    if (!root) {
        result.error = QStringLiteral("GUI Triggers has no TriggerData root.");
        return result;
    }
    QVector<pugi::xml_node> originalNodes;
    for (auto node : root.children("Element")) {
        if (QString::fromUtf8(node.child("Identifier").text().get()).startsWith(QStringLiteral("SC2DH_MapEnvironment_"))) {
            result.error = QStringLiteral("This map already has a generated GUI action. Use that action or disable GUI generation.");
            return result;
        }
        originalNodes << node;
    }
    QSet<QString> ids;
    QVector<pugi::xml_node> includeNodes;
    QVector<pugi::xml_node> pending{root};
    while (!pending.isEmpty()) {
        auto node = pending.takeLast();
        if (node.attribute("Id")) ids.insert(QString::fromUtf8(node.attribute("Id").value()).toUpper());
        if (QString::fromUtf8(node.attribute("Type").value()) == QStringLiteral("CustomScript") && node.child("ScriptCode"))
            includeNodes << node;
        for (auto child : node.children()) if (child.type() == pugi::node_element) pending << child;
    }
    int ordinal = 0;
    const auto nextId = [&]() {
        QString id;
        do {
            id = QString::fromLatin1(QCryptographicHash::hash(
                QByteArray("SC2DH GUI action v1:") + QByteArray::number(ordinal++), QCryptographicHash::Sha256).toHex().left(8)).toUpper();
        } while (ids.contains(id));
        ids.insert(id);
        return id;
    };
    const auto element = [&](const char *type, const QString &id) {
        auto node = root.append_child("Element");
        node.append_attribute("Type") = type;
        node.append_attribute("Id") = id.toUtf8().constData();
        return node;
    };
    const QString categoryId = nextId(), presetId = nextId(), functionId = nextId(), parameterId = nextId();
    const QString functionIdentifier = QStringLiteral("SC2DH_MapEnvironment_%1").arg(functionId);
    const QString galaxyFunction = QStringLiteral("gf_%1").arg(functionIdentifier);
    auto items = root.child("Root");
    if (!items) items = root.prepend_child("Root");
    auto category = element("Category", categoryId);
    auto categoryItem = items.prepend_child("Item");
    categoryItem.append_attribute("Type") = "Category";
    categoryItem.append_attribute("Id") = categoryId.toUtf8().constData();
    reference(category, "Item", "Preset", presetId);
    reference(category, "Item", "FunctionDef", functionId);
    auto preset = element("Preset", presetId);
    text(preset, "Identifier", QStringLiteral("SC2DH_GalaxyMapRegion_%1").arg(presetId));
    preset.append_child("PresetInteger");
    preset.append_child("BaseType").append_attribute("Value") = "int";
    preset.append_child("PresetGenConstVar");
    auto function = element("FunctionDef", functionId);
    text(function, "Identifier", functionIdentifier);
    reference(function, "Label", "Label", QStringLiteral("1C34099B"), "Ntve");
    function.append_child("FlagAction");
    reference(function, "Parameter", "ParamDef", parameterId);
    auto parameter = element("ParamDef", parameterId);
    text(parameter, "Identifier", QStringLiteral("region"));
    auto parameterType = parameter.append_child("ParameterType");
    parameterType.append_child("Type").append_attribute("Value") = "preset";
    reference(parameterType, "TypeElement", "Preset", presetId);

    const QString switchId = nextId(), switchArgumentId = nextId();
    reference(function, "FunctionCall", "FunctionCall", switchId);
    auto switchCall = element("FunctionCall", switchId);
    reference(switchCall, "FunctionDef", "FunctionDef", QStringLiteral("91C49196"), "Ntve");
    reference(switchCall, "Parameter", "Param", switchArgumentId);
    auto switchArgument = element("Param", switchArgumentId);
    reference(switchArgument, "ParameterDef", "ParamDef", QStringLiteral("B4ACF12A"), "Ntve");
    reference(switchArgument, "Parameter", "ParamDef", parameterId);

    QHash<QString, int> callIndices;
    QVector<QString> presetValues;
    QString body = QStringLiteral("void %1(int lp_region) {\n").arg(galaxyFunction);
    for (int i = 0; i < plan.zones.size(); ++i) {
        const auto &zone = plan.zones[i];
        const QString stem = galaxyRegionFunctionStem(plan, zone, galaxyOptions);
        const QString call = QStringLiteral("Create_%1();").arg(stem);
        callIndices.insert(call, i);
        if (zone.zoneName.endsWith(QStringLiteral("_Doodads"))) {
            const QString shortName = zone.zoneName.left(zone.zoneName.size() - 8);
            if (QRegularExpression(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$")).match(shortName).hasMatch())
                callIndices.insert(QStringLiteral("Create_%1();").arg(shortName), i);
        }
        // Aliases from the Region name are part of the generated public API too.
        auto originalNames = galaxyOptions;
        originalNames.publicFunctionNames.clear();
        callIndices.insert(QStringLiteral("Create_%1();").arg(galaxyRegionFunctionStem(plan, zone, originalNames)), i);
        const QString valueId = nextId(), caseId = nextId(), caseArgumentId = nextId(), scriptId = nextId();
        presetValues << valueId;
        reference(preset, "Item", "PresetValue", valueId);
        auto value = element("PresetValue", valueId);
        text(value, "Identifier", QStringLiteral("Region_%1").arg(zone.zoneId));
        result.triggerStrings.insert(QStringLiteral("PresetValue/Name/%1").arg(valueId),
            zone.zoneName.isEmpty() ? stem : zone.zoneName);
        reference(switchCall, "FunctionCall", "FunctionCall", caseId);
        auto caseCall = element("FunctionCall", caseId);
        reference(caseCall, "FunctionDef", "FunctionDef", QStringLiteral("3A1227AB"), "Ntve");
        reference(caseCall, "SubFunctionType", "SubFuncType", QStringLiteral("AD43BC93"), "Ntve");
        reference(caseCall, "Parameter", "Param", caseArgumentId);
        reference(caseCall, "FunctionCall", "FunctionCall", scriptId);
        auto caseArgument = element("Param", caseArgumentId);
        reference(caseArgument, "ParameterDef", "ParamDef", QStringLiteral("9A8F1093"), "Ntve");
        caseArgument.append_child("ValueType").append_attribute("Type") = "preset";
        reference(caseArgument, "ValueElement", "Preset", presetId);
        reference(caseArgument, "ValuePreset", "PresetValue", valueId);
        auto script = element("FunctionCall", scriptId);
        reference(script, "FunctionDef", "FunctionDef", QStringLiteral("00000123"), "Ntve");
        reference(script, "SubFunctionType", "SubFuncType", QStringLiteral("363CE97C"), "Ntve");
        scriptCode(script, call);
        body += QStringLiteral("    %1if (lp_region == %2) { %3 }\n").arg(i ? QStringLiteral("else ") : QString(), QString::number(i), call);
    }
    body += QStringLiteral("}\n");
    const auto presetArgument = [&](const QString &argumentId, int index) {
        auto arg = element("Param", argumentId);
        reference(arg, "ParameterDef", "ParamDef", parameterId);
        arg.append_child("ValueType").append_attribute("Type") = "preset";
        reference(arg, "ValueElement", "Preset", presetId);
        reference(arg, "ValuePreset", "PresetValue", presetValues[index]);
    };
    const QString defaultId = nextId();
    reference(parameter, "Default", "Param", defaultId);
    presetArgument(defaultId, 0);

    // Existing single-statement Create calls become editable GUI action calls.
    // Preserve their IDs and SubFunctionType so parent triggers/loops stay intact.
    bool guiInclude = false;
    for (auto node : includeNodes) {
        bool found = false, complete = true;
        auto code = node.child("ScriptCode");
        const QString normalized = canonicalIncludes(QString::fromUtf8(code.text().get()), includePath, &found, &complete);
        if (!complete) { result.error = QStringLiteral("Incomplete GUI Custom Script; no copy created."); return result; }
        if (found) {
            code.text().set(QStringLiteral("\n            %1\n        ").arg(normalized.trimmed()).toUtf8().constData());
            guiInclude = true;
        }
    }
    for (auto node : originalNodes) {
        const QString type = QString::fromUtf8(node.attribute("Type").value());
        auto code = node.child("ScriptCode");
        if (!code) continue;
        const QString sourceCode = QString::fromUtf8(code.text().get());
        if (type != QStringLiteral("FunctionCall")) continue;
        const auto def = node.child("FunctionDef");
        if (QString::fromUtf8(def.attribute("Library").value()) != QStringLiteral("Ntve")
            || QString::fromUtf8(def.attribute("Id").value()) != QStringLiteral("00000123")) continue;
        const auto found = callIndices.constFind(sourceCode.trimmed());
        if (found == callIndices.cend()) continue;
        node.remove_child("ScriptCode");
        // Keep the original field order and parent/loop references intact.
        node.child("FunctionDef").attribute("Id").set_value(functionId.toUtf8().constData());
        node.child("FunctionDef").remove_attribute("Library");
        const QString argumentId = nextId();
        reference(node, "Parameter", "Param", argumentId);
        presetArgument(argumentId, found.value());
        ++result.convertedCalls;
    }
    if (!guiInclude) {
        const QString includeId = nextId();
        auto include = element("CustomScript", includeId);
        scriptCode(include, QStringLiteral("include \"%1\"").arg(includePath));
        auto item = items.insert_child_after("Item", categoryItem);
        item.append_attribute("Type") = "CustomScript";
        item.append_attribute("Id") = includeId.toUtf8().constData();
        result.triggerStrings.insert(QStringLiteral("CustomScript/Name/%1").arg(includeId), QStringLiteral("Galaxy map environment include"));
    }

    bool scriptInclude = false, complete = true;
    QString script = canonicalIncludes(QString::fromUtf8(mapScript), includePath, &scriptInclude, &complete);
    if (!complete) { result.error = QStringLiteral("Incomplete MapScript Galaxy; no copy created."); return result; }
    const QRegularExpression nativeInclude(QStringLiteral("^\\s*include\\s+\"TriggerLibs/NativeLib\"[^\\n]*\\n"), QRegularExpression::MultilineOption);
    const auto native = nativeInclude.match(script);
    const int insertion = native.hasMatch() ? int(native.capturedEnd()) : (script.startsWith(QChar(0xFEFF)) ? 1 : 0);
    script.insert(insertion, (scriptInclude ? QString() : QStringLiteral("include \"%1\"\n").arg(includePath))
                  + QStringLiteral("void %1(int lp_region);\n").arg(galaxyFunction));
    // Replace only executable zero-argument calls; leave comments and strings intact.
    const auto tokens = sc2dh::galaxy::lex(script, &complete);
    for (int i = tokens.size() - 4; i >= 0; --i) {
        if (tokens[i].literal || tokens[i + 1].raw != QStringLiteral("(")
            || tokens[i + 2].raw != QStringLiteral(")") || tokens[i + 3].raw != QStringLiteral(";")) continue;
        if (i > 0 && (tokens[i - 1].raw == QStringLiteral("void") || tokens[i - 1].raw == QStringLiteral("bool"))) continue;
        const auto found = callIndices.constFind(tokens[i].raw + QStringLiteral("();"));
        if (found == callIndices.cend()) continue;
        script.replace(tokens[i].start, tokens[i + 3].start + tokens[i + 3].length - tokens[i].start,
                       QStringLiteral("%1(%2);").arg(galaxyFunction).arg(found.value()));
    }
    script += QLatin1Char('\n') + body;
    std::ostringstream serialized;
    document.save(serialized, "    ", pugi::format_default, pugi::encoding_utf8);
    result.triggers = QByteArray::fromStdString(serialized.str());
    result.mapScript = script.toUtf8();
    result.triggerStrings.insert(QStringLiteral("Category/Name/%1").arg(categoryId), QStringLiteral("Galaxy Map Environment"));
    result.triggerStrings.insert(QStringLiteral("FunctionDef/Name/%1").arg(functionId), guiOptions.actionName.trimmed());
    result.triggerStrings.insert(QStringLiteral("FunctionDef/Grammar/%1").arg(functionId), guiOptions.actionName.trimmed() + QStringLiteral(" (~region~)"));
    result.triggerStrings.insert(QStringLiteral("ParamDef/Name/%1").arg(parameterId), QStringLiteral("Region"));
    result.triggerStrings.insert(QStringLiteral("Preset/Name/%1").arg(presetId), guiOptions.presetName.trimmed());
    result.valid = true;
    return result;
}
}
