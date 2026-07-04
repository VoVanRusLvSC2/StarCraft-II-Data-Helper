#pragma once

#include "core/AnalysisModels.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

class AnalysisIndex
{
public:
    explicit AnalysisIndex(const AnalysisResult &analysis);

    static QString typeIdKey(const QString &elementName, const QString &id);

    const QSet<QString> &knownIds() const { return m_knownIds; }
    const QHash<QString, const DataNode *> &nodesByTypeAndId() const { return m_nodesByTypeAndId; }
    const QHash<QString, QVector<int>> &nodeIndicesById() const { return m_nodeIndicesById; }

private:
    QSet<QString> m_knownIds;
    QHash<QString, const DataNode *> m_nodesByTypeAndId;
    QHash<QString, QVector<int>> m_nodeIndicesById;
};
