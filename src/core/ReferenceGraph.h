#pragma once

#include "core/DataNode.h"

#include <QHash>
#include <QStringList>
#include <QVector>

class ReferenceGraph
{
public:
    void build(const QVector<DataNode> &nodes);
    QStringList inboundReferencesFor(const QString &id) const;
    QStringList outboundReferencesFor(const QString &id) const;

private:
    QHash<QString, QStringList> m_inbound;
    QHash<QString, QStringList> m_outbound;
};
