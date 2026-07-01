#include "app/Sc2FileDialogs.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QPushButton>
#include <QRegion>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
#include <QSizePolicy>
#include <QSplitter>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QTransform>
#include <QVBoxLayout>
#include <QEasingCurve>

namespace
{
    struct Sc2FileOpenFilter
    {
        QString label;
        QStringList suffixes;
    };

    QString humanFileSize(qint64 bytes)
    {
        static const QStringList units{QStringLiteral("B"), QStringLiteral("KB"), QStringLiteral("MB"), QStringLiteral("GB")};
        double value = double(bytes);
        int unit = 0;
        while (value >= 1024.0 && unit + 1 < units.size())
        {
            value /= 1024.0;
            ++unit;
        }
        return unit == 0
                   ? QStringLiteral("%1 %2").arg(bytes).arg(units[unit])
                   : QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(units[unit]);
    }

    bool matchesOpenFilter(const QFileInfo &info, const Sc2FileOpenFilter &filter)
    {
        if (info.isDir() || filter.suffixes.isEmpty())
            return true;
        const QString suffix = QStringLiteral(".%1").arg(info.suffix()).toLower();
        return filter.suffixes.contains(suffix);
    }


    QPixmap makeFrameHighlightDelta(const QPixmap &basePixmap, const QPixmap &highlightPixmap)
    {
        if (highlightPixmap.isNull() || basePixmap.isNull())
            return highlightPixmap;

        const QImage base = basePixmap.toImage().convertToFormat(QImage::Format_ARGB32);
        const QImage highlight = highlightPixmap.toImage().convertToFormat(QImage::Format_ARGB32);
        QImage delta(highlight.size(), QImage::Format_ARGB32_Premultiplied);
        delta.fill(Qt::transparent);

        const int width = qMin(base.width(), highlight.width());
        const int height = qMin(base.height(), highlight.height());
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const QColor baseColor = QColor::fromRgba(base.pixel(x, y));
                const QColor highlightColor = QColor::fromRgba(highlight.pixel(x, y));
                if (highlightColor.alpha() <= 0)
                    continue;

                const int gainR = highlightColor.red() - baseColor.red();
                const int gainG = highlightColor.green() - baseColor.green();
                const int gainB = highlightColor.blue() - baseColor.blue();
                const int colorGain = qMax(0, qMax(gainR, qMax(gainG, gainB)));
                const int alphaGain = highlightColor.alpha() - baseColor.alpha();
                const int keep = qMax(colorGain, alphaGain);
                if (keep < 6)
                    continue;

                const int alpha = qBound(0, highlightColor.alpha() * qMin(255, keep * 9) / 255, 220);
                if (alpha <= 0)
                    continue;

                delta.setPixelColor(x, y, QColor(highlightColor.red(), highlightColor.green(), highlightColor.blue(), alpha));
            }
        }

        return QPixmap::fromImage(delta);
    }

    void drawNinePatchPixmap(QPainter &painter, const QRect &target, const QPixmap &pixmap,
                             const QMargins &sourceMargins, const QMargins &targetMargins)
    {
        if (target.isEmpty() || pixmap.isNull())
            return;

        const int sourceLeft = qBound(0, sourceMargins.left(), pixmap.width() / 2);
        const int sourceRight = qBound(0, sourceMargins.right(), pixmap.width() - sourceLeft);
        const int sourceTop = qBound(0, sourceMargins.top(), pixmap.height() / 2);
        const int sourceBottom = qBound(0, sourceMargins.bottom(), pixmap.height() - sourceTop);

        const int targetLeft = qBound(0, targetMargins.left(), target.width() / 2);
        const int targetRight = qBound(0, targetMargins.right(), target.width() - targetLeft);
        const int targetTop = qBound(0, targetMargins.top(), target.height() / 2);
        const int targetBottom = qBound(0, targetMargins.bottom(), target.height() - targetTop);

        const int sourceX[] = {0, sourceLeft, pixmap.width() - sourceRight, pixmap.width()};
        const int sourceY[] = {0, sourceTop, pixmap.height() - sourceBottom, pixmap.height()};
        const int targetX[] = {target.left(), target.left() + targetLeft,
                               target.right() + 1 - targetRight, target.right() + 1};
        const int targetY[] = {target.top(), target.top() + targetTop,
                               target.bottom() + 1 - targetBottom, target.bottom() + 1};

        for (int y = 0; y < 3; ++y)
        {
            for (int x = 0; x < 3; ++x)
            {
                const QRect sourceRect(sourceX[x], sourceY[y],
                                       sourceX[x + 1] - sourceX[x],
                                       sourceY[y + 1] - sourceY[y]);
                const QRect targetRect(targetX[x], targetY[y],
                                       targetX[x + 1] - targetX[x],
                                       targetY[y + 1] - targetY[y]);
                if (!sourceRect.isEmpty() && !targetRect.isEmpty())
                    painter.drawPixmap(targetRect, pixmap, sourceRect);
            }
        }
    }

    struct Sc2FrameHighlightFrame
    {
        QPixmap top;
        QPixmap bottom;
        QPixmap left;
        QPixmap right;
    };

    QRect alphaBounds(const QImage &image)
    {
        if (image.isNull())
            return QRect();

        int left = image.width();
        int top = image.height();
        int right = -1;
        int bottom = -1;
        for (int y = 0; y < image.height(); ++y)
        {
            for (int x = 0; x < image.width(); ++x)
            {
                if (qAlpha(image.pixel(x, y)) <= 0)
                    continue;
                left = qMin(left, x);
                top = qMin(top, y);
                right = qMax(right, x);
                bottom = qMax(bottom, y);
            }
        }

        if (right < left || bottom < top)
            return QRect();
        return QRect(QPoint(left, top), QPoint(right, bottom));
    }

    class Sc2FrameHighlightRenderer final
    {
    public:
        Sc2FrameHighlightRenderer()
            : m_base(QStringLiteral(":/textures/ui_nova_archives_backgroundframe.png")),
              m_highlight(makeFrameHighlightDelta(m_base, QPixmap(QStringLiteral(":/textures/ui_nova_archives_backgroundframehighlight.png")))),
              m_revealMask(QStringLiteral(":/textures/ui_nova_archives_backgroundframehighlightmask.png"))
        {
        }

        static constexpr int periodFrames()
        {
            return kPeriod;
        }

        static void warmUp(const QSize &size)
        {
            if (size.isEmpty())
                return;

            Sc2FrameHighlightRenderer renderer;
            renderer.ensureFrameCache(size);
        }

        void invalidate()
        {
            m_cachedSize = QSize();
            m_frames.clear();
        }

        void prepare(const QSize &size)
        {
            ensureFrameCache(size);
        }

        void paint(QPainter &painter, const QRect &target, int phase, const QRegion &exposedRegion)
        {
            if (target.isEmpty() || m_highlight.isNull())
                return;

            if (phase < kNormalHoldFrames)
                return;

            ensureFrameCache(target.size());
            if (m_frames.isEmpty())
                return;

            const int clampedPhase = qBound(0, phase, kPeriod - 1);
            const int frameIndex = qBound(0,
                                          (clampedPhase * (m_frames.size() - 1)) / qMax(1, kPeriod - 1),
                                          m_frames.size() - 1);
            paintCachedFrame(painter, target, exposedRegion, m_frames.at(frameIndex));
        }

    private:
        static constexpr int kPeriod = 54;
        static constexpr int kRenderedFrameCount = 54;
        static constexpr int kNormalHoldFrames = 1;
        static constexpr int kFullHighlightFrames = 3;
        static constexpr int kSourceFrameSlice = 24;
        static constexpr int kTargetFrameBorder = 12;
        static constexpr int kHorizontalFrameBand = 176;
        static constexpr int kVerticalFrameBand = 72;

        static void drawFrameTexture(QPainter &painter, const QRect &target, const QPixmap &pixmap)
        {
            const QMargins sourceMargins(kSourceFrameSlice, kSourceFrameSlice,
                                         kSourceFrameSlice, kSourceFrameSlice);
            const QMargins targetMargins(kTargetFrameBorder, kTargetFrameBorder,
                                         kTargetFrameBorder, kTargetFrameBorder);
            drawNinePatchPixmap(painter, target, pixmap, sourceMargins, targetMargins);
        }

        QRect topBand() const
        {
            return QRect(0, 0, m_cachedSize.width(), horizontalBandHeight());
        }

        QRect bottomBand() const
        {
            const int bandHeight = horizontalBandHeight();
            return QRect(0, qMax(0, m_cachedSize.height() - bandHeight), m_cachedSize.width(), bandHeight);
        }

        QRect leftBand() const
        {
            const int bandHeight = horizontalBandHeight();
            const int sideHeight = qMax(0, m_cachedSize.height() - bandHeight * 2);
            return QRect(0, bandHeight, verticalBandWidth(), sideHeight);
        }

        QRect rightBand() const
        {
            const int bandHeight = horizontalBandHeight();
            const int sideWidth = verticalBandWidth();
            const int sideHeight = qMax(0, m_cachedSize.height() - bandHeight * 2);
            return QRect(qMax(0, m_cachedSize.width() - sideWidth), bandHeight, sideWidth, sideHeight);
        }

        int horizontalBandHeight() const
        {
            return qMin(kHorizontalFrameBand, m_cachedSize.height());
        }

        int verticalBandWidth() const
        {
            return qMin(kVerticalFrameBand, m_cachedSize.width());
        }

        void paintCachedFrame(QPainter &painter, const QRect &target, const QRegion &exposedRegion,
                              const Sc2FrameHighlightFrame &frame)
        {
            paintPixmapBand(painter, target, topBand(), exposedRegion, frame.top);
            paintPixmapBand(painter, target, bottomBand(), exposedRegion, frame.bottom);
            paintPixmapBand(painter, target, leftBand(), exposedRegion, frame.left);
            paintPixmapBand(painter, target, rightBand(), exposedRegion, frame.right);
        }

        void paintPixmapBand(QPainter &painter, const QRect &target, const QRect &sourceRect,
                             const QRegion &exposedRegion, const QPixmap &pixmap)
        {
            if (sourceRect.isEmpty() || pixmap.isNull())
                return;

            const QRect targetRect = sourceRect.translated(target.topLeft());
            if (!exposedRegion.isEmpty() && !exposedRegion.intersects(targetRect))
                return;

            painter.drawPixmap(targetRect.topLeft(), pixmap);
        }

        QPixmap makeHighlightBand(const QRect &sourceRect) const
        {
            return sourceRect.isEmpty() ? QPixmap() : m_highlightFrame.copy(sourceRect);
        }

        QPixmap makeMaskedBand(const QRect &sourceRect, int maskX, int revealedWidth) const
        {
            if (sourceRect.isEmpty() || m_revealMaskFrame.isNull())
                return QPixmap();

            QPixmap maskBuffer(sourceRect.size());
            maskBuffer.fill(Qt::transparent);

            {
                QPainter maskPainter(&maskBuffer);
                const int localRevealWidth = qBound(0, revealedWidth - sourceRect.left(), sourceRect.width());
                if (localRevealWidth > 0)
                {
                    if (localRevealWidth >= sourceRect.width())
                    {
                        maskPainter.fillRect(0, 0, sourceRect.width(), sourceRect.height(), Qt::white);
                    }
                    else
                    {
                        const int fadeWidth = qMin(qMax(24, m_revealMaskFrame.width() / 5), localRevealWidth);
                        const int solidWidth = qMax(0, localRevealWidth - fadeWidth);
                        if (solidWidth > 0)
                            maskPainter.fillRect(0, 0, solidWidth, sourceRect.height(), Qt::white);
                        QLinearGradient fadeGradient(solidWidth, 0, localRevealWidth, 0);
                        fadeGradient.setColorAt(0.0, Qt::white);
                        fadeGradient.setColorAt(1.0, QColor(255, 255, 255, 0));
                        maskPainter.fillRect(solidWidth, 0, fadeWidth, sourceRect.height(), fadeGradient);
                    }
                }

                const QRect maskDest(maskX - sourceRect.left(), -sourceRect.top(),
                                     m_revealMaskFrame.width(), m_revealMaskFrame.height());
                const QRect bufferRect(QPoint(0, 0), sourceRect.size());
                const QRect clippedDest = maskDest.intersected(bufferRect);
                if (!clippedDest.isEmpty())
                {
                    const QRect clippedSource = clippedDest.translated(-maskDest.topLeft());
                    maskPainter.drawPixmap(clippedDest, m_revealMaskFrame, clippedSource);
                }
            }

            QPixmap revealLayer(sourceRect.size());
            revealLayer.fill(Qt::transparent);
            {
                QPainter layerPainter(&revealLayer);
                layerPainter.drawPixmap(QRect(QPoint(0, 0), sourceRect.size()), m_highlightFrame, sourceRect);
                layerPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                layerPainter.drawPixmap(QPoint(0, 0), maskBuffer);
            }

            return revealLayer;
        }

        void ensureFrameCache(const QSize &size)
        {
            if (size.isEmpty())
                return;

            if (m_cachedSize == size && !m_frames.isEmpty())
                return;

            if (s_sharedCacheSize == size && !s_sharedFrames.isEmpty())
            {
                m_cachedSize = size;
                m_frames = s_sharedFrames;
                return;
            }

            m_cachedSize = size;
            m_frames.clear();

            m_highlightFrame = QPixmap(m_cachedSize);
            m_highlightFrame.fill(Qt::transparent);
            {
                QPainter painter(&m_highlightFrame);
                painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                drawFrameTexture(painter, m_highlightFrame.rect(), m_highlight);
            }

            const int revealFrontWidth = qBound(220, m_cachedSize.width() / 3, 420);
            m_revealMaskFrame = QPixmap(qMax(1, revealFrontWidth), qMax(1, m_cachedSize.height()));
            m_revealMaskFrame.fill(Qt::transparent);
            if (!m_revealMask.isNull())
            {
                const QImage maskImage = m_revealMask.toImage().convertToFormat(QImage::Format_ARGB32);
                const QRect activeBounds = alphaBounds(maskImage);
                const QRect sourceBounds = activeBounds.isEmpty() ? maskImage.rect() : activeBounds;
                QTransform rotate;
                rotate.rotate(90.0);
                const QPixmap croppedMask = QPixmap::fromImage(maskImage.copy(sourceBounds));
                const QPixmap rotatedMask = croppedMask.transformed(rotate, Qt::SmoothTransformation);
                QPainter painter(&m_revealMaskFrame);
                painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                painter.drawPixmap(m_revealMaskFrame.rect(), rotatedMask, rotatedMask.rect());
            }

            static const QEasingCurve easing(QEasingCurve::OutCubic);
            m_frames.reserve(kRenderedFrameCount);
            for (int frame = 0; frame < kRenderedFrameCount; ++frame)
            {
                Sc2FrameHighlightFrame cachedFrame;
                if (frame < kNormalHoldFrames)
                {
                    m_frames.append(cachedFrame);
                    continue;
                }

                if (frame >= kRenderedFrameCount - kFullHighlightFrames || m_revealMaskFrame.isNull())
                {
                    cachedFrame.top = makeHighlightBand(topBand());
                    cachedFrame.bottom = makeHighlightBand(bottomBand());
                    cachedFrame.left = makeHighlightBand(leftBand());
                    cachedFrame.right = makeHighlightBand(rightBand());
                }
                else
                {
                    const qreal progress = qreal(frame - kNormalHoldFrames + 1)
                                           / qreal(qMax(1, kRenderedFrameCount - kNormalHoldFrames - kFullHighlightFrames));
                    const qreal eased = easing.valueForProgress(qBound<qreal>(0.0, progress, 1.0));
                    const int startX = -m_revealMaskFrame.width();
                    const int endX = m_cachedSize.width() - m_revealMaskFrame.width() / 4;
                    const int maskX = qRound(qreal(startX) + eased * qreal(endX - startX));
                    const int revealedWidth = qBound(0,
                                                     maskX + (m_revealMaskFrame.width() * 3) / 4,
                                                     m_cachedSize.width());
                    cachedFrame.top = makeMaskedBand(topBand(), maskX, revealedWidth);
                    cachedFrame.bottom = makeMaskedBand(bottomBand(), maskX, revealedWidth);
                    cachedFrame.left = makeMaskedBand(leftBand(), maskX, revealedWidth);
                    cachedFrame.right = makeMaskedBand(rightBand(), maskX, revealedWidth);
                }

                m_frames.append(cachedFrame);
            }

            s_sharedCacheSize = m_cachedSize;
            s_sharedFrames = m_frames;
        }

        QPixmap m_base;
        QPixmap m_highlight;
        QPixmap m_revealMask;
        QPixmap m_highlightFrame;
        QPixmap m_revealMaskFrame;
        QVector<Sc2FrameHighlightFrame> m_frames;
        QSize m_cachedSize;
        inline static QSize s_sharedCacheSize;
        inline static QVector<Sc2FrameHighlightFrame> s_sharedFrames;
    };

    QSize sc2FileOpenDialogTargetSize(const QWidget *parent)
    {
        QSize targetSize(1180, 720);
        const QScreen *screen = parent ? parent->screen() : QApplication::primaryScreen();
        if (!screen)
            screen = QApplication::primaryScreen();
        if (screen)
        {
            const QRect available = screen->availableGeometry();
            targetSize.setWidth(qMin(targetSize.width(), qMax(940, available.width() - 90)));
            targetSize.setHeight(qMin(targetSize.height(), qMax(560, available.height() - 110)));
        }
        return targetSize;
    }


    QString fileTypeLabel(const QFileInfo &info)
    {
        if (info.isDir())
            return QStringLiteral("Folder");
        const QString suffix = info.suffix().trimmed();
        return suffix.isEmpty() ? QStringLiteral("File")
                                : QStringLiteral("%1 file").arg(suffix.toUpper());
    }

    class Sc2FileOpenDialog final : public QDialog
    {
    public:
        explicit Sc2FileOpenDialog(QWidget *parent, const QString &startPath)
            : QDialog(parent)
        {
            setObjectName(QStringLiteral("sc2OpenFileDialog"));
            setWindowTitle(QStringLiteral("Open SC2 File"));
            setWindowFlags((windowFlags() | Qt::FramelessWindowHint) & ~Qt::WindowContextHelpButtonHint);
            setModal(true);
            setMinimumSize(940, 560);
            resize(sc2FileOpenDialogTargetSize(parent));

            m_filters = {
                {QStringLiteral("Supported (*.SC2Map, *.SC2Mod, *.SC2Components, *.SC2Campaign, *.SC2Archive, *.xml)"),
                 {QStringLiteral(".sc2map"), QStringLiteral(".sc2mod"), QStringLiteral(".sc2components"),
                  QStringLiteral(".sc2campaign"), QStringLiteral(".sc2archive"), QStringLiteral(".xml")}},
                {QStringLiteral("SC2 Archives (*.SC2Map, *.SC2Mod, *.SC2Components, *.SC2Campaign, *.SC2Archive)"),
                 {QStringLiteral(".sc2map"), QStringLiteral(".sc2mod"), QStringLiteral(".sc2components"),
                  QStringLiteral(".sc2campaign"), QStringLiteral(".sc2archive")}},
                {QStringLiteral("XML (*.xml)"), {QStringLiteral(".xml")}},
                {QStringLiteral("All Files (*.*)"), {}},
            };

            auto *layout = new QVBoxLayout(this);
            layout->setContentsMargins(18, 8, 24, 18);
            layout->setSpacing(5);

            m_hexOverlay = new QFrame(this);
            m_hexOverlay->setObjectName(QStringLiteral("sc2OpenFileHexOverlay"));
            m_hexOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
            m_hexOverlay->lower();
            auto *hexEffect = new QGraphicsOpacityEffect(m_hexOverlay);
            hexEffect->setOpacity(0.16);
            m_hexOverlay->setGraphicsEffect(hexEffect);
            m_highlightTimer = new QTimer(this);
            connect(m_highlightTimer, &QTimer::timeout, this, [this]()
                    {
                if (m_highlightPhase + 1 >= Sc2FrameHighlightRenderer::periodFrames())
                {
                    m_highlightPhase = Sc2FrameHighlightRenderer::periodFrames() - 1;
                    if (m_highlightTimer)
                        m_highlightTimer->stop();
                }
                else
                {
                    ++m_highlightPhase;
                }
                updateHighlightFrameRegion();
            });
            m_highlightTimer->setTimerType(Qt::PreciseTimer);
            m_highlightTimer->setInterval(11);

            auto *titleBar = new QFrame(this);
            titleBar->setObjectName(QStringLiteral("sc2OpenFileTitleBar"));
            titleBar->setMouseTracking(true);
            titleBar->setMinimumHeight(66);
            titleBar->setMaximumHeight(68);
            auto *titleBarLayout = new QHBoxLayout(titleBar);
            titleBarLayout->setContentsMargins(0, 16, 30, 8);
            titleBarLayout->setSpacing(0);
            titleBarLayout->addStretch(1);
            layout->addWidget(titleBar);

            auto *panelHeader = new QFrame(this);
            panelHeader->setObjectName(QStringLiteral("sc2OpenFilePanelHeader"));
            panelHeader->setMouseTracking(true);
            auto *panelHeaderLayout = new QHBoxLayout(panelHeader);
            panelHeaderLayout->setContentsMargins(0, 0, 0, 0);
            panelHeaderLayout->setSpacing(8);
            auto *panelIcon = new QLabel(panelHeader);
            panelIcon->setObjectName(QStringLiteral("sc2OpenFilePanelIcon"));
            panelIcon->setPixmap(QPixmap(QStringLiteral(":/icons/Icon.png")).scaled(30, 30, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            auto *title = new QLabel(QStringLiteral("OPEN SC2 FILE"), panelHeader);
            title->setObjectName(QStringLiteral("sc2OpenFilePanelTitle"));
            auto *titleGlow = new QGraphicsDropShadowEffect(title);
            titleGlow->setBlurRadius(14.0);
            titleGlow->setColor(QColor(130, 255, 221, 190));
            titleGlow->setOffset(0.0, 0.0);
            title->setGraphicsEffect(titleGlow);
            panelHeaderLayout->addWidget(panelIcon);
            panelHeaderLayout->addWidget(title);
            panelHeaderLayout->addStretch(1);
            layout->addWidget(panelHeader);

            m_status = new QLabel(this);
            m_status->setObjectName(QStringLiteral("inspectorSubtitle"));
            m_status->setWordWrap(false);
            m_status->setMaximumHeight(24);
            m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            layout->addWidget(m_status);

            m_path = new QLineEdit(this);
            m_path->setObjectName(QStringLiteral("sc2OpenFilePath"));
            m_path->setMinimumHeight(34);
            m_path->setMaximumHeight(34);
            m_path->setPlaceholderText(QStringLiteral("Current folder..."));
            layout->addWidget(m_path);

            auto *searchRow = new QHBoxLayout;
            m_parentButton = new QPushButton(QStringLiteral("Up"), this);
            m_parentButton->setObjectName(QStringLiteral("sc2OpenFileParentButton"));
            m_parentButton->setToolTip(QStringLiteral("Open parent directory"));
            m_parentButton->setMinimumSize(74, 34);
            m_parentButton->setMaximumHeight(34);
            m_search = new QLineEdit(this);
            m_search->setObjectName(QStringLiteral("sc2OpenFileSearch"));
            m_search->setMinimumHeight(34);
            m_search->setMaximumHeight(34);
            m_search->setPlaceholderText(QStringLiteral("Search files and folders..."));
            m_filter = new QComboBox(this);
            m_filter->setObjectName(QStringLiteral("sc2OpenFileFilter"));
            m_filter->setMinimumHeight(34);
            m_filter->setMaximumHeight(34);
            m_filter->setMinimumWidth(430);
            m_filter->setMaximumWidth(620);
            for (const Sc2FileOpenFilter &filter : m_filters)
                m_filter->addItem(filter.label);
            searchRow->addWidget(m_parentButton);
            searchRow->addWidget(m_search, 1);
            searchRow->addWidget(m_filter);
            layout->addLayout(searchRow);

            auto *splitter = new QSplitter(Qt::Horizontal, this);
            splitter->setObjectName(QStringLiteral("sc2OpenFileSplitter"));
            splitter->setChildrenCollapsible(false);
            m_places = new QListWidget(splitter);
            m_places->setObjectName(QStringLiteral("fileOpenPlaces"));
            m_places->setProperty("textureType", QStringLiteral("border"));
            m_places->setProperty("texturetype", QStringLiteral("border"));
            m_places->viewport()->setObjectName(QStringLiteral("fileOpenPlacesViewport"));
            m_places->setSelectionMode(QAbstractItemView::SingleSelection);
            m_places->setSpacing(2);
            m_places->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            m_places->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            m_table = new QTableWidget(splitter);
            m_table->setObjectName(QStringLiteral("fileOpenTable"));
            m_table->setProperty("textureType", QStringLiteral("border"));
            m_table->setProperty("texturetype", QStringLiteral("border"));
            m_table->viewport()->setObjectName(QStringLiteral("fileOpenTableViewport"));
            m_table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            m_table->setColumnCount(4);
            m_table->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Type"),
                                                QStringLiteral("Size"), QStringLiteral("Modified")});
            m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
            m_table->setSelectionMode(QAbstractItemView::SingleSelection);
            m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
            m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            m_table->setShowGrid(false);
            m_table->verticalHeader()->hide();
            m_table->verticalHeader()->setDefaultSectionSize(36);
            m_table->horizontalHeader()->setFixedHeight(34);
            m_table->horizontalHeader()->setStretchLastSection(true);
            m_table->setColumnWidth(0, 445);
            m_table->setColumnWidth(1, 118);
            m_table->setColumnWidth(2, 100);
            splitter->addWidget(m_places);
            splitter->addWidget(m_table);
            splitter->setStretchFactor(0, 1);
            splitter->setStretchFactor(1, 4);
            splitter->setSizes({230, 870});
            layout->addWidget(splitter, 1);

            auto *nameRow = new QHBoxLayout;
            auto *nameLabel = new QLabel(QStringLiteral("Selected:"), this);
            nameLabel->setObjectName(QStringLiteral("inspectorSubtitle"));
            m_name = new QLineEdit(this);
            m_name->setObjectName(QStringLiteral("sc2OpenFileName"));
            m_name->setMinimumHeight(34);
            m_name->setMaximumHeight(34);
            m_name->setPlaceholderText(QStringLiteral("Selected file or folder..."));
            nameRow->addWidget(nameLabel);
            nameRow->addWidget(m_name, 1);
            layout->addLayout(nameRow);

            m_selection = new QLabel(this);
            m_selection->setObjectName(QStringLiteral("inspectorSubtitle"));
            m_selection->setTextInteractionFlags(Qt::TextSelectableByMouse);
            m_selection->setMinimumHeight(30);
            m_selection->setMaximumHeight(32);
            m_selection->setMinimumWidth(0);
            m_selection->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            m_selection->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

            auto *buttonRow = new QHBoxLayout;
            buttonRow->setContentsMargins(0, 1, 12, 2);
            buttonRow->setSpacing(12);
            buttonRow->addWidget(m_selection, 1);
            m_open = new QPushButton(QStringLiteral("Open"), this);
            m_cancel = new QPushButton(QStringLiteral("Cancel"), this);
            m_open->setObjectName(QStringLiteral("sc2OpenFileOpenButton"));
            m_cancel->setObjectName(QStringLiteral("sc2OpenFileCancelButton"));
            m_open->setMinimumWidth(132);
            m_cancel->setMinimumWidth(132);
            buttonRow->addWidget(m_open);
            buttonRow->addWidget(m_cancel);
            layout->addLayout(buttonRow);

            populatePlaces();
            wireEvents();

            QFileInfo startInfo(startPath);
            QString initialDir;
            QString initialFile;
            if (startInfo.isFile())
            {
                initialDir = startInfo.absolutePath();
                initialFile = startInfo.fileName();
            }
            else if (startInfo.isDir())
            {
                initialDir = startInfo.absoluteFilePath();
            }
            else
            {
                initialDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
            }
            if (initialDir.isEmpty())
                initialDir = QDir::homePath();
            openDirectory(initialDir, initialFile);
            updateHexOverlayGeometry();
            updateHighlightFrameRegion();
        }

        QString selectedFile() const { return m_selectedFile; }

    protected:
        void keyPressEvent(QKeyEvent *event) override
        {
            if (!event)
            {
                QDialog::keyPressEvent(event);
                return;
            }
            if (event->key() == Qt::Key_Escape)
            {
                reject();
                return;
            }
            if (event->key() == Qt::Key_F5)
            {
                openDirectory(m_currentDir, m_name->text());
                return;
            }
            if ((event->key() == Qt::Key_Backspace) || (event->key() == Qt::Key_Up && event->modifiers().testFlag(Qt::AltModifier)))
            {
                openParent();
                return;
            }
            if (event->matches(QKeySequence::Find))
            {
                m_search->setFocus();
                m_search->selectAll();
                return;
            }
            if (event->key() == Qt::Key_L && event->modifiers().testFlag(Qt::ControlModifier))
            {
                m_path->setFocus();
                m_path->selectAll();
                return;
            }
            QDialog::keyPressEvent(event);
        }

        void mousePressEvent(QMouseEvent *event) override
        {
            QWidget *child = event ? childAt(event->pos()) : nullptr;
            const QString childName = child ? child->objectName() : QString();
            const bool titleDrag = childName == QStringLiteral("sc2OpenFileTitleBar")
                                   || childName == QStringLiteral("sc2OpenFilePanelHeader")
                                   || childName.startsWith(QStringLiteral("sc2OpenFilePanel"));
            if (event && event->button() == Qt::LeftButton && titleDrag) {
                m_dragPosition = event->globalPosition().toPoint() - frameGeometry().topLeft();
                m_dragging = true;
                event->accept();
                return;
            }
            QDialog::mousePressEvent(event);
        }

        void mouseMoveEvent(QMouseEvent *event) override
        {
            if (m_dragging && event && (event->buttons() & Qt::LeftButton)) {
                move(event->globalPosition().toPoint() - m_dragPosition);
                event->accept();
                return;
            }
            QDialog::mouseMoveEvent(event);
        }

        void mouseReleaseEvent(QMouseEvent *event) override
        {
            m_dragging = false;
            QDialog::mouseReleaseEvent(event);
        }

        void resizeEvent(QResizeEvent *event) override
        {
            QDialog::resizeEvent(event);
            updateHexOverlayGeometry();
            m_highlightRenderer.invalidate();
            updateHighlightFrameRegion();
            updateSelectionText();
        }

        void showEvent(QShowEvent *event) override
        {
            QDialog::showEvent(event);
            m_highlightPhase = 0;
            updateHighlightFrameRegion();
            if (m_highlightTimer)
            {
                m_highlightTimer->stop();
                m_highlightRenderer.prepare(rect().size());
                m_highlightTimer->start();
            }
        }

        void paintEvent(QPaintEvent *event) override
        {
            QDialog::paintEvent(event);
            QPainter painter(this);
            m_highlightRenderer.paint(painter, rect(), m_highlightPhase, event ? event->region() : QRegion(rect()));
        }

    private:
        void wireEvents()
        {
            connect(m_path, &QLineEdit::returnPressed, this, [this]
                    { handlePathEntered(); });
            connect(m_name, &QLineEdit::returnPressed, this, [this]
                    { confirmSelection(); });
            connect(m_search, &QLineEdit::textChanged, this, [this]
                    { populateFiles(); });
            connect(m_filter, &QComboBox::currentIndexChanged, this, [this]
                    { populateFiles(); });
            connect(m_parentButton, &QPushButton::clicked, this, [this]
                    { openParent(); });
            connect(m_places, &QListWidget::itemClicked, this, [this](QListWidgetItem *item)
                    {
                if (!item) return;
                const QString path = item->data(Qt::UserRole).toString();
                if (!path.isEmpty()) openDirectory(path); });
            connect(m_table, &QTableWidget::itemSelectionChanged, this, [this]
                    {
                const int row = m_table->currentRow();
                if (row < 0) return;
                QTableWidgetItem *nameItem = m_table->item(row, 0);
                if (!nameItem) return;
                const QString path = nameItem->data(Qt::UserRole).toString();
                m_name->setText(QFileInfo(path).fileName());
                setSelectionText(QDir::toNativeSeparators(path)); });
            connect(m_table, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem *item)
                    {
                if (!item) return;
                activatePath(item->data(Qt::UserRole).toString()); });
            connect(m_table, &QTableWidget::itemActivated, this, [this](QTableWidgetItem *item)
                    {
                if (!item) return;
                activatePath(item->data(Qt::UserRole).toString()); });
            connect(m_open, &QPushButton::clicked, this, [this]
                    { confirmSelection(); });
            connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);
        }

        void populatePlaces()
        {
            const auto addPlace = [this](const QString &title, const QString &path)
            {
                if (path.isEmpty() || !QFileInfo(path).isDir())
                    return;
                auto *item = new QListWidgetItem(title, m_places);
                item->setData(Qt::UserRole, QFileInfo(path).absoluteFilePath());
            };
            addPlace(QStringLiteral("Home"), QDir::homePath());
            addPlace(QStringLiteral("Desktop"), QStandardPaths::writableLocation(QStandardPaths::DesktopLocation));
            addPlace(QStringLiteral("Documents"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
            addPlace(QStringLiteral("Downloads"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
            for (const QFileInfo &drive : QDir::drives())
                addPlace(drive.absoluteFilePath(), drive.absoluteFilePath());
        }

        Sc2FileOpenFilter currentFilter() const
        {
            const int index = m_filter ? m_filter->currentIndex() : 0;
            return m_filters.value(qBound(0, index, m_filters.size() - 1));
        }

        void openDirectory(const QString &path, const QString &suggestedFile = QString())
        {
            QFileInfo info(path);
            if (!info.isDir())
            {
                setStatus(QStringLiteral("Directory not found: %1").arg(QDir::toNativeSeparators(path)));
                return;
            }
            m_currentDir = info.absoluteFilePath();
            m_path->setText(QDir::toNativeSeparators(m_currentDir));
            if (m_parentButton)
            {
                QDir parentDir(m_currentDir);
                m_parentButton->setEnabled(parentDir.cdUp());
            }
            m_name->setText(suggestedFile);
            setSelectionText(QDir::toNativeSeparators(suggestedFile.isEmpty()
                                                           ? m_currentDir
                                                           : QDir(m_currentDir).absoluteFilePath(suggestedFile)));
            populateFiles();
            if (!suggestedFile.isEmpty())
                selectFileName(suggestedFile);
        }

        void populateFiles()
        {
            const QDir dir(m_currentDir);
            const QString query = m_search->text().trimmed().toLower();
            const Sc2FileOpenFilter filter = currentFilter();
            QFileInfoList entries = dir.entryInfoList(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Readable,
                                                      QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
            m_table->setUpdatesEnabled(false);
            m_table->setRowCount(0);
            int folders = 0;
            int files = 0;
            for (const QFileInfo &entry : entries)
            {
                if (!query.isEmpty() && !entry.fileName().toLower().contains(query))
                    continue;
                if (!matchesOpenFilter(entry, filter))
                    continue;
                const int row = m_table->rowCount();
                m_table->insertRow(row);
                auto *name = new QTableWidgetItem(entry.fileName());
                name->setData(Qt::UserRole, entry.absoluteFilePath());
                name->setData(Qt::UserRole + 1, entry.isDir());
                auto *type = new QTableWidgetItem(fileTypeLabel(entry));
                auto *size = new QTableWidgetItem(entry.isDir() ? QStringLiteral("-") : humanFileSize(entry.size()));
                auto *modified = new QTableWidgetItem(entry.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
                for (QTableWidgetItem *item : {name, type, size, modified})
                    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                m_table->setItem(row, 0, name);
                m_table->setItem(row, 1, type);
                m_table->setItem(row, 2, size);
                m_table->setItem(row, 3, modified);
                if (entry.isDir())
                    ++folders;
                else
                    ++files;
            }
            m_table->setUpdatesEnabled(true);
            updateHighlightFrameRegion();
            setStatus(QStringLiteral("Current folder: %1   |   folders: %2   |   files: %3")
                          .arg(QDir::toNativeSeparators(m_currentDir))
                          .arg(folders)
                          .arg(files));
        }

        void selectFileName(const QString &fileName)
        {
            for (int row = 0; row < m_table->rowCount(); ++row)
            {
                QTableWidgetItem *item = m_table->item(row, 0);
                if (item && item->text().compare(fileName, Qt::CaseInsensitive) == 0)
                {
                    m_table->selectRow(row);
                    m_table->scrollToItem(item);
                    return;
                }
            }
        }

        void handlePathEntered()
        {
            const QString raw = m_path->text().trimmed().remove(QLatin1Char('"'));
            QFileInfo info(raw);
            if (info.isDir())
            {
                openDirectory(info.absoluteFilePath());
                return;
            }
            if (info.isFile())
            {
                activatePath(info.absoluteFilePath());
                return;
            }
            setStatus(QStringLiteral("Directory or file not found: %1").arg(QDir::toNativeSeparators(raw)));
        }

        void activatePath(const QString &path)
        {
            QFileInfo info(path);
            if (info.isDir())
            {
                openDirectory(info.absoluteFilePath());
                return;
            }
            if (info.isFile())
            {
                m_name->setText(info.fileName());
                setSelectionText(QDir::toNativeSeparators(info.absoluteFilePath()));
                confirmSelection();
            }
        }

        void confirmSelection()
        {
            QString candidate = m_name->text().trimmed().remove(QLatin1Char('"'));
            if (candidate.isEmpty() && m_table->currentRow() >= 0)
            {
                if (QTableWidgetItem *item = m_table->item(m_table->currentRow(), 0))
                    candidate = item->data(Qt::UserRole).toString();
            }
            QFileInfo info(candidate);
            if (!info.isAbsolute())
                info = QFileInfo(QDir(m_currentDir).absoluteFilePath(candidate));
            if (info.isDir())
            {
                openDirectory(info.absoluteFilePath());
                return;
            }
            if (!info.isFile())
            {
                setStatus(QStringLiteral("Select a file."));
                return;
            }
            if (!matchesOpenFilter(info, currentFilter()))
            {
                setStatus(QStringLiteral("File does not match selected filter."));
                return;
            }
            m_selectedFile = info.absoluteFilePath();
            accept();
        }

        void openParent()
        {
            QDir dir(m_currentDir);
            if (dir.cdUp())
                openDirectory(dir.absolutePath());
        }

        void setStatus(const QString &text)
        {
            if (m_status)
                m_status->setText(text);
        }

        void setSelectionText(const QString &text)
        {
            m_selectionFullText = text;
            updateSelectionText();
        }

        void updateSelectionText()
        {
            if (!m_selection)
                return;
            m_selection->setToolTip(m_selectionFullText);
            const int textWidth = qMax(24, m_selection->contentsRect().width() - 6);
            m_selection->setText(m_selection->fontMetrics().elidedText(m_selectionFullText,
                                                                       Qt::ElideMiddle,
                                                                       textWidth));
        }

        void updateHexOverlayGeometry()
        {
            if (!m_hexOverlay)
                return;
            const int inset = 24;
            m_hexOverlay->setGeometry(inset, 12, qMax(0, width() - inset * 2), 136);
            m_hexOverlay->lower();
        }

        void updateHighlightFrameRegion()
        {
            const int overlayWidth = qMax(0, width());
            const int overlayHeight = qMax(0, height());
            if (overlayWidth <= 0 || overlayHeight <= 0)
                return;

            const int horizontalBand = qMin(176, overlayHeight);
            const int verticalBand = qMin(72, overlayWidth);
            const int sideHeight = qMax(0, overlayHeight - horizontalBand * 2);
            QRegion frameRegion(0, 0, overlayWidth, horizontalBand);
            frameRegion += QRegion(0, qMax(0, overlayHeight - horizontalBand), overlayWidth, horizontalBand);
            frameRegion += QRegion(0, horizontalBand, verticalBand, sideHeight);
            frameRegion += QRegion(qMax(0, overlayWidth - verticalBand), horizontalBand, verticalBand, sideHeight);
            update(frameRegion);
        }

        QVector<Sc2FileOpenFilter> m_filters;
        QListWidget *m_places = nullptr;
        QTableWidget *m_table = nullptr;
        QLineEdit *m_path = nullptr;
        QLineEdit *m_search = nullptr;
        QLineEdit *m_name = nullptr;
        QComboBox *m_filter = nullptr;
        QLabel *m_status = nullptr;
        QLabel *m_selection = nullptr;
        QPushButton *m_open = nullptr;
        QPushButton *m_cancel = nullptr;
        QPushButton *m_parentButton = nullptr;
        QFrame *m_hexOverlay = nullptr;
        Sc2FrameHighlightRenderer m_highlightRenderer;
        QTimer *m_highlightTimer = nullptr;
        int m_highlightPhase = 0;
        QString m_currentDir;
        QString m_selectedFile;
        QString m_selectionFullText;
        QPoint m_dragPosition;
        bool m_dragging = false;
    };

}

namespace sc2dh::app
{
    void warmUpSc2FileOpenDialogHighlight(const QWidget *parent)
    {
        Sc2FrameHighlightRenderer::warmUp(sc2FileOpenDialogTargetSize(parent));
    }

    QString openSc2FileStyled(QWidget *parent, const QString &startPath)
    {
        Sc2FileOpenDialog dialog(parent, startPath);
        return dialog.exec() == QDialog::Accepted ? dialog.selectedFile() : QString();
    }

    QString openFolderStyled(QWidget *parent, const QString &startPath)
    {
        QFileDialog dialog(parent, QStringLiteral("Open SC2 Folder"), startPath);
        dialog.setObjectName(QStringLiteral("sc2FileDialog"));
        dialog.setOption(QFileDialog::DontUseNativeDialog, true);
        dialog.setOption(QFileDialog::ShowDirsOnly, true);
        dialog.setFileMode(QFileDialog::Directory);
        dialog.setAcceptMode(QFileDialog::AcceptOpen);
        dialog.setLabelText(QFileDialog::Accept, QStringLiteral("Open"));
        dialog.setMinimumSize(980, 640);
        return dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty()
            ? dialog.selectedFiles().front()
            : QString();
    }

    QString saveTextFileStyled(QWidget *parent, const QString &title, const QString &startPath)
    {
        QFileDialog dialog(parent, title, QFileInfo(startPath).absolutePath(), QStringLiteral("Text files (*.txt)"));
        dialog.setObjectName(QStringLiteral("sc2FileDialog"));
        dialog.setOption(QFileDialog::DontUseNativeDialog, true);
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setFileMode(QFileDialog::AnyFile);
        dialog.setDefaultSuffix(QStringLiteral("txt"));
        dialog.selectFile(QFileInfo(startPath).fileName());
        dialog.setMinimumSize(980, 640);
        return dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty()
            ? dialog.selectedFiles().front()
            : QString();
    }

}

