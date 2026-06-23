#pragma once

#include "core/UnitFamilyDetector.h"

class DataCollectionAliasMapper
{
public:
    QString catalogType(const QString &elementName) const;
    QString aliasFor(const DataNode &node, const QString &rootId, UnitFamilyRole role) const;
};
