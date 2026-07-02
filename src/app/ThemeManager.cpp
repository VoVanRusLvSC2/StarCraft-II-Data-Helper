#include "app/ThemeManager.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QFileInfo>
#include <QPainter>
#include <QProxyStyle>
#include <QStyleOption>
#include <QStyleOptionSlider>
#include <QTransform>
#include <QStringList>

#include <algorithm>

namespace {

void drawAxisTexture(QPainter *painter, const QRect &target, const QPixmap &source, Qt::Orientation orientation)
{
    if (!painter || target.isEmpty() || source.isNull()) {
        return;
    }

    QPixmap texture = source;
    if (orientation == Qt::Horizontal) {
        texture = texture.transformed(QTransform().rotate(90.0), Qt::SmoothTransformation);
        texture = texture.scaledToHeight(target.height(), Qt::SmoothTransformation);
    } else {
        texture = texture.scaledToWidth(target.width(), Qt::SmoothTransformation);
    }

    const int targetLength = orientation == Qt::Horizontal ? target.width() : target.height();
    const int sourceLength = orientation == Qt::Horizontal ? texture.width() : texture.height();
    const int cap = std::min({12, targetLength / 2, sourceLength / 4});
    if (cap <= 0 || targetLength <= cap * 2) {
        painter->drawPixmap(target, texture);
        return;
    }

    if (orientation == Qt::Horizontal) {
        painter->drawPixmap(QRect(target.left(), target.top(), cap, target.height()),
                            texture, QRect(0, 0, cap, texture.height()));
        painter->drawPixmap(QRect(target.right() - cap + 1, target.top(), cap, target.height()),
                            texture, QRect(texture.width() - cap, 0, cap, texture.height()));
        painter->drawTiledPixmap(QRect(target.left() + cap, target.top(), target.width() - cap * 2, target.height()),
                                 texture.copy(QRect(cap, 0, texture.width() - cap * 2, texture.height())));
    } else {
        painter->drawPixmap(QRect(target.left(), target.top(), target.width(), cap),
                            texture, QRect(0, 0, texture.width(), cap));
        painter->drawPixmap(QRect(target.left(), target.bottom() - cap + 1, target.width(), cap),
                            texture, QRect(0, texture.height() - cap, texture.width(), cap));
        painter->drawTiledPixmap(QRect(target.left(), target.top() + cap, target.width(), target.height() - cap * 2),
                                 texture.copy(QRect(0, cap, texture.width(), texture.height() - cap * 2)));
    }
}

void drawCheckTexture(QPainter *painter, QRect target, QStyle::State state)
{
    if (!painter || target.isEmpty()) {
        return;
    }

    static const QPixmap normal(QStringLiteral(":/textures/ui_glue_checkbox_normalpressed_terran.png"));
    static const QPixmap hover(QStringLiteral(":/textures/ui_glue_checkbox_normaloverpressedover_terran.png"));
    static const QPixmap mark(QStringLiteral(":/textures/ui_glue_checkboxmark_terran.png"));
    const QPixmap &frame = state.testFlag(QStyle::State_MouseOver) ? hover : normal;
    if (frame.isNull()) {
        return;
    }

    const int side = std::min({24, target.width(), target.height()});
    target = QRect(target.left() + (target.width() - side) / 2,
                   target.top() + (target.height() - side) / 2,
                   side,
                   side);
    const bool checked = state.testFlag(QStyle::State_On);
    const int frameHeight = frame.height() / 2;
    const QRect source(0, checked ? frameHeight : 0, frame.width(), frameHeight);

    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->setOpacity(state.testFlag(QStyle::State_Enabled) ? 1.0 : 0.45);
    painter->drawPixmap(target, frame, source);
    if (checked && !mark.isNull()) {
        painter->drawPixmap(target.adjusted(4, 4, -4, -4), mark);
    }
    painter->restore();
}

class Sc2ProxyStyle final : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;

    int pixelMetric(PixelMetric metric, const QStyleOption *option = nullptr, const QWidget *widget = nullptr) const override
    {
        if (metric == PM_ScrollBarExtent) {
            return 22;
        }
        if (metric == PM_ScrollBarSliderMin) {
            return 52;
        }
        if (metric == PM_IndicatorWidth || metric == PM_IndicatorHeight) {
            return 24;
        }
        if (metric == PM_CheckBoxLabelSpacing) {
            return 10;
        }
        return QProxyStyle::pixelMetric(metric, option, widget);
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option,
                       QPainter *painter, const QWidget *widget = nullptr) const override
    {
        if ((element == PE_IndicatorCheckBox || element == PE_IndicatorItemViewItemCheck) && option) {
            drawCheckTexture(painter, option->rect, option->state);
            return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

    QRect subControlRect(ComplexControl control, const QStyleOptionComplex *option,
                         SubControl subControl, const QWidget *widget = nullptr) const override
    {
        if (control != CC_ScrollBar) {
            return QProxyStyle::subControlRect(control, option, subControl, widget);
        }
        const auto *slider = qstyleoption_cast<const QStyleOptionSlider *>(option);
        if (!slider) {
            return {};
        }
        if (subControl == SC_ScrollBarSubLine || subControl == SC_ScrollBarAddLine) {
            return {};
        }

        const QRect area = slider->rect;
        if (subControl == SC_ScrollBarGroove) {
            return area;
        }

        const int length = slider->orientation == Qt::Vertical ? area.height() : area.width();
        const int range = slider->maximum - slider->minimum;
        const int page = qMax(1, slider->pageStep);
        const int sliderLength = range <= 0 ? length : qMin(length, qMax(38, (length * page) / (range + page)));
        const int available = qMax(0, length - sliderLength);
        const int position = QStyle::sliderPositionFromValue(slider->minimum, slider->maximum,
                                                              slider->sliderPosition, available,
                                                              slider->upsideDown);
        QRect sliderRect = slider->orientation == Qt::Vertical
                               ? QRect(area.left(), area.top() + position, area.width(), sliderLength)
                               : QRect(area.left() + position, area.top(), sliderLength, area.height());
        if (subControl == SC_ScrollBarSlider) {
            return sliderRect;
        }
        if (subControl == SC_ScrollBarSubPage) {
            return slider->orientation == Qt::Vertical
                       ? QRect(area.left(), area.top(), area.width(), qMax(0, sliderRect.top() - area.top()))
                       : QRect(area.left(), area.top(), qMax(0, sliderRect.left() - area.left()), area.height());
        }
        if (subControl == SC_ScrollBarAddPage) {
            return slider->orientation == Qt::Vertical
                       ? QRect(area.left(), sliderRect.bottom() + 1, area.width(), qMax(0, area.bottom() - sliderRect.bottom()))
                       : QRect(sliderRect.right() + 1, area.top(), qMax(0, area.right() - sliderRect.right()), area.height());
        }
        return {};
    }

    void drawComplexControl(ComplexControl control, const QStyleOptionComplex *option,
                            QPainter *painter, const QWidget *widget = nullptr) const override
    {
        if (control != CC_ScrollBar) {
            QProxyStyle::drawComplexControl(control, option, painter, widget);
            return;
        }
        const auto *slider = qstyleoption_cast<const QStyleOptionSlider *>(option);
        if (!slider) {
            return;
        }

        static const QPixmap track(QStringLiteral(":/textures/ui_nova_global_scrollbar_bg.png"));
        static const QPixmap handle(QStringLiteral(":/textures/ui_nova_global_scrollbarbutton_normal.png"));
        static const QPixmap handleOver(QStringLiteral(":/textures/ui_nova_global_scrollbarbutton_over.png"));
        drawAxisTexture(painter, subControlRect(control, option, SC_ScrollBarGroove, widget), track, slider->orientation);
        const bool hover = (slider->state & State_MouseOver) && (slider->activeSubControls & SC_ScrollBarSlider);
        drawAxisTexture(painter, subControlRect(control, option, SC_ScrollBarSlider, widget),
                        hover ? handleOver : handle, slider->orientation);
    }
};

bool loadStyleFromPath(const QString &path, QString *styleSheet)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    if (styleSheet) {
        *styleSheet = QString::fromUtf8(file.readAll());
    }
    return true;
}

QString readStyleFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QString stylePath(const QString &root, const QString &relative)
{
    if (root.startsWith(QStringLiteral(":/"))) {
        return root + QLatin1Char('/') + relative;
    }
    return QDir(root).filePath(relative);
}

QStringList darkThemeFiles()
{
    return {
        QStringLiteral("dark/00_foundation.qss"),
        QStringLiteral("dark/10_controls.qss"),
        QStringLiteral("dark/20_workspace.qss"),
        QStringLiteral("dark/30_dialogs.qss"),
        QStringLiteral("dark/40_optimization.qss"),
        QStringLiteral("dark/50_progress.qss"),
    };
}

bool loadStyleBundle(const QString &root, QString *styleSheet)
{
    QStringList parts;
    for (const QString &relative : darkThemeFiles()) {
        const QString path = stylePath(root, relative);
        const QString part = readStyleFile(path);
        if (part.isEmpty()) {
            return false;
        }
        parts.push_back(QStringLiteral("/* %1 */\n%2").arg(relative, part));
    }
    if (styleSheet) {
        *styleSheet = parts.join(QStringLiteral("\n\n"));
    }
    return true;
}

}

bool ThemeManager::applyDarkTheme(QApplication *application, QString *loadedFrom, QString *errorMessage)
{
    if (!application) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("No QApplication instance available.");
        }
        return false;
    }

    if (!application->property("sc2ProxyStyleInstalled").toBool()) {
        application->setStyle(new Sc2ProxyStyle(application->style()->name()));
        application->setProperty("sc2ProxyStyleInstalled", true);
    }

    const QString appStylesDir = QCoreApplication::applicationDirPath() + QStringLiteral("/resources/styles");
    const QString workStylesDir = QDir::currentPath() + QStringLiteral("/resources/styles");
    const QString projectStylesDir = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../../resources/styles"));

    const QStringList splitRoots = {
        QStringLiteral(":/styles"),
        appStylesDir,
        workStylesDir,
        projectStylesDir
    };

    for (const QString &root : splitRoots) {
        QString styleSheet;
        if (!loadStyleBundle(root, &styleSheet)) {
            continue;
        }

        application->setStyleSheet(styleSheet);
        if (loadedFrom) {
            *loadedFrom = root + QStringLiteral("/dark/*.qss");
        }
        return true;
    }

    const QStringList legacyCandidates = {
        QStringLiteral(":/styles/dark.qss"),
        QDir(appStylesDir).filePath(QStringLiteral("dark.qss")),
        QDir(workStylesDir).filePath(QStringLiteral("dark.qss")),
        QDir(projectStylesDir).filePath(QStringLiteral("dark.qss"))
    };

    for (const QString &candidate : legacyCandidates) {
        QString styleSheet;
        if (!loadStyleFromPath(candidate, &styleSheet) || styleSheet.trimmed().isEmpty()) {
            continue;
        }

        application->setStyleSheet(styleSheet);
        if (loadedFrom) {
            *loadedFrom = candidate;
        }
        return true;
    }

    application->setStyleSheet(QString());
    if (errorMessage) {
        *errorMessage = QStringLiteral("Unable to load dark theme from qrc or file-system fallbacks.");
    }
    return false;
}
