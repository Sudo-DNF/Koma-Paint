/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KOMAPAINTTOKENS_H
#define KOMAPAINTTOKENS_H

#include <QColor>
#include <QPalette>
#include <QRgb>
#include <QString>

/**
 * Koma Paint design-system tokens.
 *
 * These mirror the values in the Koma Paint design system. Pulse is the only
 * accent and is reserved for primary actions, the active tool and selection.
 */
namespace KomaPaint
{
namespace Dark
{
constexpr QRgb bg            = 0xFF14151A;
constexpr QRgb surface       = 0xFF24262E;
constexpr QRgb surfaceRaised = 0xFF2E313B;
constexpr QRgb border        = 0xFF3A3D49;
constexpr QRgb text          = 0xFFF6F6F8;
constexpr QRgb textMuted     = 0xFF9DA2B0;
constexpr QRgb pulse         = 0xFFFF2E7E;
constexpr QRgb pulseText     = 0xFFFF6BA3;
constexpr QRgb onPulse       = 0xFF14151A;
constexpr QRgb success       = 0xFF2FD4A0;
constexpr QRgb warning       = 0xFFFFB020;
constexpr QRgb danger        = 0xFFFF6A60;
}

namespace Light
{
constexpr QRgb bg            = 0xFFF6F6F8;
constexpr QRgb surface       = 0xFFFFFFFF;
constexpr QRgb surfaceRaised = 0xFFECEDF1;
constexpr QRgb border        = 0xFFD9DBE2;
constexpr QRgb text          = 0xFF14151A;
constexpr QRgb textMuted     = 0xFF5C6170;
constexpr QRgb pulse         = 0xFFFF2E7E;
constexpr QRgb pulseText     = 0xFFD81B60;
constexpr QRgb onPulse       = 0xFF14151A;
constexpr QRgb success       = 0xFF0B7F5C;
constexpr QRgb warning       = 0xFF8F5B00;
constexpr QRgb danger        = 0xFFC92F25;
}

namespace Space
{
constexpr int s1 = 4;
constexpr int s2 = 8;
constexpr int s3 = 12;
constexpr int s4 = 16;
constexpr int s5 = 24;
constexpr int s6 = 32;
constexpr int s7 = 48;
constexpr int s8 = 64;
}

namespace Radius
{
constexpr int xs = 4;    ///< inputs, sliders
constexpr int sm = 8;    ///< buttons, tool wells
constexpr int md = 12;   ///< panels, menus
constexpr int lg = 16;   ///< dialogs
constexpr int pill = 999;
}

/// True when the palette has a dark window background.
inline bool isDarkPalette(const QPalette &palette)
{
    return palette.color(QPalette::Window).lightness() < 128;
}

/// The token set matching a palette, resolved for light or dark themes.
struct Theme {
    QColor bg;
    QColor surface;
    QColor surfaceRaised;
    QColor border;
    QColor text;
    QColor textMuted;
    QColor pulse;
    QColor pulseText;
    QColor onPulse;
};

inline Theme themeFor(const QPalette &palette)
{
    if (isDarkPalette(palette)) {
        return Theme{QColor(Dark::bg), QColor(Dark::surface), QColor(Dark::surfaceRaised),
                     QColor(Dark::border), QColor(Dark::text), QColor(Dark::textMuted),
                     QColor(Dark::pulse), QColor(Dark::pulseText), QColor(Dark::onPulse)};
    }
    return Theme{QColor(Light::bg), QColor(Light::surface), QColor(Light::surfaceRaised),
                 QColor(Light::border), QColor(Light::text), QColor(Light::textMuted),
                 QColor(Light::pulse), QColor(Light::pulseText), QColor(Light::onPulse)};
}

/// Name of the colour scheme Koma Paint starts with.
inline QString defaultThemeName()
{
    return QStringLiteral("Koma Paint Dark");
}

/// Name of the default Koma Paint workspace.
inline QString defaultWorkspaceName()
{
    return QStringLiteral("Koma Studio");
}

/// Name of the default canvas input profile.
inline QString defaultInputProfileName()
{
    return QStringLiteral("Koma Paint");
}

/// Widget style key for the Koma Paint style.
inline QString styleKey()
{
    return QStringLiteral("komapaint");
}

/// Pulse colour suitable for text on the given palette (AA contrast).
inline QColor pulseTextColor(const QPalette &palette)
{
    return QColor(isDarkPalette(palette) ? Dark::pulseText : Light::pulseText);
}
}

#endif // KOMAPAINTTOKENS_H
