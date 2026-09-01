#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

struct Sc2ArchiveEntryMetadata
{
    QString name;
    quint32 flags = 0;
    quint32 locale = 0;
};

class Sc2Archive
{
public:
    bool load(const QString &archivePath, QString *errorMessage);
    QString archivePath() const { return m_archivePath; }
    int totalEntriesCount() const { return m_allEntries.size(); }
    QStringList allEntries() const { return m_allEntries; }
    QStringList gameDataXmlEntries() const { return m_gameDataEntries; }
    bool readEntry(const QString &entryName, QByteArray *bytes, QString *errorMessage) const;
    // Returns physical MPQ metadata needed by archive-rewrite preflight.
    // A mismatch between physicalEntryCount and entries.size() means the
    // archive inventory is incomplete and compression must fail closed.
    bool inspectEntryMetadata(QVector<Sc2ArchiveEntryMetadata> *entries,
                              int *physicalEntryCount,
                              QString *errorMessage) const;
    bool saveCopy(const QString &targetPath,
                  const QHash<QString, QByteArray> &replacementEntries,
                  const QStringList &removedEntries,
                  QString *errorMessage) const;

private:
    QString m_archivePath;
    QStringList m_allEntries;
    QStringList m_gameDataEntries;
};
