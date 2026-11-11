/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KOMALINEARTRECORDER_H
#define KOMALINEARTRECORDER_H

#include <QColor>
#include <QPainterPath>
#include <QPointer>
#include <QSet>
#include <QVector>

#include <kis_types.h>

#include "flake/KomaLineartShape.h"

class QPainter;
class KoViewConverter;
class KisCanvas2;
class KisShapeLayer;

/**
 * Records a brush-tool stroke on a vector layer.
 *
 * In ink mode the stroke becomes a KomaLineartShape: the pen positions and
 * the width from pen pressure form the line's centerline, which is smoothed
 * and simplified when the stroke ends. In eraser mode every lineart stroke
 * the eraser touches is removed when the stroke ends ("erase whole line").
 * Both are single undo steps.
 */
class KomaLineartRecorder
{
public:
    KomaLineartRecorder();

    bool isActive() const;

    /**
     * Starts a stroke at @p documentPoint.
     * @param erase true to erase whole lines instead of inking.
     * @return false if the layer cannot take lineart.
     */
    bool begin(KisCanvas2 *canvas, KisShapeLayer *layer, const QPointF &documentPoint, qreal pressure, bool erase);
    void add(const QPointF &documentPoint, qreal pressure);
    void finish();
    void cancel();

    /// Draws the stroke in progress (or the lines about to be erased).
    void paint(QPainter &painter, const KoViewConverter &converter) const;

private:
    qreal widthForPressure(qreal pressure) const;
    void collectErasedLines(const QPointF &documentPoint);
    void requestUpdate(const QRectF &documentRect);
    QRectF previewBounds() const;

    QPointer<KisCanvas2> m_canvas;
    KisShapeLayer *m_layer {nullptr};
    bool m_active {false};
    bool m_erase {false};

    qreal m_baseWidth {1.0};   ///< Line width at full pressure, document units.
    QColor m_color;
    QVector<KomaLineartPoint> m_raw;
    QPainterPath m_preview;     ///< Outline of the stroke so far, document units.

    qreal m_eraserRadius {1.0};
    QSet<KomaLineartShape*> m_erased;
};

#endif // KOMALINEARTRECORDER_H
