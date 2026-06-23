#pragma once

#include <QByteArray>
#include <QString>

namespace pugi {
class xml_document;
class xml_node;
}

class XmlFormatter
{
public:
    QString formatDocument(const QByteArray &xmlBytes, QString *errorMessage) const;
    QString formatNode(const pugi::xml_node &node, QString *errorMessage) const;
};
