#pragma once

#include "core/AnalysisModels.h"

#include <QByteArray>
#include <QString>

class AssetReferenceScanner
{
public:
    QString buildCorpus(const AnalysisResult &analysis) const;

    static QString extractPrintableAssetReferences(const QByteArray &bytes);
    static bool stringLooksLikeAssetReference(const QString &value);

private:
    static bool isBinaryAssetReferenceSource(const QString &relative);
};
