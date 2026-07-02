#pragma once

class MainWindow;

namespace sc2dh::app
{
class OptimizationWizardController
{
public:
    explicit OptimizationWizardController(MainWindow &window);
    void applyPlan();

private:
    MainWindow &m_window;
};
}

