#include "app/UpdateChecker.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

UpdateChecker::UpdateChecker(QObject *parent) : QObject(parent), m_manager(new QNetworkAccessManager(this)) {}

void UpdateChecker::check()
{
    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/VoVanRusLvSC2/StarCraft-II-Data-Helper/releases/latest")));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("SC2DataHelper/%1").arg(QCoreApplication::applicationVersion()));
    request.setRawHeader("Accept", "application/vnd.github+json");
    QNetworkReply *reply = m_manager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const QByteArray payload = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit checkFailed(reply->errorString());
            reply->deleteLater();
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(payload);
        const QJsonObject object = document.object();
        const QString tag = object.value(QStringLiteral("tag_name")).toString();
        const QUrl url(object.value(QStringLiteral("html_url")).toString());
        if (tag.isEmpty() || !url.isValid()) {
            emit checkFailed(QStringLiteral("GitHub release response is invalid."));
        } else if (tag != QStringLiteral("v%1").arg(QCoreApplication::applicationVersion())) {
            emit updateAvailable(tag, url);
        }
        reply->deleteLater();
    });
}
