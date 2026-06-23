#include "core/Sc2Archive.h"

#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QThread>

#include <cstdlib>
#include <cstring>

#if defined(SC2DH_USE_LIBZIP) && __has_include(<zip.h>)
#include <zip.h>
#define SC2DH_HAS_LIBZIP 1
#endif

namespace
{

    bool isGameDataXml(const QString &entryName)
    {
        const QString normalized = QDir::cleanPath(entryName).replace('\\', '/');
        return normalized.contains(QStringLiteral("Base.SC2Data/GameData/"), Qt::CaseInsensitive) && normalized.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive);
    }

    QString normalizeZipPath(const QString &path)
    {
        return QDir::cleanPath(path).replace('\\', '/');
    }

    QString mpqHelperScript()
    {
        return QStringLiteral(R"(import json
import os
import sys
from pathlib import Path

from mpyq import MPQArchive


def display_name(name):
    if isinstance(name, bytes):
        name = name.decode("utf-8", "replace")
    return name


def is_game_data_xml(name):
    normalized = display_name(name).replace("\\", "/")
    return normalized.lower().startswith("base.sc2data/gamedata/") and normalized.lower().endswith(".xml")


def list_archive(archive_path):
    archive = MPQArchive(archive_path)
    entries = [display_name(name) for name in (archive.files or [])]
    game_data_xml = [name for name in entries if is_game_data_xml(name)]
    print(json.dumps({
        "ok": True,
        "entries": entries,
        "gameDataXml": game_data_xml
    }, ensure_ascii=False))


def read_entry(archive_path, entry_name):
    archive = MPQArchive(archive_path)
    # mpyq stores MPQ listfile names as bytes and hashes bytes differently
    # from Python strings. Use the exact byte representation for lookup.
    encoded_name = entry_name.encode("utf-8")
    data = archive.read_file(encoded_name)
    # MPQEditor may store an incompressible replacement as raw bytes while
    # retaining the compression flag. mpyq then mistakes the XML prefix for
    # a sector table and returns an empty payload. The equal archived/original
    # sizes prove that the block is raw, so read it directly.
    if not data:
        hash_entry = archive.get_hash_table_entry(encoded_name)
        if hash_entry is not None:
            block = archive.block_table[hash_entry.block_table_index]
            if block.size > 0 and block.archived_size == block.size:
                archive.file.seek(block.offset + archive.header["offset"])
                data = archive.file.read(block.size)
    if data is None:
        raise KeyError(entry_name)
    sys.stdout.buffer.write(data)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(2)
    command = sys.argv[1]
    archive_path = sys.argv[2]
    if command == "list":
        list_archive(archive_path)
        return
    if command == "read":
        if len(sys.argv) < 4:
            raise SystemExit(2)
        read_entry(archive_path, sys.argv[3])
        return
    raise SystemExit(2)


if __name__ == "__main__":
    main()
)");
    }

    bool runPythonMpqHelper(const QStringList &arguments, QByteArray *stdoutData, QString *errorMessage)
    {
        const QStringList programs = {
            QStringLiteral("python.exe"),
            QStringLiteral("python"),
            QStringLiteral("py.exe")};

        for (const QString &program : programs)
        {
            QProcess process;
            process.setProgram(program);
            process.setArguments(arguments);
            process.setProcessChannelMode(QProcess::MergedChannels);
            process.start();
            if (!process.waitForStarted(5000))
            {
                continue;
            }
            if (!process.waitForFinished(120000))
            {
                process.kill();
                process.waitForFinished(5000);
                continue;
            }

            const QByteArray output = process.readAllStandardOutput();
            if (process.exitCode() != 0)
            {
                if (errorMessage)
                {
                    const QString message = QString::fromUtf8(output).trimmed();
                    *errorMessage = message.isEmpty()
                                        ? QStringLiteral("Archive open failed. Check libzip/MPQ support.")
                                        : QStringLiteral("Archive open failed. Check libzip/MPQ support. Details: %1").arg(message);
                }
                return false;
            }

            if (stdoutData)
            {
                *stdoutData = output;
            }
            return true;
        }

        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Archive open failed. Check libzip/MPQ support.");
        }
        return false;
    }

    QString mpqEditorPath()
    {
        const QStringList candidates = {
            QStandardPaths::findExecutable(QStringLiteral("MPQEditor.exe")),
            QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("MPQEditor.exe")),
            QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).absoluteFilePath(QStringLiteral("MPQEditor.exe"))};
        for (const QString &candidate : candidates)
            if (!candidate.isEmpty() && QFileInfo::exists(candidate)) return candidate;
        return {};
    }

    bool runMpqEditor(const QStringList &arguments, QString *errorMessage)
    {
        const QString editor = mpqEditorPath();
        if (editor.isEmpty()) {
            if (errorMessage) *errorMessage = QStringLiteral("MPQEditor.exe was not found in PATH, the application folder, or Documents.");
            return false;
        }
        QProcess process;
        process.setProgram(editor);
        process.setArguments(arguments);
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start();
        if (!process.waitForStarted(5000) || !process.waitForFinished(120000)
            || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            if (errorMessage) {
                const QString details = QString::fromUtf8(process.readAll()).trimmed();
                *errorMessage = QStringLiteral("MPQEditor failed%1").arg(details.isEmpty() ? QString() : QStringLiteral(": ") + details);
            }
            return false;
        }
        return true;
    }

    bool readWithMpqEditor(const QString &archivePath, const QString &entryName, QByteArray *bytes, QString *errorMessage)
    {
        QTemporaryDir directory;
        if (!directory.isValid()) {
            if (errorMessage) *errorMessage = QStringLiteral("Unable to create an archive extraction directory.");
            return false;
        }
        QString editorError;
        const bool commandReportedSuccess = runMpqEditor(
            {QStringLiteral("extract"), archivePath, entryName, directory.path(), QStringLiteral("/fp")}, &editorError);
        const QString relative = normalizeZipPath(entryName);
        const QString extracted = QDir(directory.path()).absoluteFilePath(relative);
        QElapsedTimer wait;
        wait.start();
        while (!QFileInfo::exists(extracted) && wait.elapsed() < 10000) QThread::msleep(50);
        QFile file(extracted);
        if (!file.open(QIODevice::ReadOnly)) {
            if (errorMessage) *errorMessage = commandReportedSuccess
                ? QStringLiteral("MPQEditor did not extract archive entry: %1").arg(entryName)
                : editorError;
            return false;
        }
        *bytes = file.readAll();
        return true;
    }

}

bool Sc2Archive::load(const QString &archivePath, QString *errorMessage)
{
    m_archivePath = archivePath;
    m_allEntries.clear();
    m_gameDataEntries.clear();

#ifdef SC2DH_HAS_LIBZIP
    int err = 0;
    zip_t *archive = zip_open(QFile::encodeName(archivePath).constData(), ZIP_RDONLY, &err);
    if (!archive)
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Unable to open archive with libzip.");
        }
        return false;
    }

    const zip_int64_t count = zip_get_num_entries(archive, 0);
    for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(count); ++i)
    {
        const char *name = zip_get_name(archive, i, 0);
        if (!name)
        {
            continue;
        }
        const QString entryName = QString::fromUtf8(name);
        m_allEntries.append(entryName);
        if (isGameDataXml(entryName))
        {
            m_gameDataEntries.append(entryName);
        }
    }

    zip_close(archive);
    return true;
#else
    QByteArray output;
    const QStringList arguments = {
        QStringLiteral("-c"),
        mpqHelperScript(),
        QStringLiteral("list"),
        archivePath};
    if (!runPythonMpqHelper(arguments, &output, errorMessage))
    {
        return false;
    }

    const QJsonDocument json = QJsonDocument::fromJson(output);
    if (!json.isObject())
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Archive open failed. Check libzip/MPQ support.");
        }
        return false;
    }

    const QJsonObject object = json.object();
    const QJsonArray entries = object.value(QStringLiteral("entries")).toArray();
    const QJsonArray gameDataEntries = object.value(QStringLiteral("gameDataXml")).toArray();
    m_allEntries.reserve(entries.size());
    for (const QJsonValue &value : entries)
    {
        m_allEntries.append(value.toString());
    }
    m_gameDataEntries.reserve(gameDataEntries.size());
    for (const QJsonValue &value : gameDataEntries)
    {
        m_gameDataEntries.append(value.toString());
    }

    if (m_allEntries.isEmpty())
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Archive open failed. Check libzip/MPQ support.");
        }
        return false;
    }
    return true;
#endif
}

bool Sc2Archive::readEntry(const QString &entryName, QByteArray *bytes, QString *errorMessage) const
{
#ifdef SC2DH_HAS_LIBZIP
    int err = 0;
    zip_t *archive = zip_open(QFile::encodeName(m_archivePath).constData(), ZIP_RDONLY, &err);
    if (!archive)
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Unable to reopen archive for reading.");
        }
        return false;
    }

    const QByteArray encoded = normalizeZipPath(entryName).toUtf8();
    zip_stat_t stat;
    if (zip_stat(archive, encoded.constData(), 0, &stat) != 0)
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Archive entry not found: %1").arg(entryName);
        }
        zip_close(archive);
        return false;
    }

    zip_file_t *file = zip_fopen(archive, encoded.constData(), 0);
    if (!file)
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Unable to open archive entry: %1").arg(entryName);
        }
        zip_close(archive);
        return false;
    }

    QByteArray buffer;
    buffer.resize(static_cast<int>(stat.size));
    const zip_int64_t readBytes = zip_fread(file, buffer.data(), stat.size);
    zip_fclose(file);
    zip_close(archive);

    if (readBytes < 0 || static_cast<zip_uint64_t>(readBytes) != stat.size)
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Failed to read archive entry: %1").arg(entryName);
        }
        return false;
    }

    *bytes = buffer;
    return true;
#else
    if (!bytes)
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Internal error: bytes is null.");
        }
        return false;
    }

    QByteArray output;
    const QStringList arguments = {
        QStringLiteral("-c"),
        mpqHelperScript(),
        QStringLiteral("read"),
        m_archivePath,
        entryName};
    if (!runPythonMpqHelper(arguments, &output, errorMessage))
    {
        return false;
    }
    if (!output.isEmpty()) {
        *bytes = output;
        return true;
    }
    return readWithMpqEditor(m_archivePath, entryName, bytes, errorMessage);
#endif
}

bool Sc2Archive::saveCopy(const QString &targetPath,
                          const QHash<QString, QByteArray> &replacementEntries,
                          const QStringList &removedEntries,
                          QString *errorMessage) const
{
#ifdef SC2DH_HAS_LIBZIP
    if (m_archivePath.isEmpty())
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("No archive loaded.");
        }
        return false;
    }

    const QString tempPath = targetPath + QStringLiteral(".sc2dh.tmp");
    QFile::remove(tempPath);

    int err = 0;
    zip_t *sourceArchive = zip_open(QFile::encodeName(m_archivePath).constData(), ZIP_RDONLY, &err);
    if (!sourceArchive)
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Unable to open source archive for saving.");
        }
        return false;
    }

    zip_t *targetArchive = zip_open(QFile::encodeName(tempPath).constData(), ZIP_CREATE | ZIP_TRUNCATE, &err);
    if (!targetArchive)
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Unable to create temporary archive.");
        }
        zip_close(sourceArchive);
        return false;
    }

    bool hadError = false;
    const zip_int64_t count = zip_get_num_entries(sourceArchive, 0);
    for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(count); ++i)
    {
        const char *name = zip_get_name(sourceArchive, i, 0);
        if (!name)
        {
            hadError = true;
            continue;
        }

        const QString entryName = QString::fromUtf8(name);
        if (removedEntries.contains(entryName, Qt::CaseInsensitive))
        {
            continue;
        }

        QByteArray entryBytes;
        if (replacementEntries.contains(entryName))
        {
            entryBytes = replacementEntries.value(entryName);
        }
        else
        {
            zip_stat_t stat;
            if (zip_stat_index(sourceArchive, i, 0, &stat) != 0)
            {
                hadError = true;
                continue;
            }

            zip_file_t *file = zip_fopen_index(sourceArchive, i, 0);
            if (!file)
            {
                hadError = true;
                continue;
            }

            entryBytes.resize(static_cast<int>(stat.size));
            const zip_int64_t readBytes = zip_fread(file, entryBytes.data(), stat.size);
            zip_fclose(file);
            if (readBytes < 0 || static_cast<zip_uint64_t>(readBytes) != stat.size)
            {
                hadError = true;
                continue;
            }
        }

        auto *buffer = static_cast<uchar *>(malloc(static_cast<size_t>(entryBytes.size())));
        if (entryBytes.size() > 0 && !buffer)
        {
            zip_close(targetArchive);
            zip_close(sourceArchive);
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Out of memory while writing archive.");
            }
            return false;
        }

        if (entryBytes.size() > 0)
        {
            memcpy(buffer, entryBytes.constData(), static_cast<size_t>(entryBytes.size()));
        }

        zip_source_t *source = zip_source_buffer(targetArchive, buffer, static_cast<zip_uint64_t>(entryBytes.size()), 1);
        if (!source)
        {
            free(buffer);
            hadError = true;
            continue;
        }

        const QString normalizedName = normalizeZipPath(entryName);
        if (zip_file_add(targetArchive, normalizedName.toUtf8().constData(), source, ZIP_FL_OVERWRITE) < 0)
        {
            zip_source_free(source);
            hadError = true;
            continue;
        }
    }

    if (hadError)
    {
        zip_discard(targetArchive);
        zip_close(sourceArchive);
        QFile::remove(tempPath);
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Archive save failed while writing one or more entries.");
        }
        return false;
    }

    if (zip_close(targetArchive) != 0)
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Failed to close temporary archive.");
        }
        zip_close(sourceArchive);
        QFile::remove(tempPath);
        return false;
    }
    zip_close(sourceArchive);

    QFile::remove(targetPath);
    if (!QFile::rename(tempPath, targetPath))
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Failed to move temporary archive into place.");
        }
        QFile::remove(tempPath);
        return false;
    }
    return true;
#else
    if (m_archivePath.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("No archive loaded.");
        return false;
    }
    if (mpqEditorPath().isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("Archive rewrite requires MPQEditor.exe.");
        return false;
    }
    // MPQEditor selects its archive handler partly by extension.
    const QString tempPath = targetPath + QStringLiteral(".sc2dh.SC2Map");
    QFile::remove(tempPath);
    QFile::remove(targetPath);
    if (!QFile::copy(m_archivePath, tempPath)) {
        if (errorMessage) *errorMessage = QStringLiteral("Unable to create a working archive copy.");
        return false;
    }
    QTemporaryDir sources;
    if (!sources.isValid()) {
        QFile::remove(tempPath);
        if (errorMessage) *errorMessage = QStringLiteral("Unable to create temporary replacement files.");
        return false;
    }
    int fileIndex = 0;
    for (auto it = replacementEntries.cbegin(); it != replacementEntries.cend(); ++it) {
        // StormLib/MPQEditor owns the special (listfile) entry and updates it
        // when files are added. Writing it as a normal file is rejected.
        if (it.key().compare(QStringLiteral("(listfile)"), Qt::CaseInsensitive) == 0) continue;
        const QString sourcePath = QDir(sources.path()).absoluteFilePath(QStringLiteral("replacement_%1.xml").arg(fileIndex++));
        QSaveFile source(sourcePath);
        if (!source.open(QIODevice::WriteOnly) || source.write(it.value()) != it.value().size() || !source.commit()) {
            QFile::remove(tempPath);
            return false;
        }
        QString editorStatus;
        runMpqEditor({QStringLiteral("add"), tempPath, sourcePath, it.key(), QStringLiteral("/c")}, &editorStatus);
        // MPQEditor can briefly retain the archive handle. The retrying
        // verification below is authoritative, so only yield momentarily.
        QThread::msleep(75);
    }
    for (const QString &entry : removedEntries) {
        QString editorStatus;
        runMpqEditor({QStringLiteral("delete"), tempPath, entry}, &editorStatus);
        QThread::msleep(75);
    }
    Sc2Archive verification;
    QString verifyError;
    if (!verification.load(tempPath, &verifyError)) {
        QFile::remove(tempPath);
        if (errorMessage) *errorMessage = QStringLiteral("Rewritten archive verification failed: %1").arg(verifyError);
        return false;
    }
    for (auto it = replacementEntries.cbegin(); it != replacementEntries.cend(); ++it) {
        QByteArray actual;
        bool matched = false;
        QElapsedTimer wait;
        wait.start();
        do {
            verifyError.clear();
            bool contentMatches = false;
            if (verification.readEntry(it.key(), &actual, &verifyError)) {
                if (it.key().compare(QStringLiteral("(listfile)"), Qt::CaseInsensitive) == 0) {
                    const QString actualText = QString::fromUtf8(actual).replace('/', '\\');
                    contentMatches = true;
                    for (const QString &line : QString::fromUtf8(it.value()).split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts)) {
                        if (!actualText.contains(line.trimmed().replace('/', '\\'), Qt::CaseInsensitive)) {
                            contentMatches = false;
                            break;
                        }
                    }
                } else {
                    contentMatches = actual == it.value();
                }
            }
            if (contentMatches) {
                matched = true;
                break;
            }
            QThread::msleep(100);
        } while (wait.elapsed() < 10000);
        if (!matched) {
            QFile::remove(tempPath);
            if (errorMessage) *errorMessage = QStringLiteral("Rewritten entry verification failed: %1 (%2)").arg(it.key(), verifyError);
            return false;
        }
    }
    if (!QFile::rename(tempPath, targetPath)) {
        QFile::remove(tempPath);
        if (errorMessage) *errorMessage = QStringLiteral("Unable to finalize the rewritten archive copy.");
        return false;
    }
    return true;
#endif
}
