#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

struct AnalysisResult;

namespace sc2dh
{
namespace gui { class Registry; }

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
                                  QString *errorMessage,
                                  const QHash<QString, QString> &catalogs = {},
                                  const gui::Registry *guiRegistry = nullptr);

bool previewArchiveReferenceFileRewrites(const QString &rootFolder,
                                         const QStringList &relativeFiles,
                                         const QHash<QString, QString> &renames,
                                         ArchiveReferenceRewriteReport *report,
                                         QString *errorMessage,
                                  const QHash<QString, QString> &catalogs = {},
                                  const gui::Registry *guiRegistry = nullptr);

QHash<QString, QString> unambiguousArchiveReferenceRenames(const AnalysisResult &analysis,
                                                           const QHash<QString, QString> &renames,
                                                           QStringList *skippedIds = nullptr);

} // namespace sc2dh
