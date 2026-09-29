#pragma once
#include <QString>

namespace sc2dh {
struct ObjectKey { QString catalog; QString id; };
struct DeclarationKey { ObjectKey object; QString source; QString location; };
struct FieldAddress { QString path; QString carrier; };
enum class ValuePresence { Unknown, Present, Removed };
struct ResolvedValue {
    QString raw;
    QString effective;
    QString source;
    QString rule;
    bool known = false;
    FieldAddress field;
    DeclarationKey declaration;
    ValuePresence presence = ValuePresence::Unknown;
};
// Source declarations only: XSD multiplicity does not establish how SC2
// composes inherited, indexed or removed array entries at runtime.
struct ArrayItemDeclaration {
    QString field;
    FieldAddress address;
    DeclarationKey declaration;
    int fieldOrdinal = -1;
    bool hasIndex = false;
    QString rawIndex;
    bool hasRemoved = false;
    QString rawRemoved;
    bool hasValue = false;
    QString rawValue;
    bool hasLink = false;
    QString rawLink;
};
struct ReferenceEdge { ObjectKey source; ObjectKey target; FieldAddress field; QString evidence; bool rewritable = false; };
struct CoverageIssue { QString catalog; QString source; QString reason; };
}
