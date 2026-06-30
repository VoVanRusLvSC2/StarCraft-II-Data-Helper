#pragma once

#include <QByteArray>
#include <QString>

namespace sc2dh
{

bool repairKnownCatalogEnumDamage(QByteArray *xmlBytes, int *changes, QString *errorMessage = nullptr);

} // namespace sc2dh
