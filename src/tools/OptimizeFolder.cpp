#include "core/ConfigManager.h"
#include "core/FolderAnalyzer.h"
#include "core/MergeService.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <algorithm>

namespace {

bool analyze(const QString &folder, const QSet<QString> &whitelist, AnalysisResult *result, QString *error)
{
    FolderAnalyzer analyzer;
    return analyzer.analyzeFolder(folder, whitelist, result, error);
}

bool writeReport(const QString &path, const QJsonObject &report, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("Cannot write optimization report: %1").arg(path);
        return false;
    }
    file.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);
    const QStringList args = app.arguments();
    if (args.size() < 5 || args.size() > 6) {
        err << "Usage: SC2OptimizeFolder <folder> <rules.json> <whitelist.json> <report.json> [--delete-unused]\n";
        return 2;
    }
    const bool deleteUnused = args.size() == 6 && args.at(5) == QStringLiteral("--delete-unused");

    const QString folder = QDir(args.at(1)).absolutePath();
    ConfigManager config;
    QString error;
    if (!config.load(args.at(2), args.at(3), &error)) {
        err << error << '\n';
        return 3;
    }
    const QSet<QString> whitelist = config.whitelistIds();

    AnalysisResult analysis;
    if (!analyze(folder, whitelist, &analysis, &error)) {
        err << "Initial analysis failed: " << error << '\n';
        return 4;
    }
    const int initialObjects = analysis.totalDataNodes();

    QJsonArray merges;
    int mergedNodes = 0;
    int redirectedReferences = 0;
    int mergePass = 0;
    while (true) {
        const DuplicateContentGroup *candidate = nullptr;
        for (const DuplicateContentGroup &group : analysis.duplicateContentGroups) {
            if (group.mergeCandidate && group.nodeIndices.size() > 1) {
                candidate = &group;
                break;
            }
        }
        if (!candidate) break;
        if (++mergePass > 10000) {
            err << "Merge safety limit exceeded.\n";
            return 5;
        }

        MergeRequest request;
        request.keepNodeIndex = candidate->nodeIndices.first();
        for (int index : candidate->nodeIndices) {
            if (index != request.keepNodeIndex) request.removeNodeIndices.append(index);
        }
        MergeService service;
        const MergePreview preview = service.preview(analysis, request);
        if (!preview.valid) {
            err << "Merge preview failed: " << preview.warnings.join("; ") << '\n';
            return 6;
        }
        const MergeApplyResult applied = service.apply(analysis, request, folder, whitelist);
        if (!applied.success) {
            err << "Merge apply failed: " << applied.error << '\n';
            return 7;
        }
        QJsonObject item;
        item.insert(QStringLiteral("kept"), preview.keptId);
        item.insert(QStringLiteral("removed"), QJsonArray::fromStringList(preview.removedIds));
        item.insert(QStringLiteral("referencesRedirected"), applied.referencesRedirected);
        item.insert(QStringLiteral("nodesDeleted"), applied.nodesDeleted);
        item.insert(QStringLiteral("risk"), preview.riskLevel);
        merges.append(item);
        mergedNodes += applied.nodesDeleted;
        redirectedReferences += applied.referencesRedirected;

        if (!analyze(folder, whitelist, &analysis, &error)) {
            err << "Analysis after merge failed: " << error << '\n';
            return 8;
        }
    }

    QVector<int> safeUnused;
    QJsonArray unusedIds;
    for (const UnusedCandidateInfo &candidate : analysis.unusedCandidates) {
        if (candidate.state != CandidateState::Safe) continue;
        safeUnused.append(candidate.nodeIndex);
        unusedIds.append(analysis.nodes.at(candidate.nodeIndex).id);
    }

    int removedUnused = 0;
    int skippedUnused = 0;
    QString unusedBackup;
    QStringList unusedFiles;
    if (deleteUnused && !safeUnused.isEmpty()) {
        FolderAnalyzer analyzer;
        if (!analyzer.applySelectedChanges(analysis, safeUnused, folder, whitelist, &unusedBackup,
                                           &error, &unusedFiles, &removedUnused, &skippedUnused)) {
            err << "Unused-object apply failed: " << error << '\n';
            return 9;
        }
    }

    AnalysisResult verified;
    if (!analyze(folder, whitelist, &verified, &error)) {
        err << "Final verification failed: " << error << '\n';
        return 10;
    }
    for (const QJsonValue &value : deleteUnused ? unusedIds : QJsonArray{}) {
        const QString removedId = value.toString();
        for (const DataNode &node : verified.nodes) {
            if (node.id == removedId || node.referencedIds.contains(removedId)) {
                err << "Removed unused ID remains after verification: " << removedId << '\n';
                return 11;
            }
        }
    }

    QJsonObject report;
    report.insert(QStringLiteral("success"), true);
    report.insert(QStringLiteral("initialObjects"), initialObjects);
    report.insert(QStringLiteral("finalObjects"), verified.totalDataNodes());
    report.insert(QStringLiteral("mergeGroupsApplied"), merges.size());
    report.insert(QStringLiteral("duplicateNodesRemoved"), mergedNodes);
    report.insert(QStringLiteral("referencesRedirected"), redirectedReferences);
    report.insert(QStringLiteral("unusedObjectsRemoved"), removedUnused);
    report.insert(QStringLiteral("unusedObjectsSkipped"), skippedUnused);
    report.insert(QStringLiteral("unusedSafeCandidatesPreviewed"), safeUnused.size());
    report.insert(QStringLiteral("unusedApplyMode"), deleteUnused
                      ? QStringLiteral("applied")
                      : QStringLiteral("preview-only: archive binary references are not fully analyzable"));
    report.insert(QStringLiteral("merges"), merges);
    report.insert(QStringLiteral("unusedIds"), unusedIds);
    report.insert(QStringLiteral("finalDuplicateMergeCandidates"),
                  int(std::count_if(verified.duplicateContentGroups.cbegin(), verified.duplicateContentGroups.cend(),
                                    [](const DuplicateContentGroup &g) { return g.mergeCandidate; })));
    if (!writeReport(args.at(4), report, &error)) {
        err << error << '\n';
        return 12;
    }
    out << QJsonDocument(report).toJson(QJsonDocument::Compact) << '\n';
    return 0;
}
