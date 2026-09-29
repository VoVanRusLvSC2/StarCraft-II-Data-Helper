#pragma once

#include "core/AnalysisModels.h"
#include "core/CatalogProtection.h"

#include <QFileInfo>
#include <QSet>
#include <algorithm>
#include <pugixml.hpp>

namespace sc2dh {

inline void indexLayeredDeclarations(AnalysisResult *analysis)
{
    if(!analysis) return;
    analysis->layeredNamedDeclarations.clear();
    analysis->layeredClassDefaults.clear();
    const auto append=[&](const DataNode &node,int layerIndex,int nodeIndex) {
        LayeredDeclarationRef ref;
        ref.declaration={{catalogIdentityScope(node.elementName),node.id},
                         node.sourceFile,node.originalLocation};
        ref.elementName=node.elementName;
        ref.rawId=node.id;
        ref.dependencyLayerIndex=layerIndex;
        ref.nodeIndex=nodeIndex;
        if(!node.id.isEmpty())
            analysis->layeredNamedDeclarations[catalogIdentityKey(node.elementName,node.id)].append(ref);
        else if(node.attributes.value(QStringLiteral("default"))==QStringLiteral("1"))
            analysis->layeredClassDefaults[node.elementName.trimmed().toCaseFolded()].append(ref);
    };
    for(int layer=0;layer<analysis->dependencyLayers.size();++layer) {
        const auto &source=analysis->dependencyLayers[layer];
        if(!source.activeGameData) continue;
        for(int node=0;node<source.nodes.size();++node) append(source.nodes[node],layer,node);
    }
    for(int node=0;node<analysis->nodes.size();++node)
        append(analysis->nodes[node],-1,node);
    QHash<QString,int> rankBySource;
    for(int rank=0;rank<analysis->hypotheticalLayerOrder.size();++rank)
        rankBySource.insert(analysis->hypotheticalLayerOrder[rank].toCaseFolded(),rank);
    const auto layerRank=[&](const LayeredDeclarationRef &ref) {
        if(ref.dependencyLayerIndex<0) return int(analysis->dependencyLayers.size());
        const QString source=analysis->dependencyLayers[ref.dependencyLayerIndex].sourcePath;
        return rankBySource.value(source.toCaseFolded(),ref.dependencyLayerIndex);
    };
    const auto ordered=[&](const LayeredDeclarationRef &a,const LayeredDeclarationRef &b) {
        const int aRank=layerRank(a),bRank=layerRank(b);
        if(aRank!=bRank) return aRank<bRank;
        if(a.dependencyLayerIndex!=b.dependencyLayerIndex)
            return a.dependencyLayerIndex<b.dependencyLayerIndex;
        if(a.nodeIndex!=b.nodeIndex) return a.nodeIndex<b.nodeIndex;
        const int location=QString::compare(a.declaration.location,b.declaration.location,Qt::CaseInsensitive);
        if(location!=0) return location<0;
        if(a.declaration.location!=b.declaration.location)
            return a.declaration.location<b.declaration.location;
        if(a.elementName!=b.elementName) return a.elementName<b.elementName;
        return a.rawId<b.rawId;
    };
    for(auto it=analysis->layeredNamedDeclarations.begin();it!=analysis->layeredNamedDeclarations.end();++it)
        std::sort(it.value().begin(),it.value().end(),ordered);
    for(auto it=analysis->layeredClassDefaults.begin();it!=analysis->layeredClassDefaults.end();++it)
        std::sort(it.value().begin(),it.value().end(),ordered);
}

// Editor 5.0.16.97563 displayed the last direct dependency's LifeMax for
// B,A -> 110 and A,B -> 220, and the local value for B,A + local -> 330.
// Keep this narrowly scoped observation separate from runtime/Safe decisions.
inline void collectEditorLayeredScalarDiagnostics(AnalysisResult *analysis)
{
    if(!analysis) return;
    analysis->editorLayeredScalars.clear();
    if(analysis->sourceChangedDuringAnalysis || !analysis->dependencySourcesLocated
        || analysis->declaredDependencies.size()!=2 || analysis->dependencySources.size()!=2
        || analysis->dependencyLayers.size()!=2 || analysis->hypotheticalLayerOrder.size()!=3
        || analysis->nodes.size()>1 || !analysis->layeredClassDefaults.isEmpty()
        || !QFileInfo(analysis->rootFolder).isDir()
        || !analysis->rootFolder.endsWith(QStringLiteral(".SC2Mod"),Qt::CaseInsensitive)) return;
    for(int i=0;i<2;++i) {
        const auto &source=analysis->dependencySources[i];
        const auto &layer=analysis->dependencyLayers[i];
        if(source.depth!=0 || source.ownerSource!=analysis->rootFolder
            || source.status!=DependencySourceStatus::Located
            || !QFileInfo(source.sourcePath).isDir()
            || !source.sourcePath.endsWith(QStringLiteral(".SC2Mod"),Qt::CaseInsensitive)
            || !layer.activeGameData
            || layer.gameDataEntries.size()!=1 || layer.nodes.size()!=1
            || !layer.issues.isEmpty()
            || layer.sourcePath!=source.sourcePath
            || analysis->hypotheticalLayerOrder[i]!=source.sourcePath) return;
    }
    if(analysis->hypotheticalLayerOrder.last()!=analysis->rootFolder) return;

    const auto simpleLifeMax=[](const DataNode &node,const QString &id,QString *value) {
        if(node.elementName!=QStringLiteral("CUnit") || node.id!=id) return false;
        pugi::xml_document document;
        const QByteArray xml=node.serializedXml.toUtf8();
        if(!document.load_buffer(xml.constData(),size_t(xml.size()),pugi::parse_default,pugi::encoding_utf8)) return false;
        const pugi::xml_node root=document.document_element();
        if(QString::fromUtf8(root.name())!=QStringLiteral("CUnit")) return false;
        int rootAttributes=0;
        for(pugi::xml_attribute attribute:root.attributes()) ++rootAttributes;
        if(rootAttributes!=1 || QString::fromUtf8(root.attribute("id").value())!=id) return false;
        pugi::xml_node field;
        for(pugi::xml_node child:root.children()) {
            if(child.type()!=pugi::node_element || field) return false;
            field=child;
        }
        if(!field || QString::fromUtf8(field.name())!=QStringLiteral("LifeMax")) return false;
        int fieldAttributes=0;
        for(pugi::xml_attribute attribute:field.attributes()) ++fieldAttributes;
        if(fieldAttributes!=1 || !field.attribute("value") || field.first_child()) return false;
        const QString raw=QString::fromUtf8(field.attribute("value").value());
        if(raw.isEmpty()) return false;
        for(QChar character:raw)
            if(character.unicode()<'0' || character.unicode()>'9') return false;
        *value=raw;
        return true;
    };

    QStringList keys=analysis->layeredNamedDeclarations.keys();
    keys.sort(Qt::CaseSensitive);
    for(const QString &key:keys) {
        const auto &refs=analysis->layeredNamedDeclarations.value(key);
        if(refs.size()!=2 && refs.size()!=3) continue;
        const QString id=refs.first().rawId;
        if(id.isEmpty()) continue;
        QVector<QString> values;
        QVector<sc2dh::DeclarationKey> declarations;
        bool valid=true;
        for(int index=0;index<refs.size();++index) {
            const auto &ref=refs[index];
            if(ref.elementName!=QStringLiteral("CUnit") || ref.rawId!=id
                || ref.dependencyLayerIndex!=(index<2 ? index : -1)) { valid=false;break; }
            const auto &nodes=index<2 ? analysis->dependencyLayers[index].nodes : analysis->nodes;
            if(ref.nodeIndex<0 || ref.nodeIndex>=nodes.size()) { valid=false;break; }
            QString value;
            if(!simpleLifeMax(nodes[ref.nodeIndex],id,&value)) { valid=false;break; }
            values.append(value);
            declarations.append(ref.declaration);
        }
        if(!valid) continue;
        EditorLayeredScalarDiagnostic diagnostic;
        diagnostic.object=refs.first().declaration.object;
        diagnostic.field=QStringLiteral("LifeMax");
        diagnostic.value=values.last();
        diagnostic.selectedDeclaration=declarations.last();
        diagnostic.declarationsInOrder=declarations;
        diagnostic.evidence=QStringLiteral("Editor 5.0.16.97563: direct two-folder CUnit.LifeMax B,A / A,B / local observation; diagnostic only");
        analysis->editorLayeredScalars.append(diagnostic);
    }
}

// Local resolution still provides useful raw/source facts, but a dependency
// declaration with the same identity (or exact class default) prevents claiming
// its effective value is known until layer precedence has been established.
inline void markUnresolvedLayerEffects(AnalysisResult *analysis)
{
    if(!analysis || analysis->dependencyLayers.isEmpty()) return;
    QHash<QString,QVector<int>> localByClassAndId;
    for(int index=0;index<analysis->nodes.size();++index) {
        const auto &node=analysis->nodes[index];
        if(node.id.isEmpty()) continue;
        localByClassAndId[node.elementName.trimmed().toCaseFolded()+QChar(0x1f)+node.id.trimmed().toCaseFolded()].append(index);
    }
    const auto hasDependency=[](const QVector<LayeredDeclarationRef> &refs) {
        for(const auto &ref:refs) if(ref.dependencyLayerIndex>=0) return true;
        return false;
    };
    for(auto &node:analysis->nodes) {
        QStringList issues;
        if(hasDependency(analysis->layeredClassDefaults.value(node.elementName.trimmed().toCaseFolded())))
            issues << QStringLiteral("Class default also exists in a dependency layer; precedence is unresolved.");
        QSet<QString> seen;
        QString currentId=node.id;
        QString currentClass=node.elementName;
        const DataNode *current=&node;
        for(int depth=0;current && depth<32;++depth) {
            if(!currentId.isEmpty()) {
                const QString key=catalogIdentityKey(currentClass,currentId);
                if(hasDependency(analysis->layeredNamedDeclarations.value(key))) {
                    issues << QStringLiteral("Identity %1 also exists in a dependency layer; precedence is unresolved.")
                        .arg(currentId);
                    break;
                }
            }
            const QString parent=current->attributes.value(QStringLiteral("parent"));
            if(parent.isEmpty()) break;
            const QString exact=currentClass.trimmed().toCaseFolded()+QChar(0x1f)+parent.trimmed().toCaseFolded();
            if(seen.contains(exact)) break;
            seen.insert(exact);
            const auto candidates=localByClassAndId.value(exact);
            if(candidates.size()!=1) break; // The local resolver records this ambiguity.
            current=&analysis->nodes[candidates.first()];
            currentId=current->id;
            currentClass=current->elementName;
        }
        if(issues.isEmpty()) continue;
        node.resolutionIssues += issues;
        node.unknownReferenceCatalogs << catalogIdentityScope(node.elementName);
        for(auto &value:node.resolvedValues) {
            value.known=false;
            value.rule += QStringLiteral("; dependency layer precedence unresolved");
        }
    }
}
}
