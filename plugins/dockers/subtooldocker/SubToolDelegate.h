/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SUBTOOLDELEGATE_H
#define SUBTOOLDELEGATE_H

#include <QStyledItemDelegate>

#include <KisResourceThumbnailPainter.h>

/**
 * Draws one sub tool row: a preview on the left (the preset's stroke
 * thumbnail, or the tool icon) and the sub tool name beside it.
 *
 * Rows from the tool-variant model carry their tool id in ToolIdRole;
 * all other rows are treated as paintop preset resources.
 */
class SubToolDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    /// Role holding the tool id of a tool-variant row.
    static constexpr int ToolIdRole = Qt::UserRole + 1000;

    explicit SubToolDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    /// Preset names without the sorting prefix and underscores, e.g.
    /// "c)_Pencil-4_Soft" or "c) Pencil-4 Soft" becomes "Pencil-4 Soft".
    static QString displayName(const QString &resourceName);

private:
    KisResourceThumbnailPainter m_thumbnailPainter;
};

#endif // SUBTOOLDELEGATE_H
