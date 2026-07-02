#include "app/Sc2MessageDialog.h"

#include "app/ModalBackdrop.h"

#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>

namespace sc2dh::app
{
    class Sc2DialogDragFilter final : public QObject
    {
    public:
        Sc2DialogDragFilter(QDialog *dialog, QPoint *dragPosition, bool *dragging)
            : QObject(dialog), m_dialog(dialog), m_dragPosition(dragPosition), m_dragging(dragging)
        {
        }

    protected:
        bool eventFilter(QObject *watched, QEvent *event) override
        {
            Q_UNUSED(watched);
            if (!m_dialog || !m_dragPosition || !m_dragging)
                return false;
            if (event->type() == QEvent::MouseButtonPress) {
                auto *mouse = static_cast<QMouseEvent *>(event);
                if (mouse->button() == Qt::LeftButton) {
                    *m_dragPosition = mouse->globalPosition().toPoint() - m_dialog->frameGeometry().topLeft();
                    *m_dragging = true;
                    return true;
                }
            } else if (event->type() == QEvent::MouseMove) {
                auto *mouse = static_cast<QMouseEvent *>(event);
                if (*m_dragging && (mouse->buttons() & Qt::LeftButton)) {
                    m_dialog->move(mouse->globalPosition().toPoint() - *m_dragPosition);
                    return true;
                }
            } else if (event->type() == QEvent::MouseButtonRelease) {
                *m_dragging = false;
            }
            return false;
        }

    private:
        QDialog *m_dialog = nullptr;
        QPoint *m_dragPosition = nullptr;
        bool *m_dragging = nullptr;
    };

    QMessageBox::StandardButton showSc2MessageDialog(QWidget *parent,
                                                     QMessageBox::Icon icon,
                                                     const QString &title,
                                                     const QString &message,
                                                     QMessageBox::StandardButtons buttons,
                                                     int minimumWidth)
    {
        QDialog dialog(parent);
        dialog.setObjectName(QStringLiteral("sc2MessageDialog"));
        dialog.setWindowTitle(title);
        dialog.setWindowIcon(QIcon(QStringLiteral(":/icons/Icon.png")));
        dialog.setWindowFlags((dialog.windowFlags() | Qt::FramelessWindowHint) & ~Qt::WindowContextHelpButtonHint);
        dialog.setModal(true);
        dialog.setMinimumWidth(minimumWidth);

        auto *layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(12, 10, 12, 12);
        layout->setSpacing(10);

        auto *titleBar = new QFrame(&dialog);
        titleBar->setObjectName(QStringLiteral("sc2MessageTitleBar"));
        auto *titleLayout = new QHBoxLayout(titleBar);
        titleLayout->setContentsMargins(8, 4, 4, 4);
        titleLayout->setSpacing(8);
        auto *appIcon = new QLabel(titleBar);
        appIcon->setPixmap(QPixmap(QStringLiteral(":/icons/Icon.png")).scaled(18, 18, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        auto *titleLabel = new QLabel(title, titleBar);
        titleLabel->setObjectName(QStringLiteral("sc2MessageTitle"));
        auto *closeButton = new QPushButton(QStringLiteral("X"), titleBar);
        closeButton->setObjectName(QStringLiteral("sc2MessageCloseButton"));
        closeButton->setFixedSize(38, 32);
        closeButton->setFocusPolicy(Qt::NoFocus);
        titleLayout->addWidget(appIcon);
        titleLayout->addWidget(titleLabel, 1);
        titleLayout->addWidget(closeButton);
        layout->addWidget(titleBar);

        auto *body = new QHBoxLayout;
        body->setContentsMargins(4, 2, 4, 0);
        body->setSpacing(14);
        auto *iconLabel = new QLabel(&dialog);
        iconLabel->setObjectName(QStringLiteral("sc2MessageIcon"));
        const QStyle::StandardPixmap standardIcon = icon == QMessageBox::Critical ? QStyle::SP_MessageBoxCritical
            : icon == QMessageBox::Warning ? QStyle::SP_MessageBoxWarning
            : icon == QMessageBox::Question ? QStyle::SP_MessageBoxQuestion
                                            : QStyle::SP_MessageBoxInformation;
        iconLabel->setPixmap(dialog.style()->standardIcon(standardIcon).pixmap(34, 34));
        iconLabel->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
        body->addWidget(iconLabel);

        auto *scroll = new QScrollArea(&dialog);
        scroll->setObjectName(QStringLiteral("sc2MessageScroll"));
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *textLabel = new QLabel(message, scroll);
        textLabel->setObjectName(QStringLiteral("sc2MessageText"));
        textLabel->setWordWrap(true);
        textLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        textLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        textLabel->setMinimumWidth(qMax(420, minimumWidth - 150));
        scroll->setWidget(textLabel);
        body->addWidget(scroll, 1);
        layout->addLayout(body, 1);

        auto *buttonRow = new QHBoxLayout;
        buttonRow->setSpacing(10);
        buttonRow->addStretch(1);

        QMessageBox::StandardButton clicked = QMessageBox::NoButton;
        const auto addButton = [&](QMessageBox::StandardButton button, const QString &text, bool defaultButton = false)
        {
            if (!buttons.testFlag(button))
                return;
            auto *push = new QPushButton(text, &dialog);
            push->setMinimumWidth(84);
            push->setFocusPolicy(Qt::NoFocus);
            if (defaultButton)
                push->setDefault(true);
            QObject::connect(push, &QPushButton::clicked, &dialog, [&dialog, &clicked, button]
            {
                clicked = button;
                dialog.accept();
            });
            buttonRow->addWidget(push);
        };

        addButton(QMessageBox::Yes, QStringLiteral("Yes"), buttons.testFlag(QMessageBox::Yes));
        addButton(QMessageBox::No, QStringLiteral("No"), !buttons.testFlag(QMessageBox::Yes) && buttons.testFlag(QMessageBox::No));
        addButton(QMessageBox::Ok, QStringLiteral("OK"), buttons == QMessageBox::Ok);
        addButton(QMessageBox::Cancel, QStringLiteral("Cancel"));
        layout->addLayout(buttonRow);

        QObject::connect(closeButton, &QPushButton::clicked, &dialog, [&dialog, &clicked, buttons]
        {
            clicked = buttons.testFlag(QMessageBox::No) ? QMessageBox::No
                : buttons.testFlag(QMessageBox::Cancel) ? QMessageBox::Cancel
                                                       : QMessageBox::Ok;
            dialog.reject();
        });

        QPoint dragPosition;
        bool dragging = false;
        titleBar->installEventFilter(new Sc2DialogDragFilter(&dialog, &dragPosition, &dragging));

        dialog.resize(minimumWidth, qMin(720, qMax(230, textLabel->sizeHint().height() + 130)));
        ScopedModalBackdrop backdrop(parent);
        animateModalOpen(&dialog);
        dialog.exec();
        return clicked == QMessageBox::NoButton ? QMessageBox::Ok : clicked;
    }

}

