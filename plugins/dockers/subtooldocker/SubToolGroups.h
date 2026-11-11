/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SUBTOOLGROUPS_H
#define SUBTOOLGROUPS_H

#include <QString>
#include <QStringList>
#include <QVector>

/**
 * The tool groups the Sub Tool panel shows.
 *
 * Brush tools show brush presets, grouped by preset tag. Every other group
 * lists the Krita tools that act as variants of one primary tool, the way a
 * single tool in Clip Studio Paint holds several sub tools (for example the
 * selection tool holds rectangle, ellipse, lasso and so on).
 */
namespace SubToolGroups
{

struct ToolGroup {
    QString id;          ///< Stable identifier, used to remember the last tab.
    QString title;       ///< Translated group title, shown as the tab label.
    QStringList toolIds; ///< Krita tool ids, in the order they are listed.
};

/// Tools whose sub tools are brush presets.
const QStringList &brushToolIds();

/// True if @p toolId is a brush tool.
bool isBrushTool(const QString &toolId);

/// All tool-variant groups, in display order.
const QVector<ToolGroup> &toolGroups();

/// The group that contains @p toolId, or nullptr if it belongs to none.
const ToolGroup *groupForTool(const QString &toolId);

/// Preferred order of preset tags (by tag URL); other tags follow by name.
const QStringList &preferredTagOrder();

}

#endif // SUBTOOLGROUPS_H
