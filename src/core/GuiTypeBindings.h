#pragma once

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <memory>
#include <mutex>

namespace sc2dh::gui {

inline QByteArray nativeBindingBytes()
{
    const QStringList paths = {
        QCoreApplication::applicationDirPath() + QStringLiteral("/resources/gui_type_bindings.json"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../resources/gui_type_bindings.json"),
        QDir::current().absoluteFilePath(QStringLiteral("resources/gui_type_bindings.json")),
        QStringLiteral(":/gui_type_bindings.json")
    };
    for (const auto &path : paths) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) return file.readAll();
    }
    return {};
}

struct NativeParameterType { QString kind, gameType; };
struct NativeBindings {
    bool valid = false;
    QString provenance;
    QHash<QString, NativeParameterType> parameters;
    QHash<QString, QSet<QString>> functions;
};

inline std::shared_ptr<const NativeBindings> nativeBindings()
{
    static std::mutex mutex;
    static QByteArray previousHash;
    static std::shared_ptr<const NativeBindings> previous;
    const auto bytes = nativeBindingBytes();
    const auto hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    std::lock_guard<std::mutex> lock(mutex);
    if (previous && hash == previousHash) return previous;
    auto result = std::make_shared<NativeBindings>();
    // Immutable evidence from the supplied NativeLib; editing the JSON does not establish new types.
    const QByteArray expectedHash = "c58ea0734694b55153dba68f96d80801fc9b717ad758e23fb2a96bd7d571fdb5";
    if (hash == expectedHash) {
        const auto root = QJsonDocument::fromJson(bytes).object();
        if (root.value(QStringLiteral("formatVersion")).toInt() == 1
            && root.value(QStringLiteral("build")).toInt() == 97563
            && root.value(QStringLiteral("sourceSha256")).toString()
                == QStringLiteral("cbb2302825dce57ff9bb8efd44f9b0ed5cb056eee00265e5b711b0824d872396")) {
            const auto parameters = root.value(QStringLiteral("parameters")).toObject();
            for (auto it = parameters.begin(); it != parameters.end(); ++it) {
                const auto value = it.value().toObject();
                result->parameters.insert(it.key(), {value.value(QStringLiteral("kind")).toString(),
                                                     value.value(QStringLiteral("gameType")).toString()});
            }
            const auto functions = root.value(QStringLiteral("functions")).toObject();
            for (auto it = functions.begin(); it != functions.end(); ++it) {
                QSet<QString> ids;
                for (const auto &value : it.value().toArray()) ids.insert(value.toString());
                result->functions.insert(it.key(), ids);
            }
            result->valid = result->parameters.size() == 6554 && result->functions.size() == 3196;
            result->provenance = QStringLiteral("NativeLib build 97563, source SHA256 cbb2302825dce57ff9bb8efd44f9b0ed5cb056eee00265e5b711b0824d872396");
        }
    }
    previousHash = hash; previous = result; return result;
}

} // namespace sc2dh::gui
