#include "app/AudioManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmsystem.h>
#endif

namespace
{

constexpr double kDefaultMusicVolume = 0.32;

double clampVolume(double value)
{
    if (value < 0.0)
    {
        return 0.0;
    }
    if (value > 1.0)
    {
        return 1.0;
    }
    return value;
}

} // namespace

AudioManager *AudioManager::instance()
{
    static AudioManager manager;
    return &manager;
}

AudioManager::AudioManager(QObject *parent)
    : QObject(parent)
{
}

void AudioManager::initialize()
{
    if (m_initialized)
    {
        return;
    }

    m_runtimeTrackPath = ensureRuntimeTrackPath();
    m_initialized = true;
    applySettings();
}

void AudioManager::applySettings()
{
    if (!m_initialized)
    {
        return;
    }

    if (isMusicEnabled())
    {
        startMusic();
    }
    else
    {
        stopMusic();
    }
}

bool AudioManager::isMusicEnabled()
{
    QSettings settings;
    return settings.value(QStringLiteral("audio/musicEnabled"), true).toBool();
}

double AudioManager::musicVolume()
{
    QSettings settings;
    return clampVolume(settings.value(QStringLiteral("audio/musicVolume"), kDefaultMusicVolume).toDouble());
}

void AudioManager::setMusicSettings(bool enabled, double volume)
{
    QSettings settings;
    settings.setValue(QStringLiteral("audio/musicEnabled"), enabled);
    settings.setValue(QStringLiteral("audio/musicVolume"), clampVolume(volume));
    instance()->applySettings();
}

QString AudioManager::ensureRuntimeTrackPath()
{
    const QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (baseDir.isEmpty())
    {
        return QString();
    }

    const QString targetDir = QDir(baseDir).absoluteFilePath(QStringLiteral("audio"));
    QDir().mkpath(targetDir);
    const QString targetPath = QDir(targetDir).absoluteFilePath(QStringLiteral("nova_cue2.wav"));

    QFile resource(QStringLiteral(":/music/nova_cue2.wav"));
    if (!resource.open(QIODevice::ReadOnly))
    {
        return QString();
    }
    const QByteArray bytes = resource.readAll();
    resource.close();

    QFile existing(targetPath);
    if (existing.open(QIODevice::ReadOnly))
    {
        if (existing.readAll() == bytes)
        {
            existing.close();
            return targetPath;
        }
        existing.close();
    }

    QFile output(targetPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return QString();
    }
    output.write(bytes);
    output.close();
    return targetPath;
}

void AudioManager::startMusic()
{
#ifdef Q_OS_WIN
    if (m_runtimeTrackPath.isEmpty() || !QFileInfo::exists(m_runtimeTrackPath))
    {
        return;
    }

    stopMusic();
    PlaySoundW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(m_runtimeTrackPath).utf16()),
               nullptr,
               SND_FILENAME | SND_ASYNC | SND_LOOP | SND_NODEFAULT);
#endif
}

void AudioManager::stopMusic()
{
#ifdef Q_OS_WIN
    PlaySoundW(nullptr, nullptr, 0);
#endif
}
