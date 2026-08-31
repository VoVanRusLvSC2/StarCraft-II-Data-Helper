#include "app/TranslationManager.h"

#include "app/AppSettings.h"

#include <QAction>
#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QToolBox>
#include <QTreeWidget>
#include <QVariant>
#include <QWidget>
#include <QWizardPage>

namespace
{
constexpr auto SourceTextProperty = "sc2dh.translationSourceText";
constexpr auto RenderedTextProperty = "sc2dh.translationRenderedText";
constexpr auto SourcePlaceholderProperty = "sc2dh.translationSourcePlaceholder";
constexpr auto RenderedPlaceholderProperty = "sc2dh.translationRenderedPlaceholder";
constexpr auto SourceToolTipProperty = "sc2dh.translationSourceToolTip";
constexpr auto RenderedToolTipProperty = "sc2dh.translationRenderedToolTip";
constexpr auto SourceStatusTipProperty = "sc2dh.translationSourceStatusTip";
constexpr auto RenderedStatusTipProperty = "sc2dh.translationRenderedStatusTip";
constexpr auto SourceWhatsThisProperty = "sc2dh.translationSourceWhatsThis";
constexpr auto RenderedWhatsThisProperty = "sc2dh.translationRenderedWhatsThis";
constexpr auto SourceAccessibleNameProperty = "sc2dh.translationSourceAccessibleName";
constexpr auto RenderedAccessibleNameProperty = "sc2dh.translationRenderedAccessibleName";
constexpr auto SourceAccessibleDescriptionProperty = "sc2dh.translationSourceAccessibleDescription";
constexpr auto RenderedAccessibleDescriptionProperty = "sc2dh.translationRenderedAccessibleDescription";
constexpr auto SourceWindowTitleProperty = "sc2dh.translationSourceWindowTitle";
constexpr auto RenderedWindowTitleProperty = "sc2dh.translationRenderedWindowTitle";
constexpr auto SourceIconTextProperty = "sc2dh.translationSourceIconText";
constexpr auto RenderedIconTextProperty = "sc2dh.translationRenderedIconText";
constexpr auto SourceHeaderProperty = "sc2dh.translationSourceHeaders";
constexpr auto RenderedHeaderProperty = "sc2dh.translationRenderedHeaders";
constexpr auto SourceItemTextProperty = "sc2dh.translationSourceItemTexts";
constexpr auto RenderedItemTextProperty = "sc2dh.translationRenderedItemTexts";
constexpr auto SourceItemToolTipProperty = "sc2dh.translationSourceItemToolTips";
constexpr auto RenderedItemToolTipProperty = "sc2dh.translationRenderedItemToolTips";
constexpr auto SourceItemWhatsThisProperty = "sc2dh.translationSourceItemWhatsThis";
constexpr auto RenderedItemWhatsThisProperty = "sc2dh.translationRenderedItemWhatsThis";
constexpr auto SourcePrefixProperty = "sc2dh.translationSourcePrefix";
constexpr auto RenderedPrefixProperty = "sc2dh.translationRenderedPrefix";
constexpr auto SourceSuffixProperty = "sc2dh.translationSourceSuffix";
constexpr auto RenderedSuffixProperty = "sc2dh.translationRenderedSuffix";
constexpr auto SourceStatusMessageProperty = "sc2dh.translationSourceStatusMessage";
constexpr auto RenderedStatusMessageProperty = "sc2dh.translationRenderedStatusMessage";
constexpr auto SourceProgressFormatProperty = "sc2dh.translationSourceProgressFormat";
constexpr auto RenderedProgressFormatProperty = "sc2dh.translationRenderedProgressFormat";
constexpr auto SourceWizardSubtitleProperty = "sc2dh.translationSourceWizardSubtitle";
constexpr auto RenderedWizardSubtitleProperty = "sc2dh.translationRenderedWizardSubtitle";

QString dynamicText(const QString &source)
{
    if (source.isEmpty())
        return source;
    const QByteArray utf8 = source.toUtf8();
    return sc2dh::app::TranslationManager::translateDynamicUi(utf8.constData());
}

void captureTextProperty(QObject *object, const char *sourceProperty,
                         const char *renderedProperty, const QString &value)
{
    if (!object || object->property(sourceProperty).isValid())
        return;
    object->setProperty(sourceProperty, value);
    object->setProperty(renderedProperty, value);
}

template<typename Setter>
void retranslateTextProperty(QObject *object, const char *sourceProperty,
                             const char *renderedProperty, const QString &current,
                             Setter &&setter)
{
    if (!object || !object->property(sourceProperty).isValid())
        return;
    const QString source = object->property(sourceProperty).toString();
    const QString rendered = object->property(renderedProperty).toString();
    // A dynamic value such as "Root ID: Marine" must survive a language switch.
    if (source.isEmpty() || current != rendered)
        return;
    const QString translated = dynamicText(source);
    if (current != translated)
        setter(translated);
    object->setProperty(renderedProperty, translated);
}

void captureStringList(QObject *object, const char *sourceProperty,
                       const char *renderedProperty, const QStringList &values)
{
    if (!object)
        return;

    QStringList source = object->property(sourceProperty).toStringList();
    QStringList rendered = object->property(renderedProperty).toStringList();
    if (!object->property(sourceProperty).isValid()) {
        object->setProperty(sourceProperty, values);
        object->setProperty(renderedProperty, values);
        return;
    }

    while (rendered.size() < source.size())
        rendered.append(source.at(rendered.size()));
    for (int index = source.size(); index < values.size(); ++index) {
        source.append(values.at(index));
        rendered.append(values.at(index));
    }
    object->setProperty(sourceProperty, source);
    object->setProperty(renderedProperty, rendered);
}

template<typename Setter>
void retranslateStringList(QObject *object, const char *sourceProperty,
                           const char *renderedProperty, const QStringList &current,
                           Setter &&setter)
{
    if (!object || !object->property(sourceProperty).isValid())
        return;
    const QStringList source = object->property(sourceProperty).toStringList();
    QStringList rendered = object->property(renderedProperty).toStringList();
    while (rendered.size() < source.size())
        rendered.append(source.at(rendered.size()));

    const int count = qMin(source.size(), current.size());
    for (int index = 0; index < count; ++index) {
        if (source.at(index).isEmpty() || current.at(index) != rendered.at(index))
            continue;
        const QString translated = dynamicText(source.at(index));
        if (current.at(index) != translated)
            setter(index, translated);
        rendered[index] = translated;
    }
    object->setProperty(renderedProperty, rendered);
}

void captureWidgetMetadata(QWidget *widget)
{
    captureTextProperty(widget, SourceToolTipProperty, RenderedToolTipProperty, widget->toolTip());
    captureTextProperty(widget, SourceStatusTipProperty, RenderedStatusTipProperty, widget->statusTip());
    captureTextProperty(widget, SourceWhatsThisProperty, RenderedWhatsThisProperty, widget->whatsThis());
    captureTextProperty(widget, SourceAccessibleNameProperty, RenderedAccessibleNameProperty, widget->accessibleName());
    captureTextProperty(widget, SourceAccessibleDescriptionProperty, RenderedAccessibleDescriptionProperty,
                        widget->accessibleDescription());
    captureTextProperty(widget, SourceWindowTitleProperty, RenderedWindowTitleProperty, widget->windowTitle());
}

void retranslateWidgetMetadata(QWidget *widget)
{
    retranslateTextProperty(widget, SourceToolTipProperty, RenderedToolTipProperty, widget->toolTip(),
                            [widget](const QString &value) { widget->setToolTip(value); });
    retranslateTextProperty(widget, SourceStatusTipProperty, RenderedStatusTipProperty, widget->statusTip(),
                            [widget](const QString &value) { widget->setStatusTip(value); });
    retranslateTextProperty(widget, SourceWhatsThisProperty, RenderedWhatsThisProperty, widget->whatsThis(),
                            [widget](const QString &value) { widget->setWhatsThis(value); });
    retranslateTextProperty(widget, SourceAccessibleNameProperty, RenderedAccessibleNameProperty,
                            widget->accessibleName(),
                            [widget](const QString &value) { widget->setAccessibleName(value); });
    retranslateTextProperty(widget, SourceAccessibleDescriptionProperty,
                            RenderedAccessibleDescriptionProperty, widget->accessibleDescription(),
                            [widget](const QString &value) { widget->setAccessibleDescription(value); });
    retranslateTextProperty(widget, SourceWindowTitleProperty, RenderedWindowTitleProperty, widget->windowTitle(),
                            [widget](const QString &value) { widget->setWindowTitle(value); });
}

QStringList comboBoxItems(const QComboBox *combo)
{
    QStringList values;
    for (int index = 0; combo && index < combo->count(); ++index)
        values.append(combo->itemText(index));
    return values;
}

QStringList listWidgetItems(const QListWidget *list)
{
    QStringList values;
    for (int index = 0; list && index < list->count(); ++index)
        values.append(list->item(index) ? list->item(index)->text() : QString());
    return values;
}

QStringList tabTexts(const QTabWidget *tabs)
{
    QStringList values;
    for (int index = 0; tabs && index < tabs->count(); ++index)
        values.append(tabs->tabText(index));
    return values;
}

QStringList tabToolTips(const QTabWidget *tabs)
{
    QStringList values;
    for (int index = 0; tabs && index < tabs->count(); ++index)
        values.append(tabs->tabToolTip(index));
    return values;
}

QStringList tabWhatsThis(const QTabWidget *tabs)
{
    QStringList values;
    for (int index = 0; tabs && index < tabs->count(); ++index)
        values.append(tabs->tabWhatsThis(index));
    return values;
}

QStringList toolBoxTexts(const QToolBox *toolBox)
{
    QStringList values;
    for (int index = 0; toolBox && index < toolBox->count(); ++index)
        values.append(toolBox->itemText(index));
    return values;
}

QStringList toolBoxToolTips(const QToolBox *toolBox)
{
    QStringList values;
    for (int index = 0; toolBox && index < toolBox->count(); ++index)
        values.append(toolBox->itemToolTip(index));
    return values;
}

QStringList treeHeaderTexts(const QTreeWidget *tree)
{
    QStringList values;
    const QTreeWidgetItem *header = tree ? tree->headerItem() : nullptr;
    for (int index = 0; tree && index < tree->columnCount(); ++index)
        values.append(header ? header->text(index) : QString());
    return values;
}

QStringList tableHeaderTexts(const QTableWidget *table)
{
    QStringList values;
    for (int index = 0; table && index < table->columnCount(); ++index) {
        const QTableWidgetItem *item = table->horizontalHeaderItem(index);
        values.append(item ? item->text() : QString());
    }
    return values;
}

void captureAction(QAction *action)
{
    if (!action)
        return;
    captureTextProperty(action, SourceTextProperty, RenderedTextProperty, action->text());
    captureTextProperty(action, SourceIconTextProperty, RenderedIconTextProperty, action->iconText());
    captureTextProperty(action, SourceToolTipProperty, RenderedToolTipProperty, action->toolTip());
    captureTextProperty(action, SourceStatusTipProperty, RenderedStatusTipProperty, action->statusTip());
    captureTextProperty(action, SourceWhatsThisProperty, RenderedWhatsThisProperty, action->whatsThis());
}

void retranslateAction(QAction *action)
{
    if (!action)
        return;
    retranslateTextProperty(action, SourceTextProperty, RenderedTextProperty, action->text(),
                            [action](const QString &value) { action->setText(value); });
    retranslateTextProperty(action, SourceIconTextProperty, RenderedIconTextProperty, action->iconText(),
                            [action](const QString &value) { action->setIconText(value); });
    retranslateTextProperty(action, SourceToolTipProperty, RenderedToolTipProperty, action->toolTip(),
                            [action](const QString &value) { action->setToolTip(value); });
    retranslateTextProperty(action, SourceStatusTipProperty, RenderedStatusTipProperty, action->statusTip(),
                            [action](const QString &value) { action->setStatusTip(value); });
    retranslateTextProperty(action, SourceWhatsThisProperty, RenderedWhatsThisProperty, action->whatsThis(),
                            [action](const QString &value) { action->setWhatsThis(value); });
}

void captureModelHeaders(QAbstractItemModel *model)
{
    if (!model)
        return;
    QStringList headers;
    for (int column = 0; column < model->columnCount(); ++column)
        headers << model->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString();
    captureStringList(model, SourceHeaderProperty, RenderedHeaderProperty, headers);
}

void retranslateModelHeaders(QAbstractItemModel *model)
{
    if (!model)
        return;
    QStringList headers;
    for (int column = 0; column < model->columnCount(); ++column)
        headers << model->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString();
    retranslateStringList(model, SourceHeaderProperty, RenderedHeaderProperty, headers,
                          [model](int column, const QString &value) {
                              model->setHeaderData(column, Qt::Horizontal, value, Qt::DisplayRole);
                          });
}

} // namespace

namespace sc2dh::app
{

TranslationManager &TranslationManager::instance()
{
    // The singleton has static lifetime.  Do not also parent it to qApp:
    // QApplication would delete the child during shutdown and the static
    // destructor would then run a second time.
    static TranslationManager manager;
    return manager;
}

TranslationManager::TranslationManager(QObject *parent)
    : QObject(parent)
{
}

QString TranslationManager::translateDynamicUi(const char *source)
{
    return source ? QCoreApplication::translate("DynamicUI", source) : QString();
}

QString TranslationManager::resolveLanguage(const QString &languageCode) const
{
    if (languageCode != QStringLiteral("system"))
        return languageCode;
    const QString systemName = QLocale::system().name();
    for (const LanguageOption &language : AppSettings::supportedLanguages()) {
        if (!language.localeName.isEmpty()
            && (systemName.compare(language.localeName, Qt::CaseInsensitive) == 0
                || systemName.startsWith(language.localeName.left(2), Qt::CaseInsensitive)))
            return language.code;
    }
    return QStringLiteral("enUS");
}

bool TranslationManager::setLanguage(const QString &languageCode, bool persist)
{
    QString selected = languageCode;
    bool known = false;
    QString localeName;
    for (const LanguageOption &language : AppSettings::supportedLanguages()) {
        if (language.code == selected) {
            known = true;
            break;
        }
    }
    if (!known)
        selected = QStringLiteral("system");
    const QString resolved = resolveLanguage(selected);
    for (const LanguageOption &language : AppSettings::supportedLanguages()) {
        if (language.code == resolved) {
            localeName = language.localeName;
            break;
        }
    }

    qApp->removeTranslator(&m_translator);
    bool loaded = true;
    if (!localeName.isEmpty() && resolved != QStringLiteral("enUS")) {
        loaded = m_translator.load(QStringLiteral(":/i18n/SC2DataHelper_%1.qm").arg(localeName));
        if (loaded)
            qApp->installTranslator(&m_translator);
    }
    m_selectedLanguage = selected;
    m_resolvedLanguage = resolved;
    if (persist)
        QSettings().setValue(QStringLiteral("ui/language"), selected);

    const auto topLevels = QApplication::topLevelWidgets();
    for (QWidget *widget : topLevels) {
        if (!widget)
            continue;
        // sendEvent does not take ownership of events. Keep this event on the
        // stack so every language switch remains leak-free.
        QEvent languageChangeEvent(QEvent::LanguageChange);
        QCoreApplication::sendEvent(widget, &languageChangeEvent);
        // Not every custom widget implements changeEvent(LanguageChange).
        // The captured generic UI text still gets a chance to update, without
        // touching editable content or the current selection.
        retranslateWidgetTree(widget);
    }
    emit languageChanged(selected);
    return loaded;
}

void TranslationManager::captureWidgetTree(QWidget *root)
{
    if (!root)
        return;
    QList<QWidget *> widgets{root};
    widgets.append(root->findChildren<QWidget *>());
    for (QWidget *widget : widgets) {
        captureWidgetMetadata(widget);
        if (auto *label = qobject_cast<QLabel *>(widget))
            captureTextProperty(label, SourceTextProperty, RenderedTextProperty, label->text());
        else if (auto *button = qobject_cast<QAbstractButton *>(widget))
            captureTextProperty(button, SourceTextProperty, RenderedTextProperty, button->text());
        else if (auto *group = qobject_cast<QGroupBox *>(widget))
            captureTextProperty(group, SourceTextProperty, RenderedTextProperty, group->title());
        else if (auto *menu = qobject_cast<QMenu *>(widget))
            captureTextProperty(menu, SourceTextProperty, RenderedTextProperty, menu->title());
        else if (auto *wizardPage = qobject_cast<QWizardPage *>(widget)) {
            captureTextProperty(wizardPage, SourceTextProperty, RenderedTextProperty, wizardPage->title());
            captureTextProperty(wizardPage, SourceWizardSubtitleProperty, RenderedWizardSubtitleProperty,
                                wizardPage->subTitle());
        }

        if (auto *lineEdit = qobject_cast<QLineEdit *>(widget)) {
            captureTextProperty(lineEdit, SourcePlaceholderProperty, RenderedPlaceholderProperty,
                                lineEdit->placeholderText());
        } else if (auto *plain = qobject_cast<QPlainTextEdit *>(widget)) {
            captureTextProperty(plain, SourcePlaceholderProperty, RenderedPlaceholderProperty,
                                plain->placeholderText());
        } else if (auto *text = qobject_cast<QTextEdit *>(widget)) {
            captureTextProperty(text, SourcePlaceholderProperty, RenderedPlaceholderProperty,
                                text->placeholderText());
        }

        if (auto *combo = qobject_cast<QComboBox *>(widget)) {
            captureStringList(combo, SourceItemTextProperty, RenderedItemTextProperty, comboBoxItems(combo));
        } else if (auto *list = qobject_cast<QListWidget *>(widget)) {
            captureStringList(list, SourceItemTextProperty, RenderedItemTextProperty, listWidgetItems(list));
        } else if (auto *tabs = qobject_cast<QTabWidget *>(widget)) {
            captureStringList(tabs, SourceItemTextProperty, RenderedItemTextProperty, tabTexts(tabs));
            captureStringList(tabs, SourceItemToolTipProperty, RenderedItemToolTipProperty, tabToolTips(tabs));
            captureStringList(tabs, SourceItemWhatsThisProperty, RenderedItemWhatsThisProperty, tabWhatsThis(tabs));
        } else if (auto *toolBox = qobject_cast<QToolBox *>(widget)) {
            captureStringList(toolBox, SourceItemTextProperty, RenderedItemTextProperty, toolBoxTexts(toolBox));
            captureStringList(toolBox, SourceItemToolTipProperty, RenderedItemToolTipProperty,
                              toolBoxToolTips(toolBox));
        } else if (auto *tree = qobject_cast<QTreeWidget *>(widget)) {
            captureStringList(tree, SourceHeaderProperty, RenderedHeaderProperty, treeHeaderTexts(tree));
        } else if (auto *table = qobject_cast<QTableWidget *>(widget)) {
            captureStringList(table, SourceHeaderProperty, RenderedHeaderProperty, tableHeaderTexts(table));
        }

        if (auto *spin = qobject_cast<QSpinBox *>(widget)) {
            captureTextProperty(spin, SourcePrefixProperty, RenderedPrefixProperty, spin->prefix());
            captureTextProperty(spin, SourceSuffixProperty, RenderedSuffixProperty, spin->suffix());
        } else if (auto *spin = qobject_cast<QDoubleSpinBox *>(widget)) {
            captureTextProperty(spin, SourcePrefixProperty, RenderedPrefixProperty, spin->prefix());
            captureTextProperty(spin, SourceSuffixProperty, RenderedSuffixProperty, spin->suffix());
        }
        if (auto *statusBar = qobject_cast<QStatusBar *>(widget)) {
            captureTextProperty(statusBar, SourceStatusMessageProperty, RenderedStatusMessageProperty,
                                statusBar->currentMessage());
        }
        if (auto *progress = qobject_cast<QProgressBar *>(widget)) {
            captureTextProperty(progress, SourceProgressFormatProperty, RenderedProgressFormatProperty,
                                progress->format());
        }
    }

    QList<QAction *> actions = root->findChildren<QAction *>();
    for (QAction *action : root->actions()) {
        if (!actions.contains(action))
            actions.append(action);
    }
    for (QAction *action : actions)
        captureAction(action);

    const auto models = root->findChildren<QAbstractItemModel *>();
    for (QAbstractItemModel *model : models)
        captureModelHeaders(model);
}

void TranslationManager::retranslateWidgetTree(QWidget *root)
{
    if (!root)
        return;
    captureWidgetTree(root);
    QList<QWidget *> widgets{root};
    widgets.append(root->findChildren<QWidget *>());
    for (QWidget *widget : widgets) {
        retranslateWidgetMetadata(widget);
        if (auto *label = qobject_cast<QLabel *>(widget))
            retranslateTextProperty(label, SourceTextProperty, RenderedTextProperty, label->text(),
                                    [label](const QString &value) { label->setText(value); });
        else if (auto *button = qobject_cast<QAbstractButton *>(widget))
            retranslateTextProperty(button, SourceTextProperty, RenderedTextProperty, button->text(),
                                    [button](const QString &value) { button->setText(value); });
        else if (auto *group = qobject_cast<QGroupBox *>(widget))
            retranslateTextProperty(group, SourceTextProperty, RenderedTextProperty, group->title(),
                                    [group](const QString &value) { group->setTitle(value); });
        else if (auto *menu = qobject_cast<QMenu *>(widget))
            retranslateTextProperty(menu, SourceTextProperty, RenderedTextProperty, menu->title(),
                                    [menu](const QString &value) { menu->setTitle(value); });
        else if (auto *wizardPage = qobject_cast<QWizardPage *>(widget)) {
            retranslateTextProperty(wizardPage, SourceTextProperty, RenderedTextProperty, wizardPage->title(),
                                    [wizardPage](const QString &value) { wizardPage->setTitle(value); });
            retranslateTextProperty(wizardPage, SourceWizardSubtitleProperty, RenderedWizardSubtitleProperty,
                                    wizardPage->subTitle(),
                                    [wizardPage](const QString &value) { wizardPage->setSubTitle(value); });
        }

        if (auto *lineEdit = qobject_cast<QLineEdit *>(widget))
            retranslateTextProperty(lineEdit, SourcePlaceholderProperty, RenderedPlaceholderProperty,
                                    lineEdit->placeholderText(),
                                    [lineEdit](const QString &value) { lineEdit->setPlaceholderText(value); });
        else if (auto *plain = qobject_cast<QPlainTextEdit *>(widget))
            retranslateTextProperty(plain, SourcePlaceholderProperty, RenderedPlaceholderProperty,
                                    plain->placeholderText(),
                                    [plain](const QString &value) { plain->setPlaceholderText(value); });
        else if (auto *text = qobject_cast<QTextEdit *>(widget))
            retranslateTextProperty(text, SourcePlaceholderProperty, RenderedPlaceholderProperty,
                                    text->placeholderText(),
                                    [text](const QString &value) { text->setPlaceholderText(value); });

        if (auto *combo = qobject_cast<QComboBox *>(widget)) {
            retranslateStringList(combo, SourceItemTextProperty, RenderedItemTextProperty, comboBoxItems(combo),
                                  [combo](int index, const QString &value) { combo->setItemText(index, value); });
        } else if (auto *list = qobject_cast<QListWidget *>(widget)) {
            retranslateStringList(list, SourceItemTextProperty, RenderedItemTextProperty, listWidgetItems(list),
                                  [list](int index, const QString &value) {
                                      if (QListWidgetItem *item = list->item(index))
                                          item->setText(value);
                                  });
        } else if (auto *tabs = qobject_cast<QTabWidget *>(widget)) {
            retranslateStringList(tabs, SourceItemTextProperty, RenderedItemTextProperty, tabTexts(tabs),
                                  [tabs](int index, const QString &value) { tabs->setTabText(index, value); });
            retranslateStringList(tabs, SourceItemToolTipProperty, RenderedItemToolTipProperty, tabToolTips(tabs),
                                  [tabs](int index, const QString &value) { tabs->setTabToolTip(index, value); });
            retranslateStringList(tabs, SourceItemWhatsThisProperty, RenderedItemWhatsThisProperty,
                                  tabWhatsThis(tabs),
                                  [tabs](int index, const QString &value) { tabs->setTabWhatsThis(index, value); });
        } else if (auto *toolBox = qobject_cast<QToolBox *>(widget)) {
            retranslateStringList(toolBox, SourceItemTextProperty, RenderedItemTextProperty,
                                  toolBoxTexts(toolBox),
                                  [toolBox](int index, const QString &value) { toolBox->setItemText(index, value); });
            retranslateStringList(toolBox, SourceItemToolTipProperty, RenderedItemToolTipProperty,
                                  toolBoxToolTips(toolBox),
                                  [toolBox](int index, const QString &value) { toolBox->setItemToolTip(index, value); });
        } else if (auto *tree = qobject_cast<QTreeWidget *>(widget)) {
            retranslateStringList(tree, SourceHeaderProperty, RenderedHeaderProperty, treeHeaderTexts(tree),
                                  [tree](int index, const QString &value) {
                                      if (QTreeWidgetItem *header = tree->headerItem())
                                          header->setText(index, value);
                                  });
        } else if (auto *table = qobject_cast<QTableWidget *>(widget)) {
            retranslateStringList(table, SourceHeaderProperty, RenderedHeaderProperty, tableHeaderTexts(table),
                                  [table](int index, const QString &value) {
                                      if (QTableWidgetItem *item = table->horizontalHeaderItem(index))
                                          item->setText(value);
                                  });
        }

        if (auto *spin = qobject_cast<QSpinBox *>(widget)) {
            retranslateTextProperty(spin, SourcePrefixProperty, RenderedPrefixProperty, spin->prefix(),
                                    [spin](const QString &value) { spin->setPrefix(value); });
            retranslateTextProperty(spin, SourceSuffixProperty, RenderedSuffixProperty, spin->suffix(),
                                    [spin](const QString &value) { spin->setSuffix(value); });
        } else if (auto *spin = qobject_cast<QDoubleSpinBox *>(widget)) {
            retranslateTextProperty(spin, SourcePrefixProperty, RenderedPrefixProperty, spin->prefix(),
                                    [spin](const QString &value) { spin->setPrefix(value); });
            retranslateTextProperty(spin, SourceSuffixProperty, RenderedSuffixProperty, spin->suffix(),
                                    [spin](const QString &value) { spin->setSuffix(value); });
        }
        if (auto *statusBar = qobject_cast<QStatusBar *>(widget)) {
            retranslateTextProperty(statusBar, SourceStatusMessageProperty, RenderedStatusMessageProperty,
                                    statusBar->currentMessage(),
                                    [statusBar](const QString &value) { statusBar->showMessage(value); });
        }
        if (auto *progress = qobject_cast<QProgressBar *>(widget)) {
            retranslateTextProperty(progress, SourceProgressFormatProperty, RenderedProgressFormatProperty,
                                    progress->format(),
                                    [progress](const QString &value) { progress->setFormat(value); });
        }
    }

    QList<QAction *> actions = root->findChildren<QAction *>();
    for (QAction *action : root->actions()) {
        if (!actions.contains(action))
            actions.append(action);
    }
    for (QAction *action : actions)
        retranslateAction(action);

    const auto models = root->findChildren<QAbstractItemModel *>();
    for (QAbstractItemModel *model : models)
        retranslateModelHeaders(model);
}

} // namespace sc2dh::app
