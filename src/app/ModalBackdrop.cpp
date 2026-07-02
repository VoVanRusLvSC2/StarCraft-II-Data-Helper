#include "app/ModalBackdrop.h"

#include <QAbstractAnimation>
#include <QEasingCurve>
#include <QEvent>
#include <QGraphicsBlurEffect>
#include <QGraphicsOpacityEffect>
#include <QMainWindow>
#include <QPropertyAnimation>
#include <QTimer>
#include <QWidget>

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

    m_blurTarget = findBlurTarget(window);
    if (m_blurTarget && !m_blurTarget->graphicsEffect())
    {
        auto *blur = new QGraphicsBlurEffect(m_blurTarget);
        blur->setBlurRadius(0.0);
        blur->setBlurHints(QGraphicsBlurEffect::PerformanceHint);
        m_blurTarget->setGraphicsEffect(blur);
        m_appliedBlur = true;

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
    if (m_blurTarget && m_appliedBlur)
        m_blurTarget->setGraphicsEffect(nullptr);
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

QWidget *ScopedModalBackdrop::findBlurTarget(QWidget *window) const
{
    if (!window)
        return nullptr;
    if (auto *workspaceRoot = window->findChild<QWidget *>(QStringLiteral("workspaceRoot")))
        return workspaceRoot;
    if (auto *mainWindow = qobject_cast<QMainWindow *>(window))
        return mainWindow->centralWidget();
    return nullptr;
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
