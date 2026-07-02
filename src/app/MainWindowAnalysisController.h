#pragma once

#include <QString>

class MainWindow;

namespace sc2dh::app
{
class MainWindowAnalysisController
{
public:
    explicit MainWindowAnalysisController(MainWindow &window);
    void analyzeCurrentSource();
    bool loadPathAndAnalyze(const QString &path);

private:
    MainWindow &m_window;
};
}

