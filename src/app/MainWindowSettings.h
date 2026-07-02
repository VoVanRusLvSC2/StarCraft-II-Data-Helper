#pragma once

class MainWindow;

namespace sc2dh::app
{
class MainWindowSettings
{
public:
    explicit MainWindowSettings(MainWindow &window);
    void show();

private:
    MainWindow &m_window;
};
}

