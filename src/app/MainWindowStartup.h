#pragma once

class MainWindow;

namespace sc2dh::app
{
class MainWindowStartup
{
public:
    explicit MainWindowStartup(MainWindow &window);
    void initialize();

private:
    MainWindow &m_window;
};
}

