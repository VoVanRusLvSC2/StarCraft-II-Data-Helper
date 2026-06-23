#pragma once

#include "core/DataNode.h"

#include <QHash>
#include <QString>
#include <QVector>

struct DuplicateGroup
{
    QString key;
    QVector<int> indices;
};

class DuplicateFinder
{
public:
    QVector<DuplicateGroup> findDuplicateIds(const QVector<DataNode> &nodes) const;
    QVector<DuplicateGroup> findDuplicateContentHashes(const QVector<DataNode> &nodes) const;
};
