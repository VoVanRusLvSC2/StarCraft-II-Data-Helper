#pragma once

class QApplication;
class QString;

class ThemeManager
{
public:
    static bool applyDarkTheme(QApplication *application, QString *loadedFrom = nullptr, QString *errorMessage = nullptr);
};
