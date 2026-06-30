#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

struct AnalysisResult;

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

QHash<QString, QString> unambiguousArchiveReferenceRenames(const AnalysisResult &analysis,
                                                           const QHash<QString, QString> &renames,
                                                           QStringList *skippedIds = nullptr);

} // namespace sc2dh
