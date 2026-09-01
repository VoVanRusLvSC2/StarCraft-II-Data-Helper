#include "core/ArchiveCompressionService.h"

#include "core/Sc2Archive.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QStorageInfo>
#include <QTemporaryFile>

#include <StormLib.h>

namespace
{

bool samePath(const QString &first, const QString &second)
{
    return QDir::cleanPath(QFileInfo(first).absoluteFilePath())
               .compare(QDir::cleanPath(QFileInfo(second).absoluteFilePath()), Qt::CaseInsensitive) == 0;
}

bool fileSha256(const QString &path, QByteArray *digest, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Unable to read %1 for SHA-256: %2").arg(path, file.errorString());
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(4 * 1024 * 1024);
        if (chunk.isEmpty() && file.error() != QFile::NoError) {
            if (error)
                *error = QStringLiteral("Unable to hash %1: %2").arg(path, file.errorString());
            return false;
        }
        hash.addData(chunk);
    }
    if (digest)
        *digest = hash.result();
    return true;
}

bool cancelled(const sc2dh::compression::ArchiveCompressionRequest &request)
{
    return request.isCancelled && request.isCancelled();
}

bool logicalHashes(const Sc2Archive &archive,
                   const sc2dh::compression::ArchiveCompressionRequest &request,
                   QHash<QString, QByteArray> *hashes,
                   QString *error)
{
    hashes->clear();
    for (const QString &entry : archive.allEntries()) {
        if (cancelled(request)) {
            if (error)
                *error = QStringLiteral("Operation cancelled while verifying logical entries.");
            return false;
        }
        QByteArray bytes;
        QString readError;
        if (!archive.readEntry(entry, &bytes, &readError)) {
            if (error)
                *error = QStringLiteral("Unable to read logical entry %1: %2").arg(entry, readError);
            return false;
        }
        hashes->insert(entry.toCaseFolded(), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
    }
    return true;
}

QString stagingPathFor(const QString &outputPath, QString *error)
{
    const QFileInfo outputInfo(outputPath);
    QTemporaryFile reservation(
        QDir(outputInfo.absolutePath()).absoluteFilePath(outputInfo.fileName() + QStringLiteral(".compress-stage-XXXXXX")));
    reservation.setAutoRemove(false);
    if (!reservation.open()) {
        if (error)
            *error = QStringLiteral("Output path is not writable: %1").arg(outputInfo.absolutePath());
        return {};
    }
    const QString path = reservation.fileName();
    reservation.close();
    QFile::remove(path);
    return path;
}

} // namespace

namespace sc2dh::compression
{

ArchiveCompressionResult ArchiveCompressionService::compressCompatibleCopy(
    const ArchiveCompressionRequest &request) const
{
    ArchiveCompressionResult result;
    result.sourceArchivePath = request.sourceArchivePath.isEmpty()
        ? QString() : QFileInfo(request.sourceArchivePath).absoluteFilePath();
    result.outputArchivePath = request.outputArchivePath.isEmpty()
        ? QString() : QFileInfo(request.outputArchivePath).absoluteFilePath();

    const QFileInfo sourceInfo(result.sourceArchivePath);
    if (!sourceInfo.isFile() || !sourceInfo.isReadable()) {
        result.error = QStringLiteral("Source archive is absent or unreadable.");
        return result;
    }
    result.sourceBytes = sourceInfo.size();
    result.predictedTemporaryBytes = result.sourceBytes * 2 + 64ll * 1024ll * 1024ll;

    QString error;
    if (!fileSha256(result.sourceArchivePath, &result.sourceSha256Before, &error)) {
        result.error = error;
        return result;
    }
    const auto finish = [&]() {
        QByteArray latest;
        QString hashError;
        if (fileSha256(result.sourceArchivePath, &latest, &hashError)) {
            result.sourceSha256After = latest;
            result.sourceUnchanged = latest == result.sourceSha256Before;
            if (!result.sourceUnchanged && result.status != QStringLiteral("BLOCKED_SOURCE_CHANGED")) {
                result.success = false;
                result.status = QStringLiteral("BLOCKED_SOURCE_CHANGED");
                result.error = QStringLiteral("Source archive changed while the operation result was finalized.");
            }
        } else {
            result.sourceUnchanged = false;
            result.warnings << QStringLiteral("Final source SHA-256 could not be read: %1").arg(hashError);
        }
        return result;
    };
    if (result.outputArchivePath.isEmpty() || samePath(result.sourceArchivePath, result.outputArchivePath)) {
        result.error = QStringLiteral("Compression output must be a separately named archive.");
        return finish();
    }
    if (QFileInfo::exists(result.outputArchivePath)) {
        result.error = QStringLiteral("Compression output already exists; it was not changed.");
        return finish();
    }
    const QStorageInfo storage(QFileInfo(result.outputArchivePath).absolutePath());
    result.availableBytes = request.availableBytesOverride >= 0
        ? request.availableBytesOverride : storage.bytesAvailable();
    if (!storage.isValid() && request.availableBytesOverride < 0) {
        result.error = QStringLiteral("Unable to determine output filesystem free space.");
        return finish();
    }
    if (result.availableBytes < result.predictedTemporaryBytes) {
        result.status = QStringLiteral("BLOCKED_INSUFFICIENT_SPACE");
        result.error = QStringLiteral("Insufficient free space: need %1 bytes, have %2 bytes.")
                           .arg(result.predictedTemporaryBytes).arg(result.availableBytes);
        return finish();
    }
    if (cancelled(request)) {
        result.status = QStringLiteral("CANCELLED");
        result.error = QStringLiteral("Operation cancelled before archive open.");
        return finish();
    }

    Sc2Archive source;
    if (!source.load(result.sourceArchivePath, &error)) {
        result.error = QStringLiteral("Archive backend preflight failed: %1").arg(error);
        return finish();
    }
    QVector<Sc2ArchiveEntryMetadata> metadata;
    int physicalCount = -1;
    if (!source.inspectEntryMetadata(&metadata, &physicalCount, &error)) {
        result.error = QStringLiteral("Archive flags preflight failed: %1").arg(error);
        return finish();
    }
    if (physicalCount != metadata.size()) {
        result.status = QStringLiteral("BLOCKED_INCOMPLETE_INVENTORY");
        result.error = QStringLiteral("Physical MPQ entry count (%1) differs from the named inventory (%2).")
                           .arg(physicalCount).arg(metadata.size());
        return finish();
    }
    for (const Sc2ArchiveEntryMetadata &entry : metadata) {
        const quint32 unknownFlags = entry.flags & ~quint32(MPQ_FILE_VALID_FLAGS);
        // Compaction operates on a byte-for-byte archive copy and does not
        // rename or re-add entries, so StormLib-supported encrypted entries
        // (including the customary encrypted MPQ listfile) retain their key
        // and flags. Patch/delete/signature records require format-specific
        // handling that this strategy does not claim to support.
        const quint32 unsafeFlags = entry.flags
            & quint32(MPQ_FILE_PATCH_FILE | MPQ_FILE_DELETE_MARKER | MPQ_FILE_SIGNATURE);
        if (unknownFlags != 0 || unsafeFlags != 0) {
            result.status = QStringLiteral("BLOCKED_UNSUPPORTED_ARCHIVE_FLAGS");
            result.error = QStringLiteral("Entry %1 uses unsupported MPQ flags 0x%2 (unknown mask 0x%3).")
                               .arg(entry.name)
                               .arg(entry.flags, 8, 16, QLatin1Char('0'))
                               .arg(unknownFlags, 8, 16, QLatin1Char('0'));
            return finish();
        }
    }

    QHash<QString, QByteArray> beforeHashes;
    if (!logicalHashes(source, request, &beforeHashes, &error)) {
        result.status = cancelled(request) ? QStringLiteral("CANCELLED") : QStringLiteral("BLOCKED_LOGICAL_READ");
        result.error = error;
        return finish();
    }
    if (!fileSha256(result.sourceArchivePath, &result.sourceSha256After, &error)
        || result.sourceSha256After != result.sourceSha256Before) {
        result.status = QStringLiteral("BLOCKED_SOURCE_CHANGED");
        result.error = error.isEmpty() ? QStringLiteral("Source changed during compression preflight.") : error;
        return finish();
    }
    result.sourceUnchanged = true;

    const QString stage = stagingPathFor(result.outputArchivePath, &error);
    if (stage.isEmpty()) {
        result.error = error;
        return finish();
    }
    const auto discardStage = [&]() {
        for (const QString &candidate : {
                 stage,
                 stage + QStringLiteral(".compact"),
                 stage + QStringLiteral(".sc2dh.SC2Map"),
                 stage + QStringLiteral(".sc2dh.SC2Map.compact")}) {
            QFile::remove(candidate);
        }
    };
    if (cancelled(request)) {
        discardStage();
        result.status = QStringLiteral("CANCELLED");
        result.error = QStringLiteral("Operation cancelled before archive write.");
        return finish();
    }
    if (!source.saveCopy(stage, {}, {}, &error)) {
        discardStage();
        result.status = QStringLiteral("BLOCKED_ARCHIVE_WRITE");
        result.error = error;
        return finish();
    }
    if (cancelled(request)) {
        discardStage();
        result.status = QStringLiteral("CANCELLED");
        result.error = QStringLiteral("Operation cancelled before verification.");
        return finish();
    }

    Sc2Archive verification;
    if (!verification.load(stage, &error)) {
        discardStage();
        result.status = QStringLiteral("BLOCKED_REOPEN");
        result.error = QStringLiteral("Compacted archive did not reopen: %1").arg(error);
        return finish();
    }
    QHash<QString, QByteArray> afterHashes;
    if (!logicalHashes(verification, request, &afterHashes, &error)) {
        discardStage();
        result.status = cancelled(request) ? QStringLiteral("CANCELLED") : QStringLiteral("BLOCKED_LOGICAL_VERIFY");
        result.error = error;
        return finish();
    }
    result.entriesVerified = afterHashes.size();
    result.logicalEntryEquality = beforeHashes == afterHashes;
    result.structuralVerification = verification.allEntries().size() == source.allEntries().size();
    if (!result.logicalEntryEquality || !result.structuralVerification) {
        discardStage();
        result.status = QStringLiteral("BLOCKED_SEMANTIC_MISMATCH");
        result.error = QStringLiteral("Logical entry equality or structural verification failed.");
        return finish();
    }

    result.outputBytes = QFileInfo(stage).size();
    if (result.outputBytes >= result.sourceBytes) {
        discardStage();
        result.status = QStringLiteral("NO_COMPATIBLE_SIZE_GAIN");
        result.success = true;
        result.outputBytes = 0;
        result.warnings << QStringLiteral("Verified compaction did not make the archive smaller; no output was created.");
        return finish();
    }

    if (!fileSha256(result.sourceArchivePath, &result.sourceSha256After, &error)
        || result.sourceSha256After != result.sourceSha256Before) {
        discardStage();
        result.sourceUnchanged = false;
        result.status = QStringLiteral("BLOCKED_SOURCE_CHANGED");
        result.error = error.isEmpty() ? QStringLiteral("Source changed before output commit.") : error;
        return finish();
    }
    if (!QFile::rename(stage, result.outputArchivePath)) {
        discardStage();
        result.status = QStringLiteral("BLOCKED_COMMIT");
        result.error = QStringLiteral("Unable to atomically commit compacted output.");
        return finish();
    }
    if (!fileSha256(result.outputArchivePath, &result.outputSha256, &error)) {
        QFile::remove(result.outputArchivePath);
        result.status = QStringLiteral("BLOCKED_OUTPUT_HASH");
        result.error = error;
        return finish();
    }
    if (!fileSha256(result.sourceArchivePath, &result.sourceSha256After, &error)
        || result.sourceSha256After != result.sourceSha256Before) {
        QFile::remove(result.outputArchivePath);
        result.sourceUnchanged = false;
        result.status = QStringLiteral("BLOCKED_SOURCE_CHANGED");
        result.error = error.isEmpty() ? QStringLiteral("Source changed during output commit; generated output was removed.") : error;
        return finish();
    }

    result.sourceUnchanged = true;
    result.outputBytes = QFileInfo(result.outputArchivePath).size();
    result.savedBytes = result.sourceBytes - result.outputBytes;
    result.savedPercent = result.sourceBytes > 0
        ? double(result.savedBytes) * 100.0 / double(result.sourceBytes) : 0.0;
    result.status = QStringLiteral("VERIFIED_PENDING_EDITOR");
    result.success = true;
    return finish();
}

} // namespace sc2dh::compression
