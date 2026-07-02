#include "app/ModalBackdrop.h"

#include <QApplication>
#include <QEvent>
#include <QEventLoop>
#include <QGraphicsBlurEffect>
#include <QMainWindow>
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
    syncGeometry();
    m_overlay->raise();
    m_overlay->show();

    m_blurTarget = findBlurTarget(window);
    if (m_blurTarget && !m_blurTarget->graphicsEffect())
    {
        auto *blur = new QGraphicsBlurEffect(m_blurTarget);
        blur->setBlurRadius(3.5);
        blur->setBlurHints(QGraphicsBlurEffect::PerformanceHint);
        m_blurTarget->setGraphicsEffect(blur);
        m_appliedBlur = true;
    }

    window->installEventFilter(this);
    qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
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
}
