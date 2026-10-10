#pragma once
#include "core/GalaxyReferenceSpans.h"
#include <QRegularExpression>

namespace sc2dh::galaxy {
inline bool declarationIdentifier(const Token &token)
{
    static const QRegularExpression identifier(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    return !token.literal && identifier.match(token.raw).hasMatch();
}

// Return the semicolon token of a local declaration, or -1 for a statement.
inline int localDeclarationEnd(const QVector<Token> &tokens, int start)
{
    static const QSet<QString> statements{QStringLiteral("if"), QStringLiteral("else"), QStringLiteral("while"),
        QStringLiteral("for"), QStringLiteral("switch"), QStringLiteral("return"), QStringLiteral("break"),
        QStringLiteral("continue"), QStringLiteral("do")};
    int p = start;
    if (p < tokens.size() && tokens[p].raw == QStringLiteral("const")) ++p;
    if (p >= tokens.size() || !declarationIdentifier(tokens[p]) || statements.contains(tokens[p].raw)) return -1;
    ++p;
    while (p < tokens.size() && tokens[p].raw == QStringLiteral("[")) {
        int depth = 1; ++p;
        while (p < tokens.size() && depth) {
            if (!tokens[p].literal && tokens[p].raw == QStringLiteral("[")) ++depth;
            if (!tokens[p].literal && tokens[p].raw == QStringLiteral("]")) --depth;
            ++p;
        }
        if (depth) return -1;
    }
    if (p >= tokens.size() || !declarationIdentifier(tokens[p])) return -1;
    ++p;
    if (p >= tokens.size() || (tokens[p].raw != QStringLiteral(";") && tokens[p].raw != QStringLiteral("="))) return -1;
    int depth = 0;
    for (; p < tokens.size(); ++p) {
        if (tokens[p].literal) continue;
        const auto &t = tokens[p].raw;
        if (t == QStringLiteral("(") || t == QStringLiteral("[") || t == QStringLiteral("{")) ++depth;
        if (t == QStringLiteral(")") || t == QStringLiteral("]") || t == QStringLiteral("}")) --depth;
        if (depth < 0) return -1;
        if (depth == 0 && t == QStringLiteral(";")) return p;
    }
    return -1;
}

inline qsizetype afterFunctionDeclarations(const QString &source, const QString &function)
{
    bool complete = true;
    const auto tokens = lex(source, &complete);
    if (!complete) return -1;
    for (int i = 1; i + 1 < tokens.size(); ++i) {
        if (tokens[i].literal || tokens[i].raw != function || tokens[i-1].raw != QStringLiteral("void")
            || tokens[i+1].raw != QStringLiteral("(")) continue;
        int p = i+2, depth = 1;
        while (p < tokens.size() && depth) {
            if (tokens[p].raw == QStringLiteral("(")) ++depth;
            if (tokens[p].raw == QStringLiteral(")")) --depth;
            ++p;
        }
        if (p >= tokens.size() || tokens[p].raw != QStringLiteral("{")) continue;
        qsizetype offset = tokens[p].start + tokens[p].length;
        ++p;
        for (int end = localDeclarationEnd(tokens, p); end >= 0; end = localDeclarationEnd(tokens, p)) {
            offset = tokens[end].start + tokens[end].length; p = end+1;
        }
        return offset;
    }
    return -1;
}

inline QStringList declarationOrderErrors(const QString &source)
{
    QStringList errors;
    bool complete = true;
    const auto tokens = lex(source, &complete);
    if (!complete) return {QStringLiteral("Incomplete Galaxy token stream.")};
    struct Scope { bool statement = false; bool start = true; };
    QVector<Scope> scopes;
    int parentheses = 0;
    for (int i = 0; i < tokens.size(); ++i) {
        if (tokens[i].literal) { if (!scopes.isEmpty()) scopes.last().start = false; continue; }
        const auto &t = tokens[i].raw;
        if (t == QStringLiteral("{")) { scopes << Scope{}; continue; }
        if (t == QStringLiteral("}")) { if (!scopes.isEmpty()) scopes.removeLast(); if (!scopes.isEmpty()) scopes.last().start = true; continue; }
        if (t == QStringLiteral("(")) ++parentheses;
        if (t == QStringLiteral(")")) --parentheses;
        if (scopes.isEmpty()) continue;
        auto &scope = scopes.last();
        if (scope.start && parentheses == 0) {
            const int end = localDeclarationEnd(tokens, i);
            if (end >= 0) {
                if (scope.statement) errors << QStringLiteral("Local declaration after executable code near offset %1.").arg(tokens[i].start);
                i = end; continue;
            }
            scope.statement = true; scope.start = false;
        }
        if (t == QStringLiteral(";") && parentheses == 0) scope.start = true;
    }
    return errors;
}
} // namespace sc2dh::galaxy
