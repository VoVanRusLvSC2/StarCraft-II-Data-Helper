#include "core/ConfigManager.h"

#include <QFile>
#include <QFileInfo>
#include <QIODevice>

#include <nlohmann/json.hpp>

bool ConfigManager::load(const QString &rulesPath, const QString &whitelistPath, QString *errorMessage)
{
    m_whitelistIds.clear();

    QFile rulesFile(rulesPath);
    if (!rulesFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unable to open rules config: %1").arg(rulesPath);
        }
        return false;
    }
    const QByteArray rulesRaw = rulesFile.readAll();
    rulesFile.close();

    try {
        (void)nlohmann::json::parse(rulesRaw.constData(), rulesRaw.constData() + rulesRaw.size());
    } catch (const std::exception &e) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to parse rules JSON: %1").arg(QString::fromUtf8(e.what()));
        }
        return false;
    }

    QFile whitelistFile(whitelistPath);
    if (!whitelistFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unable to open whitelist config: %1").arg(whitelistPath);
        }
        return false;
    }

    const QByteArray raw = whitelistFile.readAll();
    whitelistFile.close();

    try {
        const auto json = nlohmann::json::parse(raw.constData(), raw.constData() + raw.size());
        if (json.is_array()) {
            for (const auto &entry : json) {
                if (entry.is_string()) {
                    m_whitelistIds.insert(QString::fromStdString(entry.get<std::string>()));
                }
            }
        }
    } catch (const std::exception &e) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to parse whitelist JSON: %1").arg(QString::fromUtf8(e.what()));
        }
        return false;
    }

    return true;
}
