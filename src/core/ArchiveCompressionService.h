#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

namespace sc2dh::compression
{

struct ArchiveCompressionRequest
{
    QString sourceArchivePath;
    QString outputArchivePath;
    qint64 availableBytesOverride = -1;
    std::function<bool()> isCancelled;
};

struct ArchiveCompressionResult
{
    bool success = false;
    QString status = QStringLiteral("BLOCKED");
    QString sourceArchivePath;
    QString outputArchivePath;
    QByteArray sourceSha256Before;
    QByteArray sourceSha256After;
    QByteArray outputSha256;
    qint64 sourceBytes = 0;
    qint64 outputBytes = 0;
    qint64 savedBytes = 0;
    double savedPercent = 0.0;
    qint64 availableBytes = -1;
    qint64 predictedTemporaryBytes = 0;
    int entriesVerified = 0;
    bool sourceUnchanged = false;
    bool structuralVerification = false;
    bool logicalEntryEquality = false;
    QString editorAcceptance = QStringLiteral("NOT_RUN");
    QString strategy = QStringLiteral("verified MPQ compaction; logical entry bytes preserved");
    QStringList warnings;
    QString error;
};

class ArchiveCompressionService
{
public:
    ArchiveCompressionResult compressCompatibleCopy(const ArchiveCompressionRequest &request) const;
};

} // namespace sc2dh::compression
