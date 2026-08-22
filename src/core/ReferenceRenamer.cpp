#include "core/ReferenceRenamer.h"

#include "core/BackupManager.h"
#include "core/CatalogLinkSchema.h"
#include "core/CatalogProtection.h"
#include "core/FolderAnalyzer.h"
#include "core/MergeService.h"
#include "core/UnifiedReferenceIndex.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

#include <pugixml.hpp>

#include <algorithm>
#include <sstream>
#include <utility>

namespace {

QString nodePath(const pugi::xml_node &node)
{
    QStringList parts;
    for (pugi::xml_node current = node; current && current.type() == pugi::node_element; current = current.parent()) {
        int index = 1;
        for (pugi::xml_node previous = current.previous_sibling(current.name()); previous;
             previous = previous.previous_sibling(current.name())) ++index;
        parts.prepend(QStringLiteral("%1[%2]").arg(QString::fromUtf8(current.name())).arg(index));
    }
    return QStringLiteral("/") + parts.join(QLatin1Char('/'));
}

bool readFile(const QString &path, QByteArray *bytes, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { *error = QStringLiteral("Unable to read %1").arg(path); return false; }
    *bytes = file.readAll();
    return true;
}

QString relativeAnalysisPath(const AnalysisResult &analysis, const QString &path)
{
    QString relative = QDir(analysis.rootFolder).relativeFilePath(path);
    relative = QDir::cleanPath(relative).replace('\\', '/');
    return relative;
}

QString fastLookupKey(const QString &left, const QString &right)
{
    return left + QChar(0x1f) + right;
}

QString catalogLookupKey(const QString &elementName, const QString &id)
{
    return sc2dh::catalogIdentityKey(elementName, id);
}

bool isTopLevelCatalogIdentity(const pugi::xml_node &node)
{
    pugi::xml_node parent = node.parent();
    return parent
        && parent.type() == pugi::node_element
        && QString::fromUtf8(parent.name()) == QStringLiteral("Catalog")
        && node.attribute("id");
}

const QRegularExpression &renameTokenExpression()
{
    static const QRegularExpression expression(QStringLiteral("(?<![A-Za-z0-9_@])([A-Za-z0-9_@]+)(?![A-Za-z0-9_@])"));
    return expression;
}

bool isPlacementObjectsPath(const QString &relativePath)
{
    return relativePath.compare(QStringLiteral("Objects"), Qt::CaseInsensitive) == 0
        || relativePath.endsWith(QStringLiteral("/Objects"), Qt::CaseInsensitive);
}

bool shouldRewriteSafeTextReferenceFile(const ScannedFileInfo &file, const QString &relativePath)
{
    if (file.isXml)
        return false;
    return file.isSc2DataLike || isPlacementObjectsPath(relativePath);
}

bool isParentAttributeReferenceOnly(const DataNode &node, const QString &oldId)
{
    bool parentMatches = false;
    for (auto it = node.attributes.cbegin(); it != node.attributes.cend(); ++it) {
        if (it.key().compare(QStringLiteral("parent"), Qt::CaseInsensitive) == 0 && it.value() == oldId) {
            parentMatches = true;
            break;
        }
    }
    if (!parentMatches || node.serializedXml.isEmpty())
        return false;

    pugi::xml_document document;
    const QByteArray bytes = node.serializedXml.toUtf8();
    if (!document.load_buffer(bytes.constData(), size_t(bytes.size())))
        return false;

    pugi::xml_node root = document.first_child();
    while (root && root.type() != pugi::node_element)
        root = root.next_sibling();
    if (!root)
        return false;

    pugi::xml_attribute parent = root.attribute("parent");
    if (!parent || QString::fromUtf8(parent.value()) != oldId)
        return false;
    root.remove_attribute(parent);

    std::ostringstream stream;
    root.print(stream, "  ", pugi::format_raw, pugi::encoding_utf8);

    DataNode parentlessNode = node;
    parentlessNode.serializedXml = QString::fromStdString(stream.str());
    for (auto it = parentlessNode.attributes.begin(); it != parentlessNode.attributes.end();) {
        if (it.key().compare(QStringLiteral("parent"), Qt::CaseInsensitive) == 0)
            it = parentlessNode.attributes.erase(it);
        else
            ++it;
    }

    return !sc2dh::extractCatalogLinkReferences(parentlessNode).contains(oldId);
}

struct RenameTarget
{
    QString newId;
    QString elementName;
};

using RenameTargetMap = QHash<QString, QVector<RenameTarget>>;

bool matchesCatalogPrefix(const QString &elementName, const QString &prefix)
{
    return prefix.isEmpty() || elementName.startsWith(prefix, Qt::CaseInsensitive);
}

QString typedReferenceCatalogPrefix(pugi::xml_node node, const QString &fieldName)
{
    const QString field = fieldName.toLower();
    const QString nodeName = QString::fromUtf8(node.name()).toLower();

    if (field == QStringLiteral("unitname") && nodeName.startsWith(QStringLiteral("cactorunit")))
        return QStringLiteral("CUnit");

    if (field == QStringLiteral("link")) {
        if (nodeName == QStringLiteral("abilarray"))
            return QStringLiteral("CAbil");
        if (nodeName == QStringLiteral("weaponarray"))
            return QStringLiteral("CWeapon");
        if (nodeName == QStringLiteral("behaviorarray"))
            return QStringLiteral("CBehavior");
    }

    const QString combined = field + QLatin1Char(' ') + nodeName;
    if (combined.contains(QStringLiteral("model")))
        return QStringLiteral("CModel");
    if (combined.contains(QStringLiteral("button")) || combined.contains(QStringLiteral("face")))
        return QStringLiteral("CButton");
    if (combined.contains(QStringLiteral("weapon")))
        return QStringLiteral("CWeapon");
    if (combined.contains(QStringLiteral("abil")) || combined.contains(QStringLiteral("ability")))
        return QStringLiteral("CAbil");
    if (combined.contains(QStringLiteral("effect")))
        return QStringLiteral("CEffect");
    if (combined.contains(QStringLiteral("behavior")) || combined.contains(QStringLiteral("buff")))
        return QStringLiteral("CBehavior");
    if (combined.contains(QStringLiteral("validator")))
        return QStringLiteral("CValidator");
    if (combined.contains(QStringLiteral("requirement")))
        return QStringLiteral("CRequirement");
    if (combined.contains(QStringLiteral("mover")))
        return QStringLiteral("CMover");
    if (combined.contains(QStringLiteral("turret")))
        return QStringLiteral("CTurret");
    if (combined.contains(QStringLiteral("sound")))
        return QStringLiteral("CSound");
    if (combined.contains(QStringLiteral("actor")))
        return QStringLiteral("CActor");
    if (field.contains(QStringLiteral("unit")) || nodeName == QStringLiteral("unit"))
        return QStringLiteral("CUnit");
    return {};
}

QString resolvedReplacement(const QVector<RenameTarget> &targets, const QString &catalogPrefix)
{
    QSet<QString> distinct;
    for (const RenameTarget &target : targets) {
        if (!matchesCatalogPrefix(target.elementName, catalogPrefix))
            continue;
        distinct.insert(target.newId);
    }
    if (distinct.size() != 1)
        return {};
    return *distinct.cbegin();
}

QHash<QString, QString> unambiguousRenames(const RenameTargetMap &renames)
{
    QHash<QString, QString> result;
    for (auto it = renames.cbegin(); it != renames.cend(); ++it) {
        const QString replacement = resolvedReplacement(it.value(), {});
        if (!replacement.isEmpty())
            result.insert(it.key(), replacement);
    }
    return result;
}

int simultaneousReplace(QString *value, const RenameTargetMap &renames, const QString &catalogPrefix = {})
{
    if (!value || value->isEmpty() || renames.isEmpty())
        return 0;
    QString output;
    qsizetype last = 0;
    int replacements = 0;
    auto matches = renameTokenExpression().globalMatch(*value);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const QString oldId = match.captured(1);
        if (!sc2dh::isSafeAutomaticObjectId(oldId) || sc2dh::isReservedCatalogToken(oldId))
            continue;
        const auto replacement = renames.constFind(oldId);
        if (replacement == renames.cend())
            continue;
        const QString newId = resolvedReplacement(replacement.value(), catalogPrefix);
        if (newId.isEmpty())
            continue;
        output += value->mid(last, match.capturedStart() - last);
        output += newId;
        last = match.capturedEnd();
        ++replacements;
    }
    if (replacements > 0) {
        output += value->mid(last);
        *value = output;
    }
    return replacements;
}

bool looksLikeUtf8Text(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return false;
    const qsizetype sampleSize = std::min<qsizetype>(bytes.size(), 8192);
    int printable = 0;
    int zeros = 0;
    for (qsizetype i = 0; i < sampleSize; ++i) {
        const uchar value = uchar(bytes.at(i));
        if (value == 0)
            ++zeros;
        if (value == '\r' || value == '\n' || value == '\t' || (value >= 32 && value < 127) || value >= 128)
            ++printable;
    }
    return zeros == 0 && printable >= (sampleSize * 85) / 100;
}

bool looksLikeUtf16LeText(const QByteArray &bytes)
{
    if (bytes.size() < 4 || bytes.size() % 2 != 0)
        return false;
    const qsizetype pairs = std::min<qsizetype>(bytes.size() / 2, 4096);
    int textPairs = 0;
    int zeroHigh = 0;
    for (qsizetype i = 0; i < pairs; ++i) {
        const uchar low = uchar(bytes.at(i * 2));
        const uchar high = uchar(bytes.at(i * 2 + 1));
        if (high == 0)
            ++zeroHigh;
        if (high == 0 && (low == '\r' || low == '\n' || low == '\t' || (low >= 32 && low < 127)))
            ++textPairs;
    }
    return zeroHigh >= (pairs * 70) / 100 && textPairs >= (pairs * 60) / 100;
}

bool containsRenameTokenText(const QString &text, const RenameTargetMap &renames)
{
    auto matches = renameTokenExpression().globalMatch(text);
    while (matches.hasNext()) {
        if (renames.contains(matches.next().captured(1)))
            return true;
    }
    return false;
}

bool containsRenameTokenBytes(const QByteArray &bytes, const RenameTargetMap &renames)
{
    if (looksLikeUtf8Text(bytes) && containsRenameTokenText(QString::fromUtf8(bytes), renames))
        return true;
    if (looksLikeUtf16LeText(bytes)) {
        const auto *data = reinterpret_cast<const char16_t *>(bytes.constData());
        if (containsRenameTokenText(QString::fromUtf16(data, bytes.size() / 2), renames))
            return true;
    }

    const auto isIdChar = [](uchar value) {
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')
            || (value >= '0' && value <= '9') || value == '_' || value == '@';
    };
    for (qsizetype start = 0; start < bytes.size();) {
        while (start < bytes.size() && !isIdChar(uchar(bytes[start])))
            ++start;
        qsizetype end = start;
        while (end < bytes.size() && isIdChar(uchar(bytes[end])))
            ++end;
        if (end > start && renames.contains(QString::fromLatin1(bytes.constData() + start, end - start)))
            return true;
        start = std::max(end, start + 1);
    }
    for (qsizetype start = 0; start + 1 < bytes.size();) {
        while (start + 1 < bytes.size()
               && (!isIdChar(uchar(bytes[start])) || bytes[start + 1] != '\0'))
            ++start;
        qsizetype end = start;
        QByteArray tokenBytes;
        while (end + 1 < bytes.size()
               && isIdChar(uchar(bytes[end])) && bytes[end + 1] == '\0') {
            tokenBytes.append(bytes[end]);
            end += 2;
        }
        if (!tokenBytes.isEmpty() && renames.contains(QString::fromLatin1(tokenBytes)))
            return true;
        start = std::max(end, start + 1);
    }
    return false;
}

bool rewriteSafeTextBytes(const QByteArray &original, const RenameTargetMap &renames,
                          QByteArray *rewritten, int *replacementCount)
{
    if (looksLikeUtf8Text(original)) {
        QString text = QString::fromUtf8(original);
        const int replacements = simultaneousReplace(&text, renames);
        if (replacementCount)
            *replacementCount = replacements;
        if (rewritten)
            *rewritten = replacements > 0 ? text.toUtf8() : original;
        return true;
    }
    if (looksLikeUtf16LeText(original)) {
        const auto *data = reinterpret_cast<const char16_t *>(original.constData());
        QString text = QString::fromUtf16(data, original.size() / 2);
        const int replacements = simultaneousReplace(&text, renames);
        if (replacementCount)
            *replacementCount = replacements;
        if (rewritten)
            *rewritten = replacements > 0
                ? QByteArray(reinterpret_cast<const char *>(text.utf16()), text.size() * 2)
                : original;
        return true;
    }
    return false;
}

QString blockingReferenceSummary(const sc2dh::refs::UnifiedReferenceIndex &index, const QString &id)
{
    QStringList sources;
    for (const sc2dh::refs::ReferenceRecord &record : index.strongReferencesToId(id)) {
        if (record.rewritable)
            continue;
        QString source = record.sourceFile.isEmpty() ? QStringLiteral("<unknown>") : record.sourceFile;
        if (record.lineNumber > 0)
            source += QStringLiteral(":%1").arg(record.lineNumber);
        sources << QStringLiteral("%1 in %2").arg(sc2dh::refs::referenceKindName(record.kind), source);
    }
    sources.removeDuplicates();
    std::sort(sources.begin(), sources.end());
    if (sources.size() > 6)
        sources = sources.mid(0, 6) << QStringLiteral("...");
    return sources.join(QStringLiteral(", "));
}

bool isActorEventElementName(const QString &name)
{
    return name.compare(QStringLiteral("On"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("Remove"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("Do"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("Event"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("Term"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("Terms"), Qt::CaseInsensitive) == 0;
}

bool hasActorCatalogAncestor(pugi::xml_node node)
{
    for (pugi::xml_node parent = node.parent(); parent && parent.type() == pugi::node_element; parent = parent.parent()) {
        if (QString::fromUtf8(parent.name()).startsWith(QStringLiteral("CActor"), Qt::CaseInsensitive))
            return true;
    }
    return false;
}

bool isInsideActorEvent(pugi::xml_node node)
{
    for (pugi::xml_node current = node; current && current.type() == pugi::node_element; current = current.parent()) {
        if (isActorEventElementName(QString::fromUtf8(current.name())) && hasActorCatalogAncestor(current))
            return true;
    }
    return false;
}

bool shouldRewriteReferenceValue(pugi::xml_node node, const QString &fieldName, const QString &value)
{
    if (fieldName.compare(QStringLiteral("parent"), Qt::CaseInsensitive) == 0)
        return false;
    const bool actorEventValue = isInsideActorEvent(node)
        && (fieldName.compare(QStringLiteral("Terms"), Qt::CaseInsensitive) == 0
            || fieldName.compare(QStringLiteral("Send"), Qt::CaseInsensitive) == 0
            || fieldName.compare(QStringLiteral("value"), Qt::CaseInsensitive) == 0
            || fieldName.isEmpty());
    if (!actorEventValue && (sc2dh::isNonReferenceCatalogFieldName(fieldName) || sc2dh::looksLikeCatalogFilterList(value)))
        return false;
    // Generic scalar <Field value="..."> nodes are commonly enums, numbers,
    // flags, animation names or editor metadata. Rewrite them only when the
    // element/attribute name identifies a catalog type (Effect, Unit, Model,
    // Validator, etc.). This prevents an object named "Moving" from corrupting
    // <AllowedMovement value="Moving"/> during a broad batch rename.
    if (fieldName.compare(QStringLiteral("value"), Qt::CaseInsensitive) == 0
        && !actorEventValue
        && typedReferenceCatalogPrefix(node, fieldName).isEmpty())
        return false;

    for (pugi::xml_node current = node; current && current.type() == pugi::node_element; current = current.parent()) {
        if (!isInsideActorEvent(current)
            && sc2dh::isNonReferenceCatalogFieldName(QString::fromUtf8(current.name())))
            return false;
    }
    return true;
}

struct RewriteResult {
    int identities = 0;
    int references = 0;
    QStringList changes;
};

struct PendingRename {
    QString oldId;
    QString newId;
    QString elementName;
    QString sourceFile;
    QString expectedLocation;
    QString actualLocation;
    bool found = false;
};

QStringList linkedDataCollectionElements(const QString &elementName)
{
    if (elementName.compare(QStringLiteral("CUnit"), Qt::CaseInsensitive) == 0)
        return {QStringLiteral("CDataCollectionUnit")};
    if (elementName.startsWith(QStringLiteral("CAbil"), Qt::CaseInsensitive))
        return {QStringLiteral("CDataCollectionAbil")};
    if (elementName.startsWith(QStringLiteral("CWeapon"), Qt::CaseInsensitive))
        return {QStringLiteral("CDataCollection"), QStringLiteral("CDataCollectionWeapon")};
    return {};
}

bool isDefaultDataCollectionNode(const DataNode &node)
{
    if (!node.elementName.startsWith(QStringLiteral("CDataCollection"), Qt::CaseInsensitive)
        || node.elementName.startsWith(QStringLiteral("CDataCollectionPattern"), Qt::CaseInsensitive))
        return false;
    pugi::xml_document fragment;
    if (!fragment.load_string(node.serializedXml.toUtf8().constData()))
        return false;
    const pugi::xml_node collection = fragment.first_child();
    return QString::fromUtf8(collection.attribute("default").value()).compare(QStringLiteral("1"), Qt::CaseInsensitive) == 0;
}

bool containsTarget(const RenameTargetMap &renames, const PendingRename &pending)
{
    const auto it = renames.constFind(pending.oldId);
    if (it == renames.cend())
        return false;
    for (const RenameTarget &target : it.value())
        if (target.newId == pending.newId && target.elementName == pending.elementName)
            return true;
    return false;
}

void removeTarget(RenameTargetMap *renames, const PendingRename &pending)
{
    if (!renames)
        return;
    auto it = renames->find(pending.oldId);
    if (it == renames->end())
        return;
    QVector<RenameTarget> kept;
    for (const RenameTarget &target : std::as_const(it.value())) {
        if (target.newId == pending.newId && target.elementName == pending.elementName)
            continue;
        kept.append(target);
    }
    if (kept.isEmpty())
        renames->erase(it);
    else
        it.value() = kept;
}

void locateIdentityTargets(pugi::xml_node node,
                           QHash<QString, int> *targetIndexByElementAndId,
                           QHash<QString, int> *targetIndexByLocation,
                           QVector<PendingRename> *targets)
{
    if (!targets || !targetIndexByElementAndId || !targetIndexByLocation)
        return;
    if (node.type() == pugi::node_element) {
        const pugi::xml_attribute idAttribute = node.attribute("id");
        if (idAttribute) {
            const QString id = QString::fromUtf8(idAttribute.value());
            const QString elementName = QString::fromUtf8(node.name());
            const QString path = nodePath(node);
            int targetIndex = targetIndexByElementAndId->value(fastLookupKey(elementName, id), -1);
            if (targetIndex < 0)
                targetIndex = targetIndexByLocation->value(path, -1);
            if (targetIndex >= 0 && targetIndex < targets->size() && !(*targets)[targetIndex].found) {
                PendingRename &target = (*targets)[targetIndex];
                if ((target.elementName == elementName && target.oldId == id)
                    || (target.expectedLocation == path && target.oldId == id)) {
                    target.found = true;
                    target.actualLocation = path;
                }
            }
        }
    }
    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling())
        locateIdentityTargets(child, targetIndexByElementAndId, targetIndexByLocation, targets);
}

void collectIdentityKeys(pugi::xml_node node, QSet<QString> *keys)
{
    if (!keys)
        return;
    if (node.type() == pugi::node_element) {
        const pugi::xml_attribute idAttribute = node.attribute("id");
        if (idAttribute)
            keys->insert(catalogLookupKey(QString::fromUtf8(node.name()), QString::fromUtf8(idAttribute.value())));
    }
    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling())
        collectIdentityKeys(child, keys);
}

void rewrite(pugi::xml_node node, const QString &file, const QHash<QString, QString> &identityByLocation,
             const RenameTargetMap &renames, RewriteResult *result, bool collectChanges)
{
    if (node.type() == pugi::node_element) {
        const QString path = nodePath(node);
        for (pugi::xml_attribute attribute : node.attributes()) {
            const QString attributeName = QString::fromUtf8(attribute.name());
            if (attributeName.compare(QStringLiteral("id"), Qt::CaseInsensitive) == 0 && identityByLocation.contains(path)) {
                const QString before = QString::fromUtf8(attribute.value());
                const QString after = identityByLocation.value(path);
                attribute.set_value(after.toUtf8().constData());
                ++result->identities;
                if (collectChanges)
                    result->changes << QStringLiteral("%1 %2 @id: %3 -> %4 (object identity)").arg(file, path, before, after);
                continue;
            }
            if (attributeName.compare(QStringLiteral("id"), Qt::CaseInsensitive) == 0 && isTopLevelCatalogIdentity(node))
                continue;
            QString value = QString::fromUtf8(attribute.value());
            const QString before = value;
            if (!shouldRewriteReferenceValue(node, attributeName, value))
                continue;
            const int count = simultaneousReplace(&value, renames, typedReferenceCatalogPrefix(node, attributeName));
            if (count) {
                attribute.set_value(value.toUtf8().constData());
                result->references += count;
                if (collectChanges)
                    result->changes << QStringLiteral("%1 %2 @%3: %4 -> %5").arg(file, path, attributeName, before, value);
            }
        }
    } else if (node.type() == pugi::node_pcdata || node.type() == pugi::node_cdata) {
        QString value = QString::fromUtf8(node.value());
        const QString before = value;
        if (!shouldRewriteReferenceValue(node.parent(), QString(), value))
            return;
        const int count = simultaneousReplace(&value, renames, typedReferenceCatalogPrefix(node.parent(), QString()));
        if (count) {
            node.set_value(value.toUtf8().constData());
            result->references += count;
            if (collectChanges)
                result->changes << QStringLiteral("%1 %2 text: %3 -> %4").arg(file, nodePath(node.parent()), before, value);
        }
    }
    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling())
        rewrite(child, file, identityByLocation, renames, result, collectChanges);
}

bool restore(const QString &root, const QString &backup, const QStringList &files, QString *error)
{
    if (backup.startsWith(QStringLiteral("disabled"), Qt::CaseInsensitive))
        return false;
    bool ok = true;
    for (const QString &relative : files) {
        const QString target = QDir(root).absoluteFilePath(relative);
        QFile::remove(target);
        if (!QFile::copy(QDir(backup).absoluteFilePath(relative), target)) {
            ok = false; *error += QStringLiteral(" Rollback failed for %1.").arg(target);
        }
    }
    return ok;
}

QString buildReport(const AnalysisResult &analysis, const RenamePlan &plan, const RewriteResult &rewriteResult,
                    const QStringList &files, const QStringList &warnings, const QStringList &conflicts,
                    const QString &finalResult)
{
    QString report = QStringLiteral("Rename To Standard Preview\nSelected family: %1\nRoot ID: %2\nTarget root ID: %3\nSource files: %4\n")
                         .arg(plan.family.rootId, plan.family.rootId, plan.targetRootId).arg(files.size());
    for (const QString &file : files) report += QStringLiteral("- %1\n").arg(file);
    report += QStringLiteral("\nDetected objects\n");
    for (const UnitFamilyObject &object : plan.family.objects) {
        const DataNode &node = analysis.nodes[object.nodeIndex];
        report += QStringLiteral("- %1 | %2 | role: %3 | confidence: %4 | %5\n")
                      .arg(node.id, node.elementName, unitFamilyRoleName(object.role), object.confidence, node.sourceFile);
    }
    report += QStringLiteral("\nNon-standard objects / rename plan\n");
    for (const RenamePlanItem &item : plan.items)
        report += QStringLiteral("- %1 -> %2 | %3 | confidence: %4 | risk: %5 | %6\n")
                      .arg(item.oldId, item.newId, unitFamilyRoleName(item.role), item.confidence, item.riskLevel, item.reason);
    report += QStringLiteral("\nManual review objects\n");
    for (const UnitFamilyObject &object : plan.manualReview)
        report += QStringLiteral("- %1 | %2\n").arg(analysis.nodes[object.nodeIndex].id, object.reason);
    report += QStringLiteral("\nReference update plan\n");
    for (const QString &change : rewriteResult.changes) report += QStringLiteral("- %1\n").arg(change);
    report += QStringLiteral("\nConflicts\n- %1\nWarnings\n- %2\nSkipped objects\n")
                  .arg(conflicts.isEmpty() ? QStringLiteral("none") : conflicts.join(QStringLiteral("\n- ")),
                       warnings.isEmpty() ? QStringLiteral("none") : warnings.join(QStringLiteral("\n- ")));
    QSet<int> planned;
    for (const RenamePlanItem &item : plan.items) planned.insert(item.nodeIndex);
    for (const UnitFamilyObject &object : plan.family.objects)
        if (!planned.contains(object.nodeIndex)) report += QStringLiteral("- %1 (already standard or manual review)\n").arg(analysis.nodes[object.nodeIndex].id);
    report += QStringLiteral("\nIdentities renamed: %1\nReferences updated: %2\nFinal result: %3\n")
                  .arg(rewriteResult.identities).arg(rewriteResult.references, 0, 10).arg(finalResult);
    return report;
}

bool prepare(const AnalysisResult &analysis, const RenamePlan &plan, QHash<QString, QByteArray> *staged,
             RewriteResult *totals, QStringList *files, QStringList *warnings, QString *error,
             bool collectChanges, const ReferenceRenamer::ProgressCallback &progress,
             QHash<QString, QString> *appliedRenames = nullptr)
{
    QHash<QString, QVector<PendingRename>> pendingByFile;
    QStringList unsafeIds;
    QStringList blockedReferenceIds;
    sc2dh::refs::UnifiedReferenceIndex referenceIndex;
    referenceIndex.build(analysis);
    QSet<QString> explicitIdentityKeys;
    struct LinkedCollectionCandidate {
        QString oldId;
        QString newId;
        QStringList collectionElements;
    };
    QVector<LinkedCollectionCandidate> linkedCollectionCandidates;
    for (const RenamePlanItem &item : plan.items) {
        if (!item.selected || item.blocked) continue;
        if (!sc2dh::isSafeAutomaticObjectId(item.oldId) || sc2dh::isReservedCatalogToken(item.oldId)) {
            unsafeIds << item.oldId;
            continue;
        }
        const DataNode &node = analysis.nodes[item.nodeIndex];
        if (sc2dh::isProtectedCatalogNode(node)) {
            unsafeIds << item.oldId;
            continue;
        }
        if (referenceIndex.hasNonRewritableStrongReferenceToId(item.oldId)) {
            const QString summary = blockingReferenceSummary(referenceIndex, item.oldId);
            blockedReferenceIds << QStringLiteral("%1 (%2)").arg(item.oldId, summary);
            continue;
        }
        PendingRename pending;
        pending.oldId = item.oldId;
        pending.newId = item.newId;
        pending.elementName = node.elementName;
        pending.sourceFile = node.sourceFile;
        pending.expectedLocation = node.originalLocation;
        pendingByFile[node.sourceFile].append(pending);
        explicitIdentityKeys.insert(catalogLookupKey(node.elementName, item.oldId));

        const QStringList linkedCollections = linkedDataCollectionElements(node.elementName);
        if (!linkedCollections.isEmpty())
            linkedCollectionCandidates.append({item.oldId, item.newId, linkedCollections});
    }
    QSet<QString> implicitIdentityKeys;
    for (const LinkedCollectionCandidate &candidate : std::as_const(linkedCollectionCandidates)) {
        if (!sc2dh::isSafeAutomaticObjectId(candidate.oldId) || !sc2dh::isSafeAutomaticObjectId(candidate.newId))
            continue;
        for (const DataNode &node : analysis.nodes) {
            if (node.id != candidate.oldId || isDefaultDataCollectionNode(node))
                continue;
            if (!candidate.collectionElements.contains(node.elementName, Qt::CaseInsensitive))
                continue;
            const QString key = catalogLookupKey(node.elementName, node.id);
            if (explicitIdentityKeys.contains(key) || implicitIdentityKeys.contains(key))
                continue;
            PendingRename pending;
            pending.oldId = candidate.oldId;
            pending.newId = candidate.newId;
            pending.elementName = node.elementName;
            pending.sourceFile = node.sourceFile;
            pending.expectedLocation = node.originalLocation;
            pendingByFile[node.sourceFile].append(pending);
            implicitIdentityKeys.insert(key);
        }
    }
    if (!unsafeIds.isEmpty() && warnings) {
        unsafeIds.removeDuplicates();
        std::sort(unsafeIds.begin(), unsafeIds.end());
        *warnings << QStringLiteral("Skipped %1 rename item(s) with numeric or unsafe IDs: %2")
                         .arg(unsafeIds.size())
                         .arg(unsafeIds.mid(0, 12).join(QStringLiteral(", "))
                              + (unsafeIds.size() > 12 ? QStringLiteral(", ...") : QString()));
    }
    if (!blockedReferenceIds.isEmpty() && warnings) {
        blockedReferenceIds.removeDuplicates();
        std::sort(blockedReferenceIds.begin(), blockedReferenceIds.end());
        *warnings << QStringLiteral("Skipped %1 rename item(s) because non-rewritable strong references still point at the old ID: %2")
                         .arg(blockedReferenceIds.size())
                         .arg(blockedReferenceIds.mid(0, 12).join(QStringLiteral("; "))
                              + (blockedReferenceIds.size() > 12 ? QStringLiteral("; ...") : QString()));
    }
    if (pendingByFile.isEmpty()) {
        *error = QStringLiteral("No selected rename items are available.");
        return false;
    }

    QSet<QString> existingIdentityKeys;
    int fileIndex = 0;
    const int totalFiles = analysis.scannedFiles.size();
    for (const ScannedFileInfo &info : analysis.scannedFiles) {
        if (progress)
            progress(QStringLiteral("locate"), fileIndex, totalFiles, info.filePath);
        ++fileIndex;
        if (!info.isXml)
            continue;
        auto targetsIt = pendingByFile.find(info.filePath);
        QByteArray bytes;
        if (!readFile(info.filePath, &bytes, error)) return false;
        pugi::xml_document doc;
        const pugi::xml_parse_result parsed = doc.load_buffer(bytes.constData(), size_t(bytes.size()));
        if (!parsed) { *error = QStringLiteral("Cannot parse %1: %2").arg(info.filePath, parsed.description()); return false; }
        collectIdentityKeys(doc, &existingIdentityKeys);
        if (targetsIt == pendingByFile.end())
            continue;
        QHash<QString, int> targetIndexByElementAndId;
        QHash<QString, int> targetIndexByLocation;
        for (int targetIndex = 0; targetIndex < targetsIt.value().size(); ++targetIndex) {
            const PendingRename &target = targetsIt.value().at(targetIndex);
            targetIndexByElementAndId.insert(fastLookupKey(target.elementName, target.oldId), targetIndex);
            if (!target.expectedLocation.isEmpty())
                targetIndexByLocation.insert(target.expectedLocation, targetIndex);
        }
        locateIdentityTargets(doc, &targetIndexByElementAndId, &targetIndexByLocation, &targetsIt.value());
    }

    RenameTargetMap renames;
    QHash<QString, QHash<QString, QString>> identities;
    QStringList missing;
    QSet<QString> missingOldIds;
    for (auto it = pendingByFile.begin(); it != pendingByFile.end(); ++it) {
        for (const PendingRename &pending : std::as_const(it.value())) {
            if (!pending.found) {
                missing << pending.oldId;
                missingOldIds.insert(pending.oldId);
                continue;
            }
            renames[pending.oldId].append({pending.newId, pending.elementName});
            identities[it.key()].insert(pending.actualLocation, pending.newId);
        }
    }
    QSet<QString> movingFoundOldKeys;
    for (auto it = pendingByFile.cbegin(); it != pendingByFile.cend(); ++it) {
        for (const PendingRename &pending : std::as_const(it.value())) {
            if (pending.found)
                movingFoundOldKeys.insert(catalogLookupKey(pending.elementName, pending.oldId));
        }
    }

    {
        bool removedDependent = true;
        while (removedDependent) {
            removedDependent = false;
            for (auto it = pendingByFile.begin(); it != pendingByFile.end(); ++it) {
                for (const PendingRename &pending : std::as_const(it.value())) {
                    if (!pending.found || !containsTarget(renames, pending))
                        continue;
                    const bool targetStayedOccupied = existingIdentityKeys.contains(catalogLookupKey(pending.elementName, pending.newId))
                        && (!movingFoundOldKeys.contains(catalogLookupKey(pending.elementName, pending.newId))
                            || !renames.contains(pending.newId));
                    if (!missingOldIds.contains(pending.newId) && !targetStayedOccupied)
                        continue;
                    removeTarget(&renames, pending);
                    identities[it.key()].remove(pending.actualLocation);
                    missing << QStringLiteral("%1 (target %2 stayed occupied or was not moved)").arg(pending.oldId, pending.newId);
                    missingOldIds.insert(pending.oldId);
                    removedDependent = true;
                }
            }
        }
    }
    if (!missing.isEmpty() && warnings) {
        std::sort(missing.begin(), missing.end());
        *warnings << QStringLiteral("Skipped %1 rename item(s) because their XML identity could not be located after earlier apply steps: %2")
                         .arg(missing.size())
                         .arg(missing.mid(0, 12).join(QStringLiteral(", "))
                              + (missing.size() > 12 ? QStringLiteral(", ...") : QString()));
    }
    if (renames.isEmpty()) {
        *error = QStringLiteral("No selected object identity could be located safely.");
        return false;
    }
    if (appliedRenames)
        *appliedRenames = unambiguousRenames(renames);

    fileIndex = 0;
    for (const ScannedFileInfo &info : analysis.scannedFiles) {
        if (progress)
            progress(QStringLiteral("rewrite"), fileIndex, totalFiles, info.filePath);
        ++fileIndex;
        if (!info.isXml) continue;
        QByteArray bytes;
        if (!readFile(info.filePath, &bytes, error)) return false;
        pugi::xml_document doc;
        const pugi::xml_parse_result parsed = doc.load_buffer(bytes.constData(), size_t(bytes.size()));
        if (!parsed) { *error = QStringLiteral("Cannot parse %1: %2").arg(info.filePath, parsed.description()); return false; }
        RewriteResult fileResult;
        rewrite(doc, info.filePath, identities.value(info.filePath), renames, &fileResult, collectChanges);
        if (fileResult.identities || fileResult.references) {
            std::ostringstream stream;
            doc.save(stream, "  ", pugi::format_default, pugi::encoding_utf8);
            staged->insert(info.filePath, QByteArray::fromStdString(stream.str()));
            files->append(info.filePath);
            totals->identities += fileResult.identities;
            totals->references += fileResult.references;
            totals->changes += fileResult.changes;
        }
    }
    for (const ScannedFileInfo &info : analysis.scannedFiles) {
        const QString relative = relativeAnalysisPath(analysis, info.filePath);
        if (!shouldRewriteSafeTextReferenceFile(info, relative))
            continue;
        QByteArray original;
        if (!readFile(info.filePath, &original, error))
            return false;
        if (!containsRenameTokenBytes(original, renames))
            continue;
        QByteArray rewritten;
        int replacements = 0;
        if (!rewriteSafeTextBytes(original, renames, &rewritten, &replacements)) {
            *error = QStringLiteral("Reference file contains renamed IDs but is not safe text: %1").arg(relative);
            return false;
        }
        if (replacements <= 0 || rewritten == original)
            continue;
        staged->insert(info.filePath, rewritten);
        if (!files->contains(info.filePath))
            files->append(info.filePath);
        totals->references += replacements;
        if (collectChanges)
            totals->changes << QStringLiteral("%1: %2 token-aware text reference replacement(s)")
                                    .arg(info.filePath)
                                    .arg(replacements);
    }
    int expectedIdentities = 0;
    for (auto it = identities.cbegin(); it != identities.cend(); ++it)
        expectedIdentities += it.value().size();
    if (totals->identities != expectedIdentities) {
        *error = QStringLiteral("Not every selected object identity could be rewritten safely.");
        return false;
    }
    if (progress)
        progress(QStringLiteral("rewrite"), totalFiles, totalFiles, QString());
    return true;
}

} // namespace

RenamePreviewReport ReferenceRenamer::preview(const AnalysisResult &analysis, const RenamePlan &plan) const
{
    RenamePreviewReport result;
    result.plan = plan;
    result.conflicts = plan.conflicts;
    result.warnings = plan.warnings;
    QHash<QString, QByteArray> staged;
    RewriteResult rewriteResult;
    QStringList prepareWarnings;
    QString error;
    if (plan.valid && !prepare(analysis, plan, &staged, &rewriteResult, &result.filesChanged,
                               &prepareWarnings, &error, true, {})) {
        const QString suffix = QFileInfo(analysis.rootFolder).suffix().toLower();
        const bool archive = suffix.startsWith(QStringLiteral("sc2"));
        if (!archive) {
            result.conflicts << error;
        } else {
            // Archive extraction is ephemeral; provide a serialized-node planning
            // preview while keeping Apply disabled at the UI boundary.
            RenameTargetMap renames;
            for (const RenamePlanItem &item : plan.items) {
                if (item.nodeIndex < 0 || item.nodeIndex >= analysis.nodes.size())
                    continue;
                const DataNode &node = analysis.nodes[item.nodeIndex];
                if (sc2dh::isSafeAutomaticObjectId(item.oldId) && !sc2dh::isProtectedCatalogNode(node))
                    renames[item.oldId].append({item.newId, node.elementName});
            }
            rewriteResult.identities = plan.items.size();
            QSet<QString> files;
            for (const DataNode &node : analysis.nodes) {
                files.insert(node.sourceFile);
                QString value = node.serializedXml;
                int count = simultaneousReplace(&value, renames);
                if (renames.contains(node.id) && count > 0) --count;
                if (count > 0) rewriteResult.references += count;
            }
            result.filesChanged = files.values();
            result.warnings << QStringLiteral("Archive mode is preview-only; reference locations are estimated from extracted serialized XML.");
            error.clear();
        }
    }
    result.warnings.append(prepareWarnings);
    result.identitiesRenamed = rewriteResult.identities;
    result.referencesUpdated = rewriteResult.references;
    result.referenceChanges = rewriteResult.changes;
    result.valid = plan.valid && result.conflicts.isEmpty() && rewriteResult.identities > 0;
    result.reportText = buildReport(analysis, plan, rewriteResult, result.filesChanged, result.warnings,
                                    result.conflicts, QStringLiteral("Preview only; no files modified"));
    return result;
}

RenameApplyResult ReferenceRenamer::apply(const AnalysisResult &analysis, const RenamePlan &plan,
                                          const QString &rootFolder, const QSet<QString> &whitelistIds,
                                          const ProgressCallback &progress) const
{
    RenameApplyResult result;
    if (!plan.valid) {
        result.error = plan.conflicts.join(QStringLiteral("; "));
        return result;
    }
    QHash<QString, QByteArray> staged;
    RewriteResult rewriteResult;
    QStringList absoluteFiles;
    QStringList warnings;
    QHash<QString, QString> appliedRenames;
    if (!prepare(analysis, plan, &staged, &rewriteResult, &absoluteFiles,
                 &warnings, &result.error, false, progress, &appliedRenames)) {
        result.warnings = warnings;
        return result;
    }
    result.warnings = warnings;
    result.appliedRenames = appliedRenames;
    if (rewriteResult.identities <= 0) {
        result.error = QStringLiteral("No selected object identity could be renamed.");
        return result;
    }
    const QString previewReportText = buildReport(analysis, plan, rewriteResult, absoluteFiles,
                                                  plan.warnings + warnings, plan.conflicts,
                                                  QStringLiteral("Apply staged; no files committed yet"));
    QStringList relativeFiles;
    for (const QString &file : absoluteFiles) relativeFiles << QDir(rootFolder).relativeFilePath(file);
    std::sort(relativeFiles.begin(), relativeFiles.end());
    BackupManager backup;
    if (progress)
        progress(QStringLiteral("backup"), 0, 1, QString());
    if (!backup.createFolderBackup(rootFolder, relativeFiles, analysis.analysisReportText,
                                   previewReportText, &result.backupFolder, &result.error)) return result;
    if (m_failureInjectionStep == QStringLiteral("after-backup")) { result.error = QStringLiteral("Injected failure after backup."); return result; }
    QStringList committed;
    int writeIndex = 0;
    const int writeTotal = staged.size();
    for (auto it = staged.cbegin(); it != staged.cend(); ++it) {
        if (progress)
            progress(QStringLiteral("write"), writeIndex, writeTotal, it.key());
        ++writeIndex;
        QSaveFile file(it.key());
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || file.write(it.value()) != it.value().size() || !file.commit()) {
            result.error = QStringLiteral("Failed to commit %1.").arg(it.key());
            restore(rootFolder, result.backupFolder, committed, &result.error);
            return result;
        }
        committed << QDir(rootFolder).relativeFilePath(it.key());
    }
    if (progress)
        progress(QStringLiteral("verify"), 0, 1, QString());
    if (m_failureInjectionStep == QStringLiteral("after-commit")) {
        result.error = QStringLiteral("Injected failure after commit.");
        restore(rootFolder, result.backupFolder, relativeFiles, &result.error);
        return result;
    }
    FolderAnalyzer analyzer;
    AnalysisResult rebuilt;
    QString verifyError;
    if (!analyzer.analyzeFolder(rootFolder, whitelistIds, &rebuilt, &verifyError)) {
        result.error = QStringLiteral("Re-analysis failed: %1").arg(verifyError);
        restore(rootFolder, result.backupFolder, relativeFiles, &result.error);
        return result;
    }
    QSet<QString> newIds;
    for (auto it = appliedRenames.cbegin(); it != appliedRenames.cend(); ++it)
        newIds.insert(it.value());
    QSet<QString> oldIdsToCheck;
    QHash<QString, QSet<QString>> oldScopesById;
    QHash<QString, QSet<QString>> newScopesById;
    QStringList postErrors;
    for (const RenamePlanItem &item : plan.items) {
        if (appliedRenames.value(item.oldId) != item.newId)
            continue;
        if (item.nodeIndex < 0 || item.nodeIndex >= analysis.nodes.size())
            continue;
        const QString scope = sc2dh::catalogIdentityScope(analysis.nodes.at(item.nodeIndex).elementName);
        newScopesById[item.newId].insert(scope);
        if (!newIds.contains(item.oldId)) {
            oldIdsToCheck.insert(item.oldId);
            oldScopesById[item.oldId].insert(scope);
        }
    }
    for (auto it = newScopesById.cbegin(); it != newScopesById.cend(); ++it) {
        bool found = false;
        for (const DataNode &node : rebuilt.nodes) {
            if (node.id == it.key() && it.value().contains(sc2dh::catalogIdentityScope(node.elementName))) {
                found = true;
                break;
            }
        }
        if (!found)
            postErrors << QStringLiteral("Refreshed analysis did not expose renamed ID %1 in the expected catalog scope.")
                              .arg(it.key());
    }
    for (auto it = oldScopesById.cbegin(); it != oldScopesById.cend(); ++it) {
        for (const DataNode &node : rebuilt.nodes) {
            if (node.id == it.key() && it.value().contains(sc2dh::catalogIdentityScope(node.elementName))) {
                postErrors << QStringLiteral("Refreshed analysis still exposes old ID %1 in %2.")
                                  .arg(it.key(), node.elementName);
                break;
            }
        }
    }
    if (!oldIdsToCheck.isEmpty()) {
        for (const DataNode &node : rebuilt.nodes) {
            for (const QString &reference : node.referencedIds) {
                if (oldIdsToCheck.contains(reference)) {
                    if (isParentAttributeReferenceOnly(node, reference))
                        continue;
                    postErrors << QStringLiteral("A refreshed-analysis strong reference to old ID %1 remains in %2.")
                                        .arg(reference, node.id);
                    break;
                }
            }
        }
    }
    if (!postErrors.isEmpty()) {
        if (postErrors.size() > 20)
            postErrors = postErrors.mid(0, 20) << QStringLiteral("... %1 more post-rename verification error(s).")
                                                   .arg(postErrors.size() - 20);
        result.error = QStringLiteral("Post-rename verification failed:\n- %1")
                           .arg(postErrors.join(QStringLiteral("\n- ")));
        restore(rootFolder, result.backupFolder, relativeFiles, &result.error);
        return result;
    }
    result.success = true;
    result.changedFiles = relativeFiles;
    result.identitiesRenamed = rewriteResult.identities;
    result.referencesUpdated = rewriteResult.references;
    result.finalReport = previewReportText + QStringLiteral("\nFinal result after apply: success\nBackup: %1\n").arg(result.backupFolder);
    return result;
}
