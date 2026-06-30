#pragma once

#include "core/DataNode.h"

#include <QSet>
#include <QString>

namespace sc2dh
{

QSet<QString> extractCatalogLinkReferences(const DataNode &node);

} // namespace sc2dh
