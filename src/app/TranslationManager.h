#pragma once

#include <QObject>
#include <QString>
#include <QTranslator>

class QWidget;

namespace sc2dh::app
{

class TranslationManager final : public QObject
{
    Q_OBJECT

public:
    static TranslationManager &instance();

    QString selectedLanguage() const { return m_selectedLanguage; }
    QString resolvedLanguage() const { return m_resolvedLanguage; }
    bool setLanguage(const QString &languageCode, bool persist = true);

    // Use this for UI-owned text assembled at runtime.  Map/catalog values
    // must remain domain data and must not be passed through this helper.
    static QString translateDynamicUi(const char *source);
    static void captureWidgetTree(QWidget *root);
    static void retranslateWidgetTree(QWidget *root);

signals:
    void languageChanged(const QString &languageCode);

private:
    explicit TranslationManager(QObject *parent = nullptr);
    QString resolveLanguage(const QString &languageCode) const;

    QTranslator m_translator;
    QString m_selectedLanguage = QStringLiteral("system");
    QString m_resolvedLanguage = QStringLiteral("enUS");
};

} // namespace sc2dh::app
