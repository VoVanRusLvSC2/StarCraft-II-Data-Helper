#pragma once
#include "core/CatalogModelTypes.h"

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

struct DataNode
{
    QString sourceFile;
    QString elementName;
    QString parentNode;
    QString id;
    QString originalLocation;
    QString contentHash;
    QString serializedXml;
    int lineNumber = -1;
    QMap<QString, QString> attributes;
    QVector<QString> referencedIds;
    QStringList referenceKeys;
    QStringList resolutionIssues;
    QStringList unknownReferenceCatalogs;
    QVector<sc2dh::ReferenceEdge> referenceEdges;
    QVector<sc2dh::ResolvedValue> resolvedValues;
    QVector<sc2dh::ArrayItemDeclaration> arrayDeclarations;
    QVector<int> arraySourceChain;
    bool duplicateId = false;
    bool duplicateContent = false;
    bool selectedForRemoval = false;
    bool candidateUnused = false;
};
