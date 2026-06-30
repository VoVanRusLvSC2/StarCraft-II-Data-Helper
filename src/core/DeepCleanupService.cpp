#include "core/DeepCleanupService.h"

#include "core/BackupManager.h"
#include "core/CatalogProtection.h"
#include "core/Sc2Archive.h"
#include "core/XmlLoader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <pugixml.hpp>

#include <algorithm>
#include <sstream>

namespace
{
QString relativePath(const QString &rootFolder, const QString &filePath)
{
    if (!QDir::isAbsolutePath(filePath))
        return QDir::cleanPath(filePath).replace('\\', '/');
    const QFileInfo rootInfo(rootFolder);
    if (rootInfo.exists() && rootInfo.isFile())
        return QDir::cleanPath(filePath).replace('\\', '/');
    return QDir(rootFolder).relativeFilePath(filePath).replace('\\', '/');
}

QString absoluteCandidatePath(const QString &rootFolder, const QString &filePath)
{
    return QDir::isAbsolutePath(filePath) ? filePath : QDir(rootFolder).absoluteFilePath(filePath);
}

bool isBackupOrTrashName(const QString &relative)
{
    const QString normalized = QDir::cleanPath(relative).replace('\\', '/').toLower();
    const QString fileName = QFileInfo(normalized).fileName();
    return normalized.contains(QStringLiteral("/backup_"))
        || fileName.startsWith(QStringLiteral("backup_"))
        || fileName.contains(QStringLiteral(".bak-"))
        || fileName.endsWith(QStringLiteral(".bak"))
        || fileName.endsWith(QStringLiteral(".tmp"))
        || fileName.endsWith(QStringLiteral(".old"))
        || fileName.endsWith(QStringLiteral(".orig"))
        || fileName.endsWith(QStringLiteral(".log"))
        || fileName.endsWith(QStringLiteral(".sc2dh.pending"))
        || fileName == QStringLiteral("analysis_report.txt")
        || fileName == QStringLiteral("planned_changes_report.txt")
        || fileName == QStringLiteral("rename_to_standard_preview.txt")
        || fileName == QStringLiteral("data_collection_preview.txt");
}

bool isArchivePath(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    static const QSet<QString> extensions = {
        QStringLiteral("sc2map"), QStringLiteral("sc2mod"), QStringLiteral("sc2components"),
        QStringLiteral("sc2campaign"), QStringLiteral("sc2archive")
    };
    return extensions.contains(suffix);
}

bool isLocalizationFile(const QString &relative);

bool isEditorManagedMapFile(const QString &relative)
{
    const QString normalized = QDir::cleanPath(relative).replace('\\', '/').toLower();
    const QString fileName = QFileInfo(normalized).fileName().toLower();
    static const QSet<QString> fileNames = {
        QStringLiteral("minimap.tga"),
        QStringLiteral("lightingmap.tga"),
        QStringLiteral("preloadassetdb.txt"),
        QStringLiteral("descindex.sc2layout"),
        QStringLiteral("descindex.version")
    };
    if (fileNames.contains(fileName))
        return true;
    return normalized == QStringLiteral("base.sc2data/ui/layout/descindex.sc2layout")
        || normalized == QStringLiteral("base.sc2data/ui/layout/descindex.version");
}

bool isMapPreviewImage(const QFileInfo &info, const QString &relative)
{
    const QString normalized = QDir::cleanPath(relative).replace('\\', '/').toLower();
    const QString fileName = QFileInfo(normalized).fileName().toLower();
    static const QSet<QString> imageExtensions = {
        QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"),
        QStringLiteral("tga"), QStringLiteral("bmp"), QStringLiteral("dds")
    };
    if (!imageExtensions.contains(info.suffix().toLower()))
        return false;
    return fileName.contains(QStringLiteral("thumbnail"))
        || fileName.contains(QStringLiteral("thumnail"))
        || fileName.contains(QStringLiteral("screenshot"))
        || fileName.contains(QStringLiteral("screen_shot"))
        || fileName.contains(QStringLiteral("preview"))
        || fileName.contains(QStringLiteral("loading"))
        || normalized.contains(QStringLiteral("/screenshots/"))
        || normalized.contains(QStringLiteral("/screenshot/"))
        || normalized.contains(QStringLiteral("/preview/"))
        || normalized.contains(QStringLiteral("/loading/"));
}

bool isAssetFile(const QFileInfo &info, const QString &relative)
{
    static const QSet<QString> extensions = {
        QStringLiteral("dds"), QStringLiteral("tga"), QStringLiteral("png"), QStringLiteral("jpg"),
        QStringLiteral("jpeg"), QStringLiteral("bmp"), QStringLiteral("m3"), QStringLiteral("ogg"),
        QStringLiteral("wav"), QStringLiteral("mp3"), QStringLiteral("webm"), QStringLiteral("mp4"),
        QStringLiteral("fxa"), QStringLiteral("fxs"), QStringLiteral("fxh"), QStringLiteral("layout"),
        QStringLiteral("sc2layout"), QStringLiteral("txt")
    };
    if (info.exists() && !info.isFile())
        return false;
    if (isBackupOrTrashName(relative) || isLocalizationFile(relative) || isEditorManagedMapFile(relative)
        || isMapPreviewImage(info, relative))
        return false;
    return extensions.contains(info.suffix().toLower());
}

bool isLocalizationFile(const QString &relative)
{
    const QString normalized = relative.toLower();
    return normalized.contains(QStringLiteral("localizeddata/"))
        || normalized.contains(QStringLiteral("gamestrings"))
        || normalized.contains(QStringLiteral("objectstrings"));
}

bool containsToken(const QString &haystack, const QString &token)
{
    if (token.trimmed().isEmpty())
        return false;
    const QRegularExpression expression(
        QStringLiteral("(?<![A-Za-z0-9_@.])%1(?![A-Za-z0-9_@.])")
            .arg(QRegularExpression::escape(token)),
        QRegularExpression::CaseInsensitiveOption);
    return expression.match(haystack).hasMatch();
}

QString readTextFileBestEffort(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll());
}

QSet<QString> knownIds(const AnalysisResult &analysis)
{
    QSet<QString> ids;
    for (const DataNode &node : analysis.nodes)
        if (!node.id.isEmpty())
            ids.insert(node.id);
    return ids;
}

QString referenceCorpus(const AnalysisResult &analysis)
{
    QStringList parts;
    for (const QString &xml : analysis.sourceXmlByFile)
        parts << xml;
    Sc2Archive archive;
    QString archiveError;
    const bool readArchiveEntries = isArchivePath(analysis.rootFolder) && archive.load(analysis.rootFolder, &archiveError);
    for (const ScannedFileInfo &file : analysis.scannedFiles) {
        if (!file.isSc2DataLike || file.isXml || file.size > 4 * 1024 * 1024)
            continue;
        if (readArchiveEntries && !QDir::isAbsolutePath(file.filePath)) {
            QByteArray bytes;
            QString readError;
            if (archive.readEntry(file.filePath, &bytes, &readError) && bytes.size() <= 4 * 1024 * 1024)
                parts << QString::fromUtf8(bytes);
            continue;
        }
        parts << readTextFileBestEffort(file.filePath);
    }
    return parts.join(QLatin1Char('\n'));
}

bool assetIsReferenced(const QString &corpus, const QString &relative, const QFileInfo &info)
{
    const QString slashPath = QDir::cleanPath(relative).replace('\\', '/');
    const QString backslashPath = QString(slashPath).replace('/', '\\');
    return containsToken(corpus, slashPath)
        || containsToken(corpus, backslashPath)
        || containsToken(corpus, info.fileName())
        || containsToken(corpus, info.completeBaseName());
}

QStringList splitLocation(const QString &location)
{
    return location.split(QLatin1Char('/'), Qt::SkipEmptyParts);
}

bool parseSegment(const QString &segment, QString *name, int *index)
{
    const int open = segment.lastIndexOf(QLatin1Char('['));
    const int close = segment.lastIndexOf(QLatin1Char(']'));
    if (open <= 0 || close <= open + 1)
        return false;
    bool ok = false;
    const int parsedIndex = segment.mid(open + 1, close - open - 1).toInt(&ok);
    if (!ok || parsedIndex <= 0)
        return false;
    *name = segment.left(open);
    *index = parsedIndex;
    return true;
}

pugi::xml_node childByNameAndIndex(const pugi::xml_node &parent, const QString &name, int index)
{
    int current = 0;
    for (pugi::xml_node child = parent.first_child(); child; child = child.next_sibling()) {
        if (child.type() != pugi::node_element || QString::fromUtf8(child.name()) != name)
            continue;
        ++current;
        if (current == index)
            return child;
    }
    return {};
}

pugi::xml_node findNodeByLocation(const pugi::xml_node &document, const QString &location)
{
    pugi::xml_node currentParent = document;
    pugi::xml_node currentNode;
    for (const QString &segment : splitLocation(location)) {
        QString name;
        int index = 0;
        if (!parseSegment(segment, &name, &index))
            return {};
        currentNode = childByNameAndIndex(currentParent, name, index);
        if (!currentNode)
            return {};
        currentParent = currentNode;
    }
    return currentNode;
}

QString buildPathFromNode(const pugi::xml_node &node)
{
    QStringList segments;
    for (pugi::xml_node current = node; current && current.type() == pugi::node_element; current = current.parent()) {
        int index = 1;
        for (pugi::xml_node sibling = current.previous_sibling(current.name()); sibling; sibling = sibling.previous_sibling(current.name()))
            ++index;
        segments.prepend(QStringLiteral("%1[%2]").arg(QString::fromUtf8(current.name())).arg(index));
        if (!current.parent() || current.parent().type() == pugi::node_document)
            break;
    }
    return QLatin1Char('/') + segments.join(QLatin1Char('/'));
}

QString serializeNode(const pugi::xml_node &node)
{
    std::ostringstream stream;
    node.print(stream, "  ", pugi::format_raw, pugi::encoding_utf8);
    return QString::fromStdString(stream.str());
}

QStringList typedReferences(const QString &text)
{
    static const QRegularExpression expression(
        QStringLiteral("\\b(?:Unit|Abil|Ability|Weapon|Effect|Behavior|Actor|Model|Sound|Button|Validator|Requirement|Upgrade|Mover),([A-Za-z0-9_@.]+)"));
    QStringList refs;
    auto matches = expression.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        refs << match.captured(1);
    }
    refs.removeDuplicates();
    return refs;
}

bool actorEventLike(const pugi::xml_node &node)
{
    const QString name = QString::fromUtf8(node.name()).toLower();
    return name.contains(QStringLiteral("event")) || name == QStringLiteral("on") || name.contains(QStringLiteral("term"));
}

bool isTrueAttribute(const pugi::xml_node &node, const char *name)
{
    const QString value = QString::fromUtf8(node.attribute(name).value()).trimmed().toLower();
    return value == QStringLiteral("1") || value == QStringLiteral("true");
}

QString brokenActorEventReason(const pugi::xml_node &node)
{
    if (isTrueAttribute(node, "removed"))
        return {};

    const QString nodeName = QString::fromUtf8(node.name()).toLower();
    if (nodeName != QStringLiteral("on"))
        return {};

    const pugi::xml_attribute termsAttr = node.attribute("Terms");
    const pugi::xml_attribute sendAttr = node.attribute("Send");
    const QString terms = QString::fromUtf8(termsAttr.value()).trimmed();
    const QString send = QString::fromUtf8(sendAttr.value()).trimmed();
    const bool missingTerms = !termsAttr || terms.isEmpty();
    const bool missingSend = !sendAttr || send.isEmpty();

    if (missingTerms && missingSend)
        return QStringLiteral("Actor event has no triggering Terms and no Send action.");
    if (missingSend)
        return QStringLiteral("Actor event has triggering Terms but no Send action.");
    if (missingTerms)
        return QStringLiteral("Actor event has a Send action but no triggering Terms.");
    return {};
}

void appendActorEventCandidates(const AnalysisResult &analysis,
                                const QSet<QString> &ids,
                                QVector<DeepCleanupCandidate> *candidates)
{
    for (auto it = analysis.sourceXmlByFile.cbegin(); it != analysis.sourceXmlByFile.cend(); ++it) {
        pugi::xml_document document;
        const QByteArray bytes = it.value().toUtf8();
        if (!document.load_buffer(bytes.constData(), size_t(bytes.size())))
            continue;
        for (const DataNode &node : analysis.nodes) {
            if (node.sourceFile != it.key() || !node.elementName.startsWith(QStringLiteral("CActor"), Qt::CaseInsensitive))
                continue;
            pugi::xml_node actor = findNodeByLocation(document, node.originalLocation);
            if (!actor)
                continue;
            for (pugi::xml_node child = actor.first_child(); child; child = child.next_sibling()) {
                if (child.type() != pugi::node_element || !actorEventLike(child))
                    continue;
                const QString xml = serializeNode(child);
                const QString structuralReason = brokenActorEventReason(child);
                if (!structuralReason.isEmpty()) {
                    DeepCleanupCandidate candidate;
                    candidate.index = candidates->size();
                    candidate.kind = DeepCleanupKind::BrokenActorEvent;
                    candidate.action = DeepCleanupAction::RemoveXmlNode;
                    candidate.state = CandidateState::Safe;
                    candidate.recommended = true;
                    candidate.filePath = it.key();
                    candidate.label = QStringLiteral("%1 event in %2").arg(QString::fromUtf8(child.name()), node.id);
                    candidate.xmlLocation = buildPathFromNode(child);
                    candidate.reason = structuralReason;
                    candidate.detail = xml.left(600);
                    candidates->append(candidate);
                    continue;
                }
                const QStringList refs = typedReferences(xml);
                if (refs.isEmpty())
                    continue;
                int existing = 0;
                for (const QString &ref : refs)
                    if (ids.contains(ref))
                        ++existing;
                DeepCleanupCandidate candidate;
                candidate.kind = DeepCleanupKind::BrokenActorEvent;
                candidate.index = candidates->size();
                candidate.filePath = it.key();
                candidate.label = QStringLiteral("%1 event in %2").arg(QString::fromUtf8(child.name()), node.id);
                candidate.xmlLocation = buildPathFromNode(child);
                candidate.detail = xml.left(600);
                if (existing == 0) {
                    candidate.action = DeepCleanupAction::RemoveXmlNode;
                    candidate.state = CandidateState::Safe;
                    candidate.recommended = true;
                    candidate.reason = QStringLiteral("Actor event references only missing typed IDs: %1").arg(refs.join(QStringLiteral(", ")));
                } else if (existing < refs.size()) {
                    candidate.action = DeepCleanupAction::ReportOnly;
                    candidate.state = CandidateState::Risky;
                    candidate.recommended = false;
                    candidate.reason = QStringLiteral("Actor event has mixed existing and missing typed IDs: %1").arg(refs.join(QStringLiteral(", ")));
                } else {
                    continue;
                }
                candidates->append(candidate);
            }
        }
    }
}

QStringList localizationKeyTokens(const QString &key)
{
    QStringList tokens = key.split(QRegularExpression(QStringLiteral("[/:.\\\\\\s]+")), Qt::SkipEmptyParts);
    tokens.removeDuplicates();
    return tokens;
}

bool keyLooksLikeObjectString(const QStringList &tokens)
{
    static const QSet<QString> objectPrefixes = {
        QStringLiteral("Unit"), QStringLiteral("Abil"), QStringLiteral("Ability"), QStringLiteral("Weapon"),
        QStringLiteral("Effect"), QStringLiteral("Actor"), QStringLiteral("Button"), QStringLiteral("Behavior"),
        QStringLiteral("Validator"), QStringLiteral("Requirement"), QStringLiteral("Upgrade"), QStringLiteral("Model"),
        QStringLiteral("Sound"), QStringLiteral("Mover")
    };
    for (const QString &token : tokens)
        if (objectPrefixes.contains(token))
            return true;
    return false;
}

bool lineEndingIsCrLf(const QByteArray &bytes)
{
    return bytes.contains("\r\n");
}

QByteArray joinLines(const QStringList &lines, const QByteArray &lineEnding, bool trailingNewline)
{
    QByteArray output;
    for (int i = 0; i < lines.size(); ++i) {
        if (i > 0)
            output += lineEnding;
        output += lines.at(i).toUtf8();
    }
    if (trailingNewline)
        output += lineEnding;
    return output;
}

int locationDepth(const QString &location)
{
    return splitLocation(location).size();
}
}

QString deepCleanupKindName(DeepCleanupKind kind)
{
    switch (kind) {
    case DeepCleanupKind::UnusedAsset: return QStringLiteral("Unused asset");
    case DeepCleanupKind::LocalizationEntry: return QStringLiteral("Localization");
    case DeepCleanupKind::RedundantDefaultField: return QStringLiteral("Default field");
    case DeepCleanupKind::BrokenActorEvent: return QStringLiteral("Actor event");
    case DeepCleanupKind::DependencyEntry: return QStringLiteral("Dependency review");
    case DeepCleanupKind::ArchiveTrash: return QStringLiteral("Archive trash");
    }
    return QStringLiteral("Deep cleanup");
}

QString deepCleanupActionName(DeepCleanupAction action)
{
    switch (action) {
    case DeepCleanupAction::DeleteFile: return QStringLiteral("Delete file");
    case DeepCleanupAction::RemoveTextLine: return QStringLiteral("Remove text line");
    case DeepCleanupAction::RemoveXmlNode: return QStringLiteral("Remove XML node");
    case DeepCleanupAction::RemoveXmlAttribute: return QStringLiteral("Remove XML attribute");
    case DeepCleanupAction::ReportOnly: return QStringLiteral("Review only");
    }
    return QStringLiteral("Review only");
}

void DeepCleanupService::populateCandidates(AnalysisResult *analysis) const
{
    if (!analysis)
        return;
    analysis->deepCleanupCandidates.clear();
    const QSet<QString> ids = knownIds(*analysis);
    const QString corpus = referenceCorpus(*analysis);

    auto append = [&](DeepCleanupCandidate candidate) {
        candidate.index = analysis->deepCleanupCandidates.size();
        analysis->deepCleanupCandidates.append(std::move(candidate));
    };

    for (const ScannedFileInfo &file : analysis->scannedFiles) {
        const QString rel = relativePath(analysis->rootFolder, file.filePath);
        const QFileInfo info(file.filePath);
        if (isBackupOrTrashName(rel)) {
            DeepCleanupCandidate candidate;
            candidate.kind = DeepCleanupKind::ArchiveTrash;
            candidate.action = DeepCleanupAction::DeleteFile;
            candidate.state = CandidateState::Safe;
            candidate.filePath = file.filePath;
            candidate.label = rel;
            candidate.reason = QStringLiteral("Temporary, report, backup or pending helper file is not game data.");
            candidate.bytes = file.size;
            candidate.recommended = true;
            append(candidate);
            continue;
        }
        if (isAssetFile(info, rel) && !assetIsReferenced(corpus, rel, info)) {
            DeepCleanupCandidate candidate;
            candidate.kind = DeepCleanupKind::UnusedAsset;
            candidate.action = DeepCleanupAction::DeleteFile;
            candidate.state = CandidateState::Safe;
            candidate.filePath = file.filePath;
            candidate.label = rel;
            candidate.reason = QStringLiteral("Asset filename/path is not referenced by XML, trigger, script or text data.");
            candidate.bytes = file.size;
            candidate.recommended = true;
            append(candidate);
        }
    }

    for (const ScannedFileInfo &file : analysis->scannedFiles) {
        const QString rel = relativePath(analysis->rootFolder, file.filePath);
        if (!file.isSc2DataLike || file.isXml || !isLocalizationFile(rel) || file.size > 4 * 1024 * 1024)
            continue;
        QFile source(file.filePath);
        if (!source.open(QIODevice::ReadOnly))
            continue;
        const QString text = QString::fromUtf8(source.readAll());
        const QStringList lines = text.split(QRegularExpression(QStringLiteral("\\r?\\n")));
        for (int i = 0; i < lines.size(); ++i) {
            const QString line = lines.at(i);
            const QString trimmed = line.trimmed();
            if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')) || trimmed.startsWith(QStringLiteral("//")))
                continue;
            const int equals = line.indexOf(QLatin1Char('='));
            if (equals <= 0)
                continue;
            const QString key = line.left(equals).trimmed();
            const QStringList tokens = localizationKeyTokens(key);
            if (!keyLooksLikeObjectString(tokens))
                continue;
            bool referencesExisting = false;
            for (const QString &token : tokens) {
                if (ids.contains(token)) {
                    referencesExisting = true;
                    break;
                }
            }
            if (referencesExisting)
                continue;
            DeepCleanupCandidate candidate;
            candidate.kind = DeepCleanupKind::LocalizationEntry;
            candidate.action = DeepCleanupAction::RemoveTextLine;
            candidate.state = CandidateState::Safe;
            candidate.filePath = file.filePath;
            candidate.label = key;
            candidate.lineNumber = i;
            candidate.reason = QStringLiteral("Localized object string does not match any existing data ID.");
            candidate.detail = line.left(600);
            candidate.recommended = true;
            append(candidate);
        }
    }

    QHash<QString, const DataNode *> nodesByTypeAndId;
    for (const DataNode &node : analysis->nodes)
        nodesByTypeAndId.insert(node.elementName.toLower() + QChar(0x1f) + node.id.toLower(), &node);
    for (const DataNode &node : analysis->nodes) {
        if (sc2dh::isProtectedCatalogNode(node))
            continue;
        const QString parentId = node.attributes.value(QStringLiteral("parent"));
        if (parentId.isEmpty())
            continue;
        const DataNode *parent = nodesByTypeAndId.value(node.elementName.toLower() + QChar(0x1f) + parentId.toLower(), nullptr);
        if (!parent)
            continue;
        for (auto it = node.attributes.cbegin(); it != node.attributes.cend(); ++it) {
            const QString attr = it.key();
            if (attr.compare(QStringLiteral("id"), Qt::CaseInsensitive) == 0
                || attr.compare(QStringLiteral("parent"), Qt::CaseInsensitive) == 0
                || attr.compare(QStringLiteral("default"), Qt::CaseInsensitive) == 0)
                continue;
            const auto parentAttr = parent->attributes.constFind(attr);
            if (parentAttr == parent->attributes.cend() || parentAttr.value() != it.value())
                continue;
            DeepCleanupCandidate candidate;
            candidate.kind = DeepCleanupKind::RedundantDefaultField;
            candidate.action = DeepCleanupAction::RemoveXmlAttribute;
            candidate.state = CandidateState::Safe;
            candidate.filePath = node.sourceFile;
            candidate.label = QStringLiteral("%1.%2").arg(node.id, attr);
            candidate.xmlLocation = node.originalLocation;
            candidate.attributeName = attr;
            candidate.reason = QStringLiteral("Attribute equals the local parent object value (%1).").arg(parent->id);
            candidate.detail = QStringLiteral("%1=\"%2\"").arg(attr, it.value());
            candidate.recommended = true;
            append(candidate);
        }
    }

    appendActorEventCandidates(*analysis, ids, &analysis->deepCleanupCandidates);

    for (const ScannedFileInfo &file : analysis->scannedFiles) {
        const QString rel = relativePath(analysis->rootFolder, file.filePath);
        const QString lower = rel.toLower();
        if (!lower.contains(QStringLiteral("documentinfo")) && !lower.contains(QStringLiteral("dependencies")))
            continue;
        const QString text = readTextFileBestEffort(file.filePath);
        if (!text.contains(QStringLiteral("SC2Mod"), Qt::CaseInsensitive)
            && !text.contains(QStringLiteral("Dependency"), Qt::CaseInsensitive))
            continue;
        DeepCleanupCandidate candidate;
        candidate.index = analysis->deepCleanupCandidates.size();
        candidate.kind = DeepCleanupKind::DependencyEntry;
        candidate.action = DeepCleanupAction::ReportOnly;
        candidate.state = CandidateState::Risky;
        candidate.filePath = file.filePath;
        candidate.label = rel;
        candidate.reason = QStringLiteral("Dependency metadata found. Dependency removal requires map/mod load testing and is review-only.");
        candidate.recommended = false;
        analysis->deepCleanupCandidates.append(candidate);
    }

    for (int index = 0; index < analysis->deepCleanupCandidates.size(); ++index)
        analysis->deepCleanupCandidates[index].index = index;
}

DeepCleanupApplyResult DeepCleanupService::apply(const AnalysisResult &analysis,
                                                 const QVector<int> &candidateIndexes,
                                                 const QString &rootFolder,
                                                 bool createBackup) const
{
    DeepCleanupApplyResult result;
    if (candidateIndexes.isEmpty()) {
        result.error = QStringLiteral("No deep cleanup rows selected.");
        return result;
    }

    QSet<int> selected;
    for (int index : candidateIndexes)
        selected.insert(index);

    QVector<DeepCleanupCandidate> candidates;
    for (const DeepCleanupCandidate &candidate : analysis.deepCleanupCandidates) {
        if (!selected.contains(candidate.index))
            continue;
        if (candidate.state != CandidateState::Safe || candidate.action == DeepCleanupAction::ReportOnly) {
            ++result.reportOnlySkipped;
            continue;
        }
        candidates.append(candidate);
    }
    if (candidates.isEmpty()) {
        result.success = true;
        return result;
    }

    QStringList backupFiles;
    for (const DeepCleanupCandidate &candidate : candidates) {
        const QString abs = absoluteCandidatePath(rootFolder, candidate.filePath);
        const QString rel = relativePath(rootFolder, abs);
        if (!backupFiles.contains(rel))
            backupFiles.append(rel);
    }
    if (createBackup) {
        BackupManager backupManager;
        if (!backupManager.createFolderBackup(rootFolder, backupFiles, analysis.analysisReportText,
                                              analysis.plannedChangesReportText, &result.backupFolder, &result.error))
            return result;
    }

    QHash<QString, QVector<DeepCleanupCandidate>> lineEdits;
    QHash<QString, QVector<DeepCleanupCandidate>> xmlEdits;
    QStringList filesToDelete;
    for (const DeepCleanupCandidate &candidate : candidates) {
        const QString abs = absoluteCandidatePath(rootFolder, candidate.filePath);
        switch (candidate.action) {
        case DeepCleanupAction::DeleteFile:
            if (!filesToDelete.contains(abs))
                filesToDelete.append(abs);
            break;
        case DeepCleanupAction::RemoveTextLine:
            lineEdits[abs].append(candidate);
            break;
        case DeepCleanupAction::RemoveXmlNode:
        case DeepCleanupAction::RemoveXmlAttribute:
            xmlEdits[abs].append(candidate);
            break;
        case DeepCleanupAction::ReportOnly:
            ++result.reportOnlySkipped;
            break;
        }
    }

    for (auto it = lineEdits.cbegin(); it != lineEdits.cend(); ++it) {
        QFile file(it.key());
        if (!file.open(QIODevice::ReadOnly)) {
            result.error = QStringLiteral("Unable to open text file for cleanup: %1").arg(it.key());
            return result;
        }
        const QByteArray bytes = file.readAll();
        file.close();
        const QByteArray ending = lineEndingIsCrLf(bytes) ? QByteArrayLiteral("\r\n") : QByteArrayLiteral("\n");
        const bool trailingNewline = bytes.endsWith('\n');
        QString text = QString::fromUtf8(bytes);
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        if (trailingNewline && text.endsWith(QLatin1Char('\n')))
            text.chop(1);
        const QStringList lines = text.split(QLatin1Char('\n'));
        QSet<int> removeLines;
        for (const DeepCleanupCandidate &candidate : it.value())
            if (candidate.lineNumber >= 0 && candidate.lineNumber < lines.size())
                removeLines.insert(candidate.lineNumber);
        QStringList kept;
        for (int line = 0; line < lines.size(); ++line)
            if (!removeLines.contains(line))
                kept.append(lines.at(line));
        QSaveFile save(it.key());
        const QByteArray output = joinLines(kept, ending, trailingNewline);
        if (!save.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || save.write(output) != output.size()
            || !save.commit()) {
            result.error = QStringLiteral("Unable to write cleaned text file: %1").arg(it.key());
            return result;
        }
        result.textLinesRemoved += removeLines.size();
        result.changedFiles.append(relativePath(rootFolder, it.key()));
    }

    for (auto it = xmlEdits.cbegin(); it != xmlEdits.cend(); ++it) {
        QFile file(it.key());
        if (!file.open(QIODevice::ReadOnly)) {
            result.error = QStringLiteral("Unable to open XML file for cleanup: %1").arg(it.key());
            return result;
        }
        const QByteArray bytes = file.readAll();
        file.close();
        pugi::xml_document document;
        const pugi::xml_parse_result parsed = document.load_buffer(bytes.constData(), size_t(bytes.size()));
        if (!parsed) {
            result.error = QStringLiteral("Unable to parse XML for cleanup: %1").arg(parsed.description());
            return result;
        }

        QVector<pugi::xml_node> nodesToRemove;
        QVector<DeepCleanupCandidate> nodeCandidates;
        for (const DeepCleanupCandidate &candidate : it.value()) {
            pugi::xml_node node = findNodeByLocation(document, candidate.xmlLocation);
            if (!node) {
                result.error = QStringLiteral("Unable to locate XML cleanup node: %1").arg(candidate.xmlLocation);
                return result;
            }
            if (candidate.action == DeepCleanupAction::RemoveXmlAttribute) {
                const QByteArray attributeName = candidate.attributeName.toUtf8();
                if (node.attribute(attributeName.constData())) {
                    node.remove_attribute(attributeName.constData());
                    ++result.xmlAttributesRemoved;
                }
            } else if (candidate.action == DeepCleanupAction::RemoveXmlNode) {
                nodesToRemove.append(node);
                nodeCandidates.append(candidate);
            }
        }
        std::sort(nodeCandidates.begin(), nodeCandidates.end(), [](const DeepCleanupCandidate &left, const DeepCleanupCandidate &right) {
            const int leftDepth = locationDepth(left.xmlLocation);
            const int rightDepth = locationDepth(right.xmlLocation);
            if (leftDepth != rightDepth)
                return leftDepth > rightDepth;
            return left.xmlLocation > right.xmlLocation;
        });
        nodesToRemove.clear();
        for (const DeepCleanupCandidate &candidate : nodeCandidates) {
            pugi::xml_node node = findNodeByLocation(document, candidate.xmlLocation);
            if (node)
                nodesToRemove.append(node);
        }
        for (const pugi::xml_node &node : nodesToRemove) {
            if (node.parent() && node.parent().remove_child(node))
                ++result.xmlNodesRemoved;
        }

        std::ostringstream stream;
        document.save(stream, "  ", pugi::format_default, pugi::encoding_utf8);
        const QByteArray output = QByteArray::fromStdString(stream.str());
        QSaveFile save(it.key());
        if (!save.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || save.write(output) != output.size()
            || !save.commit()) {
            result.error = QStringLiteral("Unable to write cleaned XML file: %1").arg(it.key());
            return result;
        }
        result.changedFiles.append(relativePath(rootFolder, it.key()));
    }

    for (const QString &filePath : filesToDelete) {
        if (!QFileInfo::exists(filePath))
            continue;
        if (!QFile::remove(filePath)) {
            result.error = QStringLiteral("Unable to delete cleanup file: %1").arg(filePath);
            return result;
        }
        ++result.filesDeleted;
        result.removedFiles.append(relativePath(rootFolder, filePath));
    }

    result.changedFiles.removeDuplicates();
    result.removedFiles.removeDuplicates();
    result.success = true;
    return result;
}
