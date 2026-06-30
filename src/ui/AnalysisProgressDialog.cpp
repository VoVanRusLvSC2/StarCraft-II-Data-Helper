#include "ui/AnalysisProgressDialog.h"

#include <QColor>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QTransform>
#include <QVBoxLayout>

#include <algorithm>

namespace {

void drawHorizontalTexture(QPainter &painter, const QRect &target, const QPixmap &source)
{
    if (target.isEmpty() || source.isNull()) {
        return;
    }

    const QPixmap scaled = source.height() == target.height()
        ? source
        : source.scaledToHeight(target.height(), Qt::SmoothTransformation);
    const int cap = std::min({24, target.width() / 2, scaled.width() / 3});
    if (cap <= 0 || target.width() <= cap * 2) {
        painter.drawPixmap(target, scaled);
        return;
    }

    painter.drawPixmap(QRect(target.left(), target.top(), cap, target.height()),
                       scaled, QRect(0, 0, cap, scaled.height()));
    painter.drawPixmap(QRect(target.right() - cap + 1, target.top(), cap, target.height()),
                       scaled, QRect(scaled.width() - cap, 0, cap, scaled.height()));

    const QRect centerTarget(target.left() + cap, target.top(), target.width() - cap * 2, target.height());
    const QRect centerSource(cap, 0, scaled.width() - cap * 2, scaled.height());
    const QPixmap centerTile = scaled.copy(centerSource);
    painter.drawTiledPixmap(centerTarget, centerTile);
}

class TexturedProgressBar final : public QProgressBar
{
public:
    explicit TexturedProgressBar(QWidget *parent = nullptr)
        : QProgressBar(parent),
          m_track(QPixmap(QStringLiteral(":/textures/ui_nova_global_scrollbar_bg.png"))
                      .transformed(QTransform().rotate(90.0), Qt::SmoothTransformation)),
          m_fill(QPixmap(QStringLiteral(":/textures/ui_nova_global_scrollbarbutton_normal.png"))
                     .transformed(QTransform().rotate(90.0), Qt::SmoothTransformation))
    {
        setAttribute(Qt::WA_OpaquePaintEvent, false);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QRect trackRect = rect().adjusted(6, 4, -6, -4);
        drawHorizontalTexture(painter, trackRect, m_track);

        const qreal ratio = maximum() > minimum()
            ? qreal(value() - minimum()) / qreal(maximum() - minimum())
            : 0.0;
        QRect fillRect = trackRect.adjusted(5, 2, -5, -2);
        fillRect.setWidth(qMax(0, qRound(fillRect.width() * ratio)));
        if (fillRect.width() > 0) {
            drawHorizontalTexture(painter, fillRect, m_fill);
        }
    }

private:
    QPixmap m_track;
    QPixmap m_fill;
};

} // namespace

AnalysisProgressDialog::AnalysisProgressDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("analysisProgressDialog"));
    setWindowTitle(QStringLiteral("SC2 Data Helper"));
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setWindowModality(Qt::ApplicationModal);
    setModal(true);
    setFixedSize(700, 304);

    auto *dialogLayout = new QVBoxLayout(this);
    dialogLayout->setContentsMargins(0, 0, 0, 0);

    auto *outerFrame = new QFrame(this);
    outerFrame->setObjectName(QStringLiteral("novaProgressFrame"));
    auto *outerLayout = new QVBoxLayout(outerFrame);
    outerLayout->setContentsMargins(12, 12, 12, 12);

    auto *innerFrame = new QFrame(outerFrame);
    innerFrame->setObjectName(QStringLiteral("novaProgressInner"));
    auto *content = new QVBoxLayout(innerFrame);
    content->setContentsMargins(28, 18, 28, 18);
    content->setSpacing(9);

    m_titleLabel = new QLabel(QStringLiteral("SC2 DATA ANALYSIS"), innerFrame);
    m_titleLabel->setObjectName(QStringLiteral("novaProgressTitle"));
    m_titleLabel->setAlignment(Qt::AlignCenter);
    auto *titleGlow = new QGraphicsDropShadowEffect(m_titleLabel);
    titleGlow->setBlurRadius(18.0);
    titleGlow->setColor(QColor(255, 111, 42, 190));
    titleGlow->setOffset(0.0, 0.0);
    m_titleLabel->setGraphicsEffect(titleGlow);
    content->addWidget(m_titleLabel);

    m_primaryLabel = new QLabel(innerFrame);
    m_primaryLabel->setObjectName(QStringLiteral("novaProgressLine"));
    m_primaryLabel->setAlignment(Qt::AlignCenter);
    m_primaryLabel->setWordWrap(true);
    content->addWidget(m_primaryLabel);

    m_secondaryLabel = new QLabel(innerFrame);
    m_secondaryLabel->setObjectName(QStringLiteral("novaProgressDetail"));
    m_secondaryLabel->setAlignment(Qt::AlignCenter);
    m_secondaryLabel->setWordWrap(true);
    m_secondaryLabel->setMinimumHeight(44);
    content->addWidget(m_secondaryLabel);

    m_percentLabel = new QLabel(QStringLiteral("0%"), innerFrame);
    m_percentLabel->setObjectName(QStringLiteral("novaProgressPercent"));
    m_percentLabel->setAlignment(Qt::AlignCenter);
    auto *percentGlow = new QGraphicsDropShadowEffect(m_percentLabel);
    percentGlow->setBlurRadius(14.0);
    percentGlow->setColor(QColor(43, 255, 177, 170));
    percentGlow->setOffset(0.0, 0.0);
    m_percentLabel->setGraphicsEffect(percentGlow);
    content->addWidget(m_percentLabel);

    m_progressBar = new TexturedProgressBar(innerFrame);
    m_progressBar->setObjectName(QStringLiteral("analysisProgressBar"));
    m_progressBar->setRange(0, 100);
    m_progressBar->setTextVisible(false);
    m_progressBar->setFixedHeight(50);
    content->addWidget(m_progressBar);

    m_cancelButton = new QPushButton(QStringLiteral("Cancel analysis"), innerFrame);
    m_cancelButton->setObjectName(QStringLiteral("analysisCancelButton"));
    content->addWidget(m_cancelButton, 0, Qt::AlignCenter);
    connect(m_cancelButton, &QPushButton::clicked, this, [this] {
        if (m_cancelled)
            return;
        m_cancelled = true;
        m_cancelButton->setEnabled(false);
        m_cancelButton->setText(QStringLiteral("Stopping..."));
        m_primaryLabel->setText(QStringLiteral("Stopping analysis"));
        emit cancellationRequested();
    });

    outerLayout->addWidget(innerFrame);
    dialogLayout->addWidget(outerFrame);
}

void AnalysisProgressDialog::setTitleText(const QString &title)
{
    if (m_titleLabel)
        m_titleLabel->setText(title);
}

void AnalysisProgressDialog::setCancelVisible(bool visible)
{
    if (m_cancelButton)
        m_cancelButton->setVisible(visible);
}

void AnalysisProgressDialog::setProgress(int percent, const QString &primaryText, const QString &secondaryText)
{
    const int bounded = qBound(0, percent, 100);
    m_primaryLabel->setText(primaryText);
    m_secondaryLabel->setText(secondaryText);
    m_percentLabel->setText(QStringLiteral("%1%").arg(bounded));
    m_progressBar->setValue(bounded);
}
