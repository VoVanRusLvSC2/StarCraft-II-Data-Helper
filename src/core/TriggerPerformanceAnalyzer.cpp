#include "core/TriggerPerformanceAnalyzer.h"

#include "core/ScannedFileReader.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace
{
bool isTriggerOrGalaxyFile(const QString &relative, const QFileInfo &info)
{
    const QString lower = relative.toLower();
    const QString fileName = info.fileName().toLower();
    const QString suffix = info.suffix().toLower();
    return suffix == QStringLiteral("galaxy")
        || suffix == QStringLiteral("sc2lib")
        || fileName == QStringLiteral("triggers")
        || fileName == QStringLiteral("mapscript.galaxy")
        || lower.contains(QStringLiteral("trigger"))
        || lower.contains(QStringLiteral("/libs/"));
}
}

void TriggerPerformanceAnalyzer::appendCandidates(const AnalysisResult &analysis,
                                                  QVector<DeepCleanupCandidate> *candidates) const
{
    if (!candidates)
        return;

    static const QRegularExpression periodicEvent(
        QStringLiteral("\\b(?:TriggerAddEventTimePeriodic|TimerStart)\\s*\\([^\\n;]*?,\\s*([0-9]+(?:\\.[0-9]+)?)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression xmlPeriodic(
        QStringLiteral("(?:Periodic|Timer|Interval|Period)[^\\n]{0,80}?\\b(?:value|Value|period|Period)\\s*=\\s*\"([0-9]+(?:\\.[0-9]+)?)\""),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression unitGroupScan(
        QStringLiteral("\\b(?:UnitGroup|UnitGroupLoop|UnitGroupCount|UnitFilter|libNtve_gf_UnitsInRegion|libNtve_gf_UnitGroup)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression catalogAccess(
        QStringLiteral("\\bCatalogFieldValue(?:Get|Set)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression loopLike(
        QStringLiteral("\\b(?:for|while)\\s*\\("),
        QRegularExpression::CaseInsensitiveOption);

    int emitted = 0;
    ScannedFileReader reader(analysis);
    for (const ScannedFileInfo &file : analysis.scannedFiles) {
        if (emitted >= 80)
            break;
        const QString rel = ScannedFileReader::relativePath(analysis.rootFolder, file.filePath);
        const QFileInfo info(file.filePath);
        if (!isTriggerOrGalaxyFile(rel, info) || file.size <= 0 || file.size > 4 * 1024 * 1024)
            continue;
        QByteArray bytes;
        if (!reader.readBytes(file, 4 * 1024 * 1024, &bytes))
            continue;
        QString text = QString::fromUtf8(bytes);
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        const QStringList lines = text.split(QLatin1Char('\n'));
        const bool fileHasLoop = loopLike.match(text).hasMatch();
        QSet<QString> seenReasons;
        for (int i = 0; i < lines.size() && emitted < 80; ++i) {
            const QString line = lines.at(i);
            QString reason;
            QRegularExpressionMatch match = periodicEvent.match(line);
            if (!match.hasMatch())
                match = xmlPeriodic.match(line);
            if (match.hasMatch()) {
                bool ok = false;
                const double interval = match.captured(1).toDouble(&ok);
                if (ok && interval > 0.0 && interval <= 0.25)
                    reason = QStringLiteral("Very frequent periodic trigger/timer interval (%1 seconds). Review for FPS/CPU cost.").arg(interval);
            }
            if (reason.isEmpty() && unitGroupScan.match(line).hasMatch())
                reason = QStringLiteral("Unit-group or region scan in trigger/Galaxy code. Review call frequency and cache the result when possible.");
            if (reason.isEmpty() && fileHasLoop && catalogAccess.match(line).hasMatch())
                reason = QStringLiteral("Catalog field access appears in a file with loops. Cache catalog reads outside hot loops when possible.");
            if (reason.isEmpty())
                continue;

            const QString seenKey = QStringLiteral("%1:%2").arg(rel, reason);
            if (seenReasons.contains(seenKey))
                continue;
            seenReasons.insert(seenKey);

            DeepCleanupCandidate candidate;
            candidate.index = candidates->size();
            candidate.kind = DeepCleanupKind::TriggerPerformance;
            candidate.action = DeepCleanupAction::ReportOnly;
            candidate.state = CandidateState::Risky;
            candidate.filePath = file.filePath;
            candidate.label = rel;
            candidate.reason = reason;
            candidate.lineNumber = i;
            candidate.detail = line.left(600);
            candidate.recommended = false;
            candidates->append(candidate);
            ++emitted;
        }
    }
}
