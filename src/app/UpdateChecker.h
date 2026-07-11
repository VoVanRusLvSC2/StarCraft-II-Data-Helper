#pragma once

#include <QObject>
#include <QUrl>

class QNetworkAccessManager;

class UpdateChecker final : public QObject
{
    Q_OBJECT
public:
    explicit UpdateChecker(QObject *parent = nullptr);
    void check();

signals:
    void updateAvailable(const QString &version, const QUrl &downloadUrl);
    void checkFailed(const QString &message);

private:
    QNetworkAccessManager *m_manager = nullptr;
};
