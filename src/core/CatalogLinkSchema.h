#pragma once

#include "core/DataNode.h"

#include <QSet>
#include <QByteArray>
#include <pugixml.hpp>
#include <QString>

namespace sc2dh
{

void refreshCatalogSchema();
bool scalarCatalogField(const QString &owner, const QString &field, bool attribute);
bool repeatedCatalogField(const QString &owner, const QString &field);
bool repeatedCatalogElement(pugi::xml_node element);
QStringList possibleCatalogReferenceScopes(const QString &owner);
bool catalogSchemaAvailable();
QByteArray catalogSchemaFingerprint();
bool catalogFieldDeclared(pugi::xml_node node, const QString &attribute);
QString catalogReferencePrefix(pugi::xml_node node, const QString &attribute);

QSet<QString> extractCatalogLinkReferences(const DataNode &node);

} // namespace sc2dh
