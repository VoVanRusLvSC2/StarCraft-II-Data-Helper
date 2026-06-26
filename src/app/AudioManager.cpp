#include "app/AudioManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include <cmath>

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmsystem.h>
#endif

namespace
{

constexpr double kDefaultMusicVolume = 0.32;
constexpr const wchar_t *kMusicAlias = L"sc2dh_music";

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

quint16 readLe16(const QByteArray &bytes, int offset)
{
    return quint16(uchar(bytes[offset])) | (quint16(uchar(bytes[offset + 1])) << 8);
}

quint32 readLe32(const QByteArray &bytes, int offset)
{
    return quint32(uchar(bytes[offset])) | (quint32(uchar(bytes[offset + 1])) << 8)
        | (quint32(uchar(bytes[offset + 2])) << 16) | (quint32(uchar(bytes[offset + 3])) << 24);
}

void writeLe16(QByteArray *bytes, int offset, qint16 value)
{
    (*bytes)[offset] = char(quint16(value) & 0xff);
    (*bytes)[offset + 1] = char((quint16(value) >> 8) & 0xff);
}

QByteArray scaledPcmWav(QByteArray bytes, double volume)
{
    if (bytes.size() < 44 || bytes.mid(0, 4) != "RIFF" || bytes.mid(8, 4) != "WAVE")
        return bytes;

    int fmtOffset = -1;
    int dataOffset = -1;
    quint32 dataSize = 0;
    for (int offset = 12; offset + 8 <= bytes.size();) {
        const QByteArray chunkId = bytes.mid(offset, 4);
        const quint32 chunkSize = readLe32(bytes, offset + 4);
        const int payload = offset + 8;
        if (payload + int(chunkSize) > bytes.size())
            break;
        if (chunkId == "fmt ")
            fmtOffset = payload;
        else if (chunkId == "data") {
            dataOffset = payload;
            dataSize = chunkSize;
            break;
        }
        offset = payload + int(chunkSize) + int(chunkSize % 2);
    }

    if (fmtOffset < 0 || dataOffset < 0 || fmtOffset + 16 > bytes.size())
        return bytes;
    const quint16 audioFormat = readLe16(bytes, fmtOffset);
    const quint16 bitsPerSample = readLe16(bytes, fmtOffset + 14);
    if (audioFormat != 1 || bitsPerSample != 16)
        return bytes;

    const double bounded = clampVolume(volume);
    const int end = qMin(bytes.size(), dataOffset + int(dataSize));
    for (int offset = dataOffset; offset + 1 < end; offset += 2) {
        const qint16 sample = qint16(readLe16(bytes, offset));
        const int scaled = qBound(-32768, int(std::lround(double(sample) * bounded)), 32767);
        writeLe16(&bytes, offset, qint16(scaled));
    }
    return bytes;
}

#ifdef Q_OS_WIN
QString mciQuote(const QString &path)
{
    QString native = QDir::toNativeSeparators(path);
    native.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("\"%1\"").arg(native);
}

bool sendMci(const QString &command)
{
    return mciSendStringW(reinterpret_cast<LPCWSTR>(command.utf16()), nullptr, 0, nullptr) == 0;
}
#endif

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

void AudioManager::shutdown()
{
    stopMusic();
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
    const int volumeStep = qBound(0, int(musicVolume() * 100.0 + 0.5), 100);
    const QString targetPath = QDir(targetDir).absoluteFilePath(QStringLiteral("nova_cue2_%1.wav").arg(volumeStep, 3, 10, QLatin1Char('0')));

    QFile resource(QStringLiteral(":/music/nova_cue2.wav"));
    if (!resource.open(QIODevice::ReadOnly))
    {
        return QString();
    }
    const QByteArray bytes = scaledPcmWav(resource.readAll(), volumeStep / 100.0);
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
    const QString requestedTrackPath = ensureRuntimeTrackPath();
    if (!requestedTrackPath.isEmpty() && requestedTrackPath != m_runtimeTrackPath) {
        stopMusic();
        m_runtimeTrackPath = requestedTrackPath;
    }
    if (m_runtimeTrackPath.isEmpty() || !QFileInfo::exists(m_runtimeTrackPath))
    {
        return;
    }

    if (!m_musicOpen)
    {
        sendMci(QStringLiteral("close %1").arg(QString::fromWCharArray(kMusicAlias)));
        const QString open = QStringLiteral("open %1 type waveaudio alias %2")
                                 .arg(mciQuote(m_runtimeTrackPath), QString::fromWCharArray(kMusicAlias));
        if (!sendMci(open))
        {
            return;
        }
        m_musicOpen = true;
    }

    const int mciVolume = qBound(0, int(musicVolume() * 1000.0 + 0.5), 1000);
    sendMci(QStringLiteral("setaudio %1 volume to %2").arg(QString::fromWCharArray(kMusicAlias)).arg(mciVolume));
    sendMci(QStringLiteral("play %1 repeat").arg(QString::fromWCharArray(kMusicAlias)));
#endif
}

void AudioManager::stopMusic()
{
#ifdef Q_OS_WIN
    if (m_musicOpen)
    {
        sendMci(QStringLiteral("stop %1").arg(QString::fromWCharArray(kMusicAlias)));
        sendMci(QStringLiteral("close %1").arg(QString::fromWCharArray(kMusicAlias)));
        m_musicOpen = false;
    }
#endif
}
