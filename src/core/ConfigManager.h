#pragma once

#include <QSet>
#include <QString>

class ConfigManager
{
public:
    bool load(const QString &rulesPath, const QString &whitelistPath, QString *errorMessage);
    QSet<QString> whitelistIds() const { return m_whitelistIds; }

private:
    QSet<QString> m_whitelistIds;
};
