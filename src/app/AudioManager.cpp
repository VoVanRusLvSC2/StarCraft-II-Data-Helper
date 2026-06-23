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
        applySettings();
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
    const QString escapedPath = QDir::toNativeSeparators(m_runtimeTrackPath).replace(QStringLiteral("\""), QStringLiteral("\"\""));
    const QString openCommand = QStringLiteral("open \"%1\" type waveaudio alias sc2dh_music").arg(escapedPath);
    mciSendStringW(reinterpret_cast<LPCWSTR>(openCommand.utf16()), nullptr, 0, nullptr);
    const int volume = int(clampVolume(musicVolume()) * 1000.0);
    const QString volumeCommand = QStringLiteral("setaudio sc2dh_music volume to %1").arg(volume);
    mciSendStringW(reinterpret_cast<LPCWSTR>(volumeCommand.utf16()), nullptr, 0, nullptr);
    mciSendStringW(L"play sc2dh_music repeat", nullptr, 0, nullptr);
#endif
}

void AudioManager::stopMusic()
{
#ifdef Q_OS_WIN
    mciSendStringW(L"stop sc2dh_music", nullptr, 0, nullptr);
    mciSendStringW(L"close sc2dh_music", nullptr, 0, nullptr);
#endif
}
