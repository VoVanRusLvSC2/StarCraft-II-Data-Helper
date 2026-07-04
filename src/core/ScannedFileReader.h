#pragma once

#include "core/AnalysisModels.h"
#include "core/Sc2Archive.h"

#include <QByteArray>
#include <QString>

class ScannedFileReader
{
public:
    explicit ScannedFileReader(const AnalysisResult &analysis);

    static bool isArchivePath(const QString &path);
    static QString relativePath(const QString &rootFolder, const QString &filePath);

    bool readBytes(const ScannedFileInfo &file, qint64 maxBytes, QByteArray *bytes) const;
    QString readText(const ScannedFileInfo &file, qint64 maxBytes) const;

private:
    QString m_rootFolder;
    bool m_archiveMode = false;
    bool m_archiveLoaded = false;
    Sc2Archive m_archive;
};
