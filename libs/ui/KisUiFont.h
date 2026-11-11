/*
 * SPDX-FileCopyrightText: 2021 Alvin Wong <alvin@alvinhc.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KIS_UI_FONT_H
#define KIS_UI_FONT_H

#include "kritaui_export.h"

#include <QFont>

namespace KisUiFont
{

/**
 * @brief Gets a font for normal UI widgets to use.
 * @return the UI font.
 */
KRITAUI_EXPORT QFont normalFont();

/**
 * @brief Gets a font with a smallish font size for dock widgets to use.
 * @return the small font.
 */
KRITAUI_EXPORT QFont dockFont();

/// Heading levels from the Koma Paint type scale.
enum class HeadingLevel {
    H1, ///< Page titles: Poppins 32px bold.
    H2, ///< Section titles: Poppins 24px semibold.
    H3, ///< Card and dialog titles: Poppins 18px semibold.
};

/**
 * @brief Gets the Poppins heading font for a level of the type scale.
 * @return the heading font.
 */
KRITAUI_EXPORT QFont headingFont(HeadingLevel level);

} // namespace KisUiFont

#endif // KIS_UI_FONT_H
