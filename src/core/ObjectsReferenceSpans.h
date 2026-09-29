#pragma once

#include <QByteArray>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QVector>
#include <functional>

namespace sc2dh::objects {

struct Occurrence
{
    qsizetype start = -1;
    qsizetype length = 0;
    QString value;
    QString catalog;
    QString reason;
    bool rewritable = false;
};

struct ScanResult
{
    QVector<Occurrence> occurrences;
    bool complete = true;
    QString issue;
};

inline bool isPlacementPath(const QString &path)
{
    const QString normalized=path.trimmed().replace('\\','/');
    return normalized.compare(QStringLiteral("Objects"),Qt::CaseInsensitive)==0
        || normalized.endsWith(QStringLiteral("/Objects"),Qt::CaseInsensitive);
}

inline bool decodeText(const QByteArray &bytes, QString *decoded)
{
    if (!decoded) return false;
    QByteArray payload=bytes;
    if (payload.startsWith("\xEF\xBB\xBF")) payload.remove(0,3);
    if (payload.startsWith("\xFE\xFF")) return false;
    const bool bomUtf16=payload.startsWith("\xFF\xFE");
    if (bomUtf16) payload.remove(0,2);
    if (bomUtf16 || payload.contains('\0')) {
        if (payload.size()%2!=0) return false;
        const qsizetype pairs=qMin<qsizetype>(payload.size()/2,4096);
        qsizetype zeroHigh=0;
        for(qsizetype i=0;i<pairs;++i) if(payload.at(i*2+1)=='\0') ++zeroHigh;
        if(!bomUtf16 && pairs>0 && zeroHigh*100<pairs*70) return false;
        *decoded=QString::fromUtf16(reinterpret_cast<const char16_t *>(payload.constData()),payload.size()/2);
        return QByteArray(reinterpret_cast<const char *>(decoded->utf16()),decoded->size()*2)==payload;
    }
    *decoded=QString::fromUtf8(payload);
    return decoded->toUtf8()==payload;
}

// Exact source coordinates are shared by reference analysis and mutation.
// Only text-form ObjectUnit/ObjectDoodad carriers have established scopes.
inline ScanResult scan(const QString &source,const QSet<QString> &targetIds)
{
    ScanResult result;
    if(targetIds.isEmpty()) return result;
    QSet<QString> keys;
    for(const QString &id:targetIds) keys.insert(id.toCaseFolded());
    const auto targeted=[&](const QString &value) {return keys.contains(value.toCaseFolded());};
    static const QRegularExpression token(QStringLiteral("(?<![A-Za-z0-9_@])([A-Za-z0-9_@]+)(?![A-Za-z0-9_@])"));
    const auto unknownPart=[&](const QString &part,qsizetype offset,const QString &reason) {
        auto matches=token.globalMatch(part);
        while(matches.hasNext()) {
            const auto match=matches.next();
            const QString value=match.captured(1);
            if(targeted(value)) result.occurrences.append({offset+match.capturedStart(1),value.size(),value,{},reason,false});
        }
    };
    if(source.trimmed().startsWith(QLatin1Char('<'))) {
        unknownPart(source,0,QStringLiteral("Objects XML carrier grammar is not established"));
        return result;
    }
    QVector<QString> scopes;
    QString pendingScope;
    const auto isIdChar=[](QChar c) {return c.isLetterOrNumber() || c==QLatin1Char('_') || c==QLatin1Char('@');};
    const auto currentScope=[&]() {return scopes.isEmpty() ? QString() : scopes.constLast();};
    for(qsizetype i=0;i<source.size();) {
        if(source.mid(i,2)==QStringLiteral("//") || source[i]==QLatin1Char('#')) {
            const qsizetype end=source.indexOf(QLatin1Char('\n'),i);
            i=end<0 ? source.size() : end;continue;
        }
        if(source.mid(i,2)==QStringLiteral("/*")) {
            const qsizetype end=source.indexOf(QStringLiteral("*/"),i+2);
            if(end<0) {result.complete=false;result.issue=QStringLiteral("Unterminated Objects comment.");return result;}
            i=end+2;continue;
        }
        if(source[i]==QLatin1Char('{')) {
            scopes.append(pendingScope.isEmpty()?currentScope():pendingScope);
            pendingScope.clear();++i;continue;
        }
        if(source[i]==QLatin1Char('}')) {
            if(!scopes.isEmpty()) scopes.removeLast();
            pendingScope.clear();++i;continue;
        }
        if(source[i]==QLatin1Char('"')) {
            const qsizetype start=++i;
            bool escaped=false;
            while(i<source.size()) {
                if(source[i]==QLatin1Char('\\')) {escaped=true;i+=2;continue;}
                if(source[i]==QLatin1Char('"')) break;
                ++i;
            }
            if(i>=source.size()) {result.complete=false;result.issue=QStringLiteral("Unterminated Objects string.");return result;}
            unknownPart(source.mid(start,i-start),start,escaped
                ? QStringLiteral("Escaped Objects string has no established carrier")
                : QStringLiteral("Unclassified Objects string has no established carrier"));
            ++i;continue;
        }
        if(!isIdChar(source[i])) {++i;continue;}
        const qsizetype start=i;
        while(i<source.size() && isIdChar(source[i])) ++i;
        const QString word=source.mid(start,i-start);
        if(word.compare(QStringLiteral("ObjectUnit"),Qt::CaseInsensitive)==0) {
            pendingScope=QStringLiteral("cunit");continue;
        }
        if(word.compare(QStringLiteral("ObjectDoodad"),Qt::CaseInsensitive)==0) {
            pendingScope=QStringLiteral("cdoodad");continue;
        }
        const bool carrier=word.compare(QStringLiteral("Type"),Qt::CaseInsensitive)==0
            || word.compare(QStringLiteral("Unit"),Qt::CaseInsensitive)==0
            || word.compare(QStringLiteral("Doodad"),Qt::CaseInsensitive)==0;
        if(carrier) {
            qsizetype cursor=i;
            while(cursor<source.size() && source[cursor].isSpace()) ++cursor;
            if(cursor<source.size() && (source[cursor]==QLatin1Char('=') || source[cursor]==QLatin1Char(':'))) {
                ++cursor;
                while(cursor<source.size() && source[cursor].isSpace()) ++cursor;
                const bool quoted=cursor<source.size() && source[cursor]==QLatin1Char('"');
                if(quoted) ++cursor;
                const qsizetype valueStart=cursor;
                while(cursor<source.size() && isIdChar(source[cursor])) ++cursor;
                const QString value=source.mid(valueStart,cursor-valueStart);
                const bool bareEnd=cursor==source.size() || source[cursor].isSpace()
                    || source[cursor]==QLatin1Char(',') || source[cursor]==QLatin1Char(';')
                    || source[cursor]==QLatin1Char('}') || source[cursor]==QLatin1Char(']');
                const bool validEnd=!value.isEmpty() && (quoted
                    ? cursor<source.size() && source[cursor]==QLatin1Char('"') : bareEnd);
                if(targeted(value)) {
                    const QString scope=currentScope();
                    const QString catalog=word.compare(QStringLiteral("Unit"),Qt::CaseInsensitive)==0
                        && scope==QStringLiteral("cunit") ? QStringLiteral("cunit")
                        : word.compare(QStringLiteral("Doodad"),Qt::CaseInsensitive)==0
                            && scope==QStringLiteral("cdoodad") ? QStringLiteral("cdoodad")
                            : word.compare(QStringLiteral("Type"),Qt::CaseInsensitive)==0 ? scope : QString();
                    const bool typed=validEnd && !catalog.isEmpty();
                    result.occurrences.append({valueStart,value.size(),value,typed?catalog:QString(),
                        typed ? QStringLiteral("Objects placement carrier")
                              : QStringLiteral("Objects carrier scope or value syntax is unsupported"),typed});
                }
                if(validEnd) {i=quoted?cursor+1:cursor;continue;}
            }
        }
        if(targeted(word)) result.occurrences.append({start,word.size(),word,{},
            QStringLiteral("Unclassified Objects token has no established carrier"),false});
    }
    return result;
}

inline bool rewrite(const QString &source,const QSet<QString> &targetIds,
                    const std::function<QString(const QString &,const QString &)> &replacement,
                    QString *output,int *replacementCount,QString *issue)
{
    if(output) *output=source;
    if(replacementCount) *replacementCount=0;
    const ScanResult parsed=scan(source,targetIds);
    if(!parsed.complete) {if(issue) *issue=parsed.issue;return false;}
    struct Edit {qsizetype start;qsizetype length;QString value;};
    QVector<Edit> edits;
    for(const Occurrence &occurrence:parsed.occurrences) {
        if(!occurrence.rewritable) {
            if(issue) *issue=occurrence.reason+QStringLiteral(": ")+occurrence.value;
            return false;
        }
        const QString next=replacement(occurrence.catalog,occurrence.value);
        if(!next.isEmpty() && next!=occurrence.value)
            edits.append({occurrence.start,occurrence.length,next});
    }
    QString changed=source;
    for(auto it=edits.crbegin();it!=edits.crend();++it)
        changed.replace(it->start,it->length,it->value);
    if(output) *output=changed;
    if(replacementCount) *replacementCount=edits.size();
    return true;
}
}
