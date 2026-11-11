/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KomaPaintStyle.h"

#include <QApplication>
#include <QDockWidget>
#include <QEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QStyleFactory>
#include <QStyleOption>

#include <KomaPaintTokens.h>
#include <kis_global.h>

namespace
{

constexpr qreal borderWidth = 1.0;
constexpr qreal chevronStroke = 1.5;

/// Name of the base style Koma Paint draws on top of.
const QString baseStyleName = QStringLiteral("fusion");

/// Dock contents sit on the surface colour, so the bg colour between docks
/// reads as the gap between panels.
const char dockPaletteAppliedProperty[] = "_komaPaintDockPalette";

QRectF crisp(const QRect &rect)
{
    // Align 1px strokes to the pixel grid.
    return QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5);
}

void fillRounded(QPainter *painter, const QRect &rect, qreal radius,
                 const QColor &fill, const QColor &border = QColor())
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setBrush(fill.isValid() ? QBrush(fill) : Qt::NoBrush);
    painter->setPen(border.isValid() ? QPen(border, borderWidth) : Qt::NoPen);
    painter->drawRoundedRect(crisp(rect), radius, radius);
    painter->restore();
}

enum class Direction { Up, Down, Left, Right };

void drawChevron(QPainter *painter, const QRect &rect, Direction direction, const QColor &color)
{
    const qreal size = qMin<qreal>(qMin(rect.width(), rect.height()), 10.0) * 0.5;
    if (size <= 0) {
        return;
    }
    const QPointF c = QRectF(rect).center();
    const qreal h = size * 0.5;

    QPainterPath path;
    switch (direction) {
    case Direction::Down:
        path.moveTo(c.x() - size, c.y() - h);
        path.lineTo(c.x(), c.y() + h);
        path.lineTo(c.x() + size, c.y() - h);
        break;
    case Direction::Up:
        path.moveTo(c.x() - size, c.y() + h);
        path.lineTo(c.x(), c.y() - h);
        path.lineTo(c.x() + size, c.y() + h);
        break;
    case Direction::Left:
        path.moveTo(c.x() + h, c.y() - size);
        path.lineTo(c.x() - h, c.y());
        path.lineTo(c.x() + h, c.y() + size);
        break;
    case Direction::Right:
        path.moveTo(c.x() - h, c.y() - size);
        path.lineTo(c.x() + h, c.y());
        path.lineTo(c.x() - h, c.y() + size);
        break;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(color, chevronStroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->drawPath(path);
    painter->restore();
}

bool isToolBoxButton(const QWidget *widget)
{
    for (const QWidget *w = widget; w; w = w->parentWidget()) {
        if (w->inherits("KoToolBoxButton") || w->inherits("KoToolBox")) {
            return true;
        }
    }
    return false;
}

bool isEnabled(const QStyleOption *option)
{
    return option->state & QStyle::State_Enabled;
}

bool isHovered(const QStyleOption *option)
{
    return isEnabled(option) && (option->state & QStyle::State_MouseOver);
}

QColor inputBorder(const QStyleOption *option, const KomaPaint::Theme &t)
{
    return (option->state & QStyle::State_HasFocus) ? t.pulse : t.border;
}

void applyDockPalette(QWidget *dock)
{
    QPalette palette = dock->palette();
    const KomaPaint::Theme t = KomaPaint::themeFor(qApp->palette());
    palette.setColor(QPalette::Window, t.surface);
    dock->setPalette(palette);
    dock->setAutoFillBackground(true);
}

/// Re-applies the dock surface colour whenever the colour scheme changes.
class DockPaletteFollower : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::ApplicationPaletteChange) {
            if (QWidget *dock = qobject_cast<QWidget*>(watched)) {
                applyDockPalette(dock);
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

QRect scrollBarSliderRect(const QStyleOptionSlider *bar)
{
    const QRect groove = bar->rect;
    const bool horizontal = bar->orientation == Qt::Horizontal;
    const int length = horizontal ? groove.width() : groove.height();
    const qint64 range = qint64(bar->maximum) - bar->minimum;

    int sliderLength = length;
    if (range > 0) {
        sliderLength = int(qint64(bar->pageStep) * length / (range + bar->pageStep));
        sliderLength = qBound(qMin(KomaPaint::Space::s5, length), sliderLength, length);
    }

    const int position = QStyle::sliderPositionFromValue(bar->minimum, bar->maximum,
                                                         bar->sliderPosition,
                                                         length - sliderLength,
                                                         bar->upsideDown);
    const QRect slider = horizontal
            ? QRect(groove.x() + position, groove.y(), sliderLength, groove.height())
            : QRect(groove.x(), groove.y() + position, groove.width(), sliderLength);
    return QStyle::visualRect(bar->direction, groove, slider);
}

} // namespace

KomaPaintStyle::KomaPaintStyle()
    : QProxyStyle(QStyleFactory::create(baseStyleName))
{
    setObjectName(KomaPaint::styleKey());
}

bool KomaPaintStyle::installIfRequested(const QString &key)
{
    if (!key.isEmpty() && key.compare(KomaPaint::styleKey(), Qt::CaseInsensitive) != 0) {
        return false;
    }
    QApplication::setStyle(new KomaPaintStyle());
    // Krita widgets tune their geometry for the style underneath.
    qApp->setProperty(currentUnderlyingStyleNameProperty, baseStyleName);
    return true;
}

void KomaPaintStyle::polish(QWidget *widget)
{
    QProxyStyle::polish(widget);

    if (qobject_cast<QMenu*>(widget)) {
        // Rounded menu corners need a transparent window behind them.
        widget->setAttribute(Qt::WA_TranslucentBackground);
    }

    if (qobject_cast<QDockWidget*>(widget) && !widget->property(dockPaletteAppliedProperty).toBool()) {
        widget->setProperty(dockPaletteAppliedProperty, true);
        applyDockPalette(widget);
        widget->installEventFilter(new DockPaletteFollower(widget));
    }
}

void KomaPaintStyle::unpolish(QWidget *widget)
{
    if (qobject_cast<QMenu*>(widget)) {
        widget->setAttribute(Qt::WA_TranslucentBackground, false);
    }
    QProxyStyle::unpolish(widget);
}

void KomaPaintStyle::drawPrimitive(PrimitiveElement element, const QStyleOption *option,
                                   QPainter *painter, const QWidget *widget) const
{
    const KomaPaint::Theme t = KomaPaint::themeFor(option->palette);

    switch (element) {
    case PE_PanelButtonCommand:
    case PE_PanelButtonBevel: {
        const QStyleOptionButton *button = qstyleoption_cast<const QStyleOptionButton*>(option);
        const bool isDefault = button && (button->features & QStyleOptionButton::DefaultButton);
        const bool isFlat = button && (button->features & QStyleOptionButton::Flat);
        const bool down = option->state & (State_Sunken | State_On);

        if (isDefault && isEnabled(option)) {
            QColor fill = t.pulse;
            if (down) {
                fill = fill.darker(112);
            } else if (isHovered(option)) {
                fill = fill.lighter(112);
            }
            fillRounded(painter, option->rect, KomaPaint::Radius::sm, fill);
            return;
        }

        if (isFlat && !down && !isHovered(option)) {
            return;
        }

        QColor fill = option->palette.color(QPalette::Button);
        if (down || isHovered(option)) {
            fill = t.surfaceRaised;
        }
        fillRounded(painter, option->rect, KomaPaint::Radius::sm, fill, isFlat ? QColor() : t.border);
        return;
    }
    case PE_FrameDefaultButton:
    case PE_FrameButtonBevel:
    case PE_FrameButtonTool:
        return;
    case PE_PanelButtonTool: {
        const bool on = option->state & State_On;
        const bool down = option->state & State_Sunken;
        const bool autoRaise = option->state & State_AutoRaise;

        if (on && isToolBoxButton(widget)) {
            // The active tool is one of the few places pulse is used.
            fillRounded(painter, option->rect, KomaPaint::Radius::sm, t.pulse);
            return;
        }
        if (on || down) {
            fillRounded(painter, option->rect, KomaPaint::Radius::sm, t.surfaceRaised, t.border);
            return;
        }
        if (isHovered(option)) {
            fillRounded(painter, option->rect, KomaPaint::Radius::sm, t.surfaceRaised);
            return;
        }
        if (!autoRaise) {
            fillRounded(painter, option->rect, KomaPaint::Radius::sm,
                        option->palette.color(QPalette::Button), t.border);
        }
        return;
    }
    case PE_PanelLineEdit: {
        const QStyleOptionFrame *frame = qstyleoption_cast<const QStyleOptionFrame*>(option);
        if (frame && frame->lineWidth > 0) {
            fillRounded(painter, option->rect, KomaPaint::Radius::xs,
                        option->palette.color(QPalette::Base), inputBorder(option, t));
            return;
        }
        break;
    }
    case PE_FrameLineEdit:
        fillRounded(painter, option->rect, KomaPaint::Radius::xs, QColor(), inputBorder(option, t));
        return;
    case PE_IndicatorCheckBox:
    case PE_IndicatorItemViewItemCheck: {
        const int side = qMin(option->rect.width(), option->rect.height());
        QRect box(0, 0, side, side);
        box.moveCenter(option->rect.center());

        const bool checked = option->state & State_On;
        const bool partial = option->state & State_NoChange;
        if ((checked || partial) && isEnabled(option)) {
            fillRounded(painter, box, KomaPaint::Radius::xs, t.pulse);
        } else {
            fillRounded(painter, box, KomaPaint::Radius::xs,
                        option->palette.color(QPalette::Base),
                        isHovered(option) ? t.textMuted : t.border);
        }

        const QColor mark = isEnabled(option) ? t.onPulse : t.textMuted;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(mark, chevronStroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const QRectF r = QRectF(box).adjusted(side * 0.25, side * 0.25, -side * 0.25, -side * 0.25);
        if (partial) {
            painter->drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
        } else if (checked) {
            QPainterPath tick;
            tick.moveTo(r.left(), r.center().y());
            tick.lineTo(r.left() + r.width() * 0.4, r.bottom());
            tick.lineTo(r.right(), r.top());
            painter->drawPath(tick);
        }
        painter->restore();
        return;
    }
    case PE_IndicatorRadioButton: {
        const int side = qMin(option->rect.width(), option->rect.height());
        QRect circle(0, 0, side, side);
        circle.moveCenter(option->rect.center());

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const bool checked = option->state & State_On;
        if (checked && isEnabled(option)) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(t.pulse);
            painter->drawEllipse(crisp(circle));
            painter->setBrush(t.onPulse);
            const qreal dot = side * 0.18;
            painter->drawEllipse(QRectF(circle).center(), dot, dot);
        } else {
            painter->setPen(QPen(isHovered(option) ? t.textMuted : t.border, borderWidth));
            painter->setBrush(option->palette.color(QPalette::Base));
            painter->drawEllipse(crisp(circle));
            if (checked) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(t.textMuted);
                const qreal dot = side * 0.18;
                painter->drawEllipse(QRectF(circle).center(), dot, dot);
            }
        }
        painter->restore();
        return;
    }
    case PE_PanelMenu:
        fillRounded(painter, option->rect, KomaPaint::Radius::md, t.surface, t.border);
        return;
    case PE_FrameMenu:
        return;
    case PE_PanelTipLabel:
        painter->save();
        painter->fillRect(option->rect, option->palette.color(QPalette::ToolTipBase));
        painter->setPen(QPen(t.border, borderWidth));
        painter->drawRect(crisp(option->rect));
        painter->restore();
        return;
    case PE_Frame:
    case PE_FrameDockWidget:
    case PE_FrameTabWidget:
    case PE_FrameWindow:
        painter->save();
        painter->setPen(QPen(t.border, borderWidth));
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(crisp(option->rect));
        painter->restore();
        return;
    case PE_FrameGroupBox:
        fillRounded(painter, option->rect, KomaPaint::Radius::sm, QColor(), t.border);
        return;
    case PE_FrameTabBarBase: {
        const QStyleOptionTabBarBase *base = qstyleoption_cast<const QStyleOptionTabBarBase*>(option);
        if (!base) {
            break;
        }
        painter->save();
        painter->setPen(QPen(t.border, borderWidth));
        const QRectF r = crisp(base->rect);
        switch (base->shape) {
        case QTabBar::RoundedSouth:
        case QTabBar::TriangularSouth:
            painter->drawLine(r.topLeft(), r.topRight());
            break;
        case QTabBar::RoundedWest:
        case QTabBar::TriangularWest:
            painter->drawLine(r.topRight(), r.bottomRight());
            break;
        case QTabBar::RoundedEast:
        case QTabBar::TriangularEast:
            painter->drawLine(r.topLeft(), r.bottomLeft());
            break;
        default:
            painter->drawLine(r.bottomLeft(), r.bottomRight());
            break;
        }
        painter->restore();
        return;
    }
    case PE_IndicatorDockWidgetResizeHandle:
        // The gap itself is the separator; nothing is drawn in it.
        return;
    case PE_IndicatorToolBarSeparator: {
        painter->save();
        painter->setPen(QPen(t.border, borderWidth));
        const QRectF r = QRectF(option->rect);
        if (option->state & State_Horizontal) {
            const qreal x = qRound(r.center().x()) + 0.5;
            painter->drawLine(QPointF(x, r.top() + 4), QPointF(x, r.bottom() - 4));
        } else {
            const qreal y = qRound(r.center().y()) + 0.5;
            painter->drawLine(QPointF(r.left() + 4, y), QPointF(r.right() - 4, y));
        }
        painter->restore();
        return;
    }
    case PE_IndicatorToolBarHandle:
        return;
    case PE_IndicatorArrowDown:
    case PE_IndicatorSpinDown:
        drawChevron(painter, option->rect, Direction::Down, isEnabled(option) ? t.text : t.textMuted);
        return;
    case PE_IndicatorArrowUp:
    case PE_IndicatorSpinUp:
        drawChevron(painter, option->rect, Direction::Up, isEnabled(option) ? t.text : t.textMuted);
        return;
    case PE_IndicatorArrowLeft:
        drawChevron(painter, option->rect, Direction::Left, isEnabled(option) ? t.text : t.textMuted);
        return;
    case PE_IndicatorArrowRight:
        drawChevron(painter, option->rect, Direction::Right, isEnabled(option) ? t.text : t.textMuted);
        return;
    default:
        break;
    }

    QProxyStyle::drawPrimitive(element, option, painter, widget);
}

void KomaPaintStyle::drawControl(ControlElement element, const QStyleOption *option,
                                 QPainter *painter, const QWidget *widget) const
{
    const KomaPaint::Theme t = KomaPaint::themeFor(option->palette);

    switch (element) {
    case CE_PushButtonLabel: {
        const QStyleOptionButton *button = qstyleoption_cast<const QStyleOptionButton*>(option);
        if (button && (button->features & QStyleOptionButton::DefaultButton) && isEnabled(option)) {
            QStyleOptionButton copy(*button);
            copy.palette.setColor(QPalette::ButtonText, t.onPulse);
            QProxyStyle::drawControl(element, &copy, painter, widget);
            return;
        }
        break;
    }
    case CE_MenuItem:
    case CE_MenuBarItem: {
        // Hover in menus is a quiet raised fill; pulse stays for selection.
        if (const QStyleOptionMenuItem *item = qstyleoption_cast<const QStyleOptionMenuItem*>(option)) {
            QStyleOptionMenuItem copy(*item);
            copy.palette.setColor(QPalette::Highlight, t.surfaceRaised);
            copy.palette.setColor(QPalette::HighlightedText, t.text);
            if (element == CE_MenuItem) {
                copy.palette.setColor(QPalette::Window, t.surface);
            }
            QProxyStyle::drawControl(element, &copy, painter, widget);
            return;
        }
        break;
    }
    case CE_MenuBarEmptyArea:
        painter->fillRect(option->rect, option->palette.color(QPalette::Window));
        return;
    case CE_TabBarTabShape: {
        const QStyleOptionTab *tab = qstyleoption_cast<const QStyleOptionTab*>(option);
        if (!tab) {
            break;
        }
        const bool selected = tab->state & State_Selected;
        const QRect r = tab->rect;
        const int radius = KomaPaint::Radius::sm;

        // Rounded on the side away from the tab bar base.
        QRect shape = r;
        QRect marker;
        const int markerInset = KomaPaint::Space::s2;
        const int markerThickness = 2;
        switch (tab->shape) {
        case QTabBar::RoundedSouth:
        case QTabBar::TriangularSouth:
            shape.adjust(1, -radius, -1, -1);
            marker = QRect(r.left() + markerInset, r.top(), r.width() - 2 * markerInset, markerThickness);
            break;
        case QTabBar::RoundedWest:
        case QTabBar::TriangularWest:
            shape.adjust(1, 1, radius, -1);
            marker = QRect(r.right() - markerThickness + 1, r.top() + markerInset, markerThickness, r.height() - 2 * markerInset);
            break;
        case QTabBar::RoundedEast:
        case QTabBar::TriangularEast:
            shape.adjust(-radius, 1, -1, -1);
            marker = QRect(r.left(), r.top() + markerInset, markerThickness, r.height() - 2 * markerInset);
            break;
        default:
            shape.adjust(1, 1, -1, radius);
            marker = QRect(r.left() + markerInset, r.bottom() - markerThickness + 1, r.width() - 2 * markerInset, markerThickness);
            break;
        }

        if (selected || isHovered(option)) {
            painter->save();
            painter->setClipRect(r);
            fillRounded(painter, shape, radius, selected ? t.surface : t.surfaceRaised);
            painter->restore();
        }
        if (selected) {
            painter->fillRect(marker, t.pulse);
        }
        return;
    }
    case CE_HeaderSection: {
        painter->save();
        QColor fill = option->palette.color(QPalette::Window);
        if (isHovered(option) || (option->state & State_Sunken)) {
            fill = t.surfaceRaised;
        }
        painter->fillRect(option->rect, fill);
        painter->setPen(QPen(t.border, borderWidth));
        const QRectF r = crisp(option->rect);
        painter->drawLine(r.topRight(), r.bottomRight());
        painter->drawLine(r.bottomLeft(), r.bottomRight());
        painter->restore();
        return;
    }
    case CE_HeaderEmptyArea:
        painter->fillRect(option->rect, option->palette.color(QPalette::Window));
        return;
    case CE_ProgressBarGroove:
        fillRounded(painter, option->rect, KomaPaint::Radius::xs,
                    option->palette.color(QPalette::Base), t.border);
        return;
    case CE_ProgressBarContents: {
        const QStyleOptionProgressBar *bar = qstyleoption_cast<const QStyleOptionProgressBar*>(option);
        if (!bar || bar->minimum >= bar->maximum) {
            // Busy indicator: keep the base animation.
            break;
        }
        const qreal fraction = qBound(0.0,
                                      qreal(bar->progress - bar->minimum) / (bar->maximum - bar->minimum),
                                      1.0);
        if (fraction <= 0.0) {
            return;
        }
        const bool horizontal = bar->state & State_Horizontal;
        const bool inverted = bar->invertedAppearance != (bar->direction == Qt::RightToLeft && horizontal);
        QRect r = bar->rect.adjusted(1, 1, -1, -1);
        if (horizontal) {
            const int w = qRound(r.width() * fraction);
            r = inverted ? QRect(r.right() - w + 1, r.top(), w, r.height()) : QRect(r.left(), r.top(), w, r.height());
        } else {
            const int h = qRound(r.height() * fraction);
            r = inverted ? QRect(r.left(), r.top(), r.width(), h) : QRect(r.left(), r.bottom() - h + 1, r.width(), h);
        }
        fillRounded(painter, r, KomaPaint::Radius::xs, t.pulse);
        return;
    }
    case CE_Splitter:
        return;
    default:
        break;
    }

    QProxyStyle::drawControl(element, option, painter, widget);
}

void KomaPaintStyle::drawComplexControl(ComplexControl control, const QStyleOptionComplex *option,
                                        QPainter *painter, const QWidget *widget) const
{
    switch (control) {
    case CC_ScrollBar:
        drawScrollBar(option, painter, widget);
        return;
    case CC_Slider:
        drawSlider(option, painter, widget);
        return;
    case CC_ComboBox:
        drawComboBox(option, painter, widget);
        return;
    case CC_SpinBox:
        drawSpinBox(option, painter, widget);
        return;
    default:
        break;
    }
    QProxyStyle::drawComplexControl(control, option, painter, widget);
}

void KomaPaintStyle::drawScrollBar(const QStyleOptionComplex *option, QPainter *painter, const QWidget *widget) const
{
    const QStyleOptionSlider *bar = qstyleoption_cast<const QStyleOptionSlider*>(option);
    if (!bar) {
        QProxyStyle::drawComplexControl(CC_ScrollBar, option, painter, widget);
        return;
    }
    const KomaPaint::Theme t = KomaPaint::themeFor(option->palette);

    if (bar->maximum <= bar->minimum) {
        return;
    }

    const QRect slider = subControlRect(CC_ScrollBar, bar, SC_ScrollBarSlider, widget);
    const bool active = (bar->activeSubControls & SC_ScrollBarSlider)
            && (bar->state & (State_MouseOver | State_Sunken));

    // A thin pill inset from the edges, with no track and no arrow buttons.
    const int inset = 3;
    const QRect pill = slider.adjusted(inset, inset, -inset, -inset);
    const qreal radius = qMin(pill.width(), pill.height()) / 2.0;
    fillRounded(painter, pill, radius, active ? t.textMuted : t.border);
}

void KomaPaintStyle::drawSlider(const QStyleOptionComplex *option, QPainter *painter, const QWidget *widget) const
{
    const QStyleOptionSlider *slider = qstyleoption_cast<const QStyleOptionSlider*>(option);
    if (!slider) {
        QProxyStyle::drawComplexControl(CC_Slider, option, painter, widget);
        return;
    }
    const KomaPaint::Theme t = KomaPaint::themeFor(option->palette);

    const QRect groove = QProxyStyle::subControlRect(CC_Slider, slider, SC_SliderGroove, widget);
    const QRect handle = QProxyStyle::subControlRect(CC_Slider, slider, SC_SliderHandle, widget);
    const bool horizontal = slider->orientation == Qt::Horizontal;
    const int thickness = KomaPaint::Space::s1;

    QRect track = groove;
    if (horizontal) {
        track.setTop(groove.center().y() - thickness / 2);
        track.setHeight(thickness);
    } else {
        track.setLeft(groove.center().x() - thickness / 2);
        track.setWidth(thickness);
    }
    fillRounded(painter, track, KomaPaint::Radius::xs, t.border);

    // The filled part runs from the minimum end to the handle.
    QRect filled = track;
    const QPoint handleCenter = handle.center();
    if (horizontal) {
        const bool fromRight = slider->upsideDown != (slider->direction == Qt::RightToLeft);
        if (fromRight) {
            filled.setLeft(handleCenter.x());
        } else {
            filled.setRight(handleCenter.x());
        }
    } else {
        if (slider->upsideDown) {
            filled.setBottom(handleCenter.y());
        } else {
            filled.setTop(handleCenter.y());
        }
    }
    if (isEnabled(option) && filled.isValid()) {
        fillRounded(painter, filled, KomaPaint::Radius::xs, t.pulse);
    }

    const int diameter = qMin(qMin(handle.width(), handle.height()), KomaPaint::Space::s4);
    QRect knob(0, 0, diameter, diameter);
    knob.moveCenter(handleCenter);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(isHovered(option) ? QPen(t.pulse, borderWidth) : Qt::NoPen);
    painter->setBrush(isEnabled(option) ? t.text : t.textMuted);
    painter->drawEllipse(crisp(knob));
    painter->restore();
}

void KomaPaintStyle::drawComboBox(const QStyleOptionComplex *option, QPainter *painter, const QWidget *widget) const
{
    const QStyleOptionComboBox *combo = qstyleoption_cast<const QStyleOptionComboBox*>(option);
    if (!combo) {
        QProxyStyle::drawComplexControl(CC_ComboBox, option, painter, widget);
        return;
    }
    const KomaPaint::Theme t = KomaPaint::themeFor(option->palette);

    if (combo->frame || isHovered(option)) {
        const QColor fill = combo->editable
                ? option->palette.color(QPalette::Base)
                : (isHovered(option) ? t.surfaceRaised : option->palette.color(QPalette::Button));
        fillRounded(painter, option->rect, KomaPaint::Radius::xs, fill,
                    combo->frame ? inputBorder(option, t) : QColor());
    }

    if (combo->subControls & SC_ComboBoxArrow) {
        const QRect arrow = QProxyStyle::subControlRect(CC_ComboBox, combo, SC_ComboBoxArrow, widget);
        drawChevron(painter, arrow, Direction::Down, isEnabled(option) ? t.textMuted : t.border);
    }
}

void KomaPaintStyle::drawSpinBox(const QStyleOptionComplex *option, QPainter *painter, const QWidget *widget) const
{
    const QStyleOptionSpinBox *spin = qstyleoption_cast<const QStyleOptionSpinBox*>(option);
    if (!spin) {
        QProxyStyle::drawComplexControl(CC_SpinBox, option, painter, widget);
        return;
    }
    const KomaPaint::Theme t = KomaPaint::themeFor(option->palette);

    if (spin->frame) {
        fillRounded(painter, option->rect, KomaPaint::Radius::xs,
                    option->palette.color(QPalette::Base), inputBorder(option, t));
    }

    if (spin->buttonSymbols == QAbstractSpinBox::NoButtons) {
        return;
    }

    const struct {
        SubControl control;
        QAbstractSpinBox::StepEnabledFlag step;
        Direction direction;
    } buttons[] = {
        {SC_SpinBoxUp, QAbstractSpinBox::StepUpEnabled, Direction::Up},
        {SC_SpinBoxDown, QAbstractSpinBox::StepDownEnabled, Direction::Down},
    };

    for (const auto &button : buttons) {
        if (!(spin->subControls & button.control)) {
            continue;
        }
        const QRect r = QProxyStyle::subControlRect(CC_SpinBox, spin, button.control, widget);
        const bool enabled = isEnabled(option) && (spin->stepEnabled & button.step);
        if (enabled && (spin->activeSubControls & button.control) && (spin->state & State_MouseOver)) {
            fillRounded(painter, r.adjusted(1, 1, -1, -1), KomaPaint::Radius::xs, t.surfaceRaised);
        }
        if (spin->buttonSymbols == QAbstractSpinBox::PlusMinus) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(QPen(enabled ? t.text : t.textMuted, chevronStroke, Qt::SolidLine, Qt::RoundCap));
            const QPointF c = QRectF(r).center();
            const qreal s = qMin(r.width(), r.height()) * 0.2;
            painter->drawLine(QPointF(c.x() - s, c.y()), QPointF(c.x() + s, c.y()));
            if (button.control == SC_SpinBoxUp) {
                painter->drawLine(QPointF(c.x(), c.y() - s), QPointF(c.x(), c.y() + s));
            }
            painter->restore();
        } else {
            drawChevron(painter, r, button.direction, enabled ? t.text : t.textMuted);
        }
    }
}

QRect KomaPaintStyle::subControlRect(ComplexControl control, const QStyleOptionComplex *option,
                                     SubControl subControl, const QWidget *widget) const
{
    if (control == CC_ScrollBar) {
        if (const QStyleOptionSlider *bar = qstyleoption_cast<const QStyleOptionSlider*>(option)) {
            const QRect slider = scrollBarSliderRect(bar);
            const bool horizontal = bar->orientation == Qt::Horizontal;
            switch (subControl) {
            case SC_ScrollBarGroove:
                return bar->rect;
            case SC_ScrollBarSlider:
                return slider;
            case SC_ScrollBarAddLine:
            case SC_ScrollBarSubLine:
                return QRect();
            case SC_ScrollBarSubPage:
                return horizontal
                        ? QStyle::visualRect(bar->direction, bar->rect,
                                             QRect(bar->rect.left(), bar->rect.top(),
                                                   QStyle::visualRect(bar->direction, bar->rect, slider).left() - bar->rect.left(),
                                                   bar->rect.height()))
                        : QRect(bar->rect.left(), bar->rect.top(), bar->rect.width(), slider.top() - bar->rect.top());
            case SC_ScrollBarAddPage: {
                if (horizontal) {
                    const QRect logical = QStyle::visualRect(bar->direction, bar->rect, slider);
                    return QStyle::visualRect(bar->direction, bar->rect,
                                              QRect(logical.right() + 1, bar->rect.top(),
                                                    bar->rect.right() - logical.right(), bar->rect.height()));
                }
                return QRect(bar->rect.left(), slider.bottom() + 1, bar->rect.width(), bar->rect.bottom() - slider.bottom());
            }
            default:
                break;
            }
        }
    }
    return QProxyStyle::subControlRect(control, option, subControl, widget);
}

int KomaPaintStyle::pixelMetric(PixelMetric metric, const QStyleOption *option, const QWidget *widget) const
{
    switch (metric) {
    case PM_DockWidgetSeparatorExtent:
        return KomaPaint::Space::s1;
    case PM_DockWidgetFrameWidth:
        return 1;
    case PM_ScrollBarExtent:
        return KomaPaint::Space::s3;
    case PM_ScrollBarSliderMin:
        return KomaPaint::Space::s5;
    case PM_ButtonShiftHorizontal:
    case PM_ButtonShiftVertical:
    case PM_TabBarTabShiftHorizontal:
    case PM_TabBarTabShiftVertical:
        return 0;
    case PM_MenuPanelWidth:
        return 1;
    case PM_MenuHMargin:
    case PM_MenuVMargin:
        return KomaPaint::Space::s1;
    case PM_ToolBarItemSpacing:
        return KomaPaint::Space::s1;
    case PM_SplitterWidth:
        return KomaPaint::Space::s1;
    default:
        break;
    }
    return QProxyStyle::pixelMetric(metric, option, widget);
}

int KomaPaintStyle::styleHint(StyleHint hint, const QStyleOption *option,
                              const QWidget *widget, QStyleHintReturn *returnData) const
{
    switch (hint) {
    case SH_DitherDisabledText:
        return false;
    case SH_ScrollBar_LeftClickAbsolutePosition:
        return true;
    case SH_DockWidget_ButtonsHaveFrame:
        return false;
    default:
        break;
    }
    return QProxyStyle::styleHint(hint, option, widget, returnData);
}
