/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KOMALINEARTRECORDER_H
#define KOMALINEARTRECORDER_H

#include <QColor>
#include <QImage>
#include <QPainterPath>
#include <QScopedPointer>
#include <QPointer>
#include <QSet>
#include <QVector>

#include <kis_types.h>
#include <kis_distance_information.h>
#include <brushengine/kis_paint_information.h>

#include "flake/KomaLineartShape.h"
#include "kritaui_export.h"

class QPainter;
class KoViewConverter;
class KisCanvas2;
class KisShapeLayer;
class KUndo2Command;
class KisPainter;

/**
 * Records a brush-tool stroke on a vector layer.
 *
 * In ink mode the stroke becomes a KomaLineartShape: the pen positions and
 * the width from pen pressure form the line's centerline, which is smoothed
 * and simplified when the stroke ends. In eraser mode the stroke erases
 * lineart in one of three ways (see EraserMode). Either is one undo step.
 */
class KRITAUI_EXPORT KomaLineartRecorder
{
public:
    /// How the eraser treats lineart, as in Clip Studio Paint.
    enum class EraserMode {
        TouchedArea = 0,     ///< Erase only where the eraser passes.
        UpToIntersection,    ///< Erase up to the nearest crossing lines.
        WholeLine,           ///< Erase every line the eraser touches.
    };

    /// The eraser mode chosen in the brush tool options.
    static EraserMode eraserMode();
    static void setEraserMode(EraserMode mode);

    KomaLineartRecorder();
    ~KomaLineartRecorder();

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
    void applyPartialErase(KUndo2Command *parent);
    void requestUpdate(const QRectF &documentRect);
    QRectF previewBounds() const;

    /// Starts painting the live preview with the stroke's brush.
    bool beginBrushPreview(KisCanvas2 *canvas);
    /// Paints the newest segment of the live preview.
    void paintBrushPreview(const QPointF &documentPoint, qreal pressure);
    void endBrushPreview();
    QPointF documentToPixel(const QPointF &documentPoint) const;
    QRectF pixelToDocument(const QRectF &pixelRect) const;

    QPointer<KisCanvas2> m_canvas;
    KisShapeLayer *m_layer {nullptr};
    bool m_active {false};
    bool m_erase {false};

    qreal m_baseWidth {1.0};   ///< Line width at full pressure, document units.
    QColor m_color;
    KisPaintOpPresetSP m_brush;  ///< Snapshot of the brush, for rendering.
    QVector<KomaLineartPoint> m_raw;
    QPainterPath m_preview;     ///< Outline of the stroke so far, document units.

    // Live preview painted with the stroke's brush, in image pixels.
    qreal m_xRes {1.0};
    qreal m_yRes {1.0};
    KisImageSP m_brushImage;
    KisPaintLayerSP m_brushLayer;
    QScopedPointer<KisPainter> m_brushPainter;
    KisDistanceInformation m_brushDistance;
    KisPaintInformation m_lastBrushInfo;
    bool m_brushStarted {false};
    QImage m_brushPreview;      ///< The painted stroke so far.
    QRect m_brushPreviewRect;   ///< Image pixels covered by m_brushPreview.

    qreal m_eraserRadius {1.0};
    EraserMode m_eraserMode {EraserMode::UpToIntersection};
    QVector<QPointF> m_eraserTrail;  ///< Eraser positions, document units.
    QSet<KomaLineartShape*> m_erased;
};

#endif // KOMALINEARTRECORDER_H
