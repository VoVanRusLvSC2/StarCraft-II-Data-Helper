#include "app/ModalBackdrop.h"

#include <QEasingCurve>
#include <QElapsedTimer>
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QMainWindow>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QWidget>

#include <functional>
#include <utility>

namespace sc2dh::app
{
namespace
{
constexpr int kModalFrameMs = 33;
constexpr int kModalOpenMs = 300;
constexpr qreal kModalStartOpacity = 0.08;

void animateOpacity(QObject *owner, const std::function<void(qreal)> &setOpacity,
                    qreal startOpacity, qreal endOpacity, int durationMs)
{
    if (!owner)
        return;

    setOpacity(startOpacity);
    auto *timer = new QTimer(owner);
    auto *elapsed = new QElapsedTimer;
    const QEasingCurve curve(QEasingCurve::OutCubic);
    QObject::connect(timer, &QObject::destroyed, timer, [elapsed]
    {
        delete elapsed;
    });
    elapsed->start();
    timer->setTimerType(Qt::PreciseTimer);
    timer->setInterval(kModalFrameMs);
    QObject::connect(timer, &QTimer::timeout, owner, [timer, elapsed, curve, setOpacity, startOpacity, endOpacity, durationMs]
    {
        const qreal progress = qBound<qreal>(0.0, qreal(elapsed->elapsed()) / qreal(durationMs), 1.0);
        const qreal eased = curve.valueForProgress(progress);
        setOpacity(startOpacity + (endOpacity - startOpacity) * eased);
        if (progress >= 1.0)
        {
            setOpacity(endOpacity);
            timer->stop();
            timer->deleteLater();
        }
    });
    timer->start();
}
}

ScopedModalBackdrop::ScopedModalBackdrop(QWidget *parent)
{
    QWidget *window = parent ? parent->window() : nullptr;
    if (!window)
        return;

    m_window = window;
    m_overlay = new QWidget(window);
    m_overlay->setObjectName(QStringLiteral("modalBackdropOverlay"));
    m_overlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_overlay->setFocusPolicy(Qt::NoFocus);
    m_overlay->setAutoFillBackground(false);
    auto *overlayOpacity = new QGraphicsOpacityEffect(m_overlay);
    overlayOpacity->setOpacity(0.0);
    m_overlay->setGraphicsEffect(overlayOpacity);
    syncGeometry();
    m_overlay->raise();
    m_overlay->show();

    animateOpacity(overlayOpacity, [overlayOpacity](qreal opacity)
    {
        overlayOpacity->setOpacity(opacity);
    }, 0.0, 1.0, 220);

    window->installEventFilter(this);
}

ScopedModalBackdrop::~ScopedModalBackdrop()
{
    if (m_window)
        m_window->removeEventFilter(this);
    for (const QPointer<QWidget> &target : std::as_const(m_blurTargets))
    {
        if (target)
            target->setGraphicsEffect(nullptr);
    }
    if (m_overlay)
        delete m_overlay;
}

bool ScopedModalBackdrop::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_window && event
        && (event->type() == QEvent::Resize || event->type() == QEvent::Move || event->type() == QEvent::Show))
        syncGeometry();
    return QObject::eventFilter(watched, event);
}

void ScopedModalBackdrop::syncGeometry()
{
    if (!m_window || !m_overlay)
        return;
    m_overlay->setGeometry(m_window->rect());
    m_overlay->raise();
}

QVector<QWidget *> ScopedModalBackdrop::findBlurTargets(QWidget *window) const
{
    QVector<QWidget *> targets;
    if (!window)
        return targets;

    if (auto *mainWindow = qobject_cast<QMainWindow *>(window))
    {
        addBlurTarget(targets, mainWindow->findChild<QToolBar *>(QStringLiteral("mainToolbar")));
        addBlurTarget(targets, window->findChild<QWidget *>(QStringLiteral("workspaceRoot")));
        if (targets.size() < 2)
            addBlurTarget(targets, mainWindow->centralWidget());
        addBlurTarget(targets, mainWindow->findChild<QStatusBar *>());
        return targets;
    }

    if (auto *workspaceRoot = window->findChild<QWidget *>(QStringLiteral("workspaceRoot")))
        addBlurTarget(targets, workspaceRoot);
    return targets;
}

void ScopedModalBackdrop::addBlurTarget(QVector<QWidget *> &targets, QWidget *candidate) const
{
    if (!candidate || candidate == m_overlay || !candidate->isVisible())
        return;
    for (QWidget *target : std::as_const(targets))
    {
        if (target == candidate || (target && target->isAncestorOf(candidate)))
            return;
    }
    for (int index = targets.size() - 1; index >= 0; --index)
    {
        QWidget *target = targets.at(index);
        if (candidate->isAncestorOf(target))
            targets.removeAt(index);
    }
    targets.push_back(candidate);
}

void animateModalOpen(QWidget *dialog)
{
    if (!dialog)
        return;

    dialog->setWindowOpacity(kModalStartOpacity);
    QTimer::singleShot(0, dialog, [dialog]
    {
        animateOpacity(dialog, [dialog](qreal opacity)
        {
            dialog->setWindowOpacity(opacity);
        }, kModalStartOpacity, 1.0, kModalOpenMs);
    });
}
}
