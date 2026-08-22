#include "core/DecorationMapCopyService.h"

#include "core/Sc2Archive.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace
{

QString defaultOutputPath(const QString &sourceArchivePath)
{
    const QFileInfo info(sourceArchivePath);
    const QString suffix = info.suffix().isEmpty() ? QStringLiteral("SC2Map") : info.suffix();
    return QDir(info.absolutePath()).absoluteFilePath(
        QStringLiteral("%1_DecorOptimized.%2").arg(info.completeBaseName(), suffix));
}

bool samePath(const QString &left, const QString &right)
{
    return QFileInfo(left).absoluteFilePath().compare(QFileInfo(right).absoluteFilePath(), Qt::CaseInsensitive) == 0;
}

} // namespace

namespace sc2dh::decor
{

DecorOptimizedMapResult DecorationMapCopyService::createOptimizedCopy(const DecorOptimizedMapRequest &request) const
{
    DecorOptimizedMapResult result;
    result.outputArchivePath = request.outputArchivePath.isEmpty()
        ? defaultOutputPath(request.sourceArchivePath)
        : request.outputArchivePath;

    if (request.sourceArchivePath.isEmpty() || !QFileInfo::exists(request.sourceArchivePath)) {
        result.error = QStringLiteral("Source SC2 archive does not exist.");
        return result;
    }
    if (result.outputArchivePath.isEmpty()) {
        result.error = QStringLiteral("Output archive path is empty.");
        return result;
    }
    if (samePath(request.sourceArchivePath, result.outputArchivePath)) {
        result.error = QStringLiteral("Decoration optimized copy must not overwrite the source archive.");
        return result;
    }
    if (QFileInfo::exists(result.outputArchivePath)) {
        if (!request.overwriteExisting) {
            result.error = QStringLiteral("Output archive already exists.");
            return result;
        }
        if (!QFile::remove(result.outputArchivePath)) {
            result.error = QStringLiteral("Unable to remove existing output archive.");
            return result;
        }
    }

    Sc2Archive archive;
    QString error;
    if (!archive.load(request.sourceArchivePath, &error)) {
        result.error = QStringLiteral("Unable to open source archive: %1").arg(error);
        return result;
    }

    QByteArray objectsBytes;
    if (!archive.readEntry(request.objectsEntry, &objectsBytes, &error)) {
        result.error = QStringLiteral("Unable to read Objects entry: %1").arg(error);
        return result;
    }
    QByteArray mapScriptBytes;
    if (!archive.readEntry(request.mapScriptEntry, &mapScriptBytes, &error)) {
        result.error = QStringLiteral("Unable to read MapScript entry: %1").arg(error);
        return result;
    }

    DecorationStreamingPlanner planner;
    result.patch = planner.prepareArchivePatch(objectsBytes,
                                               mapScriptBytes,
                                               request.zones,
                                               request.galaxyOptions,
                                               request.objectsEntry,
                                               request.mapScriptEntry,
                                               request.runtimeEntry);
    result.warnings += result.patch.warnings;
    if (!result.patch.valid) {
        result.error = result.patch.error;
        return result;
    }

    if (!archive.saveCopy(result.outputArchivePath, result.patch.replacementEntries, {}, &error)) {
        QFile::remove(result.outputArchivePath);
        result.error = QStringLiteral("Unable to save decoration optimized copy: %1").arg(error);
        return result;
    }

    Sc2Archive verification;
    if (!verification.load(result.outputArchivePath, &error)) {
        QFile::remove(result.outputArchivePath);
        result.error = QStringLiteral("Decoration optimized copy verification failed: %1").arg(error);
        return result;
    }
    for (auto it = result.patch.replacementEntries.cbegin(); it != result.patch.replacementEntries.cend(); ++it) {
        QByteArray actual;
        if (!verification.readEntry(it.key(), &actual, &error)) {
            QFile::remove(result.outputArchivePath);
            result.error = QStringLiteral("Decoration optimized copy is missing %1: %2").arg(it.key(), error);
            return result;
        }
        if (actual != it.value()) {
            QFile::remove(result.outputArchivePath);
            result.error = QStringLiteral("Decoration optimized copy verification failed for %1.").arg(it.key());
            return result;
        }
    }

    result.removedDoodads = result.patch.artifacts.removedDoodadIndices.size();
    result.success = true;
    return result;
}

} // namespace sc2dh::decor
