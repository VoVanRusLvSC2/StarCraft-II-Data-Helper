#include "core/ScannedFileReader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

ScannedFileReader::ScannedFileReader(const AnalysisResult &analysis)
    : m_rootFolder(analysis.rootFolder),
      m_archiveMode(isArchivePath(analysis.rootFolder))
{
    if (m_archiveMode) {
        QString error;
        m_archiveLoaded = m_archive.load(analysis.rootFolder, &error);
    }
}

bool ScannedFileReader::isArchivePath(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    static const QSet<QString> extensions = {
        QStringLiteral("sc2map"), QStringLiteral("sc2mod"), QStringLiteral("sc2components"),
        QStringLiteral("sc2campaign"), QStringLiteral("sc2archive")
    };
    return extensions.contains(suffix);
}

QString ScannedFileReader::relativePath(const QString &rootFolder, const QString &filePath)
{
    if (!QDir::isAbsolutePath(filePath))
        return QDir::cleanPath(filePath).replace('\\', '/');
    const QFileInfo rootInfo(rootFolder);
    if (rootInfo.exists() && rootInfo.isFile())
        return QDir::cleanPath(filePath).replace('\\', '/');
    return QDir(rootFolder).relativeFilePath(filePath).replace('\\', '/');
}

bool ScannedFileReader::readBytes(const ScannedFileInfo &file, qint64 maxBytes, QByteArray *bytes) const
{
    if (!bytes || file.size > maxBytes)
        return false;
    bytes->clear();
    if (m_archiveMode && !QDir::isAbsolutePath(file.filePath)) {
        if (!m_archiveLoaded)
            return false;
        QString error;
        return m_archive.readEntry(file.filePath, bytes, &error) && bytes->size() <= maxBytes;
    }
    QFile source(file.filePath);
    if (!source.open(QIODevice::ReadOnly))
        return false;
    *bytes = source.read(maxBytes + 1);
    return bytes->size() <= maxBytes;
}

QString ScannedFileReader::readText(const ScannedFileInfo &file, qint64 maxBytes) const
{
    QByteArray bytes;
    if (!readBytes(file, maxBytes, &bytes))
        return {};
    return QString::fromUtf8(bytes);
}
