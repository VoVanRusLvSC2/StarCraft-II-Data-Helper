#pragma once

#include "core/AnalysisModels.h"
#include "core/CatalogLinkSchema.h"
#include "core/CatalogProtection.h"
#include "core/XmlLoader.h"
#include "core/XmlParsePolicy.h"
#include "core/CatalogModelTypes.h"
#include <QRegularExpression>
#include <functional>
#include <sstream>
#include <algorithm>
#include <memory>

namespace sc2dh {
// IDs follow the existing application's case-folded catalog policy. Preserve raw
// spelling for serialization; separate declarations keep their original location.
inline void resolveCatalogReferences(AnalysisResult *analysis)
{
    struct Binding { QString unitKey; QString actorKey; QString actorId; };
    QVector<Binding> bindings;
    QHash<QString, QStringList> referenceScopesByClass;
    QHash<QString, QVector<int>> declarations;
    QHash<QString, QVector<int>> classDefaults;
    for (int i=0; i<analysis->nodes.size(); ++i) {
        const auto &node=analysis->nodes[i];
        if (!node.id.isEmpty()) declarations[catalogIdentityKey(node.elementName,node.id)].append(i);
        else if (node.attributes.value(QStringLiteral("default")) == QStringLiteral("1"))
            classDefaults[node.elementName].append(i);
    }
    for (int i=0; i<analysis->nodes.size(); ++i) {
        DataNode &node=analysis->nodes[i];
        node.referenceEdges.clear();
        node.resolvedValues.clear();
        node.arrayDeclarations.clear();
        node.arraySourceChain.clear();
        node.resolutionIssues.clear();
        node.unknownReferenceCatalogs.clear();
        QVector<int> chain;
        QSet<int> seen;
        int current=i;
        int depth=0;
        bool unresolvedParentChain=false;
        while (current>=0 && !seen.contains(current) && depth++<32) {
            seen.insert(current); chain.prepend(current);
            const DataNode &declaration=analysis->nodes[current];
            const QString parent=declaration.attributes.value(QStringLiteral("parent"));
            if (parent.isEmpty()) break;
            const auto parents=declarations.value(catalogIdentityKey(declaration.elementName,parent));
            if (parents.size()!=1) {
                // Do not choose an arbitrary override or another catalog's ID.
                node.resolutionIssues << QStringLiteral("Parent %1 has %2 local declarations; dependency/layer resolution required.").arg(parent).arg(parents.size());
                unresolvedParentChain=true;
                break;
            }
            current=parents.first();
            if (seen.contains(current)) {
                node.resolutionIssues << QStringLiteral("Parent cycle at %1").arg(parent);
                unresolvedParentChain=true;
            }
        }
        if(current>=0 && !seen.contains(current) && depth>=32) {
            node.resolutionIssues << QStringLiteral("Parent chain exceeds the 32-declaration resolution limit.");
            unresolvedParentChain=true;
        }
        if (!node.id.isEmpty()) {
            const auto defaults=classDefaults.value(node.elementName);
            if (defaults.size()==1) chain.prepend(defaults.first());
            else if (defaults.size()>1)
                node.resolutionIssues << QStringLiteral("Class %1 has %2 local default declarations; layer order is unresolved.")
                    .arg(node.elementName).arg(defaults.size());
        }
        node.arraySourceChain=chain;
        if (!node.resolutionIssues.isEmpty()) {
            if (!referenceScopesByClass.contains(node.elementName))
                referenceScopesByClass.insert(node.elementName, possibleCatalogReferenceScopes(node.elementName));
            node.unknownReferenceCatalogs += referenceScopesByClass.value(node.elementName);
        }
        // Token and reference traversals share one parse of each ancestor in this
        // descendant context; the documents are released after this declaration.
        QHash<int, std::shared_ptr<pugi::xml_document>> documents;
        for (int index : chain) {
            auto document = std::make_shared<pugi::xml_document>();
            const QByteArray raw = analysis->nodes[index].serializedXml.toUtf8();
            if (document->load_buffer(raw.constData(),size_t(raw.size()),xmlParseFlags))
                documents.insert(index, document);
        }
        QHash<QString,QString> tokens;
        for (int index:chain) {
            if (!documents.contains(index)) continue;
            const auto &doc = *documents.value(index);
            for (pugi::xml_node child:doc.document_element().children()) {
                if (child.type()!=pugi::node_pi || QString::fromUtf8(child.name())!=QStringLiteral("token")) continue;
                pugi::xml_document pi;
                const QByteArray declaration=QByteArray("<token ")+child.value()+"/>";
                if (!pi.load_buffer(declaration.constData(),size_t(declaration.size()),xmlParseFlags)) {
                    node.resolutionIssues << QStringLiteral("Malformed token declaration"); continue;
                }
                const auto token=pi.document_element();
                const QString id=QString::fromUtf8(token.attribute("id").value());
                if (!id.isEmpty() && token.attribute("value")) tokens[id]=QString::fromUtf8(token.attribute("value").value());
            }
        }
        static const QRegularExpression expression(QStringLiteral("##([^#]+)##"));
        std::function<QString(QString,QSet<QString>)> expand;
        expand=[&](QString value,QSet<QString> active) {
            auto matches=expression.globalMatch(value);
            QVector<QPair<QRegularExpressionMatch,QString>> replacements;
            while(matches.hasNext()) {
                const auto match=matches.next(); const QString name=match.captured(1);
                if (!tokens.contains(name) || active.contains(name) || active.size()>32) {
                    node.resolutionIssues << QStringLiteral("Unresolved or cyclic token %1").arg(name); continue;
                }
                auto next=active; next.insert(name);
                replacements.append({match,expand(tokens.value(name),next)});
            }
            for(auto it=replacements.crbegin();it!=replacements.crend();++it)
                value.replace(it->first.capturedStart(),it->first.capturedLength(),it->second);
            return value;
        };
        QHash<QString,ResolvedValue> scalarByField;
        for (int index:chain) {
            if (!documents.contains(index)) continue;
            const DataNode &declaration=analysis->nodes[index];
            const auto &doc=*documents.value(index);
            const pugi::xml_node root=doc.document_element();
            const QString rootPath=XmlLoader().buildNodeLocation(root);
            QSet<QString> seenFields;
            std::function<void(pugi::xml_node)> captureArrays=[&](pugi::xml_node parent) {
                QHash<QString,int> ordinals;
                for(pugi::xml_node child:parent.children()) {
                    if(child.type()!=pugi::node_element) continue;
                    const QString field=QString::fromUtf8(child.name());
                    const int ordinal=++ordinals[field];
                    if(repeatedCatalogElement(child)) {
                        const auto capture=[&](const char *name,bool *present,QString *raw) {
                            const pugi::xml_attribute attribute=child.attribute(name);
                            *present=bool(attribute);
                            if(attribute) *raw=QString::fromUtf8(attribute.value());
                        };
                        ArrayItemDeclaration item;
                        item.field=field;
                        item.address={declaration.originalLocation
                            +XmlLoader().buildNodeLocation(child).mid(rootPath.size()),{}};
                        item.declaration={{catalogIdentityScope(declaration.elementName),declaration.id},
                            declaration.sourceFile,declaration.originalLocation};
                        item.fieldOrdinal=ordinal;
                        capture("index",&item.hasIndex,&item.rawIndex);
                        capture("removed",&item.hasRemoved,&item.rawRemoved);
                        capture("value",&item.hasValue,&item.rawValue);
                        capture("Link",&item.hasLink,&item.rawLink);
                        node.arrayDeclarations.append(std::move(item));
                    }
                    captureArrays(child);
                }
            };
            if(index==i) captureArrays(root);
            for (pugi::xml_attribute attribute:root.attributes()) {
                const QString field=QString::fromUtf8(attribute.name());
                if (field==QStringLiteral("id") || field==QStringLiteral("parent")
                    || field==QStringLiteral("default")
                    || !scalarCatalogField(node.elementName,field,true)
                    || !catalogReferencePrefix(root,field).isEmpty())
                    continue;
                const QString raw=QString::fromUtf8(attribute.value());
                const QString effective=expand(raw,{});
                seenFields.insert(field);
                scalarByField.insert(field,ResolvedValue{
                    raw,effective,declaration.sourceFile,
                    index==i ? QStringLiteral("explicit local scalar attribute")
                        : declaration.id.isEmpty() ? QStringLiteral("unique local class default scalar attribute")
                                                   : QStringLiteral("unique local parent scalar attribute"),
                    !effective.contains(QStringLiteral("##")),
                    {declaration.originalLocation,field},
                    {{catalogIdentityScope(declaration.elementName),declaration.id},
                     declaration.sourceFile,declaration.originalLocation},
                    ValuePresence::Present});
            }
            for (pugi::xml_node child:root.children()) {
                if (child.type()!=pugi::node_element) continue;
                const QString field=QString::fromUtf8(child.name());
                if (repeatedCatalogField(node.elementName,field)) continue;
                if (!scalarCatalogField(node.elementName,field,false)
                    || !catalogReferencePrefix(child,QStringLiteral("value")).isEmpty())
                    continue;
                const pugi::xml_attribute valueAttribute=child.attribute("value");
                const bool removed=bool(child.attribute("removed"));
                if (!valueAttribute && !removed) continue;
                const QString raw=valueAttribute ? QString::fromUtf8(valueAttribute.value()) : QString();
                const QString effective=removed ? QString() : expand(raw,{});
                const bool duplicate=seenFields.contains(field);
                seenFields.insert(field);
                const QString localPath=XmlLoader().buildNodeLocation(child);
                ResolvedValue resolved{
                    raw,effective,declaration.sourceFile,
                    index==i ? QStringLiteral("explicit local scalar")
                        : declaration.id.isEmpty() ? QStringLiteral("unique local class default scalar")
                                                   : QStringLiteral("unique local parent scalar"),
                    !removed && !duplicate && !effective.contains(QStringLiteral("##")),
                    {declaration.originalLocation+localPath.mid(rootPath.size()),QStringLiteral("value")},
                    {{catalogIdentityScope(declaration.elementName),declaration.id},
                     declaration.sourceFile,declaration.originalLocation},
                    removed ? ValuePresence::Removed : duplicate ? ValuePresence::Unknown : ValuePresence::Present};
                scalarByField.insert(field,resolved);
            }
        }
        QStringList scalarNames=scalarByField.keys();
        std::sort(scalarNames.begin(),scalarNames.end());
        for (const QString &field:scalarNames)
            node.resolvedValues.append(scalarByField.value(field));
        QSet<QString> targets;
        QSet<QString> ids;
        // Retain all inherited link dependencies conservatively. This is a
        // reachability over-approximation, not proof of array effective equality.
        for(int index:chain) {
            if (!documents.contains(index)) continue;
            const auto &doc = *documents.value(index);
            const DataNode &origin=analysis->nodes[index];
            const QString originRootPath=XmlLoader().buildNodeLocation(doc.document_element());
            const auto sourcePath=[&](pugi::xml_node element) {
                const QString local=XmlLoader().buildNodeLocation(element);
                return origin.originalLocation+local.mid(originRootPath.size());
            };
            std::function<void(pugi::xml_node)> visit=[&](pugi::xml_node element) {
                if(element.type()!=pugi::node_element) return;
                if(catalogIdentityScope(node.elementName)==QStringLiteral("cactor") && QString::fromUtf8(element.name())==QStringLiteral("On")) {
                    const auto carrier=[&](const char *field) {
                        if(element.attribute(field)) return QString::fromUtf8(element.attribute(field).value());
                        const auto child=element.child(field);
                        return child.attribute("value") ? QString::fromUtf8(child.attribute("value").value()) : QString::fromUtf8(child.child_value());
                    };
                    const QString rawTerms=carrier("Terms");
                    const QString terms=expand(rawTerms,{}).trimmed();
                    const QString send=expand(carrier("Send"),{}).trimmed();
                    static const QRegularExpression birth(QStringLiteral("^UnitBirth\\.([A-Za-z0-9_@]+)$"));
                    const auto match=birth.match(terms);
                    if(match.hasMatch() && send==QStringLiteral("Create")) {
                        const QString unit=match.captured(1);
                        const QString actorKey=catalogIdentityKey(node.elementName,node.id);
                        ids.insert(unit); targets.insert(catalogIdentityKey(QStringLiteral("CUnit"),unit));
                        bindings.append({catalogIdentityKey(QStringLiteral("CUnit"),unit),actorKey,node.id});
                        node.referenceEdges.append({{catalogIdentityScope(node.elementName),node.id},{QStringLiteral("cunit"),unit},
                            {sourcePath(element),QStringLiteral("Terms")},QStringLiteral("UnitBirth/Create actor binding"),!rawTerms.contains(QStringLiteral("##"))});
                    } else if(terms.contains(QStringLiteral("##")) || terms.contains(QLatin1Char('*'))) {
                        node.resolutionIssues << QStringLiteral("Unresolved actor event term: %1").arg(rawTerms);
                        node.unknownReferenceCatalogs << QStringLiteral("cunit") << QStringLiteral("cactor");
                    }
                }

                for(pugi::xml_attribute attribute:element.attributes()) {
                    const QString field=QString::fromUtf8(attribute.name());
                    if(field==QStringLiteral("id") || field==QStringLiteral("parent")) continue;
                    const QString prefix=catalogReferencePrefix(element,field);
                    const QString rawValue=QString::fromUtf8(attribute.value());
                    if(prefix.isEmpty()) {
                        if(rawValue.contains(QStringLiteral("##"))) expand(rawValue,{});
                        continue;
                    }
                    const QString value=expand(rawValue,{});
                    if(value.contains(QStringLiteral("##"))) {
                        node.unknownReferenceCatalogs << catalogIdentityScope(prefix);
                        continue;
                    }
                    if(isSafeAutomaticObjectId(value)) {
                        ids.insert(value); targets.insert(catalogIdentityKey(prefix,value));
                        const bool fromLocalDefault = index != i
                            && analysis->nodes[index].id.isEmpty()
                            && analysis->nodes[index].attributes.value(QStringLiteral("default")) == QStringLiteral("1");
                        node.referenceEdges.append({{catalogIdentityScope(node.elementName),node.id},
                            {catalogIdentityScope(prefix),value}, {sourcePath(element),field},
                            index==i ? QStringLiteral("declared typed link")
                                : fromLocalDefault ? QStringLiteral("unique local class default typed link")
                                                   : QStringLiteral("inherited conservative typed link"),
                            (index==i || fromLocalDefault) && !QString::fromUtf8(attribute.value()).contains(QStringLiteral("##"))});
                        node.resolvedValues.append({QString::fromUtf8(attribute.value()), value,
                            origin.sourceFile, QStringLiteral("token expansion in descendant context"), true,
                            {sourcePath(element),field},
                            {{catalogIdentityScope(origin.elementName),origin.id},
                             origin.sourceFile,origin.originalLocation},
                            ValuePresence::Present});
                    }
                }
                for(pugi::xml_node child:element.children()) visit(child);
            };
            visit(doc.document_element());
        }
        const QString parent=node.attributes.value(QStringLiteral("parent"));
        if(!parent.isEmpty()) {ids.insert(parent); targets.insert(catalogIdentityKey(node.elementName,parent));}
        // Compatibility references whose grammar has no established catalog keep
        // conservative fan-out; structural links no longer cross catalog scopes.
        for(const QString &id:node.referencedIds) {
            bool qualified=false;
            for(const QString &target:targets) if(target.section(QChar(0x1f),1)==id.toCaseFolded()) {qualified=true; break;}
            if(!qualified) targets.insert(QStringLiteral("*")+QChar(0x1f)+id.toCaseFolded());
            ids.insert(id);
        }
        node.referenceKeys=targets.values();
        std::sort(node.referenceKeys.begin(),node.referenceKeys.end());
        node.referencedIds=ids.values();
        std::sort(node.referencedIds.begin(),node.referencedIds.end());
        if(unresolvedParentChain) {
            for(auto &value:node.resolvedValues) {
                const bool explicitLocal=value.declaration.source==node.sourceFile
                    && value.declaration.location==node.originalLocation;
                if(explicitLocal && !value.raw.contains(QStringLiteral("##"))) continue;
                value.known=false;
                value.rule += QStringLiteral("; parent chain unresolved");
            }
        }
        node.resolutionIssues.removeDuplicates();
    }
    for(const auto &binding:bindings) {
        for(int index:declarations.value(binding.unitKey)) {
            auto &unit=analysis->nodes[index];
            if(!unit.referenceKeys.contains(binding.actorKey)) unit.referenceKeys.append(binding.actorKey);
            if(!unit.referencedIds.contains(binding.actorId)) unit.referencedIds.append(binding.actorId);
            std::sort(unit.referenceKeys.begin(),unit.referenceKeys.end());
            std::sort(unit.referencedIds.begin(),unit.referencedIds.end());
        }
    }
}
}
