#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

// Read-only bridge to the separately installed SC2Editor-MCP-Codex server.
// The optimizer retains its own safety decisions; this result is evidence for
// inspecting effective catalog data, not proof that an object is unused.
struct ModkitEntityInspection
{
    bool success = false;
    QString error;
    QString inputRevision;
    QJsonObject answer;
};

class ModkitMcpClient
{
public:
    static QString namespaceForType(const QString &ctype);
    static ModkitEntityInspection inspect(const QString &sourcePath,
                                          const QString &ctype,
                                          const QString &id,
                                          const QString &mcpEntry,
                                          const QStringList &fields = {});
};
