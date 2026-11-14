/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KOMALINEARTRECORDER_H
#define KOMALINEARTRECORDER_H

#include <QColor>
#include <QPainterPath>
#include <QScopedPointer>
#include <QPointer>
#include <QSet>
#include <QVector>

#include <functional>

#include <kis_types.h>
#include <kis_distance_information.h>
#include <brushengine/kis_paint_information.h>

#include "flake/KomaLineartShape.h"
#include "KomaLineartPreview.h"
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

    /**
     * Everything a stroke needs from the canvas. begin(KisCanvas2*, ...)
     * fills it in from the canvas; tests and benchmarks fill it in
     * themselves to record strokes without a view.
     */
    struct Target {
        KisImageSP image;
        KisShapeLayer *layer {nullptr};
        KisPaintOpPresetSP preset;  ///< The current brush.
        QColor color;               ///< The foreground colour.
        EraserMode eraserMode {EraserMode::UpToIntersection};
        /// Called with each document rect whose on-canvas look changed.
        std::function<void(const QRectF &)> updateCanvas;
    };

    /// What erasing does to one line: the pieces that remain of it.
    struct PartialErase {
        KomaLineartShape *line {nullptr};
        QVector<QVector<KomaLineartPoint>> pieces; ///< Simplified, document units.
    };

    KomaLineartRecorder();
    ~KomaLineartRecorder();

    bool isActive() const;

    /**
     * Starts a stroke at @p documentPoint.
     * @param erase true to erase whole lines instead of inking.
     * @return false if the layer cannot take lineart.
     */
    bool begin(KisCanvas2 *canvas, KisShapeLayer *layer, const QPointF &documentPoint, qreal pressure, bool erase);

    /**
     * Starts a stroke on @p target without a canvas. The stroke is recorded
     * and previewed as with a canvas, but finish() only commits it when the
     * stroke was begun on a canvas.
     */
    bool begin(const Target &target, const QPointF &documentPoint, qreal pressure, bool erase);
    void add(const QPointF &documentPoint, qreal pressure);
    void finish();
    void cancel();

    /// Draws the stroke in progress (or the lines about to be erased).
    void paint(QPainter &painter, const KoViewConverter &converter) const;

    /**
     * The lines a partial eraser stroke (touched area or up to
     * intersection) changes, and what remains of each. Lines the eraser
     * only grazed are left out.
     */
    QVector<PartialErase> partialErase() const;

private:
    qreal widthForPressure(qreal pressure) const;
    void collectErasedLines(const QPointF &documentPoint);
    void applyPartialErase(KUndo2Command *parent);
    void requestUpdate(const QRectF &documentRect);
    QRectF previewBounds() const;

    /// Starts painting the live preview with the stroke's brush.
    bool beginBrushPreview();
    /// Paints the newest segment of the live preview.
    void paintBrushPreview(const QPointF &documentPoint, qreal pressure);
    void endBrushPreview();
    QPointF documentToPixel(const QPointF &documentPoint) const;
    QRectF pixelToDocument(const QRectF &pixelRect) const;

    QPointer<KisCanvas2> m_canvas;
    KisImageSP m_image;
    std::function<void(const QRectF &)> m_updateCanvas;
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
    KomaLineartPreview m_brushPreview;

    qreal m_eraserRadius {1.0};
    EraserMode m_eraserMode {EraserMode::UpToIntersection};
    QVector<QPointF> m_eraserTrail;  ///< Eraser positions, document units.
    QSet<KomaLineartShape*> m_erased;
    /// The document-space outlines of m_erased, taken when each was marked;
    /// a marked line does not change while the eraser stroke lasts.
    QVector<QPainterPath> m_erasedOutlines;
};

#endif // KOMALINEARTRECORDER_H
