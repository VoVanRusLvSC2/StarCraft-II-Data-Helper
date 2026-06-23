#pragma once

#include "core/DataNode.h"

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QSet>
#include <optional>

namespace pugi {
class xml_document;
class xml_node;
}

class XmlLoader
{
public:
    bool loadDocument(const QByteArray &xmlBytes, pugi::xml_document *document, QString *errorMessage) const;
    QString buildNodeLocation(const pugi::xml_node &node) const;
    bool extractNodes(const QString &sourceFile,
                      const QByteArray &xmlBytes,
                      QVector<DataNode> *nodes,
                      QString *errorMessage) const;
    std::optional<int> findNodeIndexByFileAndId(const QVector<DataNode> &nodes,
                                                const QString &sourceFile,
                                                const QString &id) const;
    bool removeNodesByLocation(const QByteArray &xmlBytes,
                               const QSet<QString> &locations,
                               QByteArray *rewrittenXml,
                               QString *errorMessage) const;
};
