#include "core/SemanticDuplicateAnalyzer.h"

#include "core/CatalogProtection.h"
#include "core/XmlCleanupUtils.h"

#include <QCryptographicHash>
#include <QHash>
#include <QSet>
#include <QStringList>

void SemanticDuplicateAnalyzer::appendCandidates(const AnalysisResult &analysis,
                                                 QVector<DeepCleanupCandidate> *candidates) const
{
    if (!candidates)
        return;

    QHash<QString, QVector<int>> groups;
    for (int i = 0; i < analysis.nodes.size(); ++i) {
        const DataNode &node = analysis.nodes.at(i);
        if (node.id.isEmpty() || sc2dh::isProtectedCatalogNode(node))
            continue;
        pugi::xml_document document;
        pugi::xml_node root;
        if (!sc2dh::xmlcleanup::loadSerializedRoot(node.serializedXml, &document, &root))
            continue;
        const QString canonical = sc2dh::xmlcleanup::canonicalNode(root, true, true);
        if (canonical.size() < 16)
            continue;
        const QByteArray hash = QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha256).toHex();
        groups[node.elementName.toLower() + QChar(0x1f) + QString::fromLatin1(hash)].append(i);
    }

    int emitted = 0;
    for (auto it = groups.cbegin(); it != groups.cend() && emitted < 120; ++it) {
        const QVector<int> group = it.value();
        if (group.size() < 2)
            continue;
        QSet<QString> exactHashes;
        QStringList ids;
        QStringList files;
        for (int index : group) {
            const DataNode &node = analysis.nodes.at(index);
            exactHashes.insert(node.contentHash);
            ids << node.id;
            files << node.sourceFile;
        }
        if (exactHashes.size() < 2)
            continue;
        ids.removeDuplicates();
        files.removeDuplicates();

        const DataNode &first = analysis.nodes.at(group.first());
        DeepCleanupCandidate candidate;
        candidate.index = candidates->size();
        candidate.kind = DeepCleanupKind::NearDuplicateObject;
        candidate.action = DeepCleanupAction::ReportOnly;
        candidate.state = CandidateState::Risky;
        candidate.filePath = first.sourceFile;
        candidate.label = QStringLiteral("%1: %2").arg(first.elementName, ids.mid(0, 6).join(QStringLiteral(", ")));
        candidate.reason = QStringLiteral("Objects match after ignoring editor-only text/icon/category fields. Review manually before merging.");
        candidate.detail = QStringLiteral("Objects: %1\nFiles: %2").arg(ids.join(QStringLiteral(", ")), files.join(QStringLiteral(", ")));
        candidate.recommended = false;
        candidates->append(candidate);
        ++emitted;
    }
}
