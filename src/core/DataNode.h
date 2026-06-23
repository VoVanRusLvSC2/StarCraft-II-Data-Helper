#pragma once

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
    bool duplicateId = false;
    bool duplicateContent = false;
    bool selectedForRemoval = false;
    bool candidateUnused = false;
};
