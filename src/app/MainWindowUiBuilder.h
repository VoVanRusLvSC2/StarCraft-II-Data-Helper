#pragma once

class MainWindow;

namespace sc2dh::app
{
class MainWindowUiBuilder
{
public:
    explicit MainWindowUiBuilder(MainWindow &window);
    void build();
    static void retranslate(MainWindow &window);

private:
    MainWindow &m_window;
};
}
