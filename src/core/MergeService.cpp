#include "core/MergeService.h"

#include "core/ArchiveReferenceRewriter.h"
#include "core/BackupManager.h"
#include "core/CatalogProtection.h"
#include "core/FolderAnalyzer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <pugixml.hpp>

#include <algorithm>
#include <sstream>

namespace {

QRegularExpression idExpression(const QString &id)
{
    return QRegularExpression(QStringLiteral("(?<![A-Za-z0-9_@])%1(?![A-Za-z0-9_@])")
                                  .arg(QRegularExpression::escape(id)));
}

QRegularExpression redirectExpression(const QHash<QString, QString> &redirects)
{
    QStringList ids = redirects.keys();
    std::sort(ids.begin(), ids.end(), [](const QString &left, const QString &right) {
        return left.size() > right.size();
    });
    QStringList escaped;
    escaped.reserve(ids.size());
    for (const QString &id : ids)
        escaped << QRegularExpression::escape(id);
    return QRegularExpression(QStringLiteral("(?<![A-Za-z0-9_@])(%1)(?![A-Za-z0-9_@])")
                                  .arg(escaped.join(QLatin1Char('|'))));
}

int replaceRedirectTokens(QString *value,
                          const QHash<QString, QString> &redirects,
                          const QRegularExpression &expression)
{
    if (!value || value->isEmpty() || redirects.isEmpty())
        return 0;
    QString output;
    qsizetype last = 0;
    int replacements = 0;
    auto matches = expression.globalMatch(*value);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        output += value->mid(last, match.capturedStart() - last);
        const QString oldId = match.captured(1);
        if (!sc2dh::isSafeAutomaticObjectId(oldId) || sc2dh::isReservedCatalogToken(oldId)) {
            output += oldId;
            last = match.capturedEnd();
            continue;
        }
        output += redirects.value(oldId, oldId);
        last = match.capturedEnd();
        ++replacements;
    }
    if (replacements > 0) {
        output += value->mid(last);
        *value = output;
    }
    return replacements;
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
    const bool actorEventValue = isInsideActorEvent(node)
        && (fieldName.compare(QStringLiteral("Terms"), Qt::CaseInsensitive) == 0
            || fieldName.compare(QStringLiteral("Send"), Qt::CaseInsensitive) == 0
            || fieldName.isEmpty());
    if ((!actorEventValue && sc2dh::isNonReferenceCatalogFieldName(fieldName))
        || (!actorEventValue && sc2dh::looksLikeCatalogFilterList(value)))
        return false;

    for (pugi::xml_node current = node; current && current.type() == pugi::node_element; current = current.parent()) {
        if (!isInsideActorEvent(current)
            && sc2dh::isNonReferenceCatalogFieldName(QString::fromUtf8(current.name())))
            return false;
    }
    return true;
}

QStringList nonXmlReferenceFiles(const AnalysisResult &analysis)
{
    QStringList files;
    for (const ScannedFileInfo &info : analysis.scannedFiles) {
        if (info.isXml)
            continue;
        QString relative = QDir(analysis.rootFolder).relativeFilePath(info.filePath);
        relative = QDir::cleanPath(relative).replace('\\', '/');
        if (relative.isEmpty() || relative.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(relative))
            continue;
        files << relative;
    }
    files.removeDuplicates();
    std::sort(files.begin(), files.end());
    return files;
}

QHash<QString, QString> archiveSafeRedirects(const AnalysisResult &analysis,
                                             const QHash<QString, QString> &redirects,
                                             QStringList *skippedIds)
{
    return sc2dh::unambiguousArchiveReferenceRenames(analysis, redirects, skippedIds);
}

bool loadFile(const QString &path, QByteArray *bytes, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Unable to read %1").arg(path);
        return false;
    }
    *bytes = file.readAll();
    return true;
}

QString nodePath(const pugi::xml_node &node)
{
    QStringList parts;
    for (pugi::xml_node current = node; current && current.type() == pugi::node_element; current = current.parent()) {
        int index = 1;
        for (pugi::xml_node previous = current.previous_sibling(current.name()); previous;
             previous = previous.previous_sibling(current.name())) {
            ++index;
        }
        parts.prepend(QStringLiteral("%1[%2]").arg(QString::fromUtf8(current.name())).arg(index));
    }
    return QStringLiteral("/") + parts.join(QLatin1Char('/'));
}

pugi::xml_node findObject(pugi::xml_document &doc, const QString &element, const QString &id)
{
    const pugi::xpath_node_set matches = doc.select_nodes("//*[@id]");
    for (const pugi::xpath_node &match : matches) {
        pugi::xml_node node = match.node();
        if (QString::fromUtf8(node.name()) == element
            && QString::fromUtf8(node.attribute("id").value()) == id) {
            return node;
        }
    }
    return {};
}

bool isTopLevelCatalogIdentity(const pugi::xml_node &node)
{
    pugi::xml_node parent = node.parent();
    return parent
        && parent.type() == pugi::node_element
        && QString::fromUtf8(parent.name()) == QStringLiteral("Catalog")
        && node.attribute("id");
}

struct RewriteStats {
    int fields = 0;
    int references = 0;
    QStringList changes;
};

void rewriteNode(pugi::xml_node node,
                 const QHash<QString, QString> &redirects,
                 const QRegularExpression &redirectRegex,
                 const QString &file,
                 const QSet<QString> &removedIdentityLocations,
                 RewriteStats *stats)
{
    if (node.type() == pugi::node_element) {
        for (pugi::xml_attribute attribute : node.attributes()) {
            // Top-level catalog identities are deleted separately; duplicate
            // merge redirects references only and must not rename surviving
            // objects whose IDs happen to contain a removed token.
            if (QString::fromUtf8(attribute.name()) == QStringLiteral("id")
                && (removedIdentityLocations.contains(nodePath(node)) || isTopLevelCatalogIdentity(node))) {
                continue;
            }
            QString value = QString::fromUtf8(attribute.value());
            const QString before = value;
            if (!shouldRewriteReferenceValue(node, QString::fromUtf8(attribute.name()), value))
                continue;
            const int replacements = replaceRedirectTokens(&value, redirects, redirectRegex);
            if (replacements > 0) {
                attribute.set_value(value.toUtf8().constData());
                ++stats->fields;
                stats->references += replacements;
                stats->changes.append(QStringLiteral("%1 %2 @%3: %4 -> %5")
                                          .arg(file, nodePath(node), QString::fromUtf8(attribute.name()), before, value));
            }
        }
    }
    if (node.type() == pugi::node_pcdata || node.type() == pugi::node_cdata) {
        QString value = QString::fromUtf8(node.value());
        const QString before = value;
        if (!shouldRewriteReferenceValue(node.parent(), QString(), value))
            return;
        const int replacements = replaceRedirectTokens(&value, redirects, redirectRegex);
        if (replacements > 0) {
            node.set_value(value.toUtf8().constData());
            ++stats->fields;
            stats->references += replacements;
            stats->changes.append(QStringLiteral("%1 %2 text: %3 -> %4")
                                      .arg(file, nodePath(node.parent()), before, value));
        }
    }
    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling()) {
        rewriteNode(child, redirects, redirectRegex, file, removedIdentityLocations, stats);
    }
}

QString mergeNodeLabel(const DataNode &node)
{
    return QStringLiteral("%1(%2)").arg(node.elementName.isEmpty() ? QStringLiteral("Unknown") : node.elementName,
                                        node.id.isEmpty() ? QStringLiteral("<no id>") : node.id);
}

bool postMergeStrongReferenceAudit(const AnalysisResult &rebuilt,
                                   const QHash<QString, QSet<QString>> &removedScopesById,
                                   QString *error)
{
    for (auto it = removedScopesById.cbegin(); it != removedScopesById.cend(); ++it) {
        const QString &removedId = it.key();
        for (const DataNode &node : rebuilt.nodes) {
            if (node.id == removedId && it.value().contains(sc2dh::catalogIdentityScope(node.elementName))) {
                if (error)
                    *error = QStringLiteral("Post-merge audit failed: removed %1 still exists as %2.")
                                 .arg(removedId, mergeNodeLabel(node));
                return false;
            }
            if (node.referencedIds.contains(removedId)) {
                if (error)
                    *error = QStringLiteral("Post-merge audit failed: %1 still has a strong catalog reference to removed ID %2.")
                                 .arg(mergeNodeLabel(node), removedId);
                return false;
            }
        }
    }
    return true;
}

bool normalizedTransactionPath(const QString &rootFolder,
                               const QString &path,
                               QString *relativePathOut,
                               QString *error)
{
    QString relative = path;
    if (QDir::isAbsolutePath(relative))
        relative = QDir(rootFolder).relativeFilePath(relative);
    relative = QDir::cleanPath(relative).replace('\\', '/');

    const QString root = QDir::cleanPath(QFileInfo(rootFolder).absoluteFilePath()).replace('\\', '/');
    const QString absolute = QDir::cleanPath(QDir(root).absoluteFilePath(relative)).replace('\\', '/');
    if (relative.isEmpty() || relative == QStringLiteral(".") || relative == QStringLiteral("..")
        || relative.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(relative)
        || absolute == root || !absolute.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive)) {
        if (error)
            *error = QStringLiteral("Unsafe merge transaction path: %1").arg(path);
        return false;
    }
    if (relativePathOut)
        *relativePathOut = relative;
    return true;
}

struct PreparedMergeTransaction
{
    QVector<TransactionalFileChange> changes;
    QStringList changedFiles;
    QSet<QString> xmlFiles;
    int archiveReferencesRedirected = 0;
};

bool prepareMergeTransaction(const QString &rootFolder,
                             const QHash<QString, QByteArray> &stagedXml,
                             const QStringList &archiveFiles,
                             const QHash<QString, QString> &archiveRedirects,
                             PreparedMergeTransaction *prepared,
                             QString *error)
{
    if (!prepared) {
        if (error)
            *error = QStringLiteral("Missing prepared merge transaction output.");
        return false;
    }
    *prepared = {};

    sc2dh::ArchiveReferenceRewriteReport archivePreview;
    if (!sc2dh::previewArchiveReferenceFileRewrites(rootFolder,
                                                     archiveFiles,
                                                     archiveRedirects,
                                                     &archivePreview,
                                                     error)) {
        return false;
    }

    QHash<QString, QByteArray> contentsByKey;
    QHash<QString, QString> pathsByKey;
    QSet<QString> xmlKeys;
    const auto appendChange = [&](const QString &path, const QByteArray &contents, bool isXml) {
        QString relative;
        if (!normalizedTransactionPath(rootFolder, path, &relative, error))
            return false;
        const QString key = relative.toCaseFolded();
        if (contentsByKey.contains(key)) {
            if (error)
                *error = QStringLiteral("Duplicate merge transaction path: %1").arg(relative);
            return false;
        }
        contentsByKey.insert(key, contents);
        pathsByKey.insert(key, relative);
        if (isXml)
            xmlKeys.insert(key);
        return true;
    };

    QStringList xmlSourceFiles = stagedXml.keys();
    std::sort(xmlSourceFiles.begin(), xmlSourceFiles.end());
    for (const QString &file : xmlSourceFiles) {
        if (!appendChange(file, stagedXml.value(file), true))
            return false;
    }

    if (!archivePreview.changedFiles.isEmpty()) {
        QTemporaryDir rewriteStaging(QDir::tempPath() + QStringLiteral("/sc2dh-merge-rewrite-XXXXXX"));
        if (!rewriteStaging.isValid()) {
            if (error)
                *error = QStringLiteral("Unable to create archive rewrite staging directory.");
            return false;
        }

        for (const QString &archiveFile : archivePreview.changedFiles) {
            QString relative;
            if (!normalizedTransactionPath(rootFolder, archiveFile, &relative, error))
                return false;
            const QString source = QDir(rootFolder).absoluteFilePath(relative);
            const QString staged = QDir(rewriteStaging.path()).absoluteFilePath(relative);
            if (!QDir().mkpath(QFileInfo(staged).absolutePath()) || !QFile::copy(source, staged)) {
                if (error)
                    *error = QStringLiteral("Unable to stage archive reference file: %1").arg(source);
                return false;
            }
        }

        sc2dh::ArchiveReferenceRewriteReport archiveRewrite;
        if (!sc2dh::rewriteArchiveReferenceFiles(rewriteStaging.path(),
                                                  archivePreview.changedFiles,
                                                  archiveRedirects,
                                                  &archiveRewrite,
                                                  error)) {
            return false;
        }

        for (const QString &archiveFile : archiveRewrite.changedFiles) {
            QString relative;
            if (!normalizedTransactionPath(rootFolder, archiveFile, &relative, error))
                return false;
            QByteArray contents;
            if (!loadFile(QDir(rewriteStaging.path()).absoluteFilePath(relative), &contents, error))
                return false;
            if (!appendChange(relative, contents, false))
                return false;
        }
        prepared->archiveReferencesRedirected = archiveRewrite.replacements;
    }

    QStringList keys = pathsByKey.keys();
    std::sort(keys.begin(), keys.end(), [&pathsByKey](const QString &left, const QString &right) {
        return pathsByKey.value(left).compare(pathsByKey.value(right), Qt::CaseInsensitive) < 0;
    });
    prepared->changes.reserve(keys.size());
    for (const QString &key : keys) {
        const QString &relative = pathsByKey.value(key);
        prepared->changes.push_back({relative, contentsByKey.value(key), false});
        prepared->changedFiles << relative;
        if (xmlKeys.contains(key))
            prepared->xmlFiles.insert(relative);
    }
    return true;
}

bool validateStagedMergeXml(const QString &stagingFolder,
                            const QSet<QString> &xmlFiles,
                            QString *error)
{
    QStringList files = xmlFiles.values();
    std::sort(files.begin(), files.end());
    for (const QString &relative : files) {
        QByteArray bytes;
        const QString stagedPath = QDir(stagingFolder).absoluteFilePath(relative);
        if (!loadFile(stagedPath, &bytes, error))
            return false;
        pugi::xml_document document;
        const auto parsed = document.load_buffer(bytes.constData(), size_t(bytes.size()));
        if (!parsed) {
            if (error)
                *error = QStringLiteral("Staged merge XML does not parse: %1: %2")
                             .arg(relative, QString::fromUtf8(parsed.description()));
            return false;
        }
    }
    return true;
}

bool validateCommittedMerge(const QString &rootFolder,
                            const QSet<QString> &whitelistIds,
                            const QHash<QString, QSet<QString>> &removedScopesById,
                            const QString &failureInjectionStep,
                            QString *error)
{
    if (failureInjectionStep == QStringLiteral("after-commit")) {
        if (error)
            *error = QStringLiteral("Injected failure after commit.");
        return false;
    }

    FolderAnalyzer analyzer;
    AnalysisResult rebuilt;
    QString analysisError;
    if (!analyzer.analyzeFolder(rootFolder, whitelistIds, &rebuilt, &analysisError)) {
        if (error)
            *error = QStringLiteral("Registry rebuild failed: %1").arg(analysisError);
        return false;
    }
    if (rebuilt.completeness != AnalysisCompleteness::Complete) {
        if (error) {
            *error = QStringLiteral("Post-merge verification analysis is %1.")
                         .arg(analysisCompletenessName(rebuilt.completeness));
        }
        return false;
    }
    return postMergeStrongReferenceAudit(rebuilt, removedScopesById, error);
}

QString mergeTransactionError(const FolderSaveTransactionResult &transaction)
{
    if (!transaction.error.isEmpty())
        return transaction.error;
    return QStringLiteral("Merge transaction failed: %1.")
        .arg(operationErrorCodeName(transaction.errorCode));
}

} // namespace

int MergeService::replaceIdTokens(QString *value, const QString &oldId, const QString &newId)
{
    if (!value || oldId.isEmpty() || oldId == newId || !sc2dh::isSafeAutomaticObjectId(oldId)
        || sc2dh::isReservedCatalogToken(oldId)) return 0;
    const QRegularExpression expression = idExpression(oldId);
    int count = 0;
    auto iterator = expression.globalMatch(*value);
    while (iterator.hasNext()) { iterator.next(); ++count; }
    if (count) value->replace(expression, newId);
    return count;
}

int MergeService::countIdTokens(const QString &value, const QString &id)
{
    if (!sc2dh::isSafeAutomaticObjectId(id) || sc2dh::isReservedCatalogToken(id))
        return 0;
    int count = 0;
    auto iterator = idExpression(id).globalMatch(value);
    while (iterator.hasNext()) { iterator.next(); ++count; }
    return count;
}

MergePreview MergeService::preview(const AnalysisResult &analysis, const MergeRequest &request) const
{
    MergePreview preview;
    if (request.keepNodeIndex < 0 || request.keepNodeIndex >= analysis.nodes.size()) {
        preview.warnings << QStringLiteral("A keep object must be selected.");
        return preview;
    }
    const DataNode &keep = analysis.nodes[request.keepNodeIndex];
    if (sc2dh::isProtectedCatalogNode(keep)) {
        preview.warnings << QStringLiteral("%1 is an editor/runtime catalog object and cannot be merged safely.").arg(keep.id);
        return preview;
    }
    if (!sc2dh::isSafeAutomaticObjectId(keep.id)) {
        preview.warnings << QStringLiteral("%1 has a numeric or unsafe ID and cannot be merged automatically.").arg(keep.id);
        return preview;
    }
    preview.keptId = keep.id;
    QHash<QString, QString> redirects;
    QHash<QString, QSet<QString>> removedLocations;
    QSet<QString> files;
    for (int index : request.removeNodeIndices) {
        if (index < 0 || index >= analysis.nodes.size() || index == request.keepNodeIndex) {
            preview.warnings << QStringLiteral("Invalid remove selection.");
            continue;
        }
        const DataNode &remove = analysis.nodes[index];
        if (sc2dh::isProtectedCatalogNode(remove)) {
            preview.warnings << QStringLiteral("%1 is an editor/runtime catalog object and cannot be removed by duplicate merge.").arg(remove.id);
            continue;
        }
        if (!sc2dh::isSafeAutomaticObjectId(remove.id)) {
            preview.warnings << QStringLiteral("%1 has a numeric or unsafe ID and cannot be removed by duplicate merge.").arg(remove.id);
            continue;
        }
        if (remove.id == keep.id || remove.elementName != keep.elementName || remove.contentHash != keep.contentHash) {
            preview.warnings << QStringLiteral("%1 is not a different-ID exact body duplicate of %2.").arg(remove.id, keep.id);
            continue;
        }
        redirects.insert(remove.id, keep.id);
        removedLocations[remove.sourceFile].insert(remove.originalLocation);
        preview.removedIds << remove.id;
        files.insert(remove.sourceFile);
        ++preview.nodesDeleted;
    }
    if (redirects.isEmpty()) {
        preview.warnings << QStringLiteral("Select at least one exact duplicate to remove.");
        return preview;
    }
    const QRegularExpression redirectsRegex = redirectExpression(redirects);

    for (const ScannedFileInfo &info : analysis.scannedFiles) {
        if (!info.isXml) continue;
        QByteArray bytes;
        QString error;
        if (!loadFile(info.filePath, &bytes, &error)) {
            preview.warnings << error;
            continue;
        }
        pugi::xml_document doc;
        if (!doc.load_buffer(bytes.constData(), size_t(bytes.size()))) {
            preview.warnings << QStringLiteral("Cannot parse %1.").arg(info.filePath);
            continue;
        }
        RewriteStats stats;
        rewriteNode(doc, redirects, redirectsRegex, info.filePath, removedLocations.value(info.filePath), &stats);
        if (stats.fields) files.insert(info.filePath);
        preview.fieldsChanged += stats.fields;
        preview.referencesRedirected += stats.references;
        preview.changes += stats.changes;
    }

    QStringList skippedArchiveIds;
    const QHash<QString, QString> externalRedirects = archiveSafeRedirects(analysis, redirects, &skippedArchiveIds);
    if (!skippedArchiveIds.isEmpty()) {
        preview.changes << QStringLiteral("Skipped non-XML reference rewrites for ambiguous catalog IDs: %1.")
                                .arg(skippedArchiveIds.join(QStringLiteral(", ")));
    }
    sc2dh::ArchiveReferenceRewriteReport archiveReport;
    QString archiveError;
    if (!sc2dh::previewArchiveReferenceFileRewrites(analysis.rootFolder,
                                                   nonXmlReferenceFiles(analysis),
                                                   externalRedirects,
                                                   &archiveReport,
                                                   &archiveError)) {
        preview.warnings << archiveError;
    } else {
        for (const QString &file : archiveReport.changedFiles)
            files.insert(QDir(analysis.rootFolder).absoluteFilePath(file));
        preview.referencesRedirected += archiveReport.replacements;
        if (!archiveReport.changedFiles.isEmpty()) {
            preview.changes << QStringLiteral("Non-XML archive references rewritten in: %1")
                                   .arg(archiveReport.changedFiles.join(QStringLiteral(", ")));
        }
    }

    preview.filesChanged = files.values();
    std::sort(preview.filesChanged.begin(), preview.filesChanged.end());
    preview.riskLevel = preview.warnings.isEmpty()
        ? (preview.referencesRedirected > 100 ? QStringLiteral("medium") : QStringLiteral("low"))
        : QStringLiteral("high");
    preview.valid = preview.warnings.isEmpty();
    preview.reportText = QStringLiteral("Merge Preview Result\nKept object: %1\nRemoved objects: %2\nFiles changed: %3\nFields changed: %4\nReferences redirected: %5\nNodes deleted: %6\nWarnings: %7\nRisk level: %8\n")
                             .arg(preview.keptId, preview.removedIds.join(QStringLiteral(", ")))
                             .arg(preview.filesChanged.size()).arg(preview.fieldsChanged)
                             .arg(preview.referencesRedirected).arg(preview.nodesDeleted)
                             .arg(preview.warnings.isEmpty() ? QStringLiteral("none") : preview.warnings.join(QStringLiteral("; ")),
                                  preview.riskLevel);
    for (const QString &change : preview.changes) preview.reportText += QStringLiteral("- %1\n").arg(change);
    return preview;
}

MergeApplyResult MergeService::apply(const AnalysisResult &analysis,
                                     const MergeRequest &request,
                                     const QString &rootFolder,
                                     const QSet<QString> &whitelistIds) const
{
    MergeApplyResult result;
    const DestructiveOperationPermission permission = canApplyDestructiveChanges(analysis);
    if (!permission.allowed) {
        result.error = destructiveOperationPermissionText(permission);
        return result;
    }
    const MergePreview plan = preview(analysis, request);
    if (!plan.valid) { result.error = plan.warnings.join(QStringLiteral("; ")); return result; }

    QHash<QString, QString> redirects;
    QHash<QString, QVector<const DataNode *>> removals;
    QHash<QString, QSet<QString>> removedScopesById;
    for (int index : request.removeNodeIndices) {
        const DataNode &node = analysis.nodes[index];
        redirects.insert(node.id, plan.keptId);
        removals[node.sourceFile].append(&node);
        removedScopesById[node.id].insert(sc2dh::catalogIdentityScope(node.elementName));
    }
    const QRegularExpression redirectsRegex = redirectExpression(redirects);
    QStringList skippedArchiveIds;
    const QHash<QString, QString> externalRedirects = archiveSafeRedirects(analysis, redirects, &skippedArchiveIds);
    if (!skippedArchiveIds.isEmpty()) {
        result.warnings << QStringLiteral("Skipped non-XML reference rewrites for ambiguous catalog IDs: %1.")
                               .arg(skippedArchiveIds.join(QStringLiteral(", ")));
    }

    QHash<QString, QByteArray> staged;
    RewriteStats totals;
    QString error;
    for (const ScannedFileInfo &info : analysis.scannedFiles) {
        if (!info.isXml) continue;
        QByteArray bytes;
        if (!loadFile(info.filePath, &bytes, &error)) { result.error = error; return result; }
        pugi::xml_document doc;
        const auto parsed = doc.load_buffer(bytes.constData(), size_t(bytes.size()));
        if (!parsed) { result.error = QStringLiteral("Cannot parse %1: %2").arg(info.filePath, parsed.description()); return result; }
        RewriteStats fileStats;
        QSet<QString> identityLocations;
        for (const DataNode *remove : removals.value(info.filePath)) identityLocations.insert(remove->originalLocation);
        rewriteNode(doc, redirects, redirectsRegex, info.filePath, identityLocations, &fileStats);
        int deleted = 0;
        for (const DataNode *remove : removals.value(info.filePath)) {
            pugi::xml_node node = findObject(doc, remove->elementName, remove->id);
            if (!node || !node.parent().remove_child(node)) {
                result.error = QStringLiteral("Unable to delete %1 from %2.").arg(remove->id, info.filePath);
                return result;
            }
            ++deleted;
        }
        if (fileStats.fields || deleted) {
            std::ostringstream stream;
            doc.save(stream, "  ", pugi::format_default, pugi::encoding_utf8);
            const QByteArray output = QByteArray::fromStdString(stream.str());
            const QRegularExpressionMatch remaining = redirectsRegex.match(QString::fromUtf8(output));
            if (remaining.hasMatch()) {
                result.warnings << QStringLiteral("Duplicate merge kept residual old ID token %1 in %2 for manual review.")
                                       .arg(remaining.captured(1), info.filePath);
            }
            staged.insert(info.filePath, output);
            totals.fields += fileStats.fields;
            totals.references += fileStats.references;
        }
    }
    if (staged.isEmpty()) { result.error = QStringLiteral("Merge produced no file changes."); return result; }

    PreparedMergeTransaction prepared;
    if (!prepareMergeTransaction(rootFolder,
                                 staged,
                                 nonXmlReferenceFiles(analysis),
                                 externalRedirects,
                                 &prepared,
                                 &error)) {
        result.error = error;
        return result;
    }

    BackupManager backups;
    const QString failureInjectionStep = m_failureInjectionStep;
    const FolderSaveTransactionResult transaction = backups.applyFolderTransaction(
        rootFolder,
        prepared.changes,
        analysis.analysisReportText,
        plan.reportText,
        [&prepared](const QString &stagingFolder, QString *validationError) {
            return validateStagedMergeXml(stagingFolder, prepared.xmlFiles, validationError);
        },
        [rootFolder, whitelistIds, removedScopesById, failureInjectionStep](QString *validationError) {
            return validateCommittedMerge(rootFolder,
                                          whitelistIds,
                                          removedScopesById,
                                          failureInjectionStep,
                                          validationError);
        },
        failureInjectionStep);
    result.backupFolder = transaction.backupFolder;
    if (!transaction.success) {
        result.error = mergeTransactionError(transaction);
        return result;
    }

    result.success = true;
    result.changedFiles = transaction.changedFiles;
    result.referencesRedirected = totals.references + prepared.archiveReferencesRedirected;
    result.nodesDeleted = plan.nodesDeleted;
    return result;
}

MergeApplyResult MergeService::applyBatch(const AnalysisResult &analysis,
                                          const QVector<MergeRequest> &requests,
                                          const QString &rootFolder,
                                          const QSet<QString> &whitelistIds,
                                          const std::function<void(int, int, const QString &)> &progress) const
{
    MergeApplyResult result;
    const DestructiveOperationPermission permission = canApplyDestructiveChanges(analysis);
    if (!permission.allowed) {
        result.error = destructiveOperationPermissionText(permission);
        return result;
    }
    if (requests.isEmpty()) {
        result.error = QStringLiteral("No duplicate merge requests selected.");
        return result;
    }

    QSet<QString> idsSelectedForRemoval;
    for (const MergeRequest &request : requests) {
        for (int index : request.removeNodeIndices) {
            if (index >= 0 && index < analysis.nodes.size())
                idsSelectedForRemoval.insert(analysis.nodes[index].id);
        }
    }

    QHash<QString, QString> redirects;
    QHash<QString, QVector<const DataNode *>> removals;
    QHash<QString, QSet<QString>> removedScopesById;
    QSet<int> removedNodeIndexes;
    QStringList removedIds;

    for (const MergeRequest &request : requests) {
        if (request.keepNodeIndex < 0 || request.keepNodeIndex >= analysis.nodes.size()) {
            ++result.skippedMerges;
            result.warnings << QStringLiteral("Skipped duplicate merge with missing keep object.");
            continue;
        }
        const DataNode &keep = analysis.nodes[request.keepNodeIndex];
        if (sc2dh::isProtectedCatalogNode(keep)) {
            ++result.skippedMerges;
            result.warnings << QStringLiteral("Skipped duplicate merge for protected editor/runtime object %1.").arg(keep.id);
            continue;
        }
        if (!sc2dh::isSafeAutomaticObjectId(keep.id)) {
            ++result.skippedMerges;
            result.warnings << QStringLiteral("Skipped duplicate merge for numeric or unsafe ID %1.").arg(keep.id);
            continue;
        }
        if (idsSelectedForRemoval.contains(keep.id)) {
            ++result.skippedMerges;
            result.warnings << QStringLiteral("Skipped duplicate merge for %1 because its keep ID is also selected for removal.").arg(keep.id);
            continue;
        }
        for (int removeIndex : request.removeNodeIndices) {
            if (removeIndex < 0 || removeIndex >= analysis.nodes.size() || removeIndex == request.keepNodeIndex) {
                ++result.skippedMerges;
                continue;
            }
            if (removedNodeIndexes.contains(removeIndex)) {
                ++result.skippedMerges;
                continue;
            }
            const DataNode &remove = analysis.nodes[removeIndex];
            if (sc2dh::isProtectedCatalogNode(remove)) {
                ++result.skippedMerges;
                result.warnings << QStringLiteral("Skipped duplicate merge for protected editor/runtime object %1.").arg(remove.id);
                continue;
            }
            if (!sc2dh::isSafeAutomaticObjectId(remove.id)) {
                ++result.skippedMerges;
                result.warnings << QStringLiteral("Skipped duplicate merge for numeric or unsafe ID %1.").arg(remove.id);
                continue;
            }
            if (remove.id == keep.id
                || remove.elementName != keep.elementName
                || remove.contentHash != keep.contentHash) {
                ++result.skippedMerges;
                result.warnings << QStringLiteral("Skipped invalid duplicate merge %1 -> %2.").arg(remove.id, keep.id);
                continue;
            }
            redirects.insert(remove.id, keep.id);
            removals[remove.sourceFile].append(&remove);
            removedScopesById[remove.id].insert(sc2dh::catalogIdentityScope(remove.elementName));
            removedNodeIndexes.insert(removeIndex);
            removedIds << remove.id;
        }
    }

    if (redirects.isEmpty()) {
        result.success = true;
        result.error.clear();
        return result;
    }
    const QRegularExpression redirectsRegex = redirectExpression(redirects);
    QStringList skippedArchiveIds;
    const QHash<QString, QString> externalRedirects = archiveSafeRedirects(analysis, redirects, &skippedArchiveIds);
    if (!skippedArchiveIds.isEmpty()) {
        result.warnings << QStringLiteral("Skipped non-XML reference rewrites for ambiguous catalog IDs: %1.")
                               .arg(skippedArchiveIds.join(QStringLiteral(", ")));
    }

    QHash<QString, QByteArray> staged;
    RewriteStats totals;
    QString error;
    int fileIndex = 0;
    const int totalFiles = analysis.scannedFiles.size();
    for (const ScannedFileInfo &info : analysis.scannedFiles) {
        if (progress)
            progress(fileIndex, totalFiles, info.filePath);
        ++fileIndex;
        if (!info.isXml)
            continue;
        QByteArray bytes;
        if (!loadFile(info.filePath, &bytes, &error)) {
            result.error = error;
            return result;
        }
        pugi::xml_document doc;
        const auto parsed = doc.load_buffer(bytes.constData(), size_t(bytes.size()));
        if (!parsed) {
            result.error = QStringLiteral("Cannot parse %1: %2").arg(info.filePath, parsed.description());
            return result;
        }

        RewriteStats fileStats;
        QSet<QString> identityLocations;
        for (const DataNode *remove : removals.value(info.filePath))
            identityLocations.insert(remove->originalLocation);
        rewriteNode(doc, redirects, redirectsRegex, info.filePath, identityLocations, &fileStats);

        int deleted = 0;
        for (const DataNode *remove : removals.value(info.filePath)) {
            pugi::xml_node node = findObject(doc, remove->elementName, remove->id);
            if (!node || !node.parent().remove_child(node)) {
                result.error = QStringLiteral("Unable to delete %1 from %2.").arg(remove->id, info.filePath);
                return result;
            }
            ++deleted;
        }

        if (fileStats.fields || deleted) {
            std::ostringstream stream;
            doc.save(stream, "  ", pugi::format_default, pugi::encoding_utf8);
            const QByteArray output = QByteArray::fromStdString(stream.str());
            const QRegularExpressionMatch remaining = redirectsRegex.match(QString::fromUtf8(output));
            if (remaining.hasMatch()) {
                result.warnings << QStringLiteral("Duplicate merge kept residual old ID token %1 in %2 for manual review.")
                                       .arg(remaining.captured(1), info.filePath);
            }
            staged.insert(info.filePath, output);
            totals.fields += fileStats.fields;
            totals.references += fileStats.references;
        }
    }
    if (progress)
        progress(totalFiles, totalFiles, QString());

    if (staged.isEmpty()) {
        result.success = true;
        return result;
    }

    PreparedMergeTransaction prepared;
    if (!prepareMergeTransaction(rootFolder,
                                 staged,
                                 nonXmlReferenceFiles(analysis),
                                 externalRedirects,
                                 &prepared,
                                 &error)) {
        result.error = error;
        return result;
    }

    QString reportText = QStringLiteral("Batch Merge Apply\nRemoved objects: %1\nReferences redirected: %2\nFiles changed: %3\nWarnings: %4\n")
                             .arg(removedIds.size())
                             .arg(totals.references + prepared.archiveReferencesRedirected)
                             .arg(prepared.changedFiles.size())
                             .arg(result.warnings.isEmpty() ? QStringLiteral("none") : result.warnings.join(QStringLiteral("; ")));
    BackupManager backups;
    const QString failureInjectionStep = m_failureInjectionStep;
    const FolderSaveTransactionResult transaction = backups.applyFolderTransaction(
        rootFolder,
        prepared.changes,
        analysis.analysisReportText,
        reportText,
        [&prepared](const QString &stagingFolder, QString *validationError) {
            return validateStagedMergeXml(stagingFolder, prepared.xmlFiles, validationError);
        },
        [rootFolder, whitelistIds, removedScopesById, failureInjectionStep](QString *validationError) {
            return validateCommittedMerge(rootFolder,
                                          whitelistIds,
                                          removedScopesById,
                                          failureInjectionStep,
                                          validationError);
        },
        failureInjectionStep);
    result.backupFolder = transaction.backupFolder;
    if (!transaction.success) {
        result.error = mergeTransactionError(transaction);
        return result;
    }

    result.success = true;
    result.changedFiles = transaction.changedFiles;
    result.referencesRedirected = totals.references + prepared.archiveReferencesRedirected;
    result.nodesDeleted = removedIds.size();
    return result;
}
