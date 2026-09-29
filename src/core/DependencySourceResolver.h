#pragma once

#include "core/AnalysisModels.h"
#include "core/GameDataIncludeManifest.h"
#include "core/Sc2Archive.h"
#include "core/XmlLoader.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <algorithm>
#include <functional>
#include <pugixml.hpp>

namespace sc2dh {

// Keep archive and Editor-openable folder mods on the same metadata/catalog path.
// The folder's complete file set is fingerprinted by captureSourceRevision.
class DependencyPackage
{
public:
    bool load(const QString &path, QString *error)
    {
        const QFileInfo info(path);
        m_directory=info.isDir();
        if(!m_directory) return m_archive.load(path,error);
        m_root=QDir::fromNativeSeparators(info.canonicalFilePath());
        if(info.isSymLink() || m_root.isEmpty()) {
            if(error) *error=QStringLiteral("Dependency folder is not a canonical directory: %1").arg(path);
            return false;
        }
        m_entries.clear();
        QDirIterator iterator(m_root,QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot,
                              QDirIterator::Subdirectories);
        while(iterator.hasNext()) {
            iterator.next();
            const QFileInfo child=iterator.fileInfo();
            const QString canonical=QDir::fromNativeSeparators(child.canonicalFilePath());
            if(child.isSymLink() || canonical.isEmpty()
                || !canonical.startsWith(m_root+QLatin1Char('/'),Qt::CaseInsensitive)
                || (!child.isFile() && !child.isDir())) {
                if(error) *error=QStringLiteral("Dependency folder contains an unsupported or escaping entry: %1")
                    .arg(iterator.filePath());
                return false;
            }
            if(child.isFile())
                m_entries.append(QDir(m_root).relativeFilePath(canonical).replace('\\','/'));
        }
        if(m_entries.isEmpty()) {
            if(error) *error=QStringLiteral("Dependency folder contains no files: %1").arg(path);
            return false;
        }
        std::sort(m_entries.begin(),m_entries.end());
        return true;
    }

    QStringList allEntries() const { return m_directory ? m_entries : m_archive.allEntries(); }
    QStringList gameDataXmlEntries() const
    {
        if(!m_directory) return m_archive.gameDataXmlEntries();
        QStringList entries;
        for(const QString &entry:m_entries)
            if(entry.contains(QStringLiteral("Base.SC2Data/GameData/"),Qt::CaseInsensitive)
                && entry.endsWith(QStringLiteral(".xml"),Qt::CaseInsensitive))
                entries.append(entry);
        return entries;
    }

    bool readEntry(const QString &entry, QByteArray *bytes, QString *error) const
    {
        if(!m_directory) return m_archive.readEntry(entry,bytes,error);
        if(!bytes || !m_entries.contains(entry,Qt::CaseSensitive)) {
            if(error) *error=QStringLiteral("Dependency folder entry was not enumerated: %1").arg(entry);
            return false;
        }
        const QFileInfo info(QDir(m_root).filePath(entry));
        const QString canonical=QDir::fromNativeSeparators(info.canonicalFilePath());
        if(info.isSymLink() || !info.isFile() || info.size()>16*1024*1024
            || !canonical.startsWith(m_root+QLatin1Char('/'),Qt::CaseInsensitive)) {
            if(error) *error=QStringLiteral("Dependency folder entry is unavailable, oversized or escapes its root: %1")
                .arg(entry);
            return false;
        }
        QFile file(canonical);
        if(!file.open(QIODevice::ReadOnly)) {
            if(error) *error=QStringLiteral("Dependency folder entry cannot be read: %1").arg(entry);
            return false;
        }
        *bytes=file.readAll();
        if(file.error()!=QFileDevice::NoError || bytes->size()!=info.size()) {
            if(error) *error=QStringLiteral("Dependency folder entry changed or could not be read fully: %1")
                .arg(entry);
            bytes->clear();
            return false;
        }
        return true;
    }

private:
    bool m_directory=false;
    QString m_root;
    QStringList m_entries;
    Sc2Archive m_archive;
};

inline bool parseDocumentInfoDependencies(const QByteArray &bytes, const QString &source,
                                          QVector<DependencyDeclaration> *declarations, QString *error)
{
    if (!declarations) return false;
    declarations->clear();
    pugi::xml_document metadata;
    if (!XmlLoader().loadDocument(bytes,&metadata,error)) return false;
    int ordinal=0;
    std::function<void(pugi::xml_node)> visit=[&](pugi::xml_node element) {
        if(QString::fromUtf8(element.name())==QStringLiteral("Dependencies")) {
            for(pugi::xml_node child:element.children()) {
                if(child.type()!=pugi::node_element) continue;
                const QString raw=QString::fromUtf8(child.child_value()).trimmed();
                if(raw.isEmpty()) continue;
                DependencyDeclaration declaration;
                declaration.sourceFile=source;
                declaration.ordinal=ordinal++;
                declaration.raw=raw;
                if(raw.startsWith(QStringLiteral("bnet:"),Qt::CaseInsensitive)) {
                    const int fallback=raw.indexOf(QStringLiteral(",file:"),0,Qt::CaseInsensitive);
                    declaration.bnetHandle=raw.mid(5,fallback<0 ? -1 : fallback-5);
                    if(fallback>=0) declaration.fileFallback=raw.mid(fallback+6).replace('\\','/');
                } else if(raw.startsWith(QStringLiteral("file:"),Qt::CaseInsensitive)) {
                    declaration.fileFallback=raw.mid(5).replace('\\','/');
                }
                declarations->append(declaration);
            }
        }
        for(pugi::xml_node child:element.children())
            if(child.type()==pugi::node_element) visit(child);
    };
    visit(metadata.document_element());
    return true;
}

// This finds package sources and their metadata only. It deliberately does not
// merge catalog declarations or claim Editor/runtime layer precedence.
inline bool locateDependencySources(AnalysisResult *analysis, const QStringList &searchRoots,
                                    const QHash<QString, QString> &handleMappings,
                                    const std::function<bool()> &isCancelled, QString *error)
{
    if (!analysis) return false;
    analysis->dependencySearchRoots=searchRoots;
    analysis->dependencyHandleMappings.clear();
    for(auto it=handleMappings.cbegin();it!=handleMappings.cend();++it)
        analysis->dependencyHandleMappings.insert(it.key().trimmed().toCaseFolded(),it.value().trimmed());
    analysis->dependencySources.clear();
    QStringList roots;
    for (const QString &candidate:searchRoots) {
        const QFileInfo info(candidate);
        const QString canonical=QDir::fromNativeSeparators(info.canonicalFilePath());
        if(!info.isDir() || canonical.isEmpty()) {
            analysis->incompleteSources << QStringLiteral("Dependency search root is unavailable: %1").arg(candidate);
            continue;
        }
        if(!roots.contains(canonical,Qt::CaseInsensitive)) roots.append(canonical);
    }
    QSet<QString> active, expanded;
    const QString rootCanonical=QDir::fromNativeSeparators(QFileInfo(analysis->rootFolder).canonicalFilePath());
    if(!rootCanonical.isEmpty()) active.insert(rootCanonical.toCaseFolded());
    bool cancelled=false;
    std::function<void(const DependencyDeclaration &,const QString &,int)> visit;
    visit=[&](const DependencyDeclaration &declaration,const QString &owner,int depth) {
        if(cancelled) return;
        if(isCancelled && isCancelled()) {cancelled=true;return;}
        DependencySource source;
        source.declaration=declaration;
        source.ownerSource=owner;
        source.depth=depth;
        const int index=analysis->dependencySources.size();
        analysis->dependencySources.append(source);
        QString suppliedFallback=declaration.fileFallback;
        if(suppliedFallback.isEmpty() && !declaration.bnetHandle.isEmpty()) {
            suppliedFallback=analysis->dependencyHandleMappings.value(declaration.bnetHandle.trimmed().toCaseFolded());
            source.explicitHandleMapping=!suppliedFallback.isEmpty();
        }
        source.resolvedFileFallback=suppliedFallback;
        const QString fallback=QDir::cleanPath(suppliedFallback).replace('\\','/');
        if(suppliedFallback.isEmpty()) {
            source.status=DependencySourceStatus::Missing;
            source.issue=QStringLiteral("No local file fallback for %1").arg(declaration.raw);
        } else if(fallback==QStringLiteral("..")
                  || fallback.startsWith(QStringLiteral("../"))
                  || (!QDir::isAbsolutePath(fallback) && fallback.contains(QLatin1Char(':')))) {
            source.status=DependencySourceStatus::InvalidFallback;
            source.issue=QStringLiteral("Dependency fallback escapes the explicit search roots: %1").arg(suppliedFallback);
        } else if(roots.isEmpty()) {
            source.status=DependencySourceStatus::SearchRootsAbsent;
            source.issue=QStringLiteral("No explicit dependency search root for %1").arg(declaration.raw);
        } else if(depth>32) {
            source.status=DependencySourceStatus::DepthLimit;
            source.issue=QStringLiteral("Dependency depth exceeds 32 at %1").arg(declaration.raw);
        } else {
            QStringList matches;
            const bool absoluteFallback=QDir::isAbsolutePath(fallback);
            bool withinExplicitRoot=false;
            for(const QString &root:roots) {
                const QString candidate=QDir::fromNativeSeparators(QDir::cleanPath(
                    absoluteFallback ? fallback : QDir(root).absoluteFilePath(fallback)));
                if(!candidate.startsWith(root+QLatin1Char('/'),Qt::CaseInsensitive)) continue;
                withinExplicitRoot=true;
                const QFileInfo file(candidate);
                if(!file.isFile() && !file.isDir()) continue;
                const QString canonical=QDir::fromNativeSeparators(file.canonicalFilePath());
                if(canonical.isEmpty() || !canonical.startsWith(root+QLatin1Char('/'),Qt::CaseInsensitive)) continue;
                if(!matches.contains(canonical,Qt::CaseInsensitive)) matches.append(canonical);
            }
            if(absoluteFallback && !withinExplicitRoot) {
                source.status=DependencySourceStatus::InvalidFallback;
                source.issue=QStringLiteral("Absolute dependency fallback is outside the explicit search roots: %1")
                    .arg(suppliedFallback);
            } else if(matches.isEmpty()) {
                source.status=DependencySourceStatus::Missing;
                source.issue=QStringLiteral("Dependency file not found in explicit roots: %1").arg(suppliedFallback);
            } else if(matches.size()>1) {
                source.status=DependencySourceStatus::Ambiguous;
                source.issue=QStringLiteral("Dependency file has %1 matches in explicit roots: %2")
                    .arg(matches.size()).arg(suppliedFallback);
            } else {
                source.sourcePath=matches.first();
                const QString key=source.sourcePath.toCaseFolded();
                if(active.contains(key)) {
                    source.status=DependencySourceStatus::Cycle;
                    source.issue=QStringLiteral("Dependency cycle at %1").arg(source.sourcePath);
                } else {
                    QString revisionError;
                    const SourceRevision revision=captureSourceRevision(source.sourcePath,&revisionError);
                    if(!revisionError.isEmpty()) {
                        source.status=DependencySourceStatus::MetadataUnavailable;
                        source.issue=revisionError;
                    } else {
                        source.sha256=revision.sha256;
                        source.status=DependencySourceStatus::Located;
                        if(!expanded.contains(key)) {
                            DependencyPackage archive;
                            QString archiveError;
                            if(!archive.load(source.sourcePath,&archiveError)) {
                                source.status=DependencySourceStatus::MetadataUnavailable;
                                source.issue=archiveError;
                            } else {
                                QStringList metadataIssues;
                                const QString metadataEntry=uniqueArchiveEntry(archive.allEntries(),
                                    QStringLiteral("DocumentInfo"),source.sourcePath,&metadataIssues);
                                QByteArray bytes;
                                if(metadataEntry.isEmpty() || !archive.readEntry(metadataEntry,&bytes,&archiveError)) {
                                    source.status=DependencySourceStatus::MetadataUnavailable;
                                    source.issue=QStringLiteral("Dependency DocumentInfo is unavailable or ambiguous: %1: %2")
                                        .arg(source.sourcePath,metadataEntry.isEmpty()
                                            ? metadataIssues.join(QStringLiteral("; ")) : archiveError);
                                } else {
                                    QVector<DependencyDeclaration> children;
                                    if(!parseDocumentInfoDependencies(bytes,source.sourcePath+QStringLiteral("::")+metadataEntry,
                                                                      &children,&archiveError)) {
                                        source.status=DependencySourceStatus::MetadataUnavailable;
                                        source.issue=QStringLiteral("Dependency DocumentInfo is malformed: %1: %2")
                                            .arg(source.sourcePath,archiveError);
                                    } else {
                                        analysis->sourceRevisions.append(revision);
                                        expanded.insert(key);
                                        active.insert(key);
                                        analysis->dependencySources[index]=source;
                                        for(const auto &child:children) visit(child,source.sourcePath,depth+1);
                                        active.remove(key);
                                        return;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        analysis->dependencySources[index]=source;
        if(!source.issue.isEmpty()) analysis->incompleteSources << source.issue;
    };
    for(const auto &declaration:analysis->declaredDependencies)
        visit(declaration,analysis->rootFolder,0);
    if(cancelled) {
        if(error) *error=QStringLiteral("Analysis canceled while locating dependency sources.");
        return false;
    }
    return true;
}

inline bool loadDependencyLayerData(AnalysisResult *analysis,
                                   const std::function<bool()> &isCancelled, QString *error)
{
    if(!analysis) return false;
    analysis->dependencyLayers.clear();
    QSet<QString> visited;
    for(const auto &source:analysis->dependencySources) {
        if(isCancelled && isCancelled()) {
            if(error) *error=QStringLiteral("Analysis canceled while reading dependency GameData.");
            return false;
        }
        if(source.status!=DependencySourceStatus::Located || source.sourcePath.isEmpty()) continue;
        const QString key=source.sourcePath.toCaseFolded();
        if(visited.contains(key)) continue;
        visited.insert(key);
        DependencyLayerData layer;
        layer.sourcePath=source.sourcePath;
        layer.sha256=source.sha256;
        DependencyPackage archive;
        QString archiveError;
        if(!archive.load(source.sourcePath,&archiveError)) {
            layer.issues << QStringLiteral("Dependency archive cannot be reopened: %1: %2")
                .arg(source.sourcePath,archiveError);
        } else {
            const QString componentEntry=uniqueArchiveEntry(archive.allEntries(),
                QStringLiteral("ComponentList.SC2Components"),source.sourcePath,&layer.issues);
            QByteArray componentBytes;
            if(componentEntry.isEmpty()) {
                layer.issues << QStringLiteral("Dependency component list is unavailable or ambiguous: %1")
                    .arg(source.sourcePath);
            } else if(!archive.readEntry(componentEntry,&componentBytes,&archiveError)) {
                layer.issues << QStringLiteral("Dependency component list is unavailable: %1")
                    .arg(source.sourcePath);
            } else if(componentBytes.size()>16*1024*1024) {
                layer.issues << QStringLiteral("Dependency component list exceeds 16 MiB: %1")
                    .arg(source.sourcePath);
            } else {
                pugi::xml_document components;
                if(!XmlLoader().loadDocument(componentBytes,&components,&archiveError)
                    || QString::fromUtf8(components.document_element().name())!=QStringLiteral("Components")) {
                    layer.issues << QStringLiteral("Dependency component list is malformed: %1: %2")
                        .arg(source.sourcePath,archiveError);
                } else {
                    int gameDataComponents=0;
                    for(pugi::xml_node child:components.document_element().children("DataComponent")) {
                        if(QString::fromUtf8(child.attribute("Type").value())!=QStringLiteral("gada")) continue;
                        ++gameDataComponents;
                        const QString path=QString::fromUtf8(child.child_value()).trimmed().replace('\\','/');
                        if(path.compare(QStringLiteral("GameData"),Qt::CaseInsensitive)!=0
                            && path.compare(QStringLiteral("Base.SC2Data/GameData"),Qt::CaseInsensitive)!=0)
                            layer.issues << QStringLiteral("Dependency GameData component path is unsupported: %1: %2")
                                .arg(source.sourcePath,path);
                    }
                    if(gameDataComponents==1 && layer.issues.isEmpty()) layer.activeGameData=true;
                    else if(gameDataComponents!=1)
                        layer.issues << QStringLiteral("Dependency has %1 active GameData components: %2")
                            .arg(gameDataComponents).arg(source.sourcePath);
                }
            }
            if(layer.activeGameData) {
                QStringList entries=archive.gameDataXmlEntries();
                std::sort(entries.begin(),entries.end(),[](const QString &a,const QString &b) {
                    const int folded=QString::compare(a,b,Qt::CaseInsensitive);
                    return folded==0 ? a<b : folded<0;
                });
                const QString includeManifest=uniqueArchiveEntry(archive.allEntries(),
                    QStringLiteral("Base.SC2Data/GameData.xml"),source.sourcePath,
                    &layer.issues,true);
                if(!includeManifest.isEmpty()) {
                    layer.includeManifestPresent=true;
                    QByteArray manifestBytes;
                    if(!archive.readEntry(includeManifest,&manifestBytes,&archiveError)
                        || manifestBytes.size()>16*1024*1024) {
                        layer.issues << QStringLiteral("Dependency GameData include manifest is unreadable or oversized: %1::%2")
                            .arg(source.sourcePath,includeManifest);
                    } else {
                        QStringList includePaths;
                        const bool parsed=parseGameDataCatalogIncludes(manifestBytes,
                            source.sourcePath+QStringLiteral("::")+includeManifest,
                            &includePaths,&layer.issues);
                        QStringList included;
                        QSet<QString> seenIncludes;
                        for(const QString &path:includePaths) {
                            const QString expected=QStringLiteral("Base.SC2Data/")+path;
                            const QString matched=uniqueArchiveEntry(entries,expected,
                                source.sourcePath,&layer.issues);
                            if(matched.isEmpty()) continue;
                            if(seenIncludes.contains(matched.toCaseFolded())) continue;
                            seenIncludes.insert(matched.toCaseFolded());
                            included.append(matched);
                        }
                        for(const QString &entry:entries)
                            if(!seenIncludes.contains(entry.toCaseFolded()))
                                layer.inactiveGameDataEntries.append(entry);
                        if(parsed) entries=included;
                    }
                }
                for(const QString &entry:entries) {
                    const QString normalized=QDir::cleanPath(entry).replace('\\','/');
                    if(!normalized.startsWith(QStringLiteral("Base.SC2Data/GameData/"),Qt::CaseInsensitive)) {
                        layer.issues << QStringLiteral("Dependency GameData XML is outside the selected component: %1::%2")
                            .arg(source.sourcePath,entry);
                        continue;
                    }
                    QByteArray xml;
                    if(!archive.readEntry(entry,&xml,&archiveError) || xml.size()>16*1024*1024) {
                        layer.issues << QStringLiteral("Dependency GameData XML is unreadable or exceeds 16 MiB: %1::%2")
                            .arg(source.sourcePath,entry);
                        continue;
                    }
                    QVector<DataNode> nodes;
                    const QString entrySource=source.sourcePath+QStringLiteral("::")+entry;
                    if(!XmlLoader().extractNodes(entrySource,xml,&nodes,&archiveError)) {
                        layer.issues << QStringLiteral("Dependency GameData XML is malformed: %1: %2")
                            .arg(entrySource,archiveError);
                        continue;
                    }
                    layer.gameDataEntries.append(entry);
                    layer.nodes += nodes;
                }
            }
        }
        analysis->incompleteSources += layer.issues;
        analysis->dependencyLayers.append(layer);
    }
    return true;
}

// Diagnostic metadata closure only; runtime layer semantics remain unverified.
inline bool dependencySourcesLocated(const AnalysisResult &analysis)
{
    if(analysis.declaredDependencies.isEmpty()) return true;
    if(analysis.dependencySources.isEmpty()) return false;
    QSet<QString> activeLayers;
    for(const auto &layer:analysis.dependencyLayers)
        if(layer.activeGameData && layer.issues.isEmpty())
            activeLayers.insert(layer.sourcePath.toCaseFolded());
    for(const auto &source:analysis.dependencySources)
        if(source.status!=DependencySourceStatus::Located || source.sourcePath.isEmpty()
            || !activeLayers.contains(source.sourcePath.toCaseFolded())) return false;
    return true;
}

// Candidate evaluation order derived from declared edge order. This is useful
// for inspecting possible overlays, but must not feed Safe/apply decisions.
inline QStringList hypotheticalLayerOrder(const AnalysisResult &analysis)
{
    if(!analysis.dependencySourcesLocated) return {};
    QHash<QString,QVector<const DependencySource *>> byOwner;
    for(const auto &source:analysis.dependencySources)
        byOwner[source.ownerSource.toCaseFolded()].append(&source);
    QSet<QString> active,visited;
    QStringList order;
    std::function<bool(const QString &)> visit=[&](const QString &owner) {
        for(const auto *source:byOwner.value(owner.toCaseFolded())) {
            const QString key=source->sourcePath.toCaseFolded();
            if(active.contains(key)) return false;
            if(visited.contains(key)) continue;
            active.insert(key);
            if(!visit(source->sourcePath)) return false;
            active.remove(key);
            visited.insert(key);
            order.append(source->sourcePath);
        }
        return true;
    };
    if(!visit(analysis.rootFolder)) return {};
    order.append(analysis.rootFolder);
    return order;
}

inline QString dependencySourceStatusName(DependencySourceStatus status)
{
    switch(status) {
    case DependencySourceStatus::SearchRootsAbsent: return QStringLiteral("SearchRootsAbsent");
    case DependencySourceStatus::Missing: return QStringLiteral("Missing");
    case DependencySourceStatus::Ambiguous: return QStringLiteral("Ambiguous");
    case DependencySourceStatus::InvalidFallback: return QStringLiteral("InvalidFallback");
    case DependencySourceStatus::Cycle: return QStringLiteral("Cycle");
    case DependencySourceStatus::DepthLimit: return QStringLiteral("DepthLimit");
    case DependencySourceStatus::MetadataUnavailable: return QStringLiteral("MetadataUnavailable");
    case DependencySourceStatus::Located: return QStringLiteral("Located");
    }
    return QStringLiteral("Unknown");
}
}
