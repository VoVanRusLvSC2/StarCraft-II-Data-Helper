#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

namespace sc2dh
{

struct ArchiveReferenceRewriteReport
{
    QStringList changedFiles;
    QStringList blockedFiles;
    int replacements = 0;
};

bool rewriteArchiveReferenceFiles(const QString &rootFolder,
                                  const QStringList &relativeFiles,
                                  const QHash<QString, QString> &renames,
                                  ArchiveReferenceRewriteReport *report,
                                  QString *errorMessage);

} // namespace sc2dh
