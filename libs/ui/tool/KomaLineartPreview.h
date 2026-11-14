/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KOMALINEARTPREVIEW_H
#define KOMALINEARTPREVIEW_H

#include <QImage>
#include <QMap>
#include <QPair>
#include <QRect>

#include "kritaui_export.h"

class QPainter;

class KRITAUI_EXPORT KomaLineartPreview
{
public:
    void clear();
    void update(const QRect &rect, const QImage &image);
    void setSparseImage(const QImage &image);
    QImage toImage(const QSize &size) const;
    void paint(QPainter &painter, qreal xRes, qreal yRes) const;
    QRect bounds() const;
    qint64 sizeInBytes() const;

private:
    QMap<QPair<int, int>, QImage> m_tiles;
    QRect m_bounds;
};

#endif
