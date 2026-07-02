#include "app/ModalBackdrop.h"

#include <QAbstractAnimation>
#include <QEasingCurve>
#include <QEvent>
#include <QGraphicsBlurEffect>
#include <QGraphicsOpacityEffect>
#include <QMainWindow>
#include <QPropertyAnimation>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QWidget>

#include <utility>

namespace sc2dh::app
{
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

    for (QWidget *target : findBlurTargets(window))
    {
        if (!target || target->graphicsEffect())
            continue;

        auto *blur = new QGraphicsBlurEffect(target);
        blur->setBlurRadius(0.0);
        blur->setBlurHints(QGraphicsBlurEffect::PerformanceHint);
        target->setGraphicsEffect(blur);
        m_blurTargets.push_back(target);

        QTimer::singleShot(45, blur, [blur]
        {
            auto *animation = new QPropertyAnimation(blur, "blurRadius", blur);
            animation->setDuration(220);
            animation->setStartValue(0.0);
            animation->setEndValue(3.5);
            animation->setEasingCurve(QEasingCurve::OutCubic);
            animation->start(QAbstractAnimation::DeleteWhenStopped);
        });
    }

    QTimer::singleShot(35, m_overlay, [overlayOpacity]
    {
        auto *animation = new QPropertyAnimation(overlayOpacity, "opacity", overlayOpacity);
        animation->setDuration(220);
        animation->setStartValue(0.0);
        animation->setEndValue(1.0);
        animation->setEasingCurve(QEasingCurve::OutCubic);
        animation->start(QAbstractAnimation::DeleteWhenStopped);
    });

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

    dialog->setWindowOpacity(0.0);
    QTimer::singleShot(0, dialog, [dialog]
    {
        auto *animation = new QPropertyAnimation(dialog, "windowOpacity", dialog);
        animation->setDuration(150);
        animation->setStartValue(0.0);
        animation->setEndValue(1.0);
        animation->setEasingCurve(QEasingCurve::OutCubic);
        animation->start(QAbstractAnimation::DeleteWhenStopped);
    });
}
}
