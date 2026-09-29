#include "core/FolderAnalyzer.h"
#include "core/DependencySourceResolver.h"
#include "core/Sc2Archive.h"
#include "core/UnifiedReferenceIndex.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <algorithm>
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#endif
int main(int argc,char **argv) {
 QCoreApplication app(argc,argv); const auto args=app.arguments();
 if(args.size()<3 || (args.size()-3)%2!=0) return 2;
 QStringList dependencyRoots;
 QHash<QString,QString> dependencyMappings;
 for(int index=3;index<args.size();index+=2) {
  if(args[index]==QStringLiteral("--dependency-root")) {
   dependencyRoots.append(args[index+1]);
  } else if(args[index]==QStringLiteral("--dependency-map")) {
   const QString mapping=args[index+1];
   const int separator=mapping.indexOf(QLatin1Char('='));
   if(separator<=0 || separator==mapping.size()-1) return 2;
   const QString handle=mapping.left(separator).trimmed().toCaseFolded();
   const QString relative=mapping.mid(separator+1).trimmed();
   if(handle.isEmpty() || relative.isEmpty()
      || (dependencyMappings.contains(handle) && dependencyMappings.value(handle)!=relative)) return 2;
   dependencyMappings.insert(handle,relative);
  } else return 2;
 }
 QElapsedTimer timer; timer.start(); AnalysisResult result; QString error;
 FolderAnalyzer analyzer;
 if(QFileInfo(args[1]).isDir()) {
  if(!analyzer.analyzeFolder(args[1],{},&result,&error,{}, {},dependencyRoots,dependencyMappings)) return 3;
 } else {
  Sc2Archive archive; if(!archive.load(args[1],&error)) return 4;
  result.rootFolder=args[1]; result.sourceDiscoveryComplete=true;
  result.sourceRevisions.append(captureSourceRevision(args[1],&error));
  const auto game=archive.gameDataXmlEntries();
  for(const QString &entry:archive.allEntries()) {
   QByteArray bytes; if(!archive.readEntry(entry,&bytes,&error)) {result.unreadableSources<<entry;continue;}
   const QString name=entry.toLower();
   const bool text=name.endsWith(".xml")||name.endsWith(".galaxy")||name.endsWith(".txt")||name=="objects"||name=="triggers"||name=="documentinfo";
   result.scannedFiles.append({entry,name.endsWith(".xml"),text,bytes.size()});
   if(text && bytes.size()>16*1024*1024) result.unsupportedSources<<entry+": oversized source";
   if(game.contains(entry)) {
    QVector<DataNode> nodes;
    if(!XmlLoader().extractNodes(entry,bytes,&nodes,&error)) result.parseErrors.append({entry,error});
    else result.nodes+=nodes;
    result.sourceXmlByFile.insert(entry,QString::fromUtf8(bytes));
   }
  }
  if(!analyzer.finalizeAnalysisResult(&result,{},&error,{}, {},dependencyRoots,dependencyMappings)) return 5;
 }
 const qint64 elapsed=timer.elapsed();
 sc2dh::refs::UnifiedReferenceIndex index; index.build(result);
 int safe=0,unknown=0;QHash<QString,int> reasonCounts;
 for(const auto &candidate:result.unusedCandidates) {
  if(candidate.state==CandidateState::Safe) ++safe;
  if(candidate.removalSafety==RemovalSafety::Unknown||candidate.removalSafety==RemovalSafety::BlockedIncompleteAnalysis) {
   ++unknown;++reasonCounts[candidate.reason];
  }
 }
 qint64 peak=-1;
#ifdef Q_OS_WIN
 PROCESS_MEMORY_COUNTERS counters{};
 if(GetProcessMemoryInfo(GetCurrentProcess(),&counters,sizeof(counters))) peak=qint64(counters.PeakWorkingSetSize);
#endif
 QJsonArray incomplete;for(const auto &issue:result.incompleteSources) incomplete.append(issue);
 QStringList reasonKeys=reasonCounts.keys();reasonKeys.sort(Qt::CaseSensitive);
 QJsonArray reasons;
 for(const QString &reason:reasonKeys)
  reasons.append(QJsonObject{{"reason",reason},{"count",reasonCounts.value(reason)}});
 QVector<SourceRevision> sortedRevisions=result.sourceRevisions;
 std::sort(sortedRevisions.begin(),sortedRevisions.end(),[](const auto &a,const auto &b) {
  return a.filePath<b.filePath;
 });
 QJsonArray sourceFingerprints;
 for(const auto &revision:sortedRevisions)
  sourceFingerprints.append(QJsonObject{{"source",revision.filePath},
   {"size",double(revision.size)},{"sha256",QString::fromLatin1(revision.sha256.toHex())}});
 QJsonArray dependencies;
 for(const auto &declaration:result.declaredDependencies)
  dependencies.append(QJsonObject{{"source",declaration.sourceFile},{"ordinal",declaration.ordinal},
   {"raw",declaration.raw},{"bnet_handle",declaration.bnetHandle},{"file_fallback",declaration.fileFallback}});
 QJsonArray dependencySources;
 for(const auto &source:result.dependencySources)
  dependencySources.append(QJsonObject{{"owner",source.ownerSource},{"source",source.sourcePath},
   {"declared_at",source.declaration.sourceFile},{"ordinal",source.declaration.ordinal},
   {"raw",source.declaration.raw},{"depth",source.depth},
   {"resolved_file_fallback",source.resolvedFileFallback},
   {"explicit_handle_mapping",source.explicitHandleMapping},
   {"status",sc2dh::dependencySourceStatusName(source.status)},
   {"sha256",QString::fromLatin1(source.sha256.toHex())},{"issue",source.issue}});
 QJsonArray dependencyLayers;
 for(const auto &layer:result.dependencyLayers) {
  QJsonArray entries;for(const auto &entry:layer.gameDataEntries) entries.append(entry);
  QJsonArray inactiveEntries;for(const auto &entry:layer.inactiveGameDataEntries) inactiveEntries.append(entry);
  QJsonArray issues;for(const auto &issue:layer.issues) issues.append(issue);
  dependencyLayers.append(QJsonObject{{"source",layer.sourcePath},
   {"sha256",QString::fromLatin1(layer.sha256.toHex())},
   {"active_game_data",layer.activeGameData},
   {"include_manifest_present",layer.includeManifestPresent},
   {"game_data_entries",entries},{"inactive_game_data_entries",inactiveEntries},
   {"object_count",layer.nodes.size()},{"issues",issues}});
 }
 int layeredIdentityCollisions=0, layeredCrossSourceIdentities=0, layeredCaseVariants=0;
 QJsonArray hypotheticalOverrides;
 QStringList layeredKeys=result.layeredNamedDeclarations.keys();
 layeredKeys.sort(Qt::CaseSensitive);
 for(const QString &key:layeredKeys) {
  const auto &refs=result.layeredNamedDeclarations.value(key);
  if(refs.size()<2) continue;
  ++layeredIdentityCollisions;
  QSet<int> layers;QSet<QString> rawIds;
  QJsonArray declarations;
  for(const auto &ref:refs) {
   layers.insert(ref.dependencyLayerIndex);rawIds.insert(ref.rawId);
   declarations.append(QJsonObject{{"source",ref.declaration.source},
    {"location",ref.declaration.location},{"class",ref.elementName},
    {"raw_id",ref.rawId},{"dependency_layer_index",ref.dependencyLayerIndex}});
  }
  if(layers.size()>1) ++layeredCrossSourceIdentities;
  if(rawIds.size()>1) ++layeredCaseVariants;
  if(layers.size()>1)
   hypotheticalOverrides.append(QJsonObject{{"identity",key},
    {"declarations_in_index_order",declarations}});
 }
 int layeredDefaultCollisions=0;
 for(auto it=result.layeredClassDefaults.cbegin();it!=result.layeredClassDefaults.cend();++it)
  if(it.value().size()>1) ++layeredDefaultCollisions;
 QJsonArray editorScalarDiagnostics;
 for(const auto &diagnostic:result.editorLayeredScalars) {
  QJsonArray declarations;
  for(const auto &declaration:diagnostic.declarationsInOrder)
   declarations.append(QJsonObject{{"source",declaration.source},{"location",declaration.location}});
  editorScalarDiagnostics.append(QJsonObject{{"catalog",diagnostic.object.catalog},
   {"id",diagnostic.object.id},{"field",diagnostic.field},{"value",diagnostic.value},
   {"selected_source",diagnostic.selectedDeclaration.source},
   {"selected_location",diagnostic.selectedDeclaration.location},
   {"declarations_in_order",declarations},{"evidence",diagnostic.evidence},
   {"runtime_or_safe_proof",false}});
 }
 QJsonObject dependencyMappingJson;
 qint64 rawArrayDeclarations=0, arraySourceLinks=0;
 for(const auto &node:result.nodes) {
  rawArrayDeclarations+=node.arrayDeclarations.size();
  arraySourceLinks+=node.arraySourceChain.size();
 }
 for(auto it=result.dependencyHandleMappings.cbegin();it!=result.dependencyHandleMappings.cend();++it)
  dependencyMappingJson.insert(it.key(),it.value());
 QJsonObject report{{"report_format_version",2},{"input",args[1]},
 {"duration_ms",double(elapsed)},{"peak_working_set_bytes",double(peak)},
 {"files",result.scannedFiles.size()},{"objects",result.nodes.size()},{"references",index.records().size()},
 {"raw_array_declarations",double(rawArrayDeclarations)},
 {"array_source_links",double(arraySourceLinks)},
 {"safe_candidates",safe},{"unknown_candidates",unknown},{"unknown_reasons",reasons},
 {"source_completeness",analysisCompletenessName(result.completeness)},{"dependency_graph_complete",result.dependencyGraphComplete},
 {"dependency_sources_located",result.dependencySourcesLocated},
 {"hypothetical_layer_order",QJsonArray::fromStringList(result.hypotheticalLayerOrder)},
 {"declared_dependencies",dependencies},{"dependency_sources",dependencySources},
 {"dependency_layers",dependencyLayers},
 {"inactive_game_data_sources",QJsonArray::fromStringList(result.inactiveGameDataSources)},
 {"layered_identity_count",result.layeredNamedDeclarations.size()},
 {"layered_identity_collisions",layeredIdentityCollisions},
 {"layered_cross_source_identities",layeredCrossSourceIdentities},
 {"layered_case_variants",layeredCaseVariants},
 {"hypothetical_overrides",hypotheticalOverrides},
 {"editor_observed_scalar_diagnostics",editorScalarDiagnostics},
 {"layered_class_default_collisions",layeredDefaultCollisions},
 {"dependency_search_roots",QJsonArray::fromStringList(result.dependencySearchRoots)},
 {"dependency_handle_mappings",dependencyMappingJson},
 {"source_fingerprints",sourceFingerprints},
 {"catalog_schema_sha256",QString::fromLatin1(result.catalogSchemaRevision.toHex())},
 {"optimization_settings_sha256",QString::fromLatin1(result.optimizationSettingsRevision.toHex())},
 {"incomplete_sources",incomplete}};
 QFile output(args[2]);if(!output.open(QIODevice::WriteOnly))return 6;
 output.write(QJsonDocument(report).toJson());return 0;
}
