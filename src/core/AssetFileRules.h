#pragma once

#include <QFileInfo>
#include <QString>

namespace sc2dh::asset
{
bool isBackupOrTrashName(const QString &relative);
bool isLocalizationFile(const QString &relative);
bool isEditorManagedMapFile(const QString &relative);
bool isMapPreviewImage(const QFileInfo &info, const QString &relative);
bool isAssetFile(const QFileInfo &info, const QString &relative);
bool isHashableAssetFile(const QFileInfo &info, const QString &relative, qint64 size);
qint64 oversizedAssetThreshold(const QFileInfo &info);
}
