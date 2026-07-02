#pragma once

#include <QObject>
#include <QPointer>
#include <QVector>

class QEvent;
class QWidget;

namespace sc2dh::app
{
class ScopedModalBackdrop final : public QObject
{
public:
    explicit ScopedModalBackdrop(QWidget *parent);
    ~ScopedModalBackdrop() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void syncGeometry();
    QVector<QWidget *> findBlurTargets(QWidget *window) const;
    void addBlurTarget(QVector<QWidget *> &targets, QWidget *candidate) const;

    QPointer<QWidget> m_window;
    QPointer<QWidget> m_overlay;
    QVector<QPointer<QWidget>> m_blurTargets;
};

void animateModalOpen(QWidget *dialog);
}
