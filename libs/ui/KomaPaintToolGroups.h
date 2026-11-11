/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KOMAPAINTTOOLGROUPS_H
#define KOMAPAINTTOOLGROUPS_H

#include <QString>
#include <QStringList>
#include <QVector>

#include "kritaui_export.h"

/**
 * Koma Paint's tool groups, shared by the toolbox and the Sub Tool panel.
 *
 * As in Clip Studio Paint, one primary tool holds several sub tools: the
 * selection tool holds rectangle, ellipse, lasso and so on. The toolbox
 * shows one button per group and the Sub Tool panel lists the variants.
 * Brush tools are a group too; the Sub Tool panel shows their brush
 * presets, grouped by preset tag.
 */
namespace KomaPaintToolGroups
{

struct ToolGroup {
    QString id;          ///< Stable identifier, used to remember the last tab.
    QString title;       ///< Translated group title, shown as the tab label.
    QStringList toolIds; ///< Krita tool ids, in the order they are listed.
};

/// Tools whose sub tools are brush presets.
KRITAUI_EXPORT const QStringList &brushToolIds();

/// True if @p toolId is a brush tool.
KRITAUI_EXPORT bool isBrushTool(const QString &toolId);

/// All tool-variant groups, in display order (brushes not included).
KRITAUI_EXPORT const QVector<ToolGroup> &toolGroups();

/// The group that contains @p toolId, or nullptr if it belongs to none.
/// Brush tools are not in these groups; see brushGroup().
KRITAUI_EXPORT const ToolGroup *groupForTool(const QString &toolId);

/// Preferred order of preset tags (by tag URL); other tags follow by name.
KRITAUI_EXPORT const QStringList &preferredTagOrder();

/// The brush tool group, as the toolbox shows it.
KRITAUI_EXPORT const ToolGroup &brushGroup();

/// The toolbox group for @p toolId, brushes included, or nullptr.
KRITAUI_EXPORT const ToolGroup *toolboxGroupForTool(const QString &toolId);

}

#endif // KOMAPAINTTOOLGROUPS_H
