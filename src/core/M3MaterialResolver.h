#pragma once

#include "core/M3Model.h"
#include <QHash>
#include <QImage>

class M3MaterialResolver
{
public:
    // Associates discovered texture paths with decoded images without making
    // the M3 parser depend on archives or Qt widgets.
    void resolve(M3Model *model, const QHash<QString, QImage> &images) const;
};
