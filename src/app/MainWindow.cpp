#include "app/MainWindow.h"
#include "app/AudioManager.h"
#include "app/ThemeManager.h"

#include "ui/DataCollectionPage.h"
#include "ui/AnalysisProgressDialog.h"
#include "ui/DuplicatesPage.h"
#include "ui/DependenciesPage.h"
#include "ui/FormatterPage.h"
#include "ui/GraphPage.h"
#include "ui/LogPanel.h"
#include "ui/OverviewPage.h"
#include "ui/PropertiesPage.h"
#include "ui/RenameIdsPage.h"
#include "ui/UnusedPage.h"
#include "ui/XmlSourcePage.h"
#include "core/Sc2Archive.h"
#include "core/DataCollectionPreservation.h"
#include "core/DeepCleanupService.h"
#include "core/StandardNamePlanner.h"
#include "core/UnitFamilyDetector.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QGraphicsOpacityEffect>
#include <QGraphicsDropShadowEffect>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QIODevice>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSaveFile>
#include <QStandardPaths>
#include <QAbstractAnimation>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QIcon>
#include <QKeySequence>
#include <QStatusBar>
#include <QStyle>
#include <QSlider>
#include <QSplitter>
#include <QTabWidget>
#include <QTabBar>
#include <QToolTip>
#include <QHelpEvent>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUrl>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QSizePolicy>
#include <QTemporaryDir>
#include <QToolButton>
#include <QToolBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QKeyEvent>

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmsystem.h>
#endif

#include <spdlog/spdlog.h>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <optional>
#include <algorithm>
#include <vector>

namespace
{
    constexpr const char *kDiscordInviteUrl = "https://discord.com/invite/UKYgsB6Zrx";
    constexpr const char *kBoostyUrl = "https://boosty.to/vovanruslvsc2/donate";

    struct Sc2FileOpenFilter
    {
        QString label;
        QStringList suffixes;
    };

    QString humanFileSize(qint64 bytes)
    {
        static const QStringList units{QStringLiteral("B"), QStringLiteral("KB"), QStringLiteral("MB"), QStringLiteral("GB")};
        double value = double(bytes);
        int unit = 0;
        while (value >= 1024.0 && unit + 1 < units.size())
        {
            value /= 1024.0;
            ++unit;
        }
        return unit == 0
                   ? QStringLiteral("%1 %2").arg(bytes).arg(units[unit])
                   : QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(units[unit]);
    }

    bool matchesOpenFilter(const QFileInfo &info, const Sc2FileOpenFilter &filter)
    {
        if (info.isDir() || filter.suffixes.isEmpty())
            return true;
        const QString suffix = QStringLiteral(".%1").arg(info.suffix()).toLower();
        return filter.suffixes.contains(suffix);
    }

    QString fileTypeLabel(const QFileInfo &info)
    {
        if (info.isDir())
            return QStringLiteral("Folder");
        const QString suffix = info.suffix().trimmed();
        return suffix.isEmpty() ? QStringLiteral("File")
                                : QStringLiteral("%1 file").arg(suffix.toUpper());
    }

    class Sc2FileOpenDialog final : public QDialog
    {
    public:
        explicit Sc2FileOpenDialog(QWidget *parent, const QString &startPath)
            : QDialog(parent)
        {
            setObjectName(QStringLiteral("toolDialog"));
            setWindowTitle(QStringLiteral("Open SC2 File"));
            setModal(true);
            resize(1220, 780);
            setMinimumSize(900, 560);

            m_filters = {
                {QStringLiteral("Supported (*.SC2Map, *.SC2Mod, *.SC2Components, *.SC2Campaign, *.SC2Archive, *.xml)"),
                 {QStringLiteral(".sc2map"), QStringLiteral(".sc2mod"), QStringLiteral(".sc2components"),
                  QStringLiteral(".sc2campaign"), QStringLiteral(".sc2archive"), QStringLiteral(".xml")}},
                {QStringLiteral("SC2 Archives (*.SC2Map, *.SC2Mod, *.SC2Components, *.SC2Campaign, *.SC2Archive)"),
                 {QStringLiteral(".sc2map"), QStringLiteral(".sc2mod"), QStringLiteral(".sc2components"),
                  QStringLiteral(".sc2campaign"), QStringLiteral(".sc2archive")}},
                {QStringLiteral("XML (*.xml)"), {QStringLiteral(".xml")}},
                {QStringLiteral("All Files (*.*)"), {}},
            };

            auto *layout = new QVBoxLayout(this);
            layout->setContentsMargins(18, 16, 18, 16);
            layout->setSpacing(10);

            auto *title = new QLabel(QStringLiteral("OPEN SC2 FILE"), this);
            title->setObjectName(QStringLiteral("panelTitle"));
            layout->addWidget(title);

            m_status = new QLabel(this);
            m_status->setObjectName(QStringLiteral("inspectorSubtitle"));
            m_status->setWordWrap(false);
            layout->addWidget(m_status);

            m_path = new QLineEdit(this);
            m_path->setPlaceholderText(QStringLiteral("Current folder..."));
            layout->addWidget(m_path);

            auto *searchRow = new QHBoxLayout;
            m_search = new QLineEdit(this);
            m_search->setPlaceholderText(QStringLiteral("Search files and folders..."));
            m_filter = new QComboBox(this);
            for (const Sc2FileOpenFilter &filter : m_filters)
                m_filter->addItem(filter.label);
            searchRow->addWidget(m_search, 1);
            searchRow->addWidget(m_filter);
            layout->addLayout(searchRow);

            auto *splitter = new QSplitter(Qt::Horizontal, this);
            m_places = new QListWidget(splitter);
            m_places->setObjectName(QStringLiteral("fileOpenPlaces"));
            m_places->setSelectionMode(QAbstractItemView::SingleSelection);
            m_table = new QTableWidget(splitter);
            m_table->setObjectName(QStringLiteral("fileOpenTable"));
            m_table->setColumnCount(4);
            m_table->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Type"),
                                                QStringLiteral("Size"), QStringLiteral("Modified")});
            m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
            m_table->setSelectionMode(QAbstractItemView::SingleSelection);
            m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
            m_table->setShowGrid(false);
            m_table->verticalHeader()->hide();
            m_table->horizontalHeader()->setStretchLastSection(true);
            m_table->setColumnWidth(0, 470);
            m_table->setColumnWidth(1, 130);
            m_table->setColumnWidth(2, 110);
            splitter->addWidget(m_places);
            splitter->addWidget(m_table);
            splitter->setStretchFactor(0, 1);
            splitter->setStretchFactor(1, 4);
            splitter->setSizes({260, 920});
            layout->addWidget(splitter, 1);

            auto *nameRow = new QHBoxLayout;
            auto *nameLabel = new QLabel(QStringLiteral("Selected:"), this);
            nameLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
            m_name = new QLineEdit(this);
            m_name->setPlaceholderText(QStringLiteral("Selected file or folder..."));
            nameRow->addWidget(nameLabel);
            nameRow->addWidget(m_name, 1);
            layout->addLayout(nameRow);

            m_selection = new QLabel(this);
            m_selection->setObjectName(QStringLiteral("inspectorSubtitle"));
            m_selection->setTextInteractionFlags(Qt::TextSelectableByMouse);
            layout->addWidget(m_selection);

            auto *buttonRow = new QHBoxLayout;
            buttonRow->addStretch(1);
            m_open = new QPushButton(QStringLiteral("Open"), this);
            m_cancel = new QPushButton(QStringLiteral("Cancel"), this);
            m_open->setMinimumWidth(150);
            m_cancel->setMinimumWidth(150);
            buttonRow->addWidget(m_open);
            buttonRow->addWidget(m_cancel);
            layout->addLayout(buttonRow);

            populatePlaces();
            wireEvents();

            QFileInfo startInfo(startPath);
            QString initialDir;
            QString initialFile;
            if (startInfo.isFile())
            {
                initialDir = startInfo.absolutePath();
                initialFile = startInfo.fileName();
            }
            else if (startInfo.isDir())
            {
                initialDir = startInfo.absoluteFilePath();
            }
            else
            {
                initialDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
            }
            if (initialDir.isEmpty())
                initialDir = QDir::homePath();
            openDirectory(initialDir, initialFile);
        }

        QString selectedFile() const { return m_selectedFile; }

    protected:
        void keyPressEvent(QKeyEvent *event) override
        {
            if (!event)
            {
                QDialog::keyPressEvent(event);
                return;
            }
            if (event->key() == Qt::Key_Escape)
            {
                reject();
                return;
            }
            if (event->key() == Qt::Key_F5)
            {
                openDirectory(m_currentDir, m_name->text());
                return;
            }
            if ((event->key() == Qt::Key_Backspace) || (event->key() == Qt::Key_Up && event->modifiers().testFlag(Qt::AltModifier)))
            {
                openParent();
                return;
            }
            if (event->matches(QKeySequence::Find))
            {
                m_search->setFocus();
                m_search->selectAll();
                return;
            }
            if (event->key() == Qt::Key_L && event->modifiers().testFlag(Qt::ControlModifier))
            {
                m_path->setFocus();
                m_path->selectAll();
                return;
            }
            QDialog::keyPressEvent(event);
        }

    private:
        void wireEvents()
        {
            connect(m_path, &QLineEdit::returnPressed, this, [this]
                    { handlePathEntered(); });
            connect(m_name, &QLineEdit::returnPressed, this, [this]
                    { confirmSelection(); });
            connect(m_search, &QLineEdit::textChanged, this, [this]
                    { populateFiles(); });
            connect(m_filter, &QComboBox::currentIndexChanged, this, [this]
                    { populateFiles(); });
            connect(m_places, &QListWidget::itemClicked, this, [this](QListWidgetItem *item)
                    {
                if (!item) return;
                const QString path = item->data(Qt::UserRole).toString();
                if (!path.isEmpty()) openDirectory(path); });
            connect(m_table, &QTableWidget::itemSelectionChanged, this, [this]
                    {
                const int row = m_table->currentRow();
                if (row < 0) return;
                QTableWidgetItem *nameItem = m_table->item(row, 0);
                if (!nameItem) return;
                const QString path = nameItem->data(Qt::UserRole).toString();
                m_name->setText(QFileInfo(path).fileName());
                m_selection->setText(QDir::toNativeSeparators(path)); });
            connect(m_table, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem *item)
                    {
                if (!item) return;
                activatePath(item->data(Qt::UserRole).toString()); });
            connect(m_table, &QTableWidget::itemActivated, this, [this](QTableWidgetItem *item)
                    {
                if (!item) return;
                activatePath(item->data(Qt::UserRole).toString()); });
            connect(m_open, &QPushButton::clicked, this, [this]
                    { confirmSelection(); });
            connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);
        }

        void populatePlaces()
        {
            const auto addPlace = [this](const QString &title, const QString &path)
            {
                if (path.isEmpty() || !QFileInfo(path).isDir())
                    return;
                auto *item = new QListWidgetItem(title, m_places);
                item->setData(Qt::UserRole, QFileInfo(path).absoluteFilePath());
            };
            addPlace(QStringLiteral("Home"), QDir::homePath());
            addPlace(QStringLiteral("Desktop"), QStandardPaths::writableLocation(QStandardPaths::DesktopLocation));
            addPlace(QStringLiteral("Documents"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
            addPlace(QStringLiteral("Downloads"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
            for (const QFileInfo &drive : QDir::drives())
                addPlace(drive.absoluteFilePath(), drive.absoluteFilePath());
        }

        Sc2FileOpenFilter currentFilter() const
        {
            const int index = m_filter ? m_filter->currentIndex() : 0;
            return m_filters.value(qBound(0, index, m_filters.size() - 1));
        }

        void openDirectory(const QString &path, const QString &suggestedFile = QString())
        {
            QFileInfo info(path);
            if (!info.isDir())
            {
                setStatus(QStringLiteral("Directory not found: %1").arg(QDir::toNativeSeparators(path)));
                return;
            }
            m_currentDir = info.absoluteFilePath();
            m_path->setText(QDir::toNativeSeparators(m_currentDir));
            m_name->setText(suggestedFile);
            m_selection->setText(QDir::toNativeSeparators(suggestedFile.isEmpty()
                                                              ? m_currentDir
                                                              : QDir(m_currentDir).absoluteFilePath(suggestedFile)));
            populateFiles();
            if (!suggestedFile.isEmpty())
                selectFileName(suggestedFile);
        }

        void populateFiles()
        {
            const QDir dir(m_currentDir);
            const QString query = m_search->text().trimmed().toLower();
            const Sc2FileOpenFilter filter = currentFilter();
            QFileInfoList entries = dir.entryInfoList(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Readable,
                                                      QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
            m_table->setUpdatesEnabled(false);
            m_table->setRowCount(0);
            int folders = 0;
            int files = 0;
            for (const QFileInfo &entry : entries)
            {
                if (!query.isEmpty() && !entry.fileName().toLower().contains(query))
                    continue;
                if (!matchesOpenFilter(entry, filter))
                    continue;
                const int row = m_table->rowCount();
                m_table->insertRow(row);
                auto *name = new QTableWidgetItem(entry.fileName());
                name->setData(Qt::UserRole, entry.absoluteFilePath());
                name->setData(Qt::UserRole + 1, entry.isDir());
                auto *type = new QTableWidgetItem(fileTypeLabel(entry));
                auto *size = new QTableWidgetItem(entry.isDir() ? QStringLiteral("-") : humanFileSize(entry.size()));
                auto *modified = new QTableWidgetItem(entry.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
                for (QTableWidgetItem *item : {name, type, size, modified})
                    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                m_table->setItem(row, 0, name);
                m_table->setItem(row, 1, type);
                m_table->setItem(row, 2, size);
                m_table->setItem(row, 3, modified);
                if (entry.isDir())
                    ++folders;
                else
                    ++files;
            }
            m_table->setUpdatesEnabled(true);
            setStatus(QStringLiteral("Current folder: %1   |   folders: %2   |   files: %3")
                          .arg(QDir::toNativeSeparators(m_currentDir))
                          .arg(folders)
                          .arg(files));
        }

        void selectFileName(const QString &fileName)
        {
            for (int row = 0; row < m_table->rowCount(); ++row)
            {
                QTableWidgetItem *item = m_table->item(row, 0);
                if (item && item->text().compare(fileName, Qt::CaseInsensitive) == 0)
                {
                    m_table->selectRow(row);
                    m_table->scrollToItem(item);
                    return;
                }
            }
        }

        void handlePathEntered()
        {
            const QString raw = m_path->text().trimmed().remove(QLatin1Char('"'));
            QFileInfo info(raw);
            if (info.isDir())
            {
                openDirectory(info.absoluteFilePath());
                return;
            }
            if (info.isFile())
            {
                activatePath(info.absoluteFilePath());
                return;
            }
            setStatus(QStringLiteral("Directory or file not found: %1").arg(QDir::toNativeSeparators(raw)));
        }

        void activatePath(const QString &path)
        {
            QFileInfo info(path);
            if (info.isDir())
            {
                openDirectory(info.absoluteFilePath());
                return;
            }
            if (info.isFile())
            {
                m_name->setText(info.fileName());
                m_selection->setText(QDir::toNativeSeparators(info.absoluteFilePath()));
                confirmSelection();
            }
        }

        void confirmSelection()
        {
            QString candidate = m_name->text().trimmed().remove(QLatin1Char('"'));
            if (candidate.isEmpty() && m_table->currentRow() >= 0)
            {
                if (QTableWidgetItem *item = m_table->item(m_table->currentRow(), 0))
                    candidate = item->data(Qt::UserRole).toString();
            }
            QFileInfo info(candidate);
            if (!info.isAbsolute())
                info = QFileInfo(QDir(m_currentDir).absoluteFilePath(candidate));
            if (info.isDir())
            {
                openDirectory(info.absoluteFilePath());
                return;
            }
            if (!info.isFile())
            {
                setStatus(QStringLiteral("Select a file."));
                return;
            }
            if (!matchesOpenFilter(info, currentFilter()))
            {
                setStatus(QStringLiteral("File does not match selected filter."));
                return;
            }
            m_selectedFile = info.absoluteFilePath();
            accept();
        }

        void openParent()
        {
            QDir dir(m_currentDir);
            if (dir.cdUp())
                openDirectory(dir.absolutePath());
        }

        void setStatus(const QString &text)
        {
            if (m_status)
                m_status->setText(text);
        }

        QVector<Sc2FileOpenFilter> m_filters;
        QListWidget *m_places = nullptr;
        QTableWidget *m_table = nullptr;
        QLineEdit *m_path = nullptr;
        QLineEdit *m_search = nullptr;
        QLineEdit *m_name = nullptr;
        QComboBox *m_filter = nullptr;
        QLabel *m_status = nullptr;
        QLabel *m_selection = nullptr;
        QPushButton *m_open = nullptr;
        QPushButton *m_cancel = nullptr;
        QString m_currentDir;
        QString m_selectedFile;
    };

    QString openSc2FileStyled(QWidget *parent, const QString &startPath)
    {
        Sc2FileOpenDialog dialog(parent, startPath);
        return dialog.exec() == QDialog::Accepted ? dialog.selectedFile() : QString();
    }

    QString openFolderStyled(QWidget *parent, const QString &startPath)
    {
        QFileDialog dialog(parent, QStringLiteral("Open SC2 Folder"), startPath);
        dialog.setObjectName(QStringLiteral("sc2FileDialog"));
        dialog.setOption(QFileDialog::DontUseNativeDialog, true);
        dialog.setOption(QFileDialog::ShowDirsOnly, true);
        dialog.setFileMode(QFileDialog::Directory);
        dialog.setAcceptMode(QFileDialog::AcceptOpen);
        dialog.setLabelText(QFileDialog::Accept, QStringLiteral("Open"));
        dialog.setMinimumSize(980, 640);
        return dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty()
            ? dialog.selectedFiles().front()
            : QString();
    }

    QString saveTextFileStyled(QWidget *parent, const QString &title, const QString &startPath)
    {
        QFileDialog dialog(parent, title, QFileInfo(startPath).absolutePath(), QStringLiteral("Text files (*.txt)"));
        dialog.setObjectName(QStringLiteral("sc2FileDialog"));
        dialog.setOption(QFileDialog::DontUseNativeDialog, true);
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setFileMode(QFileDialog::AnyFile);
        dialog.setDefaultSuffix(QStringLiteral("txt"));
        dialog.selectFile(QFileInfo(startPath).fileName());
        dialog.setMinimumSize(980, 640);
        return dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty()
            ? dialog.selectedFiles().front()
            : QString();
    }

    DataCollectionMode configuredDataCollectionMode()
    {
        QSettings settings;
        return settings.value(QStringLiteral("dataCollection/mode"), QStringLiteral("UnitAbilWeapon")).toString().compare(QStringLiteral("UnitAbilWeapon"), Qt::CaseInsensitive) == 0
                   ? DataCollectionMode::UnitAbilWeapon
                   : DataCollectionMode::Unit;
    }

    enum class ArchiveReferenceStrength
    {
        None,
        Weak,
        Strong
    };

    ArchiveReferenceStrength archiveReferenceStrength(const QString &entry)
    {
        const QString normalized = QDir::cleanPath(entry).replace('\\', '/').toLower();
        const QString name = normalized.section('/', -1);
        if (name == QStringLiteral("objects") || name == QStringLiteral("units")
            || name == QStringLiteral("triggers") || normalized.contains(QStringLiteral("trigger")))
            return ArchiveReferenceStrength::Strong;
        const QString suffix = QFileInfo(name).suffix().toLower();
        if (suffix == QStringLiteral("galaxy"))
            return ArchiveReferenceStrength::Strong;
        static const QSet<QString> weakNames = {
            QStringLiteral("regions"), QStringLiteral("mapinfo"), QStringLiteral("documentinfo"),
            QStringLiteral("preload.xml"), QStringLiteral("componentlist.sc2components")};
        if (weakNames.contains(name))
            return ArchiveReferenceStrength::Weak;
        static const QSet<QString> weakExtensions = {
            QStringLiteral("txt"), QStringLiteral("ini"), QStringLiteral("json"),
            QStringLiteral("yaml"), QStringLiteral("yml"), QStringLiteral("version"),
            QStringLiteral("sc2components")};
        return weakExtensions.contains(suffix) ? ArchiveReferenceStrength::Weak : ArchiveReferenceStrength::None;
    }

    void collectKnownIdTokens(const QByteArray &bytes, const QSet<QString> &knownIds, QSet<QString> *found)
    {
        const auto isIdChar = [](uchar value)
        {
            return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_' || value == '@';
        };
        for (qsizetype start = 0; start < bytes.size();)
        {
            while (start < bytes.size() && !isIdChar(uchar(bytes[start])))
                ++start;
            qsizetype end = start;
            while (end < bytes.size() && isIdChar(uchar(bytes[end])))
                ++end;
            if (end > start)
            {
                const QString token = QString::fromLatin1(bytes.constData() + start, end - start);
                if (knownIds.contains(token))
                    found->insert(token);
            }
            start = qMax(end, start + 1);
        }
        for (qsizetype start = 0; start + 1 < bytes.size();)
        {
            while (start + 1 < bytes.size() && (!isIdChar(uchar(bytes[start])) || bytes[start + 1] != '\0'))
                ++start;
            qsizetype end = start;
            QByteArray tokenBytes;
            while (end + 1 < bytes.size() && isIdChar(uchar(bytes[end])) && bytes[end + 1] == '\0')
            {
                tokenBytes.append(bytes[end]);
                end += 2;
            }
            if (!tokenBytes.isEmpty())
            {
                const QString token = QString::fromLatin1(tokenBytes);
                if (knownIds.contains(token))
                    found->insert(token);
            }
            start = qMax(end, start + 1);
        }
    }

    void collectKnownIdTokenSources(const QByteArray &bytes, const QSet<QString> &knownIds,
                                    const QString &source, QHash<QString, QStringList> *found)
    {
        QSet<QString> ids;
        collectKnownIdTokens(bytes, knownIds, &ids);
        if (!found)
            return;
        for (const QString &id : ids)
            (*found)[id].append(source);
    }

    bool isSupportedSc2Archive(const QFileInfo &info)
    {
        const QString suffix = info.suffix();
        return suffix.compare(QStringLiteral("SC2Map"), Qt::CaseInsensitive) == 0
            || suffix.compare(QStringLiteral("SC2Mod"), Qt::CaseInsensitive) == 0
            || suffix.compare(QStringLiteral("SC2Components"), Qt::CaseInsensitive) == 0
            || suffix.compare(QStringLiteral("SC2Campaign"), Qt::CaseInsensitive) == 0
            || suffix.compare(QStringLiteral("SC2Archive"), Qt::CaseInsensitive) == 0;
    }

    QStringList collectArchiveFiles(const QString &folderPath)
    {
        QStringList archives;
        QDirIterator it(folderPath, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext())
        {
            const QString filePath = it.next();
            const QString relative = QDir(folderPath).relativeFilePath(filePath).replace('\\', '/');
            const QString relativeLower = relative.toLower();
            const QString fileNameLower = QFileInfo(filePath).fileName().toLower();
            if (relative.startsWith(QStringLiteral("backup_"), Qt::CaseInsensitive)
                || relative.contains(QStringLiteral("/backup_"), Qt::CaseInsensitive)
                || relativeLower.startsWith(QStringLiteral("orig/"))
                || relativeLower.contains(QStringLiteral("/orig/"))
                || fileNameLower.contains(QStringLiteral(".bak-"))
                || fileNameLower.contains(QStringLiteral(".bak.")))
                continue;
            if (isSupportedSc2Archive(QFileInfo(filePath)))
                archives.append(filePath);
        }
        std::sort(archives.begin(), archives.end(), [](const QString &left, const QString &right)
        {
            return QString::compare(left, right, Qt::CaseInsensitive) < 0;
        });
        return archives;
    }

    bool persistentBackupsEnabledForUi()
    {
        return QSettings().value(QStringLiteral("backup/enabled"), true).toBool();
    }

    QString backupPrompt(const QString &withBackup, const QString &withoutBackup)
    {
        return persistentBackupsEnabledForUi() ? withBackup : withoutBackup;
    }

    QString archiveFolderReadOnlyMessage()
    {
        return QStringLiteral("Archive folder mode analyzes multiple maps/mods together and is read-only. Open a single SC2Map/SC2Mod archive or an extracted folder to apply changes.");
    }

    class PersistentTabToolTipFilter final : public QObject
    {
    public:
        using QObject::QObject;

    protected:
        bool eventFilter(QObject *watched, QEvent *event) override
        {
            auto *bar = qobject_cast<QTabBar *>(watched);
            if (!bar || (event->type() != QEvent::ToolTip && event->type() != QEvent::MouseMove))
                return QObject::eventFilter(watched, event);
            QPoint position;
            if (auto *help = dynamic_cast<QHelpEvent *>(event))
                position = help->pos();
            else
                position = static_cast<QMouseEvent *>(event)->position().toPoint();
            const int index = bar->tabAt(position);
            if (index >= 0)
            {
                const QString text = bar->tabText(index).isEmpty() ? bar->tabToolTip(index) : bar->tabText(index);
                QToolTip::showText(bar->mapToGlobal(position), text, bar, bar->tabRect(index), 60000);
            }
            return event->type() == QEvent::ToolTip;
        }
    };
    void drawCoverPixmap(QPainter &painter, const QRect &target, const QPixmap &pixmap)
    {
        if (target.isEmpty() || pixmap.isNull())
            return;
        const qreal targetRatio = qreal(target.width()) / qMax(1, target.height());
        const qreal sourceRatio = qreal(pixmap.width()) / qMax(1, pixmap.height());
        QRect source = pixmap.rect();
        if (sourceRatio > targetRatio)
        {
            const int width = qRound(pixmap.height() * targetRatio);
            source.setLeft((pixmap.width() - width) / 2);
            source.setWidth(width);
        }
        else
        {
            const int height = qRound(pixmap.width() / targetRatio);
            source.setTop((pixmap.height() - height) / 2);
            source.setHeight(height);
        }
        painter.drawPixmap(target, pixmap, source);
    }

    class Sc2BackgroundWidget final : public QWidget
    {
    public:
        explicit Sc2BackgroundWidget(QWidget *parent = nullptr)
            : QWidget(parent),
              m_grid(QStringLiteral(":/textures/ui_nova_storymode_bggrid.png")),
              m_points(QStringLiteral(":/textures/ui_nova_storymode_bgpointgrid.png")),
              m_lights(QStringLiteral(":/textures/ui_nova_login_backgroundlights.png")),
              m_frame(QStringLiteral(":/textures/ui_nova_archives_backgroundframe.png")),
              m_highlight(QStringLiteral(":/textures/ui_nova_archives_backgroundframehighlight.png")),
              m_scanlines(QStringLiteral(":/textures/ui_nova_archives_backgroundframe_scanlines.png"))
        {
            setAttribute(Qt::WA_StyledBackground, true);
        }

    protected:
        void paintEvent(QPaintEvent *event) override
        {
            QPainter painter(this);
            painter.fillRect(event->rect(), QColor(5, 11, 16));

            painter.setOpacity(0.12);
            painter.drawTiledPixmap(event->rect(), m_grid);
            painter.setOpacity(0.10);
            painter.drawTiledPixmap(event->rect(), m_points);

            painter.setOpacity(0.13);
            drawCoverPixmap(painter, rect(), m_lights);
            painter.setOpacity(0.20);
            drawCoverPixmap(painter, rect(), m_frame);
            painter.setOpacity(0.08);
            drawCoverPixmap(painter, rect(), m_highlight);
            painter.setOpacity(0.035);
            drawCoverPixmap(painter, rect(), m_scanlines);
        }

    private:
        QPixmap m_grid;
        QPixmap m_points;
        QPixmap m_lights;
        QPixmap m_frame;
        QPixmap m_highlight;
        QPixmap m_scanlines;
    };

    class ButtonEffects : public QObject
    {
    public:
        explicit ButtonEffects(QObject *parent = nullptr)
            : QObject(parent)
        {
            QFile soundFile(QStringLiteral(":/sounds/UI_NovaUT_ButtonClick1.wav"));
            if (soundFile.open(QIODevice::ReadOnly))
            {
                m_soundData = soundFile.readAll();
            }
        }

        void installOn(QWidget *widget)
        {
            if (!widget)
            {
                return;
            }
            widget->installEventFilter(this);
        }

    protected:
        bool eventFilter(QObject *watched, QEvent *event) override
        {
            auto *button = qobject_cast<QAbstractButton *>(watched);
            if (!button || !button->isEnabled())
            {
                return QObject::eventFilter(watched, event);
            }

            if (event->type() == QEvent::MouseButtonRelease)
            {
                auto *mouseEvent = static_cast<QMouseEvent *>(event);
                if (mouseEvent->button() == Qt::LeftButton)
                {
                    playClick(button);
                }
            }
            return QObject::eventFilter(watched, event);
        }

    private:
        void playClick(QAbstractButton *button)
        {
            QSettings settings;
#ifdef Q_OS_WIN
            if (settings.value(QStringLiteral("ui/buttonSounds"), true).toBool() && !m_soundData.isEmpty())
            {
                PlaySoundA(reinterpret_cast<LPCSTR>(m_soundData.constData()), nullptr,
                           SND_ASYNC | SND_MEMORY | SND_NODEFAULT | SND_NOSTOP);
            }
#else
            QApplication::beep();
#endif

            if (!settings.value(QStringLiteral("ui/buttonAnimations"), true).toBool())
            {
                return;
            }

            // Pulse the text only. A graphics effect on the whole button also
            // glows the texture border and produces unwanted extra colors.
            button->setProperty("textPulse", true);
            button->style()->unpolish(button);
            button->style()->polish(button);
            QTimer::singleShot(150, button, [button]
                               {
                button->setProperty("textPulse", false);
                button->style()->unpolish(button);
                button->style()->polish(button); });
        }

        QByteArray m_soundData;
    };

    class PromoButtonAnimator final : public QObject
    {
    public:
        explicit PromoButtonAnimator(QObject *parent = nullptr)
            : QObject(parent)
        {
        }

        void installOn(QToolButton *button, const QSize &normalIcon, const QSize &hoverIcon, const QColor &glowColor)
        {
            if (!button)
                return;
            auto *effect = new QGraphicsDropShadowEffect(button);
            effect->setOffset(0, 0);
            effect->setBlurRadius(0);
            effect->setColor(QColor(glowColor.red(), glowColor.green(), glowColor.blue(), 0));
            button->setGraphicsEffect(effect);

            State state;
            state.effect = effect;
            state.normalIcon = normalIcon;
            state.hoverIcon = hoverIcon;
            state.pressIcon = !normalIcon.isEmpty()
                                  ? QSize(qMax(1, int(normalIcon.width() * 0.88)), qMax(1, int(normalIcon.height() * 0.88)))
                                  : QSize();
            state.glowColor = glowColor;
            m_states.insert(button, state);
            button->installEventFilter(this);
        }

    protected:
        bool eventFilter(QObject *watched, QEvent *event) override
        {
            auto it = m_states.find(watched);
            if (it == m_states.end())
                return QObject::eventFilter(watched, event);
            auto *button = qobject_cast<QToolButton *>(watched);
            if (!button || !button->isEnabled())
                return QObject::eventFilter(watched, event);

            State &state = it.value();
            switch (event->type())
            {
            case QEvent::Enter:
                state.hovering = true;
                animateGlow(button, 18.0);
                animateIcon(button, state.hoverIcon);
                break;
            case QEvent::Leave:
                state.hovering = false;
                animateGlow(button, 0.0);
                animateIcon(button, state.normalIcon);
                break;
            case QEvent::MouseButtonPress:
                animateGlow(button, 27.0);
                animateIcon(button, state.pressIcon);
                break;
            case QEvent::MouseButtonRelease:
                animateGlow(button, state.hovering ? 18.0 : 0.0);
                animateIcon(button, state.hovering ? state.hoverIcon : state.normalIcon);
                break;
            default:
                break;
            }
            return QObject::eventFilter(watched, event);
        }

    private:
        struct State
        {
            QGraphicsDropShadowEffect *effect = nullptr;
            QSize normalIcon;
            QSize hoverIcon;
            QSize pressIcon;
            QColor glowColor;
            bool hovering = false;
        };

        void animateGlow(QToolButton *button, qreal target)
        {
            State &state = m_states[button];
            if (!state.effect)
                return;
            state.effect->setColor(target > 0.0
                                       ? state.glowColor
                                       : QColor(state.glowColor.red(), state.glowColor.green(), state.glowColor.blue(), 0));
            auto *animation = new QPropertyAnimation(state.effect, "blurRadius", state.effect);
            animation->setDuration(160);
            animation->setStartValue(state.effect->blurRadius());
            animation->setEndValue(target);
            animation->setEasingCurve(QEasingCurve::OutCubic);
            connect(animation, &QPropertyAnimation::finished, animation, &QObject::deleteLater);
            animation->start();
        }

        void animateIcon(QToolButton *button, const QSize &target)
        {
            if (!button || target.isEmpty())
                return;
            auto *animation = new QPropertyAnimation(button, "iconSize", button);
            animation->setDuration(130);
            animation->setStartValue(button->iconSize());
            animation->setEndValue(target);
            animation->setEasingCurve(QEasingCurve::OutBack);
            connect(animation, &QPropertyAnimation::finished, animation, &QObject::deleteLater);
            animation->start();
        }

        QHash<QObject *, State> m_states;
    };

    QString defaultTestFolder()
    {
        return QStringLiteral("C:/Users/Vladimir/Downloads/Regenerate_trigger/TriggerCustom/comp");
    }

    QString levelToString(spdlog::level::level_enum level)
    {
        switch (level)
        {
        case spdlog::level::trace:
            return QStringLiteral("TRACE");
        case spdlog::level::debug:
            return QStringLiteral("DEBUG");
        case spdlog::level::info:
            return QStringLiteral("INFO");
        case spdlog::level::warn:
            return QStringLiteral("WARN");
        case spdlog::level::err:
            return QStringLiteral("ERROR");
        case spdlog::level::critical:
            return QStringLiteral("CRITICAL");
        default:
            return QStringLiteral("LOG");
        }
    }

    QString modeLabelFor(int kind)
    {
        switch (kind)
        {
        case 0:
            return QStringLiteral("Mode: folder analysis");
        case 1:
            return QStringLiteral("Mode: archive folder analysis (read-only)");
        case 2:
            return QStringLiteral("Mode: XML file analysis");
        case 3:
            return QStringLiteral("Mode: archive analysis (read-only)");
        default:
            return QStringLiteral("Mode: waiting for analysis");
        }
    }

}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUi();
    setupLogging();
    setupTheme();
    AudioManager::instance()->initialize();
    QSettings settings;
    const QString collectionModeKey = QStringLiteral("dataCollection/mode");
    const QString collectionModeMigrationKey = QStringLiteral("dataCollection/unitAbilWeaponDefaultMigrated");
    if (!settings.value(collectionModeMigrationKey, false).toBool())
    {
        if (!settings.contains(collectionModeKey)
            || settings.value(collectionModeKey).toString().compare(QStringLiteral("Unit"), Qt::CaseInsensitive) == 0)
            settings.setValue(collectionModeKey, QStringLiteral("UnitAbilWeapon"));
        settings.setValue(collectionModeMigrationKey, true);
    }
    else if (!settings.contains(collectionModeKey))
    {
        settings.setValue(collectionModeKey, QStringLiteral("UnitAbilWeapon"));
    }
    const QString duplicateMergeKey = QStringLiteral("optimization/duplicateMergeEnabled");
    const QString duplicateMergeMigrationKey = QStringLiteral("optimization/duplicateMergeDefaultEnabledMigrated");
    if (!settings.value(duplicateMergeMigrationKey, false).toBool())
    {
        if (!settings.contains(duplicateMergeKey) || !settings.value(duplicateMergeKey).toBool())
            settings.setValue(duplicateMergeKey, true);
        settings.setValue(duplicateMergeMigrationKey, true);
    }
    setDuplicateMergeEnabled(settings.value(duplicateMergeKey, true).toBool());
    loadDefaultFolder();

    const QString rulesPath = runtimePath(QStringLiteral("config/rules.json"));
    const QString whitelistPath = runtimePath(QStringLiteral("config/whitelist.json"));
    QString errorMessage;
    if (m_configManager.load(rulesPath, whitelistPath, &errorMessage))
    {
        m_whitelistIds = m_configManager.whitelistIds();
        logLine(QStringLiteral("Loaded whitelist entries: %1").arg(m_whitelistIds.size()));
    }
    else
    {
        logLine(QStringLiteral("Config load skipped: %1").arg(errorMessage));
    }
    updateFullscreenActionText();
}

void MainWindow::setupUi()
{
    setWindowTitle(QStringLiteral("SC2 Data Helper"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/Icon.png")));
    resize(1550, 980);

    auto *toolbar = addToolBar(QStringLiteral("Main"));
    toolbar->setObjectName(QStringLiteral("mainToolbar"));
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(0, 0));
    toolbar->setMinimumHeight(58);

    m_openFileAction = new QAction(QStringLiteral("Open SC2 File"), this);
    m_openFolderAction = new QAction(QStringLiteral("Open Folder"), this);
    m_analyzeAction = new QAction(QStringLiteral("Analyze"), this);
    m_dryRunAction = new QAction(QStringLiteral("Optimization"), this);
    m_applyAction = new QAction(QStringLiteral("Review Optimization Plan"), this);
    m_settingsAction = new QAction(QStringLiteral("Settings"), this);
    m_fullscreenAction = new QAction(QStringLiteral("Fullscreen"), this);
    m_exitAction = new QAction(QStringLiteral("Exit"), this);
    m_analyzeAction->setShortcut(QKeySequence(Qt::Key_F5));
    m_analyzeAction->setShortcutContext(Qt::ApplicationShortcut);
    m_analyzeAction->setToolTip(QStringLiteral("Analyze / refresh the current project (F5)"));
    m_dryRunAction->setEnabled(false);
    m_applyAction->setEnabled(false);

    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setObjectName(QStringLiteral("pathEdit"));
    m_pathEdit->setPlaceholderText(QStringLiteral("Selected source path"));
    m_pathEdit->setReadOnly(true);
    m_pathEdit->setCursorPosition(0);
    m_pathEdit->hide();

    auto *toolbarContent = new QWidget(toolbar);
    toolbarContent->setObjectName(QStringLiteral("mainToolbarContent"));
    toolbarContent->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *toolbarLayout = new QHBoxLayout(toolbarContent);
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(0);
    const auto addActionButton = [toolbarContent, toolbarLayout](QAction *action, int stretch)
    {
        auto *button = new QToolButton(toolbarContent);
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setFocusPolicy(Qt::NoFocus);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        toolbarLayout->addWidget(button, stretch);
        return button;
    };
    addActionButton(m_openFileAction, 1);
    addActionButton(m_openFolderAction, 1);
    addActionButton(m_analyzeAction, 1);
    addActionButton(m_dryRunAction, 1);
    addActionButton(m_applyAction, 2);
    addActionButton(m_settingsAction, 1);
    addActionButton(m_fullscreenAction, 1);
    addActionButton(m_exitAction, 1);

    auto *discordButton = new QToolButton(toolbarContent);
    discordButton->setObjectName(QStringLiteral("discordPromoButton"));
    discordButton->setIcon(QIcon(QStringLiteral(":/textures/Discord.png")));
    discordButton->setIconSize(QSize(30, 30));
    discordButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    discordButton->setToolTip(QStringLiteral("Join Discord"));
    discordButton->setCursor(Qt::PointingHandCursor);
    discordButton->setFocusPolicy(Qt::NoFocus);
    discordButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbarLayout->addWidget(discordButton, 1);

    auto *boostyButton = new QToolButton(toolbarContent);
    boostyButton->setObjectName(QStringLiteral("boostyPromoButton"));
    boostyButton->setText(QStringLiteral("Boosty"));
    boostyButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    boostyButton->setToolTip(QStringLiteral("Support on Boosty"));
    boostyButton->setCursor(Qt::PointingHandCursor);
    boostyButton->setFocusPolicy(Qt::NoFocus);
    boostyButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbarLayout->addWidget(boostyButton, 1);
    toolbar->addWidget(toolbarContent);

    auto *root = new Sc2BackgroundWidget(this);
    root->setObjectName(QStringLiteral("workspaceRoot"));
    auto *rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto *splitterFrame = new QFrame(root);
    splitterFrame->setObjectName(QStringLiteral("workspaceFrame"));
    auto *splitterLayout = new QVBoxLayout(splitterFrame);
    splitterLayout->setContentsMargins(0, 0, 0, 0);

    m_tabs = new QTabWidget(splitterFrame);
    m_tabs->setObjectName(QStringLiteral("workspaceTabs"));
    m_analysisPage = new OverviewPage(m_tabs);
    m_dependenciesPage = new DependenciesPage(m_tabs);
    m_graphPage = new GraphPage(m_tabs);
    m_propertiesPage = new PropertiesPage(m_tabs);
    m_dataCollectionPage = new DataCollectionPage(m_tabs);
    m_renameIdsPage = new RenameIdsPage(m_tabs);
    m_duplicatesPage = new DuplicatesPage(m_tabs);
    m_cleanupPage = new UnusedPage(m_tabs);
    m_dryRunPage = new FormatterPage(this);
    m_dryRunPage->hide();
    m_logPanel = new LogPanel(m_tabs);
    m_xmlSourcePage = new XmlSourcePage(m_tabs);

    m_tabs->addTab(m_analysisPage, QStringLiteral("Objects"));
    m_tabs->addTab(m_dependenciesPage, QStringLiteral("Dependencies"));
    m_tabs->addTab(m_graphPage, QStringLiteral("Graph"));
    m_tabs->addTab(m_propertiesPage, QStringLiteral("Properties"));
    m_tabs->addTab(m_dataCollectionPage, QStringLiteral("Data Collection"));
    m_tabs->addTab(m_renameIdsPage, QStringLiteral("Rename To Standard"));
    m_tabs->addTab(m_duplicatesPage, QStringLiteral("Duplicate Merge"));
    m_tabs->addTab(m_cleanupPage, QStringLiteral("Unused Objects"));
    m_tabs->addTab(m_logPanel, QStringLiteral("Logs"));
    m_tabs->addTab(m_xmlSourcePage, QStringLiteral("XML Source"));
    m_tabs->tabBar()->setExpanding(false);
    m_tabs->tabBar()->setUsesScrollButtons(true);
    m_tabs->tabBar()->setElideMode(Qt::ElideNone);
    for (int index = 0; index < m_tabs->count(); ++index)
    {
        const QString title = m_tabs->tabText(index);
        m_tabs->setTabToolTip(index, title);
        auto *label = new QLabel(title, m_tabs->tabBar());
        label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        QFont labelFont = label->font();
        labelFont.setBold(true);
        label->setFont(labelFont);
        // Leave room for both edge glyphs and the glow. QTabBar clips custom
        // tab buttons to their size hint, which previously cut off letters.
        label->setFixedWidth(label->fontMetrics().horizontalAdvance(title) + 52);
        label->setMinimumHeight(30);
        label->setContentsMargins(12, 0, 12, 0);
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet(QStringLiteral("background: transparent; color: #e8fffb; padding: 0;"));
        m_tabs->tabBar()->setTabButton(index, QTabBar::LeftSide, label);
        m_tabs->setTabText(index, QString());
    }
    m_tabs->tabBar()->setMouseTracking(true);
    m_tabs->tabBar()->installEventFilter(new PersistentTabToolTipFilter(m_tabs->tabBar()));
    const auto updateTabGlow = [this](int selected)
    {
        for (int index = 0; index < m_tabs->count(); ++index)
        {
            auto *label = qobject_cast<QLabel *>(m_tabs->tabBar()->tabButton(index, QTabBar::LeftSide));
            if (!label)
                continue;
            auto *glow = new QGraphicsDropShadowEffect(label);
            glow->setOffset(0, 0);
            glow->setBlurRadius(index == selected ? 14.0 : 11.0);
            glow->setColor(index == selected ? QColor(255, 120, 38, 235) : QColor(65, 235, 210, 185));
            label->setStyleSheet(index == selected
                                     ? QStringLiteral("background: transparent; color: #fff4e9; padding: 0;")
                                     : QStringLiteral("background: transparent; color: #e8fffb; padding: 0;"));
            label->setGraphicsEffect(glow);
        }
    };
    updateTabGlow(m_tabs->currentIndex());
    connect(m_tabs, &QTabWidget::currentChanged, this, updateTabGlow);

    splitterLayout->addWidget(m_tabs);
    rootLayout->addWidget(splitterFrame, 1);
    setCentralWidget(root);
    connect(m_openFileAction, &QAction::triggered, this, &MainWindow::openSc2File);
    connect(m_openFolderAction, &QAction::triggered, this, &MainWindow::openSourceFolder);
    connect(m_analyzeAction, &QAction::triggered, this, &MainWindow::analyzeFolder);
    connect(m_dryRunAction, &QAction::triggered, this, &MainWindow::runDryRun);
    connect(m_applyAction, &QAction::triggered, this, &MainWindow::showDryRunTab);
    connect(m_settingsAction, &QAction::triggered, this, &MainWindow::showSettingsDialog);
    connect(m_fullscreenAction, &QAction::triggered, this, &MainWindow::toggleFullscreen);
    connect(m_exitAction, &QAction::triggered, this, []
            {
        AudioManager::instance()->shutdown();
        QApplication::closeAllWindows();
        QCoreApplication::quit(); });
    const auto openSupportUrl = [this](const char *url, const QString &label)
    {
        if (!QDesktopServices::openUrl(QUrl(QString::fromLatin1(url))))
        {
            QMessageBox::warning(this, label, QStringLiteral("Unable to open link: %1").arg(QString::fromLatin1(url)));
        }
    };
    connect(discordButton, &QToolButton::clicked, this, [openSupportUrl]
            { openSupportUrl(kDiscordInviteUrl, QStringLiteral("Discord")); });
    connect(boostyButton, &QToolButton::clicked, this, [openSupportUrl]
            { openSupportUrl(kBoostyUrl, QStringLiteral("Boosty")); });
    connect(m_duplicatesPage, &DuplicatesPage::previewMergeRequested, this, &MainWindow::previewMerge);
    connect(m_duplicatesPage, &DuplicatesPage::applyMergeRequested, this, &MainWindow::applyMerge);
    connect(m_cleanupPage, &UnusedPage::previewDeletionRequested, this, &MainWindow::previewUnusedDeletion);
    connect(m_cleanupPage, &UnusedPage::applyDeletionRequested, this, &MainWindow::applyUnusedDeletion);
    connect(m_renameIdsPage, &RenameIdsPage::previewRequested, this, &MainWindow::previewStandardRename);
    connect(m_renameIdsPage, &RenameIdsPage::applyRequested, this, &MainWindow::applyStandardRename);
    connect(m_renameIdsPage, &RenameIdsPage::exportRequested, this, &MainWindow::exportStandardRenameReport);
    connect(m_dataCollectionPage, &DataCollectionPage::previewRequested, this, &MainWindow::previewDataCollection);
    connect(m_dataCollectionPage, &DataCollectionPage::applyRequested, this, &MainWindow::applyDataCollection);
    connect(m_dataCollectionPage, &DataCollectionPage::exportRequested, this, &MainWindow::exportDataCollectionReport);
    connect(m_dryRunPage, &FormatterPage::previewBuilt, this, [this]
            { m_applyAction->setEnabled(true); });
    connect(m_dryRunPage, &FormatterPage::openUnusedRequested, this, [this](const QVector<int> &rows)
            {
        if (m_optimizationDialog) m_optimizationDialog->accept();
        m_cleanupPage->selectRows(rows); m_tabs->setCurrentWidget(m_cleanupPage); });
    connect(m_dryRunPage, &FormatterPage::openDuplicateRequested, this, [this](const MergeRequest &request)
            {
        if (m_optimizationDialog) m_optimizationDialog->accept();
        m_duplicatesPage->selectRequest(request); m_tabs->setCurrentWidget(m_duplicatesPage); });
    connect(m_dryRunPage, &FormatterPage::openRenameRequested, this, [this]
            {
        if (m_optimizationDialog) m_optimizationDialog->accept(); m_tabs->setCurrentWidget(m_renameIdsPage); });
    connect(m_dryRunPage, &FormatterPage::openCollectionRequested, this, [this]
            {
        if (m_optimizationDialog) m_optimizationDialog->accept(); m_tabs->setCurrentWidget(m_dataCollectionPage); });
    connect(m_dryRunPage, &FormatterPage::applyWizardRequested, this, &MainWindow::applyOptimizationWizardPlan);
    connect(m_dryRunPage, &FormatterPage::wizardFinished, this, [this]
            {
        if (m_optimizationDialog) m_optimizationDialog->accept(); });
    connect(m_duplicatesPage, &DuplicatesPage::sourceRequested, this, [this](int nodeIndex)
            {
        m_xmlSourcePage->showNode(nodeIndex);
        m_tabs->setCurrentWidget(m_xmlSourcePage); });

    auto *undoAction = new QAction(QStringLiteral("Undo"), this);
    undoAction->setShortcut(QKeySequence::Undo);
    undoAction->setShortcutContext(Qt::ApplicationShortcut);
    addAction(undoAction);
    connect(undoAction, &QAction::triggered, this, &MainWindow::undoFocusedEditor);

    auto *redoAction = new QAction(QStringLiteral("Redo"), this);
    redoAction->setShortcuts({QKeySequence::Redo, QKeySequence(Qt::CTRL | Qt::Key_Y)});
    redoAction->setShortcutContext(Qt::ApplicationShortcut);
    addAction(redoAction);
    connect(redoAction, &QAction::triggered, this, &MainWindow::redoFocusedEditor);

    auto *fullscreenAction = new QAction(QStringLiteral("Toggle Fullscreen"), this);
    fullscreenAction->setShortcut(QKeySequence(Qt::Key_F11));
    fullscreenAction->setShortcutContext(Qt::ApplicationShortcut);
    addAction(fullscreenAction);
    connect(fullscreenAction, &QAction::triggered, this, &MainWindow::toggleFullscreen);
    connect(m_analysisPage, &OverviewPage::folderPathChanged, this, [this](const QString &folder)
            { m_rootFolder = folder; });
    connect(m_analysisPage, &OverviewPage::currentRowChanged, this, [this](int row)
            {
        m_dependenciesPage->setCurrentRow(row);
        m_graphPage->setCurrentRow(row);
        m_propertiesPage->setCurrentRow(row);
        if (m_tabs && m_tabs->currentWidget() == m_graphPage) {
            QMetaObject::invokeMethod(m_graphPage, "fitGraph", Qt::QueuedConnection);
        }
        if (row >= 0 && row < m_result.nodes.size()) {
            const DataNode &node = m_result.nodes[row];
            statusBar()->showMessage(QStringLiteral("Loaded path: %1 | Selected: %2 / %3")
                                         .arg(m_currentSourcePath.isEmpty() ? m_rootFolder : m_currentSourcePath,
                                             node.elementName.isEmpty() ? node.id : node.elementName,
                                              node.id.isEmpty() ? QStringLiteral("-") : node.id));
        } });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index)
            {
        if (m_tabs->widget(index) == m_graphPage) {
            QMetaObject::invokeMethod(m_graphPage, "fitGraph", Qt::QueuedConnection);
        } });
    connect(m_analysisPage, &OverviewPage::objectDoubleClicked, this, [this](int row)
            { showGraphForRow(row); });

    statusBar()->showMessage(QStringLiteral("Ready"));

    auto *effects = new ButtonEffects(this);
    for (QAbstractButton *button : findChildren<QAbstractButton *>())
    {
        if (auto *toolButton = qobject_cast<QToolButton *>(button))
        {
            const QString name = toolButton->objectName();
            if (name != QStringLiteral("discordPromoButton") && name != QStringLiteral("boostyPromoButton"))
            {
                const int textWidth = toolButton->fontMetrics().horizontalAdvance(toolButton->text());
                toolButton->setMinimumWidth(qMax(toolButton->minimumWidth(), textWidth + 58));
            }
        }
        effects->installOn(button);
    }
    auto *promoAnimator = new PromoButtonAnimator(this);
    promoAnimator->installOn(discordButton, QSize(30, 30), QSize(30, 30), QColor(88, 190, 255, 220));
    promoAnimator->installOn(boostyButton, QSize(), QSize(), QColor(255, 176, 74, 230));

    for (QLabel *label : findChildren<QLabel *>())
    {
        const QString name = label->objectName();
        if (name == QStringLiteral("panelTitle") || name == QStringLiteral("modeBadge"))
        {
            auto *glow = new QGraphicsDropShadowEffect(label);
            glow->setBlurRadius(name == QStringLiteral("panelTitle") ? 16.0 : 10.0);
            glow->setColor(name == QStringLiteral("panelTitle")
                               ? QColor(74, 255, 203, 150)
                               : QColor(62, 175, 255, 120));
            glow->setOffset(0.0, 0.0);
            label->setGraphicsEffect(glow);
        }
    }
}

void MainWindow::undoFocusedEditor()
{
    if (QWidget *focused = QApplication::focusWidget())
    {
        QMetaObject::invokeMethod(focused, "undo", Qt::DirectConnection);
    }
}

void MainWindow::redoFocusedEditor()
{
    if (QWidget *focused = QApplication::focusWidget())
    {
        QMetaObject::invokeMethod(focused, "redo", Qt::DirectConnection);
    }
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event && event->type() == QEvent::WindowStateChange)
    {
        updateFullscreenActionText();
    }
}

void MainWindow::toggleFullscreen()
{
    if (isFullScreen())
    {
        showMaximized();
    }
    else
    {
        showFullScreen();
    }
    updateFullscreenActionText();
}

void MainWindow::updateFullscreenActionText()
{
    if (!m_fullscreenAction)
    {
        return;
    }
    m_fullscreenAction->setText(isFullScreen() ? QStringLiteral("Windowed")
                                               : QStringLiteral("Fullscreen"));
    m_fullscreenAction->setToolTip(QStringLiteral("Toggle fullscreen mode (F11)"));
}

void MainWindow::showSettingsDialog()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("toolDialog"));
    dialog.setWindowTitle(QStringLiteral("SC2 Data Helper Settings"));
    dialog.setMinimumSize(820, 660);
    dialog.resize(860, 700);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("INTERFACE SETTINGS"), &dialog);
    title->setObjectName(QStringLiteral("panelTitle"));
    layout->addWidget(title);

    QSettings settings;
    const auto checkBoxRow = [&dialog](const QString &text, bool checked, const QString &toolTip = QString())
    {
        auto *row = new QCheckBox(text, &dialog);
        row->setProperty("textureType", QStringLiteral("checkBoxRow"));
        row->setChecked(checked);
        row->setFocusPolicy(Qt::NoFocus);
        if (!toolTip.isEmpty())
            row->setToolTip(toolTip);
        return row;
    };
    auto *soundCheck = checkBoxRow(QStringLiteral("Button sounds"),
                                   settings.value(QStringLiteral("ui/buttonSounds"), true).toBool());
    auto *animationCheck = checkBoxRow(QStringLiteral("Button animations"),
                                       settings.value(QStringLiteral("ui/buttonAnimations"), true).toBool());
    auto *musicCheck = checkBoxRow(QStringLiteral("Background music"), AudioManager::isMusicEnabled());
    auto *musicValue = new QLabel(&dialog);
    musicValue->setObjectName(QStringLiteral("inspectorSubtitle"));
    auto *musicSlider = new QSlider(Qt::Horizontal, &dialog);
    musicSlider->setObjectName(QStringLiteral("backgroundMusicVolume"));
    musicSlider->setRange(0, 100);
    musicSlider->setValue(int(AudioManager::musicVolume() * 100.0));
    musicSlider->setFocusPolicy(Qt::NoFocus);
    QObject::connect(musicSlider, &QSlider::valueChanged, &dialog, [musicValue](int value)
                     { musicValue->setText(QStringLiteral("Music volume: %1%").arg(value)); });
    musicValue->setText(QStringLiteral("Music volume: %1%").arg(musicSlider->value()));
    auto *duplicatesCheck = checkBoxRow(
        QStringLiteral("Enable Duplicate Merge in Optimization"),
        settings.value(QStringLiteral("optimization/duplicateMergeEnabled"), true).toBool(),
        QStringLiteral("Enabled by default. When enabled, Optimization adds the Duplicate Merge review step."));
    auto *backupCheck = checkBoxRow(
        QStringLiteral("Create backup files before applying changes"),
        settings.value(QStringLiteral("backup/enabled"), true).toBool(),
        QStringLiteral("When disabled, SC2 archives and folders are edited without creating persistent .bak or backup_ copies."));
    auto *startFullscreenCheck = checkBoxRow(QStringLiteral("Start in full screen"),
                                             settings.value(QStringLiteral("ui/startFullscreen"), true).toBool());
    auto *collectionModeLabel = new QLabel(QStringLiteral("DATA COLLECTION MODE"), &dialog);
    collectionModeLabel->setObjectName(QStringLiteral("panelTitle"));
    auto *collectionMode = new QComboBox(&dialog);
    collectionMode->addItem(QStringLiteral("Unit - one collection per unit family"), QStringLiteral("Unit"));
    collectionMode->addItem(QStringLiteral("UnitAbilWeapon - separate Unit, Ability and Weapon collections"), QStringLiteral("UnitAbilWeapon"));
    const QString savedCollectionMode = settings.value(QStringLiteral("dataCollection/mode"), QStringLiteral("UnitAbilWeapon")).toString();
    collectionMode->setCurrentIndex(qMax(0, collectionMode->findData(savedCollectionMode)));
    collectionMode->setToolTip(QStringLiteral("Unit keeps the current behavior. UnitAbilWeapon creates separate collections like Gargantua, Gargantua_Jump and Gargantua_Weapon."));
    layout->addWidget(soundCheck);
    layout->addWidget(animationCheck);
    layout->addWidget(musicCheck);
    layout->addWidget(musicValue);
    layout->addWidget(musicSlider);
    layout->addWidget(duplicatesCheck);
    layout->addWidget(backupCheck);
    layout->addWidget(startFullscreenCheck);
    layout->addWidget(collectionModeLabel);
    layout->addWidget(collectionMode);
    layout->addStretch(1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]()
            {
        settings.setValue(QStringLiteral("ui/buttonSounds"), soundCheck->isChecked());
        settings.setValue(QStringLiteral("ui/buttonAnimations"), animationCheck->isChecked());
        settings.setValue(QStringLiteral("optimization/duplicateMergeEnabled"), duplicatesCheck->isChecked());
        settings.setValue(QStringLiteral("backup/enabled"), backupCheck->isChecked());
        settings.setValue(QStringLiteral("ui/startFullscreen"), startFullscreenCheck->isChecked());
        settings.setValue(QStringLiteral("dataCollection/mode"), collectionMode->currentData().toString());
        AudioManager::setMusicSettings(musicCheck->isChecked(), musicSlider->value() / 100.0);
        setDuplicateMergeEnabled(duplicatesCheck->isChecked());
        if (!m_result.nodes.isEmpty()) refreshPages();
        dialog.accept(); });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::setupLogging()
{
    const QString logFile = runtimePath(QStringLiteral("logs/sc2_data_helper.log"));
    QDir().mkpath(QFileInfo(logFile).absolutePath());

    auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logFile.toStdString(), 1024 * 1024 * 5, 3);
    auto uiSink = std::make_shared<spdlog::sinks::callback_sink_mt>(
        [this](const spdlog::details::log_msg &msg)
        {
            const QString payload = QString::fromUtf8(msg.payload.data(), static_cast<int>(msg.payload.size()));
            const QString line = QStringLiteral("[%1] %2").arg(levelToString(msg.level), payload);
            QMetaObject::invokeMethod(m_logPanel, [this, line]()
                                      { m_logPanel->appendMessage(line); }, Qt::QueuedConnection);
        });

    std::vector<spdlog::sink_ptr> sinks{fileSink, uiSink};
    m_logger = std::make_shared<spdlog::logger>("sc2dh", sinks.begin(), sinks.end());
    m_logger->set_level(spdlog::level::info);
    m_logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(m_logger);
    logLine(QStringLiteral("Logging to %1").arg(logFile));
}

void MainWindow::setupTheme()
{
    QString loadedFrom;
    QString errorMessage;
    if (ThemeManager::applyDarkTheme(qApp, &loadedFrom, &errorMessage))
    {
        logLine(QStringLiteral("Theme loaded from %1").arg(loadedFrom));
    }
    else
    {
        logLine(QStringLiteral("Theme warning: %1").arg(errorMessage));
        statusBar()->showMessage(QStringLiteral("Theme warning: %1").arg(errorMessage), 10000);
    }
}

void MainWindow::loadDefaultFolder()
{
    QSettings settings;
    const QString lastSource = settings.value(QStringLiteral("paths/lastSourcePath")).toString();
    const QFileInfo lastInfo(lastSource);
    const QString folder = !lastSource.isEmpty() && lastInfo.exists() ? lastSource : defaultTestFolder();
    const QFileInfo info(folder);
    m_rootFolder = info.isDir() ? folder : info.absolutePath();
    if (info.isDir())
        m_sourceKind = collectArchiveFiles(folder).isEmpty() ? SourceKind::Folder : SourceKind::ArchiveFolder;
    else if (info.suffix().compare(QStringLiteral("xml"), Qt::CaseInsensitive) == 0)
        m_sourceKind = SourceKind::XmlFile;
    else if (isSupportedSc2Archive(info))
        m_sourceKind = SourceKind::ArchiveFile;
    else
        m_sourceKind = SourceKind::Folder;
    m_pathEdit->setText(folder);
    m_analysisPage->setFolderPath(folder);
    m_analysisPage->setModeLabel(QStringLiteral("Mode: ready to analyze"));
    m_analysisPage->setOutputText(QStringLiteral("Loaded last path. Press Analyze to scan it."));
    setCurrentSourcePath(folder);
    if (QFileInfo::exists(folder))
    {
        logLine(QStringLiteral("Initial path set: %1").arg(folder));
    }
    else
    {
        logLine(QStringLiteral("Initial path does not exist yet: %1").arg(folder));
    }
}

QString MainWindow::runtimePath(const QString &relativePath) const
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/") + relativePath;
}

void MainWindow::logLine(const QString &line)
{
    if (m_logger)
    {
        m_logger->info("{}", line.toStdString());
    }
    else if (m_logPanel)
    {
        m_logPanel->appendMessage(line);
    }
}

void MainWindow::setCurrentSourcePath(const QString &path)
{
    m_currentSourcePath = path;
    if (m_pathEdit)
    {
        m_pathEdit->setText(path);
        m_pathEdit->setCursorPosition(0);
    }
    if (!path.isEmpty())
    {
        statusBar()->showMessage(QStringLiteral("Loaded path: %1").arg(path));
    }
}

void MainWindow::openSc2File()
{
    QSettings settings;
    const QString savedPath = settings.value(QStringLiteral("paths/lastSourcePath")).toString();
    QString startPath = !m_currentSourcePath.isEmpty() ? m_currentSourcePath : savedPath;
    if (!startPath.isEmpty() && !QFileInfo::exists(startPath))
        startPath = QFileInfo(startPath).absolutePath();

    const QString selected = openSc2FileStyled(this, startPath);
    if (selected.isEmpty())
    {
        return;
    }
    const QFileInfo info(selected);
    const bool previousOptimizationEnabled = m_dryRunAction && m_dryRunAction->isEnabled();
    const bool previousReviewEnabled = m_applyAction && m_applyAction->isEnabled();
    if (info.suffix().compare(QStringLiteral("xml"), Qt::CaseInsensitive) == 0)
        m_sourceKind = SourceKind::XmlFile;
    else if (isSupportedSc2Archive(info))
        m_sourceKind = SourceKind::ArchiveFile;
    else
        m_sourceKind = SourceKind::Unknown;
    m_rootFolder = info.absolutePath();
    setCurrentSourcePath(selected);
    settings.setValue(QStringLiteral("paths/lastSourcePath"), selected);
    m_analysisPage->setFolderPath(selected);
    m_analysisPage->setModeLabel(QStringLiteral("Mode: ready to analyze"));
    m_analysisPage->setOutputText(QStringLiteral("File selected. Press Analyze to start scanning."));
    if (m_result.nodes.isEmpty())
        refreshPages();
    m_dryRunAction->setEnabled(previousOptimizationEnabled);
    m_applyAction->setEnabled(previousReviewEnabled);
    logLine(QStringLiteral("File selected without analysis: %1").arg(selected));
}

void MainWindow::openSourceFolder()
{
    QSettings settings;
    const QString savedPath = settings.value(QStringLiteral("paths/lastSourcePath")).toString();
    QString startPath = !m_currentSourcePath.isEmpty() ? m_currentSourcePath : savedPath;
    if (!startPath.isEmpty() && !QFileInfo(startPath).isDir())
        startPath = QFileInfo(startPath).absolutePath();

    const QString selected = openFolderStyled(this, startPath);
    if (selected.isEmpty())
        return;

    const bool previousOptimizationEnabled = m_dryRunAction && m_dryRunAction->isEnabled();
    const bool previousReviewEnabled = m_applyAction && m_applyAction->isEnabled();
    m_rootFolder = selected;
    m_sourceKind = collectArchiveFiles(selected).isEmpty() ? SourceKind::Folder : SourceKind::ArchiveFolder;
    setCurrentSourcePath(selected);
    settings.setValue(QStringLiteral("paths/lastSourcePath"), selected);
    m_analysisPage->setFolderPath(selected);
    m_analysisPage->setModeLabel(QStringLiteral("Mode: ready to analyze"));
    m_analysisPage->setOutputText(QStringLiteral("Folder selected. Press Analyze to start scanning."));
    if (m_result.nodes.isEmpty())
        refreshPages();
    m_dryRunAction->setEnabled(previousOptimizationEnabled);
    m_applyAction->setEnabled(previousReviewEnabled);
    logLine(QStringLiteral("Folder selected without analysis: %1").arg(selected));
}

void MainWindow::analyzeFolder()
{
    const QString path = m_pathEdit ? m_pathEdit->text().trimmed() : QString();
    if (path.isEmpty() && m_currentSourcePath.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("Analyze"), QStringLiteral("Select a file or folder first."));
        return;
    }

    const QString effectivePath = path.isEmpty() ? m_currentSourcePath : path;
    if (!loadPathAndAnalyze(effectivePath))
    {
        return;
    }
}

bool MainWindow::loadPathAndAnalyze(const QString &path)
{
    QFileInfo info(path);
    const bool previousOptimizationEnabled = m_dryRunAction && m_dryRunAction->isEnabled();
    const bool previousReviewEnabled = m_applyAction && m_applyAction->isEnabled();
    m_dryRunAction->setEnabled(false);
    m_applyAction->setEnabled(false);
    if (!info.exists())
    {
        QMessageBox::warning(this, QStringLiteral("Load"), QStringLiteral("Path does not exist: %1").arg(path));
        logLine(QStringLiteral("Path does not exist: %1").arg(path));
        return false;
    }

    const AnalysisResult previousResult = m_result;
    QString errorMessage;
    bool ok = false;
    AnalysisProgressDialog progress(this);
    m_activeProgressDialog = &progress;
    progress.setProgress(8,
                         QStringLiteral("Preparing analysis"),
                         QFileInfo(path).fileName());
    progress.show();
    QApplication::processEvents();
    progress.setProgress(22,
                         info.isDir() ? QStringLiteral("Scanning folder") : QStringLiteral("Opening data source"),
                         path);
    QApplication::processEvents();
    if (info.isDir())
    {
        const bool hasArchives = !collectArchiveFiles(path).isEmpty();
        m_sourceKind = hasArchives ? SourceKind::ArchiveFolder : SourceKind::Folder;
        ok = hasArchives ? analyzeArchiveFolderPath(path, &errorMessage)
                         : analyzeFolderPath(path, &errorMessage);
    }
    else if (info.suffix().compare(QStringLiteral("xml"), Qt::CaseInsensitive) == 0)
    {
        m_sourceKind = SourceKind::XmlFile;
        ok = analyzeXmlFile(path, &errorMessage);
    }
    else if (isSupportedSc2Archive(info))
    {
        m_sourceKind = SourceKind::ArchiveFile;
        ok = analyzeArchiveFile(path, &errorMessage);
    }
    else
    {
        m_sourceKind = SourceKind::Unknown;
        errorMessage = QStringLiteral("Unsupported path type: %1").arg(path);
        ok = false;
    }
    if (progress.isCancelled())
    {
        ok = false;
        errorMessage = QStringLiteral("Analysis canceled.");
    }
    progress.setProgress(ok ? 88 : 100,
                         ok ? QStringLiteral("Building object registry") : QStringLiteral("Analysis failed"),
                         ok ? QStringLiteral("Preparing tables, references and reports") : errorMessage);
    QApplication::processEvents();

    if (!ok)
    {
        m_result = previousResult;
        m_dryRunAction->setEnabled(previousOptimizationEnabled);
        m_applyAction->setEnabled(previousReviewEnabled);
        m_activeProgressDialog = nullptr;
        progress.close();
        if (errorMessage == QStringLiteral("Analysis canceled."))
        {
            statusBar()->showMessage(QStringLiteral("Analysis canceled. No partial result was applied."), 8000);
            logLine(QStringLiteral("Analysis canceled by user."));
        }
        else
        {
            QMessageBox::critical(this, QStringLiteral("Analysis failed"), errorMessage);
            logLine(QStringLiteral("Analysis failed: %1").arg(errorMessage));
        }
        return false;
    }

    m_currentSourcePath = path;
    m_rootFolder = info.isDir() ? path : info.absolutePath();
    QSettings settings;
    settings.setValue(QStringLiteral("paths/lastSourcePath"), path);
    progress.setProgress(90,
                         QStringLiteral("Refreshing analysis"),
                         QStringLiteral("Updating object tables"));
    QApplication::processEvents();
    m_analysisPage->setFolderPath(path);
    m_analysisPage->setModeLabel(modeLabelFor(static_cast<int>(m_sourceKind)));
    m_analysisPage->setAnalysisResult(m_result);
    progress.setProgress(94,
                         QStringLiteral("Refreshing analysis"),
                         QStringLiteral("Updating pages and recommendations"));
    QApplication::processEvents();
    refreshPages();
    progress.setProgress(98,
                         QStringLiteral("Writing report"),
                         QStringLiteral("Saving latest analysis summary"));
    QApplication::processEvents();
    writeAnalysisReportFile();
    progress.setProgress(100,
                         QStringLiteral("Analysis complete"),
                         QStringLiteral("%1 XML files | %2 objects")
                             .arg(m_result.totalXmlFiles())
                             .arg(m_result.totalDataNodes()));
    QApplication::processEvents();
    progress.close();
    m_activeProgressDialog = nullptr;
    showAnalysisTab();
    m_dryRunAction->setEnabled(true);
    m_applyAction->setEnabled(false);
    setCurrentSourcePath(path);
    logLine(QStringLiteral("Scanned files: %1").arg(m_result.totalFilesScanned()));
    logLine(QStringLiteral("XML files: %1").arg(m_result.totalXmlFiles()));
    logLine(QStringLiteral("Data nodes: %1").arg(m_result.totalDataNodes()));
    logLine(QStringLiteral("Duplicate IDs: %1").arg(m_result.duplicateIdGroups.size()));
    logLine(QStringLiteral("Duplicate content groups: %1").arg(m_result.duplicateContentGroups.size()));
    logLine(QStringLiteral("Parse errors: %1").arg(m_result.parseErrors.size()));
    for (const ParseErrorInfo &error : m_result.parseErrors)
    {
        logLine(QStringLiteral("Parse error: %1 -> %2").arg(error.filePath, error.message));
    }
    for (const DuplicateIdGroup &group : m_result.duplicateIdGroups)
    {
        logLine(QStringLiteral("Duplicate ID group: %1 (%2 nodes)").arg(group.id).arg(group.nodeIndices.size()));
    }
    for (const DuplicateContentGroup &group : m_result.duplicateContentGroups)
    {
        logLine(QStringLiteral("Duplicate content group: %1 (%2 nodes)").arg(group.contentHash.left(12)).arg(group.nodeIndices.size()));
    }
    if (!m_optimizationDialog)
        showDryRunTab(true);
    return true;
}

bool MainWindow::analyzeFolderPath(const QString &folderPath, QString *errorMessage)
{
    m_archiveReferencedIds.clear();
    m_archiveStrongReferenceSources.clear();
    m_archiveWeakReferenceSources.clear();
    m_archiveReferenceScanComplete = false;
    return m_analyzer.analyzeFolder(folderPath, m_whitelistIds, &m_result, errorMessage, [this](int current, int total, const QString &file)
                                    {
            if (!m_activeProgressDialog) return;
            const int percent = total > 0 ? 22 + (current * 62 / total) : 22;
            m_activeProgressDialog->setProgress(percent, QStringLiteral("Scanning XML and data files"),
                                                file.isEmpty() ? QStringLiteral("Finalizing scan") : QDir::toNativeSeparators(file));
            QApplication::processEvents(); }, [this]
                                    { return m_activeProgressDialog && m_activeProgressDialog->isCancelled(); });
}

bool MainWindow::analyzeArchiveFolderPath(const QString &folderPath, QString *errorMessage)
{
    m_archiveReferencedIds.clear();
    m_archiveStrongReferenceSources.clear();
    m_archiveWeakReferenceSources.clear();
    m_archiveReferenceScanComplete = false;

    const QStringList archives = collectArchiveFiles(folderPath);
    if (archives.isEmpty())
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("No SC2 map/mod archives found in folder: %1").arg(folderPath);
        return false;
    }

    m_result = AnalysisResult{};
    m_result.rootFolder = folderPath;
    XmlLoader loader;
    QHash<QString, QStringList> entriesByArchive;
    int xmlEntriesFound = 0;
    int xmlEntriesLoaded = 0;

    for (int archiveIndex = 0; archiveIndex < archives.size(); ++archiveIndex)
    {
        const QString &archivePath = archives[archiveIndex];
        const QString archiveRelative = QDir(folderPath).relativeFilePath(archivePath).replace('\\', '/');
        if (m_activeProgressDialog)
        {
            if (m_activeProgressDialog->isCancelled())
            {
                if (errorMessage)
                    *errorMessage = QStringLiteral("Analysis canceled.");
                return false;
            }
            m_activeProgressDialog->setProgress(22 + (archiveIndex * 35 / qMax(1, archives.size())),
                                                QStringLiteral("Opening SC2 archives"), archiveRelative);
            QApplication::processEvents();
        }

        Sc2Archive archive;
        QString archiveError;
        if (!archive.load(archivePath, &archiveError))
        {
            if (errorMessage)
                *errorMessage = QStringLiteral("%1: %2").arg(archivePath, archiveError);
            return false;
        }

        const QStringList entries = archive.allEntries();
        entriesByArchive.insert(archivePath, entries);
        logLine(QStringLiteral("Archive folder entry count: %1 -> %2").arg(archiveRelative).arg(entries.size()));

        QStringList xmlEntries;
        for (const QString &entry : entries)
            if (entry.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive))
                xmlEntries.append(entry);
        xmlEntriesFound += xmlEntries.size();

        for (int entryIndex = 0; entryIndex < xmlEntries.size(); ++entryIndex)
        {
            const QString &entryName = xmlEntries[entryIndex];
            if (m_activeProgressDialog)
            {
                if (m_activeProgressDialog->isCancelled())
                {
                    if (errorMessage)
                        *errorMessage = QStringLiteral("Analysis canceled.");
                    return false;
                }
                m_activeProgressDialog->setProgress(30 + (archiveIndex * 40 / qMax(1, archives.size())),
                                                    QStringLiteral("Extracting archive XML"),
                                                    QStringLiteral("%1::%2").arg(archiveRelative, entryName));
                QApplication::processEvents();
            }

            QByteArray xmlBytes;
            QString readError;
            const QString sourceName = QStringLiteral("%1::%2").arg(archiveRelative, entryName);
            if (!archive.readEntry(entryName, &xmlBytes, &readError))
            {
                ParseErrorInfo error;
                error.filePath = sourceName;
                error.message = readError;
                m_result.parseErrors.append(error);
                continue;
            }

            ScannedFileInfo scanned;
            scanned.filePath = sourceName;
            scanned.isXml = true;
            scanned.isSc2DataLike = true;
            scanned.size = xmlBytes.size();
            m_result.scannedFiles.append(scanned);
            m_result.sourceXmlByFile.insert(sourceName, QString::fromUtf8(xmlBytes));

            QVector<DataNode> fileNodes;
            QString parseError;
            if (!loader.extractNodes(sourceName, xmlBytes, &fileNodes, &parseError))
            {
                ParseErrorInfo error;
                error.filePath = sourceName;
                error.message = parseError;
                m_result.parseErrors.append(error);
                continue;
            }
            m_result.nodes += fileNodes;
            ++xmlEntriesLoaded;
        }
    }

    logLine(QStringLiteral("Archive folder scan: archives=%1, XML entries=%2, XML loaded=%3")
                .arg(archives.size())
                .arg(xmlEntriesFound)
                .arg(xmlEntriesLoaded));
    if (m_result.sourceXmlByFile.isEmpty())
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("No XML files found inside SC2 archives in folder: %1").arg(folderPath);
        return false;
    }

    if (!m_analyzer.finalizeAnalysisResult(&m_result, m_whitelistIds, errorMessage, [this]
                                           {
            if (!m_activeProgressDialog)
                return;
            m_activeProgressDialog->setProgress(82,
                                                QStringLiteral("Analyzing extracted XML"),
                                                QStringLiteral("Building references, duplicate groups and candidates"));
            QApplication::processEvents(); }, [this]
                                           { return m_activeProgressDialog && m_activeProgressDialog->isCancelled(); }))
    {
        return false;
    }

    QSet<QString> knownIds;
    for (const DataNode &node : m_result.nodes)
        if (!node.id.isEmpty())
            knownIds.insert(node.id);

    m_archiveReferenceScanComplete = true;
    int scannedReferenceEntries = 0;
    int strongReferenceEntries = 0;
    int weakReferenceEntries = 0;
    for (int archiveIndex = 0; archiveIndex < archives.size(); ++archiveIndex)
    {
        const QString &archivePath = archives[archiveIndex];
        const QString archiveRelative = QDir(folderPath).relativeFilePath(archivePath).replace('\\', '/');
        Sc2Archive archive;
        QString archiveError;
        if (!archive.load(archivePath, &archiveError))
        {
            m_archiveReferenceScanComplete = false;
            logLine(QStringLiteral("Archive reference scan failed to reopen %1: %2").arg(archiveRelative, archiveError));
            continue;
        }

        const QStringList entries = entriesByArchive.value(archivePath, archive.allEntries());
        for (const QString &entry : entries)
        {
            const ArchiveReferenceStrength strength = archiveReferenceStrength(entry);
            if (entry.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive) || strength == ArchiveReferenceStrength::None)
                continue;
            if (m_activeProgressDialog)
            {
                if (m_activeProgressDialog->isCancelled())
                {
                    if (errorMessage)
                        *errorMessage = QStringLiteral("Analysis canceled.");
                    return false;
                }
                m_activeProgressDialog->setProgress(86,
                                                    QStringLiteral("Checking archive references"),
                                                    QStringLiteral("%1::%2").arg(archiveRelative, entry));
                QApplication::processEvents();
            }

            QByteArray bytes;
            QString readError;
            if (!archive.readEntry(entry, &bytes, &readError))
            {
                if (strength == ArchiveReferenceStrength::Strong)
                    m_archiveReferenceScanComplete = false;
                logLine(QStringLiteral("Archive reference scan failed for %1::%2: %3").arg(archiveRelative, entry, readError));
                continue;
            }
            const QString sourceLabel = QStringLiteral("%1::%2 [%3]")
                                            .arg(archiveRelative,
                                                 entry,
                                                 strength == ArchiveReferenceStrength::Strong
                                                     ? QStringLiteral("map/trigger/script")
                                                     : QStringLiteral("metadata/text"));
            if (strength == ArchiveReferenceStrength::Strong)
            {
                collectKnownIdTokenSources(bytes, knownIds, sourceLabel, &m_archiveStrongReferenceSources);
                ++strongReferenceEntries;
            }
            else
            {
                collectKnownIdTokenSources(bytes, knownIds, sourceLabel, &m_archiveWeakReferenceSources);
                ++weakReferenceEntries;
            }
            ++scannedReferenceEntries;
        }
    }

    for (auto it = m_archiveStrongReferenceSources.begin(); it != m_archiveStrongReferenceSources.end(); ++it)
    {
        it.value().removeDuplicates();
        m_archiveReferencedIds.insert(it.key());
    }
    for (auto it = m_archiveWeakReferenceSources.begin(); it != m_archiveWeakReferenceSources.end(); ++it)
        it.value().removeDuplicates();
    logLine(QStringLiteral("Archive folder reference-bearing entries scanned: %1; strong entries: %2; weak entries: %3; strong IDs: %4; weak IDs: %5")
                .arg(scannedReferenceEntries)
                .arg(strongReferenceEntries)
                .arg(weakReferenceEntries)
                .arg(m_archiveStrongReferenceSources.size())
                .arg(m_archiveWeakReferenceSources.size()));

    normalizeArchiveAnalysis(&m_result, QString(), folderPath);
    return true;
}

bool MainWindow::analyzeXmlFile(const QString &filePath, QString *errorMessage)
{
    m_archiveReferencedIds.clear();
    m_archiveStrongReferenceSources.clear();
    m_archiveWeakReferenceSources.clear();
    m_archiveReferenceScanComplete = false;
    if (m_activeProgressDialog && m_activeProgressDialog->isCancelled())
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("Analysis canceled.");
        return false;
    }
    m_result = AnalysisResult{};
    m_result.rootFolder = filePath;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Unable to open XML file: %1").arg(filePath);
        }
        return false;
    }

    const QByteArray xmlBytes = file.readAll();
    file.close();

    ScannedFileInfo scanned;
    scanned.filePath = filePath;
    scanned.isXml = true;
    scanned.isSc2DataLike = true;
    scanned.size = QFileInfo(filePath).size();
    m_result.scannedFiles.append(scanned);
    m_result.sourceXmlByFile.insert(filePath, QString::fromUtf8(xmlBytes));

    XmlLoader loader;
    QVector<DataNode> nodes;
    QString parseError;
    if (!loader.extractNodes(filePath, xmlBytes, &nodes, &parseError))
    {
        ParseErrorInfo error;
        error.filePath = filePath;
        error.message = parseError;
        m_result.parseErrors.append(error);
        if (errorMessage)
        {
            *errorMessage = parseError;
        }
        return false;
    }

    m_result.nodes = nodes;
    m_analyzer.populateReferenceIds(&m_result);
    m_result.analysisReportText = m_analyzer.buildAnalysisReport(m_result);
    m_result.plannedChangesReportText = m_analyzer.buildDryRunReport(m_result, QVector<int>{});
    return true;
}

bool MainWindow::analyzeArchiveFile(const QString &filePath, QString *errorMessage)
{
    m_archiveReferencedIds.clear();
    m_archiveStrongReferenceSources.clear();
    m_archiveWeakReferenceSources.clear();
    m_archiveReferenceScanComplete = false;
    Sc2Archive archive;
    QString archiveError;
    if (!archive.load(filePath, &archiveError))
    {
        if (errorMessage)
        {
            *errorMessage = archiveError.isEmpty()
                                ? QStringLiteral("Archive open failed. Check libzip/MPQ support.")
                                : archiveError;
        }
        return false;
    }

    QStringList xmlEntries;
    for (const QString &entry : archive.allEntries())
        if (entry.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive))
            xmlEntries.append(entry);
    logLine(QStringLiteral("Archive entries count: %1").arg(archive.totalEntriesCount()));
    logLine(QStringLiteral("Matched XML count: %1").arg(xmlEntries.size()));
    if (xmlEntries.isEmpty())
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("No XML files found inside archive");
        }
        return false;
    }

    m_result = AnalysisResult{};
    m_result.rootFolder = filePath;
    XmlLoader loader;

    for (int entryIndex = 0; entryIndex < xmlEntries.size(); ++entryIndex)
    {
        const QString &entryName = xmlEntries[entryIndex];
        if (m_activeProgressDialog)
        {
            if (m_activeProgressDialog->isCancelled())
            {
                if (errorMessage)
                    *errorMessage = QStringLiteral("Analysis canceled.");
                return false;
            }
            m_activeProgressDialog->setProgress(22 + (entryIndex * 35 / qMax(1, xmlEntries.size())),
                                                QStringLiteral("Extracting archive XML"), entryName);
            QApplication::processEvents();
        }
        QByteArray xmlBytes;
        QString readError;
        if (!archive.readEntry(entryName, &xmlBytes, &readError))
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("%1: %2").arg(entryName, readError);
            }
            return false;
        }

        ScannedFileInfo scanned;
        scanned.filePath = entryName;
        scanned.isXml = true;
        scanned.isSc2DataLike = true;
        scanned.size = xmlBytes.size();
        m_result.scannedFiles.append(scanned);
        m_result.sourceXmlByFile.insert(entryName, QString::fromUtf8(xmlBytes));

        QVector<DataNode> fileNodes;
        QString parseError;
        if (!loader.extractNodes(entryName, xmlBytes, &fileNodes, &parseError))
        {
            ParseErrorInfo error;
            error.filePath = entryName;
            error.message = parseError;
            m_result.parseErrors.append(error);
            continue;
        }
        m_result.nodes += fileNodes;
    }

    if (!m_analyzer.finalizeAnalysisResult(&m_result, m_whitelistIds, errorMessage, [this]
                                           {
            if (!m_activeProgressDialog)
                return;
            m_activeProgressDialog->setProgress(85,
                                                QStringLiteral("Analyzing extracted XML"),
                                                QStringLiteral("Building references, duplicate groups and candidates"));
            QApplication::processEvents(); }, [this]
                                           { return m_activeProgressDialog && m_activeProgressDialog->isCancelled(); }))
    {
        return false;
    }

    QSet<QString> knownIds;
    for (const DataNode &node : m_result.nodes)
        if (!node.id.isEmpty())
            knownIds.insert(node.id);
    m_archiveReferenceScanComplete = true;
    const QStringList archiveEntries = archive.allEntries();
    int scannedReferenceEntries = 0;
    int strongReferenceEntries = 0;
    int weakReferenceEntries = 0;
    for (int index = 0; index < archiveEntries.size(); ++index)
    {
        const QString &entry = archiveEntries[index];
        const ArchiveReferenceStrength strength = archiveReferenceStrength(entry);
        if (entry.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive) || strength == ArchiveReferenceStrength::None)
            continue;
        if (m_activeProgressDialog)
        {
            if (m_activeProgressDialog->isCancelled())
            {
                if (errorMessage)
                    *errorMessage = QStringLiteral("Analysis canceled.");
                return false;
            }
            m_activeProgressDialog->setProgress(86, QStringLiteral("Checking archive references"), entry);
            QApplication::processEvents();
        }
        QByteArray bytes;
        QString readError;
        if (!archive.readEntry(entry, &bytes, &readError))
        {
            if (strength == ArchiveReferenceStrength::Strong)
                m_archiveReferenceScanComplete = false;
            logLine(QStringLiteral("Archive reference scan failed for %1: %2").arg(entry, readError));
            continue;
        }
        const QString sourceLabel = QStringLiteral("%1 [%2]")
                                        .arg(entry,
                                             strength == ArchiveReferenceStrength::Strong
                                                 ? QStringLiteral("map/trigger/script")
                                                 : QStringLiteral("metadata/text"));
        if (strength == ArchiveReferenceStrength::Strong)
        {
            collectKnownIdTokenSources(bytes, knownIds, sourceLabel, &m_archiveStrongReferenceSources);
            ++strongReferenceEntries;
        }
        else
        {
            collectKnownIdTokenSources(bytes, knownIds, sourceLabel, &m_archiveWeakReferenceSources);
            ++weakReferenceEntries;
        }
        ++scannedReferenceEntries;
    }
    for (auto it = m_archiveStrongReferenceSources.begin(); it != m_archiveStrongReferenceSources.end(); ++it)
    {
        it.value().removeDuplicates();
        m_archiveReferencedIds.insert(it.key());
    }
    for (auto it = m_archiveWeakReferenceSources.begin(); it != m_archiveWeakReferenceSources.end(); ++it)
        it.value().removeDuplicates();
    logLine(QStringLiteral("Archive reference-bearing entries scanned: %1; strong entries: %2; weak entries: %3; strong IDs: %4; weak IDs: %5")
                .arg(scannedReferenceEntries)
                .arg(strongReferenceEntries)
                .arg(weakReferenceEntries)
                .arg(m_archiveStrongReferenceSources.size())
                .arg(m_archiveWeakReferenceSources.size()));

    normalizeArchiveAnalysis(&m_result, QString(), filePath);
    return true;
}

void MainWindow::normalizeArchiveAnalysis(AnalysisResult *analysis, const QString &tempRoot,
                                          const QString &archivePath) const
{
    if (!analysis)
        return;
    if (!tempRoot.isEmpty())
    {
        QHash<QString, QString> relativeXml;
        for (auto it = analysis->sourceXmlByFile.cbegin(); it != analysis->sourceXmlByFile.cend(); ++it)
            relativeXml.insert(QDir(tempRoot).relativeFilePath(it.key()), it.value());
        analysis->sourceXmlByFile = relativeXml;
        for (ScannedFileInfo &file : analysis->scannedFiles)
            file.filePath = QDir(tempRoot).relativeFilePath(file.filePath);
        for (DataNode &node : analysis->nodes)
            node.sourceFile = QDir(tempRoot).relativeFilePath(node.sourceFile);
    }
    analysis->rootFolder = archivePath;
    applyArchiveReferenceSafety(analysis);
    analysis->analysisReportText = m_analyzer.buildAnalysisReport(*analysis);
    analysis->plannedChangesReportText = m_analyzer.buildDryRunReport(*analysis, QVector<int>{});
}

void MainWindow::applyArchiveReferenceSafety(AnalysisResult *analysis) const
{
    if (!analysis)
        return;
    analysis->possibleUnusedNodeIndices.clear();
    for (UnusedCandidateInfo &candidate : analysis->unusedCandidates)
    {
        if (candidate.state != CandidateState::Safe)
            continue;
        QString id;
        if (candidate.nodeIndex >= 0 && candidate.nodeIndex < analysis->nodes.size())
            id = analysis->nodes[candidate.nodeIndex].id;
        const QStringList strongSources = m_archiveStrongReferenceSources.value(id);
        const QStringList weakSources = m_archiveWeakReferenceSources.value(id);
        if (!strongSources.isEmpty())
            candidate.externalReferenceSources.append(strongSources);
        if (!weakSources.isEmpty())
            candidate.externalReferenceSources.append(weakSources);
        candidate.externalReferenceSources.removeDuplicates();

        if (!m_archiveReferenceScanComplete || !strongSources.isEmpty())
        {
            candidate.state = CandidateState::Blocked;
            candidate.usageState = UsageState::Blocked;
            candidate.protectedObject = true;
            candidate.reason = !m_archiveReferenceScanComplete
                                   ? QStringLiteral("Archive reference scan was incomplete")
                                   : QStringLiteral("Referenced by archive placement, trigger, or script data: %1")
                                         .arg(strongSources.mid(0, 6).join(QStringLiteral(", ")));
            candidate.riskLevel = QStringLiteral("high");
            if (candidate.nodeIndex >= 0 && candidate.nodeIndex < analysis->nodes.size())
                analysis->nodes[candidate.nodeIndex].candidateUnused = false;
        }
        else
        {
            if (!weakSources.isEmpty())
            {
                candidate.reason += QStringLiteral("; weak archive metadata/text token: %1")
                                        .arg(weakSources.mid(0, 6).join(QStringLiteral(", ")));
                if (candidate.riskLevel == QStringLiteral("low"))
                    candidate.riskLevel = QStringLiteral("medium");
            }
            analysis->possibleUnusedNodeIndices.append(candidate.nodeIndex);
        }
    }
}

bool MainWindow::materializeArchiveAnalysis(const QString &tempRoot, AnalysisResult *analysis, QString *errorMessage) const
{
    if (!analysis || m_sourceKind != SourceKind::ArchiveFile)
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("Archive analysis is not available.");
        return false;
    }
    *analysis = m_result;
    QHash<QString, QString> absoluteSources;
    for (auto it = m_result.sourceXmlByFile.cbegin(); it != m_result.sourceXmlByFile.cend(); ++it)
    {
        const QString relative = QDir::cleanPath(it.key()).replace('\\', '/');
        if (relative.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(relative))
        {
            if (errorMessage)
                *errorMessage = QStringLiteral("Unsafe archive entry path: %1").arg(it.key());
            return false;
        }
        const QString target = QDir(tempRoot).absoluteFilePath(relative);
        QDir().mkpath(QFileInfo(target).absolutePath());
        QSaveFile file(target);
        const QByteArray bytes = it.value().toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        {
            if (errorMessage)
                *errorMessage = QStringLiteral("Unable to materialize archive XML: %1").arg(relative);
            return false;
        }
        absoluteSources.insert(target, it.value());
    }
    Sc2Archive archive;
    QString archiveError;
    if (archive.load(m_currentSourcePath, &archiveError))
    {
        QByteArray listfileBytes;
        if (!archive.readEntry(QStringLiteral("(listfile)"), &listfileBytes, &archiveError))
        {
            QStringList entries;
            for (QString entry : archive.allEntries())
                entries << entry.replace('/', '\\');
            listfileBytes = entries.join(QStringLiteral("\r\n")).toUtf8() + QByteArrayLiteral("\r\n");
        }
        QSaveFile listfile(QDir(tempRoot).absoluteFilePath(QStringLiteral("(listfile)")));
        if (!listfile.open(QIODevice::WriteOnly) || listfile.write(listfileBytes) != listfileBytes.size() || !listfile.commit())
        {
            if (errorMessage)
                *errorMessage = QStringLiteral("Unable to materialize archive (listfile).");
            return false;
        }
    }
    for (ScannedFileInfo &file : analysis->scannedFiles)
        file.filePath = QDir(tempRoot).absoluteFilePath(QDir::cleanPath(file.filePath));
    for (DataNode &node : analysis->nodes)
        node.sourceFile = QDir(tempRoot).absoluteFilePath(QDir::cleanPath(node.sourceFile));
    for (DeepCleanupCandidate &candidate : analysis->deepCleanupCandidates)
        candidate.filePath = QDir(tempRoot).absoluteFilePath(QDir::cleanPath(candidate.filePath));
    analysis->sourceXmlByFile = absoluteSources;
    analysis->rootFolder = tempRoot;
    return true;
}

bool MainWindow::commitArchiveChanges(const QString &tempRoot, const QStringList &changedFiles,
                                      QString *backupPath, QString *errorMessage,
                                      const QStringList &removedFiles) const
{
    if (changedFiles.isEmpty() && removedFiles.isEmpty())
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("No archive entries changed.");
        return false;
    }
    Sc2Archive archive;
    if (!archive.load(m_currentSourcePath, errorMessage))
        return false;
    QHash<QString, QByteArray> replacements;
    for (const QString &relativeFile : changedFiles)
    {
        QString normalized = QDir::cleanPath(relativeFile).replace('\\', '/');
        QString archiveName;
        for (const QString &entry : archive.allEntries())
        {
            if (QDir::cleanPath(entry).replace('\\', '/').compare(normalized, Qt::CaseInsensitive) == 0)
            {
                archiveName = entry;
                break;
            }
        }
        if (archiveName.isEmpty())
            archiveName = normalized.replace('/', '\\');
        QFile file(QDir(tempRoot).absoluteFilePath(relativeFile));
        if (!file.open(QIODevice::ReadOnly))
        {
            if (errorMessage)
                *errorMessage = QStringLiteral("Unable to read changed XML: %1").arg(relativeFile);
            return false;
        }
        QByteArray replacementBytes = file.readAll();
        if (normalized.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive))
        {
            QByteArray originalBytes;
            QString readError;
            if (archive.readEntry(archiveName, &originalBytes, &readError))
            {
                DataCollectionPreservationReport preservationReport;
                if (!restoreMissingDataCollectionRecords(originalBytes, &replacementBytes, &preservationReport, errorMessage))
                    return false;
            }
        }
        replacements.insert(archiveName, replacementBytes);
    }
    QStringList removedEntries;
    for (const QString &relativeFile : removedFiles)
    {
        QString normalized = QDir::cleanPath(relativeFile).replace('\\', '/');
        QString archiveName;
        for (const QString &entry : archive.allEntries())
        {
            if (QDir::cleanPath(entry).replace('\\', '/').compare(normalized, Qt::CaseInsensitive) == 0)
            {
                archiveName = entry;
                break;
            }
        }
        if (archiveName.isEmpty())
            archiveName = normalized.replace('/', '\\');
        removedEntries.append(archiveName);
    }
    removedEntries.removeDuplicates();

    BackupManager backupManager;
    QString backup;
    if (!backupManager.createBackup(m_currentSourcePath, &backup, errorMessage))
        return false;
    const QString pending = m_currentSourcePath + QStringLiteral(".sc2dh.pending");
    QFile::remove(pending);
    if (!archive.saveCopy(pending, replacements, removedEntries, errorMessage))
        return false;

    QFile pendingFile(pending);
    if (!pendingFile.open(QIODevice::ReadOnly))
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("Unable to read verified archive copy.");
        QFile::remove(pending);
        return false;
    }
    QSaveFile destination(m_currentSourcePath);
    const QByteArray archiveBytes = pendingFile.readAll();
    if (!destination.open(QIODevice::WriteOnly) || destination.write(archiveBytes) != archiveBytes.size() || !destination.commit())
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("Unable to atomically replace the archive; original file was preserved.");
        QFile::remove(pending);
        return false;
    }
    QFile::remove(pending);

    // saveCopy already verifies every rewritten entry before returning. The
    // final QSaveFile write is a byte-for-byte atomic copy of that verified
    // archive, so reopening and extracting every entry here was redundant.
    if (backupPath)
        *backupPath = backup;
    return true;
}

void MainWindow::runDryRun()
{
    if (m_result.nodes.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("Optimization"), QStringLiteral("Analyze a folder first."));
        return;
    }

    showDryRunTab();
}

void MainWindow::applySelectedChanges()
{
    if (m_result.nodes.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("Apply"), QStringLiteral("Analyze a folder first."));
        return;
    }

    if (m_sourceKind == SourceKind::ArchiveFile || m_sourceKind == SourceKind::ArchiveFolder)
    {
        QMessageBox::information(this, QStringLiteral("Apply"),
                                 m_sourceKind == SourceKind::ArchiveFolder
                                     ? archiveFolderReadOnlyMessage()
                                     : QStringLiteral("Archive apply is not available in this build. Use folder or XML input."));
        return;
    }

    const QVector<int> selectedRows = m_analysisPage->selectedRows();
    if (selectedRows.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("Apply"), QStringLiteral("No rows selected."));
        return;
    }

    const QString planned = m_analyzer.buildPlannedChangesReport(m_result, selectedRows);
    m_dryRunPage->setPreview(planned);
    showDryRunTab();

    const QString backupHint = backupPrompt(
        QStringLiteral("This will create a backup folder before editing. Continue?"),
        QStringLiteral("Backups are disabled in Settings. Apply selected changes without creating a persistent backup?"));
    if (QMessageBox::question(this, QStringLiteral("Apply Selected Changes"), backupHint) != QMessageBox::Yes)
    {
        logLine(QStringLiteral("Apply canceled by user."));
        return;
    }

    QString backupFolder;
    QString errorMessage;
    QStringList changedFiles;
    int removedNodes = 0;
    int skippedNodes = 0;
    if (!m_analyzer.applySelectedChanges(m_result,
                                         selectedRows,
                                         m_rootFolder,
                                         m_whitelistIds,
                                         &backupFolder,
                                         &errorMessage,
                                         &changedFiles,
                                         &removedNodes,
                                         &skippedNodes))
    {
        QMessageBox::critical(this, QStringLiteral("Apply failed"), errorMessage);
        logLine(QStringLiteral("Apply failed: %1").arg(errorMessage));
        return;
    }

    logLine(QStringLiteral("Backup folder: %1").arg(backupFolder));
    logLine(QStringLiteral("Changed files count: %1").arg(changedFiles.size()));
    logLine(QStringLiteral("Removed nodes count: %1").arg(removedNodes));
    logLine(QStringLiteral("Skipped nodes count: %1").arg(skippedNodes));
    for (int row : selectedRows)
    {
        if (row >= 0 && row < m_result.nodes.size())
        {
            const DataNode &node = m_result.nodes[row];
            if (m_whitelistIds.contains(node.id))
            {
                continue;
            }
            logLine(QStringLiteral("Removed node: %1 | %2 | %3 | %4")
                        .arg(node.sourceFile, node.elementName, node.id, node.originalLocation));
        }
    }
    for (const QString &file : changedFiles)
    {
        logLine(QStringLiteral("Changed file: %1").arg(file));
    }

    analyzeFolder();
    logLine(QStringLiteral("Save result: success"));
    QMessageBox::information(this, QStringLiteral("Apply complete"),
                             QStringLiteral("Backup: %1\nChanged files: %2\nRemoved nodes: %3\nSkipped nodes: %4")
                                 .arg(backupFolder)
                                 .arg(changedFiles.size())
                                 .arg(removedNodes)
                                 .arg(skippedNodes));
}

void MainWindow::previewMerge(const MergeRequest &request)
{
    MergePreview preview;
    if (m_sourceKind == SourceKind::ArchiveFolder)
    {
        m_previewedMerge = request;
        m_mergePreviewValid = false;
        m_duplicatesPage->setPreviewText(archiveFolderReadOnlyMessage(), false);
        return;
    }
    if (m_sourceKind == SourceKind::ArchiveFile)
    {
        QTemporaryDir workspace;
        AnalysisResult materialized;
        QString error;
        if (!workspace.isValid() || !materializeArchiveAnalysis(workspace.path(), &materialized, &error))
        {
            m_duplicatesPage->setPreviewText(QStringLiteral("Archive preview failed: %1").arg(error), false);
            return;
        }
        preview = m_mergeService.preview(materialized, request);
    }
    else
    {
        preview = m_mergeService.preview(m_result, request);
    }
    m_previewedMerge = request;
    m_mergePreviewValid = preview.valid;
    m_duplicatesPage->setPreviewText(preview.reportText.isEmpty() ? preview.warnings.join(QStringLiteral("\n")) : preview.reportText,
                                     preview.valid);
    if (!preview.reportText.isEmpty())
    {
        m_result.analysisReportText = m_analyzer.buildAnalysisReport(m_result) + QStringLiteral("\n") + preview.reportText;
        writeAnalysisReportFile();
    }
}

void MainWindow::applyMerge(const MergeRequest &request)
{
    if (m_sourceKind == SourceKind::ArchiveFolder)
    {
        QMessageBox::information(this, QStringLiteral("Apply Merge"), archiveFolderReadOnlyMessage());
        return;
    }
    const bool sameRequest = request.keepNodeIndex == m_previewedMerge.keepNodeIndex && request.removeNodeIndices == m_previewedMerge.removeNodeIndices;
    if (!m_mergePreviewValid || !sameRequest)
    {
        QMessageBox::warning(this, QStringLiteral("Apply Merge"),
                             QStringLiteral("Preview this exact merge selection before applying it."));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("Apply Merge"),
                              backupPrompt(
                                  QStringLiteral("Create a backup, redirect references, verify, and delete the selected duplicates?"),
                                  QStringLiteral("Backups are disabled in Settings. Redirect references, verify, and delete the selected duplicates without a persistent backup?"))) != QMessageBox::Yes)
        return;

    if (m_sourceKind == SourceKind::ArchiveFile)
    {
        QTemporaryDir workspace;
        AnalysisResult materialized;
        QString error;
        if (!workspace.isValid() || !materializeArchiveAnalysis(workspace.path(), &materialized, &error))
        {
            QMessageBox::critical(this, QStringLiteral("Merge failed"), error);
            return;
        }
        const MergeApplyResult result = m_mergeService.apply(materialized, request, workspace.path(), m_whitelistIds);
        if (!result.success)
        {
            QMessageBox::critical(this, QStringLiteral("Merge failed"), result.error + QStringLiteral("\nThe archive was not changed."));
            return;
        }
        QString archiveBackup;
        if (!commitArchiveChanges(workspace.path(), result.changedFiles, &archiveBackup, &error))
        {
            QMessageBox::critical(this, QStringLiteral("Merge failed"), error + QStringLiteral("\nNo partial archive change was retained."));
            return;
        }
        m_mergePreviewValid = false;
        if (!loadPathAndAnalyze(m_currentSourcePath))
        {
            QMessageBox::warning(this, QStringLiteral("Merge saved"), QStringLiteral("The archive was saved, but automatic re-analysis failed. Backup: %1").arg(archiveBackup));
            return;
        }
        m_dryRunPage->recordMergeResult(result.nodesDeleted, result.referencesRedirected);
        QMessageBox::information(this, QStringLiteral("Merge complete"),
                                 QStringLiteral("Archive backup: %1\nFiles changed: %2\nReferences redirected: %3\nNodes deleted: %4")
                                     .arg(archiveBackup)
                                     .arg(result.changedFiles.size())
                                     .arg(result.referencesRedirected)
                                     .arg(result.nodesDeleted));
        return;
    }
    const MergeApplyResult result = m_mergeService.apply(m_result, request, m_rootFolder, m_whitelistIds);
    if (!result.success)
    {
        QMessageBox::critical(this, QStringLiteral("Merge failed"),
                              result.error + QStringLiteral("\nNo partial merge was retained."));
        logLine(QStringLiteral("Merge failed: %1").arg(result.error));
        return;
    }
    m_mergePreviewValid = false;
    logLine(QStringLiteral("Merge backup: %1").arg(result.backupFolder));
    logLine(QStringLiteral("Merge redirected %1 references and deleted %2 nodes.")
                .arg(result.referencesRedirected)
                .arg(result.nodesDeleted));
    loadPathAndAnalyze(m_currentSourcePath);
    m_dryRunPage->recordMergeResult(result.nodesDeleted, result.referencesRedirected);
    QMessageBox::information(this, QStringLiteral("Merge complete"),
                             QStringLiteral("Backup: %1\nFiles changed: %2\nReferences redirected: %3\nNodes deleted: %4")
                                 .arg(result.backupFolder)
                                 .arg(result.changedFiles.size())
                                 .arg(result.referencesRedirected)
                                 .arg(result.nodesDeleted));
}

void MainWindow::previewUnusedDeletion(const QVector<int> &rows)
{
    m_previewedUnusedRows = rows;
    m_cleanupPage->setPreviewText(m_analyzer.buildDryRunReport(m_result, rows));
}

void MainWindow::applyUnusedDeletion(const QVector<int> &rows)
{
    if (m_sourceKind == SourceKind::ArchiveFolder)
    {
        QMessageBox::information(this, QStringLiteral("Delete Unused Objects"), archiveFolderReadOnlyMessage());
        return;
    }
    if (rows.isEmpty() || rows != m_previewedUnusedRows)
    {
        QMessageBox::warning(this, QStringLiteral("Delete Unused Objects"),
                             QStringLiteral("Preview this exact selection before deletion."));
        return;
    }
    if (m_sourceKind == SourceKind::ArchiveFile)
    {
        if (!m_archiveReferenceScanComplete)
        {
            QMessageBox::information(this, QStringLiteral("Delete Unused Objects"),
                                     QStringLiteral("Blocked: the archive reference scan was incomplete."));
            return;
        }
        if (QMessageBox::question(this, QStringLiteral("Delete Selected Unused Objects"),
                                  backupPrompt(
                                      QStringLiteral("Create an archive backup, delete the selected verified candidates, and atomically save the SC2 archive?"),
                                      QStringLiteral("Backups are disabled in Settings. Delete the selected verified candidates and atomically save the SC2 archive without a persistent backup?"))) != QMessageBox::Yes)
            return;
        QTemporaryDir workspace;
        AnalysisResult materialized;
        QString error;
        if (!workspace.isValid() || !materializeArchiveAnalysis(workspace.path(), &materialized, &error))
        {
            QMessageBox::critical(this, QStringLiteral("Deletion failed"), error);
            return;
        }
        QVector<WizardNodeRef> selectedRefs;
        for (int row : rows)
        {
            if (row < 0 || row >= m_result.nodes.size())
                continue;
            const DataNode &node = m_result.nodes[row];
            selectedRefs.append({node.id, node.elementName, node.sourceFile, node.originalLocation});
        }
        if (!m_analyzer.analyzeFolder(workspace.path(), m_whitelistIds, &materialized, &error))
        {
            QMessageBox::critical(this, QStringLiteral("Deletion failed"), error);
            return;
        }
        applyArchiveReferenceSafety(&materialized);
        QVector<int> refreshedRows;
        for (const WizardNodeRef &ref : selectedRefs)
        {
            const int index = findNodeIndex(materialized, ref);
            if (index >= 0)
                refreshedRows.append(index);
        }
        QString workspaceBackup;
        QStringList changedFiles;
        int removed = 0, skipped = 0;
        if (!m_analyzer.applySelectedChanges(materialized, refreshedRows, workspace.path(), m_whitelistIds,
                                             &workspaceBackup, &error, &changedFiles, &removed, &skipped))
        {
            QMessageBox::critical(this, QStringLiteral("Deletion failed"), error);
            return;
        }
        QString archiveBackup;
        if (!commitArchiveChanges(workspace.path(), changedFiles, &archiveBackup, &error))
        {
            QMessageBox::critical(this, QStringLiteral("Deletion failed"), error + QStringLiteral("\nThe original archive was preserved."));
            return;
        }
        m_previewedUnusedRows.clear();
        loadPathAndAnalyze(m_currentSourcePath);
        m_dryRunPage->recordUnusedResult(removed);
        QMessageBox::information(this, QStringLiteral("Deletion complete"),
                                 QStringLiteral("Archive backup: %1\nDeleted: %2\nSkipped: %3")
                                     .arg(archiveBackup)
                                     .arg(removed)
                                     .arg(skipped));
        return;
    }
    QString backupFolder, error;
    QStringList changedFiles;
    int removed = 0, skipped = 0;
    if (QMessageBox::question(this, QStringLiteral("Delete Selected Unused Objects"),
                              backupPrompt(
                                  QStringLiteral("A backup will be created before deleting the selected safe candidates. Continue?"),
                                  QStringLiteral("Backups are disabled in Settings. Delete the selected safe candidates without a persistent backup?"))) != QMessageBox::Yes)
        return;
    if (!m_analyzer.applySelectedChanges(m_result, rows, m_rootFolder, m_whitelistIds,
                                         &backupFolder, &error, &changedFiles, &removed, &skipped))
    {
        QMessageBox::critical(this, QStringLiteral("Deletion failed"), error);
        return;
    }
    m_previewedUnusedRows.clear();
    loadPathAndAnalyze(m_currentSourcePath);
    m_dryRunPage->recordUnusedResult(removed);
    QMessageBox::information(this, QStringLiteral("Deletion complete"),
                             QStringLiteral("Backup: %1\nDeleted: %2\nSkipped: %3")
                                 .arg(backupFolder)
                                 .arg(removed)
                                 .arg(skipped));
}

void MainWindow::previewStandardRename(const RenamePlan &plan)
{
    RenamePreviewReport report;
    if (m_sourceKind == SourceKind::ArchiveFile)
    {
        QTemporaryDir workspace;
        AnalysisResult materialized;
        QString error;
        if (!workspace.isValid() || !materializeArchiveAnalysis(workspace.path(), &materialized, &error))
        {
            report.valid = false;
            report.plan = plan;
            report.conflicts << error;
            report.reportText = QStringLiteral("Archive rename preview failed: %1").arg(error);
        }
        else
        {
            report = m_referenceRenamer.preview(materialized, plan);
        }
    }
    else
    {
        report = m_referenceRenamer.preview(m_result, plan);
    }
    m_previewedRenamePlan = plan;
    m_renamePreviewValid = report.valid;
    m_renameIdsPage->setPreviewReport(report);
    if (m_sourceKind == SourceKind::ArchiveFolder)
        m_renameIdsPage->setApplyAvailable(false);
    logLine(QStringLiteral("Rename-to-standard preview: %1 renames, %2 reference updates, valid=%3")
                .arg(report.identitiesRenamed)
                .arg(report.referencesUpdated)
                .arg(report.valid ? QStringLiteral("yes") : QStringLiteral("no")));
}

void MainWindow::applyStandardRename(const RenamePlan &plan)
{
    if (m_sourceKind == SourceKind::ArchiveFolder)
    {
        QMessageBox::information(this, QStringLiteral("Apply Rename"), archiveFolderReadOnlyMessage());
        return;
    }
    const auto signature = [](const RenamePlan &value)
    {
        QStringList parts;
        for (const RenamePlanItem &item : value.items)
            parts << item.oldId + QChar(0x1f) + item.newId;
        std::sort(parts.begin(), parts.end());
        return value.family.rootId + QChar(0x1e) + value.targetRootId + QChar(0x1e) + parts.join(QChar(0x1d));
    };
    if (!m_renamePreviewValid || signature(plan) != signature(m_previewedRenamePlan))
    {
        QMessageBox::warning(this, QStringLiteral("Apply Rename"),
                             QStringLiteral("Preview this exact family and rename selection before applying."));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("Apply Rename"),
                              backupPrompt(
                                  QStringLiteral("Create a backup, rename selected real XML IDs, update references, and verify?"),
                                  QStringLiteral("Backups are disabled in Settings. Rename selected real XML IDs, update references, and verify without a persistent backup?"))) != QMessageBox::Yes)
        return;
    if (m_sourceKind == SourceKind::ArchiveFile)
    {
        QTemporaryDir workspace;
        AnalysisResult materialized;
        QString error;
        if (!workspace.isValid() || !materializeArchiveAnalysis(workspace.path(), &materialized, &error))
        {
            QMessageBox::critical(this, QStringLiteral("Rename failed"), error);
            return;
        }
        const RenameApplyResult result = m_referenceRenamer.apply(materialized, plan, workspace.path(), m_whitelistIds);
        if (!result.success)
        {
            QMessageBox::critical(this, QStringLiteral("Rename failed"), result.error + QStringLiteral("\nThe archive was not changed."));
            return;
        }
        QString archiveBackup;
        if (!commitArchiveChanges(workspace.path(), result.changedFiles, &archiveBackup, &error))
        {
            QMessageBox::critical(this, QStringLiteral("Rename failed"), error + QStringLiteral("\nNo partial archive change was retained."));
            return;
        }
        m_renamePreviewValid = false;
        loadPathAndAnalyze(m_currentSourcePath);
        m_dryRunPage->recordRenameResult(result.identitiesRenamed);
        QMessageBox::information(this, QStringLiteral("Rename complete"),
                                 QStringLiteral("Archive backup: %1\nObjects renamed: %2\nReferences updated: %3")
                                     .arg(archiveBackup)
                                     .arg(result.identitiesRenamed)
                                     .arg(result.referencesUpdated));
        return;
    }
    const RenameApplyResult result = m_referenceRenamer.apply(m_result, plan, m_rootFolder, m_whitelistIds);
    if (!result.success)
    {
        QMessageBox::critical(this, QStringLiteral("Rename failed"), result.error + QStringLiteral("\nChanges were rolled back."));
        return;
    }
    m_renamePreviewValid = false;
    loadPathAndAnalyze(m_currentSourcePath);
    m_dryRunPage->recordRenameResult(result.identitiesRenamed);
    QMessageBox::information(this, QStringLiteral("Rename complete"),
                             QStringLiteral("Backup: %1\nObjects renamed: %2\nReferences updated: %3")
                                 .arg(result.backupFolder)
                                 .arg(result.identitiesRenamed)
                                 .arg(result.referencesUpdated));
}

void MainWindow::exportStandardRenameReport(const QString &reportText)
{
    if (reportText.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("Export Rename Report"), QStringLiteral("Preview a rename first."));
        return;
    }
    const QString selected = saveTextFileStyled(this, QStringLiteral("Export Rename Report"),
                                                QDir(m_rootFolder).absoluteFilePath(QStringLiteral("rename_to_standard_preview.txt")));
    if (selected.isEmpty())
        return;
    QSaveFile file(selected);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text) || file.write(reportText.toUtf8()) != reportText.toUtf8().size() || !file.commit())
        QMessageBox::critical(this, QStringLiteral("Export Rename Report"), QStringLiteral("Unable to write %1").arg(selected));
}

void MainWindow::previewDataCollection(const DataCollectionBuildRequest &request)
{
    DataCollectionPreviewReport report;
    if (m_sourceKind == SourceKind::ArchiveFolder)
    {
        report.valid = false;
        report.request = request;
        report.warnings << archiveFolderReadOnlyMessage();
        report.reportText = archiveFolderReadOnlyMessage();
    }
    else if (m_sourceKind == SourceKind::ArchiveFile)
    {
        QTemporaryDir workspace;
        AnalysisResult materialized;
        QString error;
        if (!workspace.isValid() || !materializeArchiveAnalysis(workspace.path(), &materialized, &error))
        {
            report.warnings << error;
            report.reportText = QStringLiteral("Archive Data Collection preview failed: %1").arg(error);
        }
        else
        {
            report = m_dataCollectionBuilder.preview(materialized, request);
        }
    }
    else
    {
        report = m_dataCollectionBuilder.preview(m_result, request);
    }
    m_previewedCollectionRequest = request;
    m_collectionPreviewValid = report.valid;
    m_dataCollectionPage->setPreviewReport(report);
    logLine(QStringLiteral("Data Collection preview: %1 records to add, valid=%2")
                .arg(report.recordsToAdd.size())
                .arg(report.valid ? QStringLiteral("yes") : QStringLiteral("no")));
}

void MainWindow::applyDataCollection(const DataCollectionBuildRequest &request)
{
    if (m_sourceKind == SourceKind::ArchiveFolder)
    {
        QMessageBox::information(this, QStringLiteral("Apply Collection"), archiveFolderReadOnlyMessage());
        return;
    }
    const auto signature = [](const DataCollectionBuildRequest &value)
    {
        QList<int> indices = value.includedNodeIndices.values();
        std::sort(indices.begin(), indices.end());
        QStringList indexText;
        for (int index : indices)
            indexText << QString::number(index);
        return value.family.rootId + QChar(0x1f) + value.requestedUnitId + QChar(0x1f) + value.parent + QChar(0x1f) + value.editorCategories + QChar(0x1f) + indexText.join(QLatin1Char(',')) + QChar(0x1f) + QString::number(value.confirmNonStandard);
    };
    if (!m_collectionPreviewValid || signature(request) != signature(m_previewedCollectionRequest))
    {
        QMessageBox::warning(this, QStringLiteral("Apply Collection"),
                             QStringLiteral("Preview this exact collection selection and field configuration before applying."));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("Apply Collection"),
                              backupPrompt(
                                  QStringLiteral("Create a backup and create or update the typed Data Collection?"),
                                  QStringLiteral("Backups are disabled in Settings. Create or update the typed Data Collection without a persistent backup?"))) != QMessageBox::Yes)
        return;
    if (m_sourceKind == SourceKind::ArchiveFile)
    {
        QTemporaryDir workspace;
        AnalysisResult materialized;
        QString error;
        if (!workspace.isValid() || !materializeArchiveAnalysis(workspace.path(), &materialized, &error))
        {
            QMessageBox::critical(this, QStringLiteral("Collection failed"), error);
            return;
        }
        const DataCollectionApplyResult result = m_dataCollectionBuilder.apply(materialized, request, workspace.path(), m_whitelistIds);
        if (!result.success)
        {
            QMessageBox::critical(this, QStringLiteral("Collection failed"), result.error + QStringLiteral("\nThe archive was not changed."));
            return;
        }
        QString archiveBackup;
        if (!commitArchiveChanges(workspace.path(), result.changedFiles, &archiveBackup, &error))
        {
            QMessageBox::critical(this, QStringLiteral("Collection failed"), error + QStringLiteral("\nNo partial archive change was retained."));
            return;
        }
        m_collectionPreviewValid = false;
        loadPathAndAnalyze(m_currentSourcePath);
        m_dryRunPage->recordCollectionResult(result.recordsAdded, result.recordsRemoved);
        QMessageBox::information(this, QStringLiteral("Collection complete"),
                                 QStringLiteral("Archive backup: %1\nDataCollectionData.xml and (listfile) saved.\nRecords added: %2")
                                     .arg(archiveBackup)
                                     .arg(result.recordsAdded));
        return;
    }
    const DataCollectionApplyResult result = m_dataCollectionBuilder.apply(m_result, request, m_rootFolder, m_whitelistIds);
    if (!result.success)
    {
        QMessageBox::critical(this, QStringLiteral("Collection failed"), result.error + QStringLiteral("\nChanges were rolled back."));
        return;
    }
    m_collectionPreviewValid = false;
    loadPathAndAnalyze(m_currentSourcePath);
    m_dryRunPage->recordCollectionResult(result.recordsAdded, result.recordsRemoved);
    QMessageBox::information(this, QStringLiteral("Collection complete"),
                             QStringLiteral("Backup: %1\nRecords added: %2\nDuplicate records skipped: %3")
                                 .arg(result.backupFolder)
                                 .arg(result.recordsAdded)
                                 .arg(result.duplicatesSkipped));
}

void MainWindow::exportDataCollectionReport(const QString &reportText)
{
    if (reportText.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("Export Collection Report"), QStringLiteral("Preview a collection first."));
        return;
    }
    const QString selected = saveTextFileStyled(this, QStringLiteral("Export Collection Report"),
                                                QDir(m_rootFolder).absoluteFilePath(QStringLiteral("data_collection_preview.txt")));
    if (selected.isEmpty())
        return;
    const QByteArray bytes = reportText.toUtf8();
    QSaveFile file(selected);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text) || file.write(bytes) != bytes.size() || !file.commit())
        QMessageBox::critical(this, QStringLiteral("Export Collection Report"), QStringLiteral("Unable to write %1").arg(selected));
}

void MainWindow::showAnalysisTab()
{
    m_tabs->setCurrentWidget(m_analysisPage);
}

void MainWindow::showDataCollectionTab()
{
    m_tabs->setCurrentWidget(m_dataCollectionPage);
}

void MainWindow::showDuplicatesTab()
{
    m_tabs->setCurrentWidget(m_duplicatesPage);
}

void MainWindow::showCleanupTab()
{
    m_tabs->setCurrentWidget(m_cleanupPage);
}

void MainWindow::showDryRunTab(bool autoBuild)
{
    if (m_result.nodes.isEmpty())
        return;
    QDialog dialog(this);
    m_optimizationDialog = &dialog;
    dialog.setWindowTitle(QStringLiteral("SC2 Data Optimization Wizard"));
    dialog.setWindowFlags(dialog.windowFlags() | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
    dialog.setMinimumSize(1200, 760);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    m_dryRunPage->setParent(&dialog);
    m_dryRunPage->show();
    layout->addWidget(m_dryRunPage);
    m_dryRunPage->startWizard(autoBuild);
    dialog.resize(1500, 900);
    dialog.showNormal();
    dialog.exec();
    layout->removeWidget(m_dryRunPage);
    m_dryRunPage->setParent(this);
    m_dryRunPage->hide();
    m_optimizationDialog = nullptr;
}

void MainWindow::setDuplicateMergeEnabled(bool enabled)
{
    if (m_dryRunPage)
        m_dryRunPage->setDuplicateMergeEnabled(enabled);
    if (!m_tabs || !m_duplicatesPage)
        return;
    const int index = m_tabs->indexOf(m_duplicatesPage);
    if (index < 0)
        return;
    if (!enabled && m_tabs->currentWidget() == m_duplicatesPage)
        m_tabs->setCurrentWidget(m_analysisPage);
    m_tabs->setTabVisible(index, enabled);
    m_tabs->setTabEnabled(index, enabled);
    m_tabs->setTabToolTip(index, enabled ? QStringLiteral("Duplicate Merge")
                                         : QStringLiteral("Enable Duplicate Merge in Settings"));
}

int MainWindow::findNodeIndex(const AnalysisResult &analysis, const WizardNodeRef &ref) const
{
    for (int index = 0; index < analysis.nodes.size(); ++index)
    {
        const DataNode &node = analysis.nodes[index];
        if (node.id == ref.id && node.elementName == ref.elementName && node.sourceFile == ref.sourceFile && node.originalLocation == ref.originalLocation)
        {
            return index;
        }
    }
    for (int index = 0; index < analysis.nodes.size(); ++index)
    {
        const DataNode &node = analysis.nodes[index];
        if (node.id == ref.id && node.elementName == ref.elementName)
            return index;
    }
    for (int index = 0; index < analysis.nodes.size(); ++index)
        if (analysis.nodes[index].id == ref.id)
            return index;
    return -1;
}

void MainWindow::applyOptimizationWizardPlan()
{
    if (!m_dryRunPage)
        return;

    const OptimizationWizardSelection selection = m_dryRunPage->currentSelection();
    if (selection.unused.isEmpty() && selection.duplicates.isEmpty() && selection.deepCleanup.isEmpty()
        && selection.rename.isEmpty() && selection.collection.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("Optimization Wizard"),
                                 QStringLiteral("Select at least one item before applying the optimization plan."));
        return;
    }
    if (m_sourceKind == SourceKind::ArchiveFolder)
    {
        QMessageBox::information(this, QStringLiteral("Optimization Wizard"), archiveFolderReadOnlyMessage());
        return;
    }

    if (QMessageBox::question(this, QStringLiteral("Apply Optimization Plan"),
                              QStringLiteral("Apply the selected optimization steps to files now, then rebuild the preview from the updated data?")) != QMessageBox::Yes)
    {
        return;
    }

    m_dryRunPage->setApplyingState(true, QStringLiteral("Applying selected optimization steps and saving files...\n\nThe wizard will rebuild the preview from updated files when the batch finishes."));
    AnalysisProgressDialog applyProgress(this);
    applyProgress.setTitleText(QStringLiteral("SC2 DATA APPLY"));
    applyProgress.setCancelVisible(false);
    applyProgress.setProgress(5, QStringLiteral("Preparing apply"), QStringLiteral("Building the selected optimization batch"));
    applyProgress.show();
    QApplication::processEvents();
    const auto updateApplyProgress = [&](int percent, const QString &primary, const QString &secondary = QString())
    {
        applyProgress.setProgress(percent, primary, secondary);
        QApplication::processEvents();
    };

    int removedUnused = 0;
    int removedDuplicates = 0;
    int redirectedReferences = 0;
    int deepCleanupChanged = 0;
    int renamedIds = 0;
    int collectionAdded = 0;
    int collectionReorganized = 0;
    QStringList warnings;
    QString failure;
    QString archiveBackup;
    bool archiveAnalysisReady = false;

    const auto reloadWorkingAnalysis = [this](const QString &rootFolder, AnalysisResult *analysis, QString *errorMessage)
    {
        return m_analyzer.analyzeFolder(rootFolder, m_whitelistIds, analysis, errorMessage);
    };
    const auto groupKey = [](const WizardNodeRef &ref)
    {
        return ref.sourceFile + QChar(0x1f) + ref.originalLocation + QChar(0x1f) + ref.elementName + QChar(0x1f) + ref.id;
    };

    if (m_sourceKind == SourceKind::ArchiveFile)
    {
        updateApplyProgress(15, QStringLiteral("Preparing archive workspace"), QStringLiteral("Materializing XML and listfile"));
        QTemporaryDir workspace;
        AnalysisResult current;
        QString error;
        if (!workspace.isValid() || !materializeArchiveAnalysis(workspace.path(), &current, &error))
        {
            failure = error;
        }
        else
        {
            QStringList changedFiles;
            QStringList removedFiles;

            if (!reloadWorkingAnalysis(workspace.path(), &current, &error))
            {
                failure = error;
            }
            else
            {
                applyArchiveReferenceSafety(&current);
            }

            if (failure.isEmpty() && !selection.deepCleanup.isEmpty())
            {
                updateApplyProgress(22, QStringLiteral("Applying deep cleanup"), QStringLiteral("Removing unused assets, stale localization and redundant XML"));
                const DeepCleanupApplyResult result = DeepCleanupService().apply(current, selection.deepCleanup, workspace.path(), false);
                if (!result.success)
                {
                    failure = result.error;
                }
                else
                {
                    deepCleanupChanged += result.filesDeleted + result.textLinesRemoved + result.xmlNodesRemoved + result.xmlAttributesRemoved;
                    if (result.reportOnlySkipped > 0)
                        warnings << QStringLiteral("Skipped %1 review-only deep cleanup item(s).").arg(result.reportOnlySkipped);
                    changedFiles.append(result.changedFiles);
                    removedFiles.append(result.removedFiles);
                    changedFiles.removeDuplicates();
                    removedFiles.removeDuplicates();
                    if (!reloadWorkingAnalysis(workspace.path(), &current, &error))
                    {
                        failure = error;
                    }
                    else
                    {
                        applyArchiveReferenceSafety(&current);
                    }
                }
            }

            if (failure.isEmpty() && !selection.unused.isEmpty())
            {
                QVector<int> unusedRows;
                for (const WizardNodeRef &ref : selection.unused)
                {
                    const int index = findNodeIndex(current, ref);
                    if (index >= 0)
                        unusedRows.append(index);
                }
                if (!unusedRows.isEmpty())
                {
                    updateApplyProgress(25, QStringLiteral("Deleting unused objects"), QStringLiteral("Rewriting verified archive XML"));
                    QString workspaceBackup;
                    QStringList unusedChangedFiles;
                    int removed = 0;
                    int skipped = 0;
                    if (!m_analyzer.applySelectedChanges(current, unusedRows, workspace.path(), m_whitelistIds,
                                                         &workspaceBackup, &error, &unusedChangedFiles, &removed, &skipped))
                    {
                        failure = error;
                    }
                    else
                    {
                        removedUnused += removed;
                        changedFiles.append(unusedChangedFiles);
                        changedFiles.removeDuplicates();
                        if (skipped > 0)
                            warnings << QStringLiteral("Skipped %1 unused objects because they were no longer safe or available.").arg(skipped);
                        if (!reloadWorkingAnalysis(workspace.path(), &current, &error))
                            failure = error;
                    }
                }
            }
            QHash<QString, QPair<WizardNodeRef, QVector<WizardNodeRef>>> mergeGroups;
            for (const WizardMergeSelection &item : selection.duplicates)
            {
                auto &group = mergeGroups[groupKey(item.keep)];
                group.first = item.keep;
                group.second.append(item.remove);
            }
            updateApplyProgress(35, QStringLiteral("Applying duplicate merges"), QStringLiteral("Redirecting references and removing duplicate objects"));
            for (auto it = mergeGroups.cbegin(); failure.isEmpty() && it != mergeGroups.cend(); ++it)
            {
                const int keepIndex = findNodeIndex(current, it.value().first);
                if (keepIndex < 0)
                {
                    warnings << QStringLiteral("Skipped a duplicate merge because keep object %1 is no longer present.").arg(it.value().first.id);
                    continue;
                }
                MergeRequest request;
                request.keepNodeIndex = keepIndex;
                for (const WizardNodeRef &remove : it.value().second)
                {
                    const int removeIndex = findNodeIndex(current, remove);
                    if (removeIndex >= 0)
                        request.removeNodeIndices.append(removeIndex);
                }
                if (request.removeNodeIndices.isEmpty())
                    continue;
                const MergeApplyResult result = m_mergeService.apply(current, request, workspace.path(), m_whitelistIds);
                if (!result.success)
                {
                    failure = result.error;
                    break;
                }
                removedDuplicates += result.nodesDeleted;
                redirectedReferences += result.referencesRedirected;
                changedFiles.append(result.changedFiles);
                changedFiles.removeDuplicates();
                if (!reloadWorkingAnalysis(workspace.path(), &current, &error))
                {
                    failure = error;
                    break;
                }
            }

            QHash<QString, QVector<WizardNodeRef>> renameByFamily;
            for (const WizardRenameSelection &item : selection.rename)
                renameByFamily[item.familyRootId].append(item.node);
            if (!renameByFamily.isEmpty())
            {
                QVector<UnitFamily> renameFamilies = UnitFamilyDetector().detect(current);
                QHash<QString, UnitFamily> renameFamilyByRoot;
                for (const UnitFamily &family : renameFamilies)
                    renameFamilyByRoot.insert(family.rootId, family);
                StandardNamePlanner planner;
                updateApplyProgress(55, QStringLiteral("Applying rename changes"), QStringLiteral("Updating real XML IDs and references"));
                for (auto it = renameByFamily.cbegin(); failure.isEmpty() && it != renameByFamily.cend(); ++it)
                {
                    const auto familyIt = renameFamilyByRoot.constFind(it.key());
                    if (familyIt == renameFamilyByRoot.cend())
                    {
                        warnings << QStringLiteral("Skipped rename family %1 because it is no longer present after apply.").arg(it.key());
                        continue;
                    }
                    QSet<int> includedNodeIndices;
                    for (const WizardNodeRef &ref : it.value())
                    {
                        const int index = findNodeIndex(current, ref);
                        if (index >= 0)
                            includedNodeIndices.insert(index);
                    }
                    if (includedNodeIndices.isEmpty())
                        continue;
                    const RenamePlan plan = planner.plan(current, familyIt.value(), familyIt.value().rootId, includedNodeIndices);
                    if (!plan.valid)
                    {
                        warnings << QStringLiteral("Skipped rename family %1 because the refreshed plan is no longer valid.").arg(it.key());
                        continue;
                    }
                    const RenameApplyResult result = m_referenceRenamer.apply(current, plan, workspace.path(), m_whitelistIds);
                    if (!result.success)
                    {
                        failure = result.error;
                        break;
                    }
                    renamedIds += result.identitiesRenamed;
                    changedFiles.append(result.changedFiles);
                    changedFiles.removeDuplicates();
                    if (!reloadWorkingAnalysis(workspace.path(), &current, &error))
                    {
                        failure = error;
                        break;
                    }
                    applyArchiveReferenceSafety(&current);
                    renameFamilies = UnitFamilyDetector().detect(current);
                    renameFamilyByRoot.clear();
                    for (const UnitFamily &family : renameFamilies)
                        renameFamilyByRoot.insert(family.rootId, family);
                }
            }

            QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(current, configuredDataCollectionMode());
            QHash<QString, UnitFamily> familyByRoot;
            for (const UnitFamily &family : families)
                familyByRoot.insert(family.rootId, family);
            bool collectionChanged = false;
            const int collectionCount = selection.collection.size();
            for (int collectionIndex = 0; collectionIndex < collectionCount; ++collectionIndex)
            {
                const WizardCollectionSelection &selectedCollection = selection.collection[collectionIndex];
                if (failure.isEmpty())
                {
                    const int percent = 65 + ((collectionIndex * 15) / qMax(1, collectionCount));
                    updateApplyProgress(percent, QStringLiteral("Applying Data Collection"),
                                        QStringLiteral("Family %1 of %2 (%3%): %4")
                                            .arg(collectionIndex + 1)
                                            .arg(collectionCount)
                                            .arg(((collectionIndex + 1) * 100) / qMax(1, collectionCount))
                                            .arg(selectedCollection.familyRootId));
                    const auto match = familyByRoot.constFind(selectedCollection.familyRootId);
                    if (match == familyByRoot.cend())
                    {
                        warnings << QStringLiteral("Skipped Data Collection family %1 because it is no longer present after apply.").arg(selectedCollection.familyRootId);
                        continue;
                    }
                    DataCollectionBuildRequest request;
                    request.family = match.value();
                    request.requestedUnitId = request.family.rootId;
                    request.confirmNonStandard = true;
                    for (const WizardNodeRef &ref : selectedCollection.nodes)
                    {
                        const int index = findNodeIndex(current, ref);
                        if (index >= 0)
                            request.includedNodeIndices.insert(index);
                    }
                    const DataCollectionApplyResult result = m_dataCollectionBuilder.apply(
                        current, request, workspace.path(), m_whitelistIds, false, &families, true);
                    if (!result.success)
                    {
                        failure = result.error;
                        break;
                    }
                    collectionAdded += result.recordsAdded;
                    collectionReorganized += result.recordsRemoved;
                    changedFiles.append(result.changedFiles);
                    changedFiles.removeDuplicates();
                    collectionChanged = true;
                }
            }

            if (failure.isEmpty() && collectionChanged && !reloadWorkingAnalysis(workspace.path(), &current, &error))
                failure = error;

            if (failure.isEmpty() && (!changedFiles.isEmpty() || !removedFiles.isEmpty()))
            {
                updateApplyProgress(85, QStringLiteral("Saving archive"), QStringLiteral("Writing verified XML back to the SC2 archive"));
                if (!commitArchiveChanges(workspace.path(), changedFiles, &archiveBackup, &error, removedFiles))
                {
                    failure = error;
                }
                else
                {
                    normalizeArchiveAnalysis(&current, workspace.path(), m_currentSourcePath);
                    m_result = std::move(current);
                    archiveAnalysisReady = true;
                }
            }
        }
    }
    else
    {
        AnalysisResult current = m_result;
        QString error;

        if (failure.isEmpty() && !selection.deepCleanup.isEmpty())
        {
            updateApplyProgress(18, QStringLiteral("Applying deep cleanup"), QStringLiteral("Removing unused assets, stale localization and redundant XML"));
            const DeepCleanupApplyResult result = DeepCleanupService().apply(current, selection.deepCleanup, m_rootFolder, true);
            if (!result.success)
            {
                failure = result.error;
            }
            else
            {
                deepCleanupChanged += result.filesDeleted + result.textLinesRemoved + result.xmlNodesRemoved + result.xmlAttributesRemoved;
                if (result.reportOnlySkipped > 0)
                    warnings << QStringLiteral("Skipped %1 review-only deep cleanup item(s).").arg(result.reportOnlySkipped);
                if (!reloadWorkingAnalysis(m_rootFolder, &current, &error))
                    failure = error;
            }
        }

        QVector<int> unusedRows;
        for (const WizardNodeRef &ref : selection.unused)
        {
            const int index = findNodeIndex(current, ref);
            if (index >= 0)
                unusedRows.append(index);
        }
        if (failure.isEmpty() && !unusedRows.isEmpty())
        {
            updateApplyProgress(20, QStringLiteral("Deleting unused objects"), QStringLiteral("Removing selected safe unused objects"));
            QString backupFolder;
            QStringList changedFiles;
            int removed = 0;
            int skipped = 0;
            if (!m_analyzer.applySelectedChanges(current, unusedRows, m_rootFolder, m_whitelistIds,
                                                 &backupFolder, &error, &changedFiles, &removed, &skipped))
            {
                failure = error;
            }
            else
            {
                removedUnused += removed;
                if (skipped > 0)
                    warnings << QStringLiteral("Skipped %1 unused objects because they were no longer safe or available.").arg(skipped);
                if (!reloadWorkingAnalysis(m_rootFolder, &current, &error))
                    failure = error;
            }
        }

        QHash<QString, QPair<WizardNodeRef, QVector<WizardNodeRef>>> mergeGroups;
        for (const WizardMergeSelection &item : selection.duplicates)
        {
            auto &group = mergeGroups[groupKey(item.keep)];
            group.first = item.keep;
            group.second.append(item.remove);
        }
        updateApplyProgress(45, QStringLiteral("Applying duplicate merges"), QStringLiteral("Redirecting references and removing duplicate objects"));
        for (auto it = mergeGroups.cbegin(); failure.isEmpty() && it != mergeGroups.cend(); ++it)
        {
            const int keepIndex = findNodeIndex(current, it.value().first);
            if (keepIndex < 0)
            {
                warnings << QStringLiteral("Skipped a duplicate merge because keep object %1 is no longer present.").arg(it.value().first.id);
                continue;
            }
            MergeRequest request;
            request.keepNodeIndex = keepIndex;
            for (const WizardNodeRef &remove : it.value().second)
            {
                const int removeIndex = findNodeIndex(current, remove);
                if (removeIndex >= 0)
                    request.removeNodeIndices.append(removeIndex);
            }
            if (request.removeNodeIndices.isEmpty())
                continue;
            const MergeApplyResult result = m_mergeService.apply(current, request, m_rootFolder, m_whitelistIds);
            if (!result.success)
            {
                failure = result.error;
                break;
            }
            removedDuplicates += result.nodesDeleted;
            redirectedReferences += result.referencesRedirected;
            if (!reloadWorkingAnalysis(m_rootFolder, &current, &error))
            {
                failure = error;
                break;
            }
        }

        QVector<UnitFamily> families = UnitFamilyDetector().detect(current);
        QHash<QString, UnitFamily> familyByRoot;
        for (const UnitFamily &family : families)
            familyByRoot.insert(family.rootId, family);
        QHash<QString, QVector<WizardNodeRef>> renameByFamily;
        for (const WizardRenameSelection &item : selection.rename)
            renameByFamily[item.familyRootId].append(item.node);
        StandardNamePlanner planner;
        updateApplyProgress(60, QStringLiteral("Applying rename changes"), QStringLiteral("Updating IDs and references"));
        for (auto it = renameByFamily.cbegin(); failure.isEmpty() && it != renameByFamily.cend(); ++it)
        {
            const auto familyIt = familyByRoot.constFind(it.key());
            if (familyIt == familyByRoot.cend())
            {
                warnings << QStringLiteral("Skipped rename family %1 because it is no longer present after apply.").arg(it.key());
                continue;
            }
            QSet<int> includedNodeIndices;
            for (const WizardNodeRef &ref : it.value())
            {
                const int index = findNodeIndex(current, ref);
                if (index >= 0)
                    includedNodeIndices.insert(index);
            }
            if (includedNodeIndices.isEmpty())
                continue;
            const RenamePlan plan = planner.plan(current, familyIt.value(), familyIt.value().rootId, includedNodeIndices);
            if (!plan.valid)
            {
                warnings << QStringLiteral("Skipped rename family %1 because the refreshed plan is no longer valid.").arg(it.key());
                continue;
            }
            const RenameApplyResult result = m_referenceRenamer.apply(current, plan, m_rootFolder, m_whitelistIds);
            if (!result.success)
            {
                failure = result.error;
                break;
            }
            renamedIds += result.identitiesRenamed;
            if (!reloadWorkingAnalysis(m_rootFolder, &current, &error))
            {
                failure = error;
                break;
            }
            families = UnitFamilyDetector().detect(current);
            familyByRoot.clear();
            for (const UnitFamily &family : families)
                familyByRoot.insert(family.rootId, family);
        }

        const QVector<UnitFamily> collectionFamilies = UnitFamilyDetector().detectCollectionFamilies(current, configuredDataCollectionMode());
        QHash<QString, UnitFamily> collectionFamilyByRoot;
        for (const UnitFamily &family : collectionFamilies)
            collectionFamilyByRoot.insert(family.rootId, family);
        const int collectionCount = selection.collection.size();
        for (int collectionIndex = 0; collectionIndex < collectionCount; ++collectionIndex)
        {
            const WizardCollectionSelection &selectedCollection = selection.collection[collectionIndex];
            if (!failure.isEmpty())
                break;
            const int percent = 80 + ((collectionIndex * 10) / qMax(1, collectionCount));
            updateApplyProgress(percent, QStringLiteral("Applying Data Collection"),
                                QStringLiteral("Family %1 of %2 (%3%): %4")
                                    .arg(collectionIndex + 1)
                                    .arg(collectionCount)
                                    .arg(((collectionIndex + 1) * 100) / qMax(1, collectionCount))
                                    .arg(selectedCollection.familyRootId));
            const auto familyIt = collectionFamilyByRoot.constFind(selectedCollection.familyRootId);
            if (familyIt == collectionFamilyByRoot.cend())
            {
                warnings << QStringLiteral("Skipped Data Collection family %1 because it is no longer present after apply.").arg(selectedCollection.familyRootId);
                continue;
            }
            DataCollectionBuildRequest request;
            request.family = familyIt.value();
            request.requestedUnitId = request.family.rootId;
            request.confirmNonStandard = true;
            for (const WizardNodeRef &ref : selectedCollection.nodes)
            {
                const int index = findNodeIndex(current, ref);
                if (index >= 0)
                    request.includedNodeIndices.insert(index);
            }
            const DataCollectionApplyResult result = m_dataCollectionBuilder.apply(
                current, request, m_rootFolder, m_whitelistIds, false, &collectionFamilies);
            if (!result.success)
            {
                failure = result.error;
                break;
            }
            collectionAdded += result.recordsAdded;
            collectionReorganized += result.recordsRemoved;
        }
    }

    m_dryRunPage->setApplyingState(false);

    if (!failure.isEmpty())
    {
        applyProgress.close();
        loadPathAndAnalyze(m_currentSourcePath);
        QMessageBox::critical(this, QStringLiteral("Optimization Apply Failed"),
                              QStringLiteral("The optimization batch stopped:\n%1").arg(failure));
        return;
    }

    if (archiveAnalysisReady)
    {
        updateApplyProgress(92, QStringLiteral("Refreshing analysis"), QStringLiteral("Updating object tables"));
        m_rootFolder = QFileInfo(m_currentSourcePath).absolutePath();
        m_analysisPage->setFolderPath(m_currentSourcePath);
        m_analysisPage->setModeLabel(modeLabelFor(static_cast<int>(m_sourceKind)));
        m_analysisPage->setAnalysisResult(m_result);
        updateApplyProgress(95, QStringLiteral("Refreshing analysis"), QStringLiteral("Updating pages and recommendations"));
        refreshPages();
        updateApplyProgress(98, QStringLiteral("Writing report"), QStringLiteral("Saving latest analysis summary"));
        writeAnalysisReportFile();
        m_dryRunAction->setEnabled(true);
        m_applyAction->setEnabled(false);
        setCurrentSourcePath(m_currentSourcePath);
    }
    else if (!loadPathAndAnalyze(m_currentSourcePath))
    {
        QMessageBox::warning(this, QStringLiteral("Optimization Applied"),
                             QStringLiteral("Changes were saved, but automatic re-analysis failed. Re-open Analyze to refresh the wizard view."));
        return;
    }
    updateApplyProgress(100, QStringLiteral("Apply complete"), QStringLiteral("Optimization changes were saved successfully"));
    applyProgress.close();

    if (removedUnused > 0)
        m_dryRunPage->recordUnusedResult(removedUnused);
    if (removedDuplicates > 0 || redirectedReferences > 0)
        m_dryRunPage->recordMergeResult(removedDuplicates, redirectedReferences);
    if (deepCleanupChanged > 0)
        m_dryRunPage->recordDeepCleanupResult(deepCleanupChanged);
    if (renamedIds > 0)
        m_dryRunPage->recordRenameResult(renamedIds);
    if (collectionAdded > 0 || collectionReorganized > 0)
        m_dryRunPage->recordCollectionResult(collectionAdded, collectionReorganized);
    m_dryRunPage->rebuildAfterApply();

    QString message = QStringLiteral("Selected optimization steps were applied and saved.\n\nUnused deleted: %1\nDuplicates deleted: %2\nReferences redirected: %3\nDeep cleanup changes: %4\nIDs renamed: %5\nCollection records added: %6\nCollection records reorganized: %7")
                          .arg(removedUnused)
                          .arg(removedDuplicates)
                          .arg(redirectedReferences)
                          .arg(deepCleanupChanged)
                          .arg(renamedIds)
                          .arg(collectionAdded)
                          .arg(collectionReorganized);
    if (!archiveBackup.isEmpty())
        message += QStringLiteral("\nArchive backup: %1").arg(archiveBackup);
    if (!warnings.isEmpty())
        message += QStringLiteral("\n\nWarnings:\n- ") + warnings.join(QStringLiteral("\n- "));
    QMessageBox::information(this, QStringLiteral("Optimization Applied"), message);
}

void MainWindow::showLogsTab()
{
    m_tabs->setCurrentWidget(m_logPanel);
}

void MainWindow::showGraphForRow(int row)
{
    if (row < 0 || row >= m_result.nodes.size())
    {
        return;
    }

    m_tabs->setCurrentWidget(m_graphPage);
    m_graphPage->setCurrentRow(row);
    m_dependenciesPage->setCurrentRow(row);
    m_propertiesPage->setCurrentRow(row);
    QMetaObject::invokeMethod(m_graphPage, "fitGraph", Qt::QueuedConnection);
}

void MainWindow::writeAnalysisReportFile() const
{
    // Reports stay in the UI unless the user explicitly exports them.
}

void MainWindow::refreshPages()
{
    m_mergePreviewValid = false;
    m_previewedUnusedRows.clear();
    m_renamePreviewValid = false;
    m_collectionPreviewValid = false;
    m_duplicatesPage->setAnalysisResult(m_result);
    m_cleanupPage->setAnalysisResult(m_result);
    m_dataCollectionPage->setAnalysisResult(m_result);
    m_renameIdsPage->setAnalysisResult(m_result);
    m_analysisPage->setAnalysisResult(m_result);
    m_dependenciesPage->setAnalysisResult(m_result);
    m_graphPage->setAnalysisResult(m_result);
    m_propertiesPage->setAnalysisResult(m_result);
    m_xmlSourcePage->setAnalysisResult(m_result);
    m_dryRunPage->setAnalysisResult(m_result);
    m_applyAction->setEnabled(false);

    const int currentRow = m_analysisPage->currentRow();
    m_dependenciesPage->setCurrentRow(currentRow);
    m_graphPage->setCurrentRow(currentRow);
    m_propertiesPage->setCurrentRow(currentRow);
}
