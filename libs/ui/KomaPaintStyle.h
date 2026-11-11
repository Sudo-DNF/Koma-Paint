/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KOMAPAINTSTYLE_H
#define KOMAPAINTSTYLE_H

#include <QProxyStyle>

#include "kritaui_export.h"

/**
 * Koma Paint widget style.
 *
 * A proxy over Fusion that draws controls with the Koma Paint design
 * system: flat fills, 1px borders, rounded corners (xs for inputs and
 * sliders, sm for buttons and tool wells, md for menus), no gradients and
 * no shadows. Colours come from the application palette, so the style
 * follows the light and dark Koma Paint colour schemes.
 *
 * Pulse is used only for primary actions: the default dialog button, the
 * active tool in the toolbox, checked indicators, progress and the current
 * tab marker.
 */
class KRITAUI_EXPORT KomaPaintStyle : public QProxyStyle
{
    Q_OBJECT
public:
    KomaPaintStyle();

    /**
     * Installs the Koma Paint style on the application when @p key names it
     * (or is empty, which means "use the default"). Returns true if the style
     * was installed.
     */
    static bool installIfRequested(const QString &key);

    void polish(QWidget *widget) override;
    void unpolish(QWidget *widget) override;
    using QProxyStyle::polish;
    using QProxyStyle::unpolish;

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option,
                       QPainter *painter, const QWidget *widget = nullptr) const override;
    void drawControl(ControlElement element, const QStyleOption *option,
                     QPainter *painter, const QWidget *widget = nullptr) const override;
    void drawComplexControl(ComplexControl control, const QStyleOptionComplex *option,
                            QPainter *painter, const QWidget *widget = nullptr) const override;
    QRect subControlRect(ComplexControl control, const QStyleOptionComplex *option,
                         SubControl subControl, const QWidget *widget = nullptr) const override;
    int pixelMetric(PixelMetric metric, const QStyleOption *option = nullptr,
                    const QWidget *widget = nullptr) const override;
    int styleHint(StyleHint hint, const QStyleOption *option = nullptr,
                  const QWidget *widget = nullptr, QStyleHintReturn *returnData = nullptr) const override;

private:
    void drawScrollBar(const QStyleOptionComplex *option, QPainter *painter, const QWidget *widget) const;
    void drawSlider(const QStyleOptionComplex *option, QPainter *painter, const QWidget *widget) const;
    void drawComboBox(const QStyleOptionComplex *option, QPainter *painter, const QWidget *widget) const;
    void drawSpinBox(const QStyleOptionComplex *option, QPainter *painter, const QWidget *widget) const;
};

#endif // KOMAPAINTSTYLE_H
