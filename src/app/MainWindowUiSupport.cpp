#include "app/MainWindowUiSupport.h"

#include <QAbstractButton>
#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFile>
#include <QGraphicsDropShadowEffect>
#include <QHash>
#include <QHelpEvent>
#include <QIODevice>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QRadialGradient>
#include <QSettings>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QWidget>
#include <QEasingCurve>

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmsystem.h>
#endif

#include <cmath>

namespace
{
    class PersistentTabToolTipFilter final : public QObject
    {
    public:
        using QObject::QObject;

    protected:
        bool eventFilter(QObject *watched, QEvent *event) override
        {
            auto *bar = qobject_cast<QTabBar *>(watched);
            if (!bar)
                return QObject::eventFilter(watched, event);
            if (event->type() == QEvent::FocusIn) {
                bar->clearFocus();
                return false;
            }
            if (event->type() != QEvent::ToolTip && event->type() != QEvent::MouseMove)
                return QObject::eventFilter(watched, event);
            QPoint position;
            if (auto *help = dynamic_cast<QHelpEvent *>(event))
                position = help->pos();
            else
                position = static_cast<QMouseEvent *>(event)->position().toPoint();
            const int index = bar->tabAt(position);
            if (index >= 0)
            {
                const QString text = bar->tabText(index).isEmpty() ? bar->tabToolTip(index) : bar->tabText(index);
                QToolTip::showText(bar->mapToGlobal(position), text, bar, bar->tabRect(index), 60000);
            }
            return event->type() == QEvent::ToolTip;
        }
    };
    void drawCoverPixmap(QPainter &painter, const QRect &target, const QPixmap &pixmap)
    {
        if (target.isEmpty() || pixmap.isNull())
            return;
        const qreal targetRatio = qreal(target.width()) / qMax(1, target.height());
        const qreal sourceRatio = qreal(pixmap.width()) / qMax(1, pixmap.height());
        QRect source = pixmap.rect();
        if (sourceRatio > targetRatio)
        {
            const int width = qRound(pixmap.height() * targetRatio);
            source.setLeft((pixmap.width() - width) / 2);
            source.setWidth(width);
        }
        else
        {
            const int height = qRound(pixmap.width() / targetRatio);
            source.setTop((pixmap.height() - height) / 2);
            source.setHeight(height);
        }
        painter.drawPixmap(target, pixmap, source);
    }

    class Sc2BackgroundWidget final : public QWidget
    {
    public:
        explicit Sc2BackgroundWidget(QWidget *parent = nullptr)
            : QWidget(parent),
              m_lights(QStringLiteral(":/textures/ui_nova_login_backgroundlights.png")),
              m_frame(QStringLiteral(":/textures/ui_nova_archives_backgroundframe.png")),
              m_highlight(QStringLiteral(":/textures/ui_nova_archives_backgroundframehighlight.png")),
              m_scanlines(QStringLiteral(":/textures/ui_nova_archives_backgroundframe_scanlines.png"))
        {
            setAttribute(Qt::WA_StyledBackground, true);
            auto *timer = new QTimer(this);
            connect(timer, &QTimer::timeout, this, [this]()
                    {
                m_glowPhase += 0.035;
                if (m_glowPhase > 1000.0)
                    m_glowPhase = 0.0;
                if (QSettings().value(QStringLiteral("ui/backgroundGlows"), true).toBool())
                    update(); });
            timer->start(70);
        }

    protected:
        void paintEvent(QPaintEvent *event) override
        {
            QPainter painter(this);
            painter.fillRect(event->rect(), QColor(5, 11, 16));

            QLinearGradient baseGradient(rect().topLeft(), rect().bottomRight());
            baseGradient.setColorAt(0.0, QColor(4, 26, 28, 120));
            baseGradient.setColorAt(0.48, QColor(2, 12, 17, 95));
            baseGradient.setColorAt(1.0, QColor(0, 5, 10, 130));
            painter.fillRect(event->rect(), baseGradient);

            if (QSettings().value(QStringLiteral("ui/backgroundGlows"), true).toBool())
                drawBackgroundGlows(painter);

            painter.setOpacity(0.13);
            drawCoverPixmap(painter, rect(), m_lights);
            painter.setOpacity(0.20);
            drawCoverPixmap(painter, rect(), m_frame);
            painter.setOpacity(0.08);
            drawCoverPixmap(painter, rect(), m_highlight);
            painter.setOpacity(0.035);
            drawCoverPixmap(painter, rect(), m_scanlines);
        }

    private:
        void drawBackgroundGlows(QPainter &painter)
        {
            const QRectF area = rect();
            const qreal w = qMax<qreal>(1.0, area.width());
            const qreal h = qMax<qreal>(1.0, area.height());
            const qreal radius = qMax(w, h) * 0.26;
            const auto drawGlow = [&painter](const QPointF &center, qreal glowRadius, QColor color, qreal alpha)
            {
                color.setAlphaF(qBound<qreal>(0.0, alpha, 1.0));
                QColor edge = color;
                edge.setAlpha(0);
                QRadialGradient gradient(center, glowRadius);
                gradient.setColorAt(0.0, color);
                gradient.setColorAt(0.55, QColor(color.red(), color.green(), color.blue(), qRound(color.alpha() * 0.28)));
                gradient.setColorAt(1.0, edge);
                painter.fillRect(QRectF(center.x() - glowRadius, center.y() - glowRadius,
                                        glowRadius * 2.0, glowRadius * 2.0),
                                 gradient);
            };

            painter.save();
            painter.setCompositionMode(QPainter::CompositionMode_Screen);
            const qreal p = m_glowPhase;
            drawGlow(QPointF(w * (0.18 + 0.025 * std::sin(p * 0.9)),
                             h * (0.22 + 0.035 * std::cos(p * 0.7))),
                     radius, QColor(0, 122, 255), 0.16);
            drawGlow(QPointF(w * (0.78 + 0.020 * std::cos(p * 0.6)),
                             h * (0.36 + 0.045 * std::sin(p * 0.8))),
                     radius * 0.82, QColor(0, 190, 255), 0.12);
            drawGlow(QPointF(w * (0.54 + 0.035 * std::sin(p * 0.45)),
                             h * (0.84 + 0.020 * std::cos(p * 0.5))),
                     radius * 0.72, QColor(23, 88, 255), 0.10);
            painter.restore();
        }

        qreal m_glowPhase = 0.0;
        QPixmap m_lights;
        QPixmap m_frame;
        QPixmap m_highlight;
        QPixmap m_scanlines;
    };

    class ButtonEffects : public QObject
    {
    public:
        explicit ButtonEffects(QObject *parent = nullptr)
            : QObject(parent)
        {
            QFile soundFile(QStringLiteral(":/sounds/UI_NovaUT_ButtonClick1.wav"));
            if (soundFile.open(QIODevice::ReadOnly))
            {
                m_soundData = soundFile.readAll();
            }
        }

        void installOn(QWidget *widget)
        {
            if (!widget)
            {
                return;
            }
            widget->installEventFilter(this);
        }

    protected:
        bool eventFilter(QObject *watched, QEvent *event) override
        {
            auto *button = qobject_cast<QAbstractButton *>(watched);
            if (!button || !button->isEnabled())
            {
                return QObject::eventFilter(watched, event);
            }

            if (event->type() == QEvent::MouseButtonRelease)
            {
                auto *mouseEvent = static_cast<QMouseEvent *>(event);
                if (mouseEvent->button() == Qt::LeftButton)
                {
                    playClick(button);
                }
            }
            return QObject::eventFilter(watched, event);
        }

    private:
        void playClick(QAbstractButton *button)
        {
            QSettings settings;
#ifdef Q_OS_WIN
            if (settings.value(QStringLiteral("ui/buttonSounds"), true).toBool() && !m_soundData.isEmpty())
            {
                PlaySoundA(reinterpret_cast<LPCSTR>(m_soundData.constData()), nullptr,
                           SND_ASYNC | SND_MEMORY | SND_NODEFAULT | SND_NOSTOP);
            }
#else
            QApplication::beep();
#endif

            if (!settings.value(QStringLiteral("ui/buttonAnimations"), true).toBool())
            {
                return;
            }

            // Pulse the text only. A graphics effect on the whole button also
            // glows the texture border and produces unwanted extra colors.
            button->setProperty("textPulse", true);
            button->style()->unpolish(button);
            button->style()->polish(button);
            QTimer::singleShot(150, button, [button]
                               {
                button->setProperty("textPulse", false);
                button->style()->unpolish(button);
                button->style()->polish(button); });
        }

        QByteArray m_soundData;
    };

    class PromoButtonAnimator final : public QObject
    {
    public:
        explicit PromoButtonAnimator(QObject *parent = nullptr)
            : QObject(parent)
        {
        }

        void installOn(QToolButton *button, const QSize &normalIcon, const QSize &hoverIcon, const QColor &glowColor)
        {
            if (!button)
                return;
            auto *effect = new QGraphicsDropShadowEffect(button);
            effect->setOffset(0, 0);
            effect->setBlurRadius(0);
            effect->setColor(QColor(glowColor.red(), glowColor.green(), glowColor.blue(), 0));
            button->setGraphicsEffect(effect);

            State state;
            state.effect = effect;
            state.normalIcon = normalIcon;
            state.hoverIcon = hoverIcon;
            state.pressIcon = !normalIcon.isEmpty()
                                  ? QSize(qMax(1, int(normalIcon.width() * 0.88)), qMax(1, int(normalIcon.height() * 0.88)))
                                  : QSize();
            state.glowColor = glowColor;
            m_states.insert(button, state);
            button->installEventFilter(this);
        }

    protected:
        bool eventFilter(QObject *watched, QEvent *event) override
        {
            auto it = m_states.find(watched);
            if (it == m_states.end())
                return QObject::eventFilter(watched, event);
            auto *button = qobject_cast<QToolButton *>(watched);
            if (!button || !button->isEnabled())
                return QObject::eventFilter(watched, event);

            State &state = it.value();
            switch (event->type())
            {
            case QEvent::Enter:
                state.hovering = true;
                animateGlow(button, 18.0);
                animateIcon(button, state.hoverIcon);
                break;
            case QEvent::Leave:
                state.hovering = false;
                animateGlow(button, 0.0);
                animateIcon(button, state.normalIcon);
                break;
            case QEvent::MouseButtonPress:
                animateGlow(button, 27.0);
                animateIcon(button, state.pressIcon);
                break;
            case QEvent::MouseButtonRelease:
                animateGlow(button, state.hovering ? 18.0 : 0.0);
                animateIcon(button, state.hovering ? state.hoverIcon : state.normalIcon);
                break;
            default:
                break;
            }
            return QObject::eventFilter(watched, event);
        }

    private:
        struct State
        {
            QGraphicsDropShadowEffect *effect = nullptr;
            QSize normalIcon;
            QSize hoverIcon;
            QSize pressIcon;
            QColor glowColor;
            bool hovering = false;
        };

        void animateGlow(QToolButton *button, qreal target)
        {
            State &state = m_states[button];
            if (!state.effect)
                return;
            state.effect->setColor(target > 0.0
                                       ? state.glowColor
                                       : QColor(state.glowColor.red(), state.glowColor.green(), state.glowColor.blue(), 0));
            auto *animation = new QPropertyAnimation(state.effect, "blurRadius", state.effect);
            animation->setDuration(160);
            animation->setStartValue(state.effect->blurRadius());
            animation->setEndValue(target);
            animation->setEasingCurve(QEasingCurve::OutCubic);
            connect(animation, &QPropertyAnimation::finished, animation, &QObject::deleteLater);
            animation->start();
        }

        void animateIcon(QToolButton *button, const QSize &target)
        {
            if (!button || target.isEmpty())
                return;
            auto *animation = new QPropertyAnimation(button, "iconSize", button);
            animation->setDuration(130);
            animation->setStartValue(button->iconSize());
            animation->setEndValue(target);
            animation->setEasingCurve(QEasingCurve::OutBack);
            connect(animation, &QPropertyAnimation::finished, animation, &QObject::deleteLater);
            animation->start();
        }

        QHash<QObject *, State> m_states;
    };

}

namespace sc2dh::app
{
    const char *discordInviteUrl()
    {
        return "https://discord.com/invite/UKYgsB6Zrx";
    }

    const char *boostyUrl()
    {
        return "https://boosty.to/vovanruslvsc2/donate";
    }

    QWidget *createSc2BackgroundWidget(QWidget *parent)
    {
        return new Sc2BackgroundWidget(parent);
    }

    void installPersistentTabToolTips(QTabBar *tabBar)
    {
        if (!tabBar)
            return;
        tabBar->installEventFilter(new PersistentTabToolTipFilter(tabBar));
    }

    QObject *installButtonEffects(QObject *owner, const QList<QAbstractButton *> &buttons)
    {
        auto *effects = new ButtonEffects(owner);
        for (QAbstractButton *button : buttons)
            effects->installOn(button);
        return effects;
    }

    QObject *installPromoButtonAnimations(QObject *owner, QToolButton *discordButton, QToolButton *boostyButton)
    {
        auto *promoAnimator = new PromoButtonAnimator(owner);
        promoAnimator->installOn(discordButton, QSize(30, 30), QSize(30, 30), QColor(88, 190, 255, 220));
        promoAnimator->installOn(boostyButton, QSize(), QSize(), QColor(255, 176, 74, 230));
        return promoAnimator;
    }
}

