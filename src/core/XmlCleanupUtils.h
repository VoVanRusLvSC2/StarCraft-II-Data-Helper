#pragma once

#include <QString>

#include <pugixml.hpp>

namespace sc2dh::xmlcleanup
{
QString locationSegmentForNode(const pugi::xml_node &node);
QString canonicalNode(const pugi::xml_node &node, bool lenient, bool objectRoot = false);
bool loadSerializedRoot(const QString &serializedXml, pugi::xml_document *document, pugi::xml_node *root);
}
