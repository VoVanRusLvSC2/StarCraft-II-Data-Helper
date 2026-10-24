#include "core/ModkitMcpClient.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

QString processError(QProcess &process, const QString &fallback)
{
    const QString stderrText = QString::fromUtf8(process.readAllStandardError()).trimmed();
    return stderrText.isEmpty() ? fallback : fallback + QStringLiteral(": ") + stderrText.left(600);
}

bool request(QProcess &process, QByteArray *pending, int id, const QString &method,
             const QJsonObject &params, QJsonObject *result, QString *error,
             int timeoutMs)
{
    const QJsonObject message{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                              {QStringLiteral("id"), id},
                              {QStringLiteral("method"), method},
                              {QStringLiteral("params"), params}};
    if (process.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n') < 0
        || !process.waitForBytesWritten(5000)) {
        *error = processError(process, QStringLiteral("Cannot send MCP request"));
        return false;
    }
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (pending->size() > 512 * 1024) {
            *error = QStringLiteral("MCP response exceeded the inspection limit");
            return false;
        }
        const qsizetype end = pending->indexOf('\n');
        if (end >= 0) {
            const QByteArray line = pending->left(end).trimmed();
            pending->remove(0, end + 1);
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                *error = QStringLiteral("Invalid MCP JSON response: %1").arg(parseError.errorString());
                return false;
            }
            const QJsonObject response = document.object();
            if (response.value(QStringLiteral("id")).toInt(-1) != id)
                continue; // Notifications and unrelated responses are not this request.
            if (response.contains(QStringLiteral("error"))) {
                *error = QStringLiteral("MCP request failed: %1")
                             .arg(QString::fromUtf8(QJsonDocument(response.value(QStringLiteral("error")).toObject())
                                                        .toJson(QJsonDocument::Compact)).left(800));
                return false;
            }
            *result = response.value(QStringLiteral("result")).toObject();
            return true;
        }
        if (!process.waitForReadyRead(qMin(1000, timeoutMs - int(timer.elapsed())))) {
            if (process.state() == QProcess::NotRunning) {
                *error = processError(process, QStringLiteral("MCP server exited before responding"));
                return false;
            }
        }
        pending->append(process.readAllStandardOutput());
    }
    *error = QStringLiteral("MCP request timed out");
    return false;
}

} // namespace

QString ModkitMcpClient::namespaceForType(const QString &ctype)
{
    if (!ctype.startsWith(QLatin1Char('C')))
        return {};
    const QString body = ctype.mid(1);
    // Domain names mirror SC2Editor-MCP-Codex/src/modules/data/domains.ts.
    // Choose the longest matching prefix (CRequirementNode before CRequirement).
    static const QStringList domains = QStringLiteral(
        "Abil|Accumulator|Achievement|AchievementTerm|Actor|ActorSupport|Alert|"
        "ArmyCategory|ArmyUnit|ArmyUpgrade|Artifact|ArtifactSlot|AttachMethod|"
        "BankCondition|Beam|Behavior|Boost|Bundle|Button|Camera|Campaign|"
        "Character|Cliff|CliffMesh|ColorStyle|Commander|Config|ConsoleSkin|"
        "Conversation|ConversationState|Cursor|DSP|DataCollection|"
        "DataCollectionPattern|DecalPack|Effect|Emoticon|EmoticonPack|Error|"
        "Footprint|FoW|Game|GameUI|Herd|HerdNode|Hero|HeroAbil|HeroStat|Item|"
        "ItemClass|ItemContainer|Kinetic|LensFlareSet|Light|Location|Loot|Map|"
        "Model|Mount|Mover|Objective|PhysicsMaterial|Ping|PlayerResponse|"
        "PortraitPack|Preload|PremiumMap|Race|RaceBannerPack|Requirement|"
        "RequirementNode|Reverb|Reward|ScoreResult|ScoreValue|Shape|Skin|"
        "SkinPack|Sound|SoundExclusivity|SoundMixSnapshot|Soundtrack|Spray|"
        "SprayPack|StimPack|TacCooldown|Tactical|Talent|TalentProfile|"
        "TargetFind|TargetSort|Terrain|TerrainObject|TerrainTex|Texture|"
        "TextureSheet|Tile|Trophy|Turret|Unit|Upgrade|User|Validator|"
        "VoiceOver|VoicePack|WarChest|WarChestSeason|Water|Weapon").split('|');
    QString match;
    for (const QString &domain : domains)
        if (body.startsWith(domain) && domain.size() > match.size())
            match = domain;
    return match;
}

ModkitEntityInspection ModkitMcpClient::inspect(const QString &sourcePath,
                                                 const QString &ctype,
                                                 const QString &id,
                                                 const QString &mcpEntry,
                                                 const QStringList &fields)
{
    ModkitEntityInspection inspection;
    const QFileInfo source(sourcePath), entry(mcpEntry);
    const QString nameSpace = namespaceForType(ctype);
    if (id.isEmpty() || nameSpace.isEmpty()) {
        inspection.error = QStringLiteral("This catalog object has no supported ModKit identity");
        return inspection;
    }
    if (fields.size() > 32) {
        inspection.error = QStringLiteral("Select at most 32 field names");
        return inspection;
    }
    for (const QString &field : fields)
        if (field.isEmpty() || field.size() > 128) {
            inspection.error = QStringLiteral("Invalid ModKit field name");
            return inspection;
        }
    if (!source.exists() || !entry.isFile()) {
        inspection.error = QStringLiteral("Source or MCP entry does not exist");
        return inspection;
    }
    const QString suffix = source.suffix();
    if (suffix.compare(QStringLiteral("SC2Map"), Qt::CaseInsensitive) != 0
        && suffix.compare(QStringLiteral("SC2Mod"), Qt::CaseInsensitive) != 0) {
        inspection.error = QStringLiteral("ModKit inspection requires one .SC2Map or .SC2Mod package");
        return inspection;
    }

    QProcess process;
    process.setWorkingDirectory(entry.absolutePath());
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("SC2_UI_ROOT"), source.absolutePath());
    environment.insert(QStringLiteral("SC2_MCP_PROFILE"), QStringLiteral("data"));
    process.setProcessEnvironment(environment);
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
#endif
    const QString node = qEnvironmentVariable("SC2_MODKIT_NODE", "node");
    process.start(node, {entry.absoluteFilePath()});
    if (!process.waitForStarted(15000)) {
        inspection.error = processError(process, QStringLiteral("Cannot start the MCP server"));
        return inspection;
    }

    QByteArray pending;
    QJsonObject response;
    const QJsonObject init{{QStringLiteral("protocolVersion"), QStringLiteral("2025-11-25")},
                           {QStringLiteral("capabilities"), QJsonObject{}},
                           {QStringLiteral("clientInfo"), QJsonObject{{QStringLiteral("name"), QStringLiteral("sc2-data-helper")},
                                                                         {QStringLiteral("version"), QStringLiteral("1")}}}};
    bool ok = request(process, &pending, 1, QStringLiteral("initialize"), init,
                      &response, &inspection.error, 45000);
    if (ok) {
        const QJsonObject initialized{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                      {QStringLiteral("method"), QStringLiteral("notifications/initialized")}};
        process.write(QJsonDocument(initialized).toJson(QJsonDocument::Compact) + '\n');
        ok = process.waitForBytesWritten(5000);
        if (!ok) inspection.error = QStringLiteral("Cannot initialize MCP session");
    }
    if (ok) {
        QJsonObject arguments{{QStringLiteral("directory"), source.fileName()},
                              {QStringLiteral("view"), QStringLiteral("entities")},
                              {QStringLiteral("refs"), QJsonArray{QJsonObject{{QStringLiteral("namespace"), nameSpace},
                                                                                {QStringLiteral("entityId"), id}}}}};
        if (!fields.isEmpty()) arguments.insert(QStringLiteral("fields"), QJsonArray::fromStringList(fields));
        const QJsonObject call{{QStringLiteral("name"), QStringLiteral("data.modkit.query")},
                               {QStringLiteral("arguments"), arguments}};
        ok = request(process, &pending, 2, QStringLiteral("tools/call"), call,
                     &response, &inspection.error, 90000);
    }
    if (ok) {
        const QJsonArray content = response.value(QStringLiteral("content")).toArray();
        const QByteArray text = content.isEmpty() ? QByteArray{} :
            content.first().toObject().value(QStringLiteral("text")).toString().toUtf8();
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(text, &parseError);
        const QJsonObject value = document.object();
        if (response.value(QStringLiteral("isError")).toBool()) {
            inspection.error = value.value(QStringLiteral("error")).toString(
                QStringLiteral("ModKit rejected the Data query"));
        } else {
            if (parseError.error != QJsonParseError::NoError || value.value(QStringLiteral("backend")).toString() != QStringLiteral("ModKit")) {
                inspection.error = QStringLiteral("Unexpected ModKit Data response");
            } else {
                inspection.success = true;
                inspection.inputRevision = value.value(QStringLiteral("inputRevision")).toString();
                inspection.answer = value.value(QStringLiteral("answer")).toObject();
            }
        }
    }
    process.closeWriteChannel();
    if (!process.waitForFinished(3000)) {
        process.kill(); // Only the private MCP child launched above.
        process.waitForFinished(3000);
    }
    return inspection;
}
