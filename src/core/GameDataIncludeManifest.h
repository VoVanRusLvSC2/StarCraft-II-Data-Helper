#pragma once

#include "core/XmlLoader.h"

#include <QDir>
#include <QSet>
#include <QStringList>
#include <pugixml.hpp>

namespace sc2dh {

inline QString uniqueArchiveEntry(const QStringList &entries, const QString &expected,
                                  const QString &source, QStringList *issues,
                                  bool optional = false)
{
    QStringList matches;
    for (const QString &entry : entries)
        if (QDir::cleanPath(entry).replace('\\', '/').compare(expected, Qt::CaseInsensitive) == 0)
            matches.append(entry);
    if (matches.size() == 1) return matches.first();
    if (matches.isEmpty() && optional) return {};
    if (issues)
        issues->append(QStringLiteral("Archive entry has %1 case-insensitive matches: %2::%3")
            .arg(matches.size()).arg(source, expected));
    return {};
}

// Paths are relative to Base.SC2Data. The manifest describes source selection,
// not catalog override semantics or a proof of runtime layer precedence.
inline bool parseGameDataCatalogIncludes(const QByteArray &bytes, const QString &source,
                                         QStringList *paths, QStringList *issues)
{
    if (!paths || !issues) return false;
    paths->clear();
    pugi::xml_document document;
    QString error;
    if (!XmlLoader().loadDocument(bytes, &document, &error)
        || QString::fromUtf8(document.document_element().name()) != QStringLiteral("Includes")) {
        issues->append(QStringLiteral("GameData include manifest is malformed: %1: %2").arg(source, error));
        return false;
    }
    QSet<QString> seen;
    for (pugi::xml_node child : document.document_element().children()) {
        if (child.type() != pugi::node_element) continue;
        if (QString::fromUtf8(child.name()) != QStringLiteral("Catalog")) {
            issues->append(QStringLiteral("Unsupported GameData include kind in %1: %2")
                .arg(source, QString::fromUtf8(child.name())));
            continue;
        }
        const QString raw = QString::fromUtf8(child.attribute("path").value()).replace('\\', '/');
        const QString clean = QDir::cleanPath(raw);
        if (raw.isEmpty() || raw.startsWith(QLatin1Char('/'))
            || raw.split(QLatin1Char('/')).contains(QStringLiteral(".."))
            || clean.contains(QLatin1Char(':'))
            || !clean.startsWith(QStringLiteral("GameData/"), Qt::CaseInsensitive)
            || !clean.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)) {
            issues->append(QStringLiteral("Unsupported GameData include path in %1: %2").arg(source, raw));
            continue;
        }
        const QString key = clean.toCaseFolded();
        if (seen.contains(key)) {
            issues->append(QStringLiteral("Repeated GameData include in %1: %2").arg(source, raw));
            continue;
        }
        seen.insert(key);
        paths->append(clean);
    }
    return true;
}

} // namespace sc2dh
