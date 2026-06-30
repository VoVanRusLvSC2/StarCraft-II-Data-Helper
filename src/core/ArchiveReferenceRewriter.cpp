#include "core/ArchiveReferenceRewriter.h"

#include "core/AnalysisModels.h"
#include "core/CatalogProtection.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <algorithm>

namespace
{

const QRegularExpression &tokenExpression()
{
    static const QRegularExpression expression(QStringLiteral("(?<![A-Za-z0-9_@])([A-Za-z0-9_@]+)(?![A-Za-z0-9_@])"));
    return expression;
}

QString rewriteText(QString value, const QHash<QString, QString> &renames, int *replacementCount)
{
    QString output;
    qsizetype last = 0;
    int count = 0;
    auto matches = tokenExpression().globalMatch(value);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const QString token = match.captured(1);
        const auto replacement = renames.constFind(token);
        if (replacement == renames.cend())
            continue;
        output += value.mid(last, match.capturedStart() - last);
        output += replacement.value();
        last = match.capturedEnd();
        ++count;
    }
    if (count <= 0)
        return value;
    output += value.mid(last);
    if (replacementCount)
        *replacementCount += count;
    return output;
}

bool looksLikeUtf8Text(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return false;
    const qsizetype sampleSize = std::min<qsizetype>(bytes.size(), 8192);
    int printable = 0;
    int zeros = 0;
    for (qsizetype i = 0; i < sampleSize; ++i) {
        const uchar value = uchar(bytes.at(i));
        if (value == 0)
            ++zeros;
        if (value == '\r' || value == '\n' || value == '\t' || (value >= 32 && value < 127) || value >= 128)
            ++printable;
    }
    return zeros == 0 && printable >= (sampleSize * 85) / 100;
}

bool looksLikeUtf16LeText(const QByteArray &bytes)
{
    if (bytes.size() < 4 || bytes.size() % 2 != 0)
        return false;
    const qsizetype pairs = std::min<qsizetype>(bytes.size() / 2, 4096);
    int textPairs = 0;
    int zeroHigh = 0;
    for (qsizetype i = 0; i < pairs; ++i) {
        const uchar low = uchar(bytes.at(i * 2));
        const uchar high = uchar(bytes.at(i * 2 + 1));
        if (high == 0)
            ++zeroHigh;
        if (high == 0 && (low == '\r' || low == '\n' || low == '\t' || (low >= 32 && low < 127)))
            ++textPairs;
    }
    return zeroHigh >= (pairs * 70) / 100 && textPairs >= (pairs * 60) / 100;
}

bool containsTokenText(const QString &text, const QHash<QString, QString> &renames)
{
    auto matches = tokenExpression().globalMatch(text);
    while (matches.hasNext()) {
        if (renames.contains(matches.next().captured(1)))
            return true;
    }
    return false;
}

bool containsTokenBytes(const QByteArray &bytes, const QHash<QString, QString> &renames)
{
    if (looksLikeUtf8Text(bytes) && containsTokenText(QString::fromUtf8(bytes), renames))
        return true;
    if (looksLikeUtf16LeText(bytes)) {
        const auto *data = reinterpret_cast<const char16_t *>(bytes.constData());
        if (containsTokenText(QString::fromUtf16(data, bytes.size() / 2), renames))
            return true;
    }

    const auto isIdChar = [](uchar value) {
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')
            || (value >= '0' && value <= '9') || value == '_' || value == '@';
    };
    for (qsizetype start = 0; start < bytes.size();) {
        while (start < bytes.size() && !isIdChar(uchar(bytes[start])))
            ++start;
        qsizetype end = start;
        while (end < bytes.size() && isIdChar(uchar(bytes[end])))
            ++end;
        if (end > start) {
            if (renames.contains(QString::fromLatin1(bytes.constData() + start, end - start)))
                return true;
        }
        start = std::max(end, start + 1);
    }
    for (qsizetype start = 0; start + 1 < bytes.size();) {
        while (start + 1 < bytes.size()
               && (!isIdChar(uchar(bytes[start])) || bytes[start + 1] != '\0'))
            ++start;
        qsizetype end = start;
        QByteArray tokenBytes;
        while (end + 1 < bytes.size()
               && isIdChar(uchar(bytes[end])) && bytes[end + 1] == '\0') {
            tokenBytes.append(bytes[end]);
            end += 2;
        }
        if (!tokenBytes.isEmpty() && renames.contains(QString::fromLatin1(tokenBytes)))
            return true;
        start = std::max(end, start + 1);
    }
    return false;
}

bool writeFile(const QString &path, const QByteArray &bytes, QString *errorMessage)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(bytes) != bytes.size()
        || !file.commit()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Unable to write rewritten archive reference file: %1").arg(path);
        return false;
    }
    return true;
}

} // namespace

namespace sc2dh
{

QHash<QString, QString> unambiguousArchiveReferenceRenames(const AnalysisResult &analysis,
                                                           const QHash<QString, QString> &renames,
                                                           QStringList *skippedIds)
{
    if (skippedIds)
        skippedIds->clear();
    if (renames.isEmpty())
        return {};

    QHash<QString, QSet<QString>> scopesById;
    for (const DataNode &node : analysis.nodes) {
        if (node.id.isEmpty())
            continue;
        const QString scope = catalogIdentityScope(node.elementName);
        if (scope.isEmpty() || scope == QStringLiteral("cdatacollection"))
            continue;
        scopesById[node.id.toCaseFolded()].insert(scope);
    }

    QHash<QString, QString> filtered;
    for (auto it = renames.cbegin(); it != renames.cend(); ++it) {
        const QString oldId = it.key();
        const QSet<QString> scopes = scopesById.value(oldId.toCaseFolded());
        if (scopes.size() > 1) {
            if (skippedIds)
                skippedIds->append(oldId);
            continue;
        }
        filtered.insert(oldId, it.value());
    }
    if (skippedIds) {
        skippedIds->removeDuplicates();
        std::sort(skippedIds->begin(), skippedIds->end(), [](const QString &a, const QString &b) {
            return a.compare(b, Qt::CaseInsensitive) < 0;
        });
    }
    return filtered;
}

bool rewriteArchiveReferenceFiles(const QString &rootFolder,
                                  const QStringList &relativeFiles,
                                  const QHash<QString, QString> &renames,
                                  ArchiveReferenceRewriteReport *report,
                                  QString *errorMessage)
{
    if (report)
        *report = {};
    if (renames.isEmpty())
        return true;

    QSet<QString> seen;
    for (QString relative : relativeFiles) {
        relative = QDir::cleanPath(relative).replace('\\', '/');
        if (relative.isEmpty() || seen.contains(relative))
            continue;
        seen.insert(relative);
        if (relative.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(relative)) {
            if (errorMessage)
                *errorMessage = QStringLiteral("Unsafe archive reference entry path: %1").arg(relative);
            return false;
        }

        const QString path = QDir(rootFolder).absoluteFilePath(relative);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QByteArray original = file.readAll();
        file.close();

        if (!containsTokenBytes(original, renames))
            continue;

        QByteArray rewritten;
        int replacements = 0;
        if (looksLikeUtf8Text(original)) {
            rewritten = rewriteText(QString::fromUtf8(original), renames, &replacements).toUtf8();
        } else if (looksLikeUtf16LeText(original)) {
            const auto *data = reinterpret_cast<const char16_t *>(original.constData());
            const QString text = QString::fromUtf16(data, original.size() / 2);
            const QString changed = rewriteText(text, renames, &replacements);
            rewritten = QByteArray(reinterpret_cast<const char *>(changed.utf16()), changed.size() * 2);
        } else {
            if (report)
                report->blockedFiles << relative;
            if (errorMessage)
                *errorMessage = QStringLiteral("Archive reference entry contains renamed IDs but is not safe text: %1").arg(relative);
            return false;
        }

        if (replacements <= 0 || rewritten == original)
            continue;
        if (!writeFile(path, rewritten, errorMessage))
            return false;
        if (report) {
            report->changedFiles << relative;
            report->replacements += replacements;
        }
    }
    if (report)
        report->changedFiles.removeDuplicates();
    return true;
}

} // namespace sc2dh
