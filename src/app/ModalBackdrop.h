#pragma once

#include <QObject>
#include <QPointer>

class QGraphicsEffect;
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
    QWidget *findBlurTarget(QWidget *window) const;

    QPointer<QWidget> m_window;
    QPointer<QWidget> m_overlay;
    QPointer<QWidget> m_blurTarget;
    bool m_appliedBlur = false;
};

void animateModalOpen(QWidget *dialog);
}
