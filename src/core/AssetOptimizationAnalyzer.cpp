#include "core/AssetOptimizationAnalyzer.h"

#include "core/AssetFileRules.h"
#include "core/ScannedFileReader.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QHash>

void AssetOptimizationAnalyzer::appendCandidates(const AnalysisResult &analysis,
                                                 QVector<DeepCleanupCandidate> *candidates) const
{
    if (!candidates)
        return;

    struct AssetInfo
    {
        QString relative;
        QString filePath;
        qint64 size = 0;
    };
    QHash<QByteArray, QVector<AssetInfo>> byHash;
    ScannedFileReader reader(analysis);

    for (const ScannedFileInfo &file : analysis.scannedFiles) {
        const QString rel = ScannedFileReader::relativePath(analysis.rootFolder, file.filePath);
        const QFileInfo info(file.filePath);
        if (!sc2dh::asset::isAssetFile(info, rel))
            continue;

        const qint64 threshold = sc2dh::asset::oversizedAssetThreshold(info);
        if (threshold > 0 && file.size > threshold) {
            DeepCleanupCandidate candidate;
            candidate.index = candidates->size();
            candidate.kind = DeepCleanupKind::AssetAudit;
            candidate.action = DeepCleanupAction::ReportOnly;
            candidate.state = CandidateState::Risky;
            candidate.filePath = file.filePath;
            candidate.label = rel;
            candidate.reason = QStringLiteral("Large imported asset (%1 bytes). Review resolution, mipmaps or audio/video compression manually.").arg(file.size);
            candidate.bytes = file.size;
            candidate.recommended = false;
            candidates->append(candidate);
        }

        if (!sc2dh::asset::isHashableAssetFile(info, rel, file.size))
            continue;
        QByteArray bytes;
        if (!reader.readBytes(file, 64 * 1024 * 1024, &bytes))
            continue;
        const QByteArray hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        byHash[hash].append({rel, file.filePath, file.size});
    }

    for (auto it = byHash.cbegin(); it != byHash.cend(); ++it) {
        const QVector<AssetInfo> group = it.value();
        if (group.size() < 2)
            continue;
        const QString keeper = group.first().relative;
        for (int i = 1; i < group.size(); ++i) {
            DeepCleanupCandidate candidate;
            candidate.index = candidates->size();
            candidate.kind = DeepCleanupKind::AssetAudit;
            candidate.action = DeepCleanupAction::ReportOnly;
            candidate.state = CandidateState::Risky;
            candidate.filePath = group.at(i).filePath;
            candidate.label = group.at(i).relative;
            candidate.reason = QStringLiteral("Byte-identical imported asset also exists as %1. Review references before deleting or redirecting.").arg(keeper);
            candidate.detail = QStringLiteral("sha256=%1").arg(QString::fromLatin1(it.key()));
            candidate.bytes = group.at(i).size;
            candidate.recommended = false;
            candidates->append(candidate);
        }
    }
}
