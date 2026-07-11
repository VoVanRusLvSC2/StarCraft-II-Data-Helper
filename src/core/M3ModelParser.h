#pragma once

#include "core/M3Model.h"

#include <QByteArray>

class M3ModelParser
{
public:
    // Parses the stable static mesh portion. The parser is deliberately
    // bounds-checked because M3 files may be imported from arbitrary maps.
    bool parseStaticModel(const QByteArray &bytes, M3Model *model, QString *error) const;
};
