#include "core/M3MaterialResolver.h"

void M3MaterialResolver::resolve(M3Model *model, const QHash<QString, QImage> &images) const
{
    if (!model) return;
    for (M3Material &material : model->materials) {
        const QImage image = images.value(material.diffuseTexturePath);
        if (!image.isNull()) material.fallbackColor = image.pixelColor(image.width() / 2, image.height() / 2);
    }
}
