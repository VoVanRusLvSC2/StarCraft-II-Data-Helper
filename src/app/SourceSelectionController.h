#pragma once

class MainWindow;

namespace sc2dh::app
{
class SourceSelectionController
{
public:
    explicit SourceSelectionController(MainWindow &window);
    void openSc2File();
    void openSourceFolder();

private:
    MainWindow &m_window;
};
}

