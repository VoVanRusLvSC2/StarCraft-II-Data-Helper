#include "core/AssetFileRules.h"

#include <QDir>
#include <QSet>

namespace sc2dh::asset
{
bool isBackupOrTrashName(const QString &relative)
{
    const QString normalized = QDir::cleanPath(relative).replace('\\', '/').toLower();
    const QString fileName = QFileInfo(normalized).fileName();
    return normalized.contains(QStringLiteral("/backup_"))
        || fileName.startsWith(QStringLiteral("backup_"))
        || fileName.contains(QStringLiteral(".bak-"))
        || fileName.endsWith(QStringLiteral(".bak"))
        || fileName.endsWith(QStringLiteral(".tmp"))
        || fileName.endsWith(QStringLiteral(".old"))
        || fileName.endsWith(QStringLiteral(".orig"))
        || fileName.endsWith(QStringLiteral(".log"))
        || fileName.endsWith(QStringLiteral(".sc2dh.pending"))
        || fileName == QStringLiteral("analysis_report.txt")
        || fileName == QStringLiteral("planned_changes_report.txt")
        || fileName == QStringLiteral("rename_to_standard_preview.txt")
        || fileName == QStringLiteral("data_collection_preview.txt");
}

bool isLocalizationFile(const QString &relative)
{
    const QString normalized = relative.toLower();
    return normalized.contains(QStringLiteral("localizeddata/"))
        || normalized.contains(QStringLiteral("gamestrings"))
        || normalized.contains(QStringLiteral("objectstrings"));
}

bool isEditorManagedMapFile(const QString &relative)
{
    const QString normalized = QDir::cleanPath(relative).replace('\\', '/').toLower();
    const QString fileName = QFileInfo(normalized).fileName().toLower();
    static const QSet<QString> fileNames = {
        QStringLiteral("minimap.tga"),
        QStringLiteral("lightingmap.tga"),
        QStringLiteral("preloadassetdb.txt"),
        QStringLiteral("descindex.sc2layout"),
        QStringLiteral("descindex.version")
    };
    if (fileNames.contains(fileName))
        return true;
    return normalized == QStringLiteral("base.sc2data/ui/layout/descindex.sc2layout")
        || normalized == QStringLiteral("base.sc2data/ui/layout/descindex.version");
}

bool isMapPreviewImage(const QFileInfo &info, const QString &relative)
{
    const QString normalized = QDir::cleanPath(relative).replace('\\', '/').toLower();
    const QString fileName = QFileInfo(normalized).fileName().toLower();
    static const QSet<QString> imageExtensions = {
        QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"),
        QStringLiteral("tga"), QStringLiteral("bmp"), QStringLiteral("dds")
    };
    if (!imageExtensions.contains(info.suffix().toLower()))
        return false;
    return fileName.contains(QStringLiteral("thumbnail"))
        || fileName.contains(QStringLiteral("thumnail"))
        || fileName.contains(QStringLiteral("screenshot"))
        || fileName.contains(QStringLiteral("screen_shot"))
        || fileName.contains(QStringLiteral("preview"))
        || fileName.contains(QStringLiteral("loading"))
        || normalized.contains(QStringLiteral("/screenshots/"))
        || normalized.contains(QStringLiteral("/screenshot/"))
        || normalized.contains(QStringLiteral("/preview/"))
        || normalized.contains(QStringLiteral("/loading/"));
}

bool isAssetFile(const QFileInfo &info, const QString &relative)
{
    static const QSet<QString> extensions = {
        QStringLiteral("dds"), QStringLiteral("tga"), QStringLiteral("png"), QStringLiteral("jpg"),
        QStringLiteral("jpeg"), QStringLiteral("bmp"), QStringLiteral("m3"), QStringLiteral("ogg"),
        QStringLiteral("wav"), QStringLiteral("mp3"), QStringLiteral("webm"), QStringLiteral("mp4"),
        QStringLiteral("fxa"), QStringLiteral("fxs"), QStringLiteral("fxh"), QStringLiteral("layout"),
        QStringLiteral("sc2layout"), QStringLiteral("txt")
    };
    if (info.exists() && !info.isFile())
        return false;
    if (isBackupOrTrashName(relative) || isLocalizationFile(relative) || isEditorManagedMapFile(relative)
        || isMapPreviewImage(info, relative))
        return false;
    return extensions.contains(info.suffix().toLower());
}

bool isHashableAssetFile(const QFileInfo &info, const QString &relative, qint64 size)
{
    return isAssetFile(info, relative) && size > 0 && size <= 64 * 1024 * 1024;
}

qint64 oversizedAssetThreshold(const QFileInfo &info)
{
    const QString suffix = info.suffix().toLower();
    static const QSet<QString> imageExtensions = {
        QStringLiteral("dds"), QStringLiteral("tga"), QStringLiteral("png"), QStringLiteral("jpg"),
        QStringLiteral("jpeg"), QStringLiteral("bmp")
    };
    static const QSet<QString> modelExtensions = {
        QStringLiteral("m3"), QStringLiteral("m3a"), QStringLiteral("m3h"), QStringLiteral("m3skl")
    };
    static const QSet<QString> audioExtensions = {
        QStringLiteral("ogg"), QStringLiteral("wav"), QStringLiteral("mp3")
    };
    static const QSet<QString> videoExtensions = {
        QStringLiteral("webm"), QStringLiteral("mp4")
    };
    if (imageExtensions.contains(suffix))
        return 8 * 1024 * 1024;
    if (modelExtensions.contains(suffix))
        return 16 * 1024 * 1024;
    if (audioExtensions.contains(suffix))
        return 10 * 1024 * 1024;
    if (videoExtensions.contains(suffix))
        return 32 * 1024 * 1024;
    return 0;
}
}
