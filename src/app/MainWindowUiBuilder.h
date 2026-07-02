#pragma once

class MainWindow;

namespace sc2dh::app
{
class MainWindowUiBuilder
{
public:
    explicit MainWindowUiBuilder(MainWindow &window);
    void build();

private:
    MainWindow &m_window;
};
}

