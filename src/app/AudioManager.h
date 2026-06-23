#pragma once

#include <QObject>
#include <QString>

class AudioManager : public QObject
{
    Q_OBJECT

public:
    static AudioManager *instance();

    void initialize();
    void applySettings();

    static bool isMusicEnabled();
    static double musicVolume();
    static void setMusicSettings(bool enabled, double volume);

private:
    explicit AudioManager(QObject *parent = nullptr);
    QString ensureRuntimeTrackPath();
    void startMusic();
    void stopMusic();

    QString m_runtimeTrackPath;
    bool m_initialized = false;
};
