/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KOMALINEARTSHAPE_H
#define KOMALINEARTSHAPE_H

#include <QColor>
#include <QImage>
#include <QMutex>
#include <QPainterPath>
#include <QPointF>
#include <QVector>

#include <KoPathShape.h>
#include <KoShapeFactoryBase.h>
#include <SvgShape.h>
#include <kundo2command.h>
#include <kis_types.h>

#include "kritaui_export.h"

#define KomaLineartShapeId "KomaLineartShape"

/**
 * One point on a lineart stroke's centerline.
 */
struct KomaLineartPoint {
    QPointF position; ///< In shape coordinates.
    qreal width {1.0}; ///< Full line width at this point, in shape units.

    bool operator==(const KomaLineartPoint &other) const
    {
        return position == other.position && qFuzzyCompare(width, other.width);
    }
};

/**
 * An editable ink line on a vector layer, in the manner of Clip Studio
 * Paint's vector lines.
 *
 * The stroke keeps its centerline: the points the pen passed through and
 * the line width at each, taken from pen pressure. The visible outline is
 * built from that centerline and filled with the line colour, so the shape
 * behaves as an ordinary filled path for selection, transformation and
 * export, while the line itself stays editable: it can be re-widened,
 * split, erased up to an intersection or simplified later.
 *
 * When the line knows the brush it was drawn with, it is rendered with
 * that brush (tip, texture, opacity) along its centerline, using the
 * stored widths as pen pressure, so pencils and textured brushes look the
 * same as on a paint layer. Without a brush it is drawn as a solid fill.
 *
 * In SVG the stroke is a normal filled path (any SVG reader can display
 * it) with the centerline stored in a "krita:lineart" attribute and the
 * brush referenced by name and MD5.
 */
class KRITAUI_EXPORT KomaLineartShape : public KoPathShape, public SvgShape
{
public:
    KomaLineartShape();
    KomaLineartShape(const KomaLineartShape &rhs);
    ~KomaLineartShape() override;

    KoShape *cloneShape() const override;
    QString pathShapeId() const override;

    /**
     * Sets the centerline, in document coordinates, and positions the shape
     * so that it covers the line. Used when a stroke is first drawn.
     */
    void setCenterlineInDocument(const QVector<KomaLineartPoint> &points);

    /// The centerline in shape coordinates.
    const QVector<KomaLineartPoint> &centerline() const;

    /// Replaces the centerline (shape coordinates) and rebuilds the outline.
    void setCenterline(const QVector<KomaLineartPoint> &points);

    /// The centerline mapped to document coordinates.
    QVector<KomaLineartPoint> centerlineInDocument() const;

    /**
     * Replaces the centerline with one given in document coordinates,
     * keeping the shape's transformation (rotation, scale) as it is.
     */
    void setCenterlineFromDocument(const QVector<KomaLineartPoint> &points);

    /**
     * Share of the full width a line keeps at the lightest pen pressure.
     * Widths map back to pen pressure with it when the line is rendered
     * with its brush.
     */
    static constexpr qreal minimumPressureShare = 0.15;

    /// Restores a centerline and transformation saved earlier (for undo).
    void restoreState(const QVector<KomaLineartPoint> &centerline, const QTransform &transformation);

    void setLineColor(const QColor &color);
    QColor lineColor() const;

    /**
     * Sets the brush the line is rendered with.
     * @param preset a resource snapshot of the brush, safe to use from any
     *        thread, or null to draw the line as a solid fill.
     * @param fullPressureWidth the line width at full pen pressure, in
     *        shape units; widths along the line are relative to it.
     */
    void setBrush(KisPaintOpPresetSP preset, qreal fullPressureWidth);
    KisPaintOpPresetSP brush() const;
    qreal fullPressureWidth() const;

    /**
     * Makes a thread-safe snapshot of @p preset for rendering lineart in
     * @p color (presets may read the foreground colour).
     */
    static KisPaintOpPresetSP brushSnapshot(KisPaintOpPresetSP preset, const QColor &color);

    // KoShape
    void setBackground(QSharedPointer<KoShapeBackground> background) override;
    void paint(QPainter &painter) const override;
    QRectF boundingRect() const override;

    /**
     * Builds the filled outline of a variable-width line through @p points:
     * smooth sides through the offset points and round caps at both ends.
     */
    static QPainterPath outlineForCenterline(const QVector<KomaLineartPoint> &points);

    /**
     * Smooths raw pen samples and removes points that do not change the
     * line's shape, keeping the width profile.
     * @param tolerance the largest deviation allowed when simplifying.
     */
    static QVector<KomaLineartPoint> processRawStroke(const QVector<KomaLineartPoint> &raw, qreal tolerance);

    /// Removes points that do not change the line's shape or width profile.
    static QVector<KomaLineartPoint> simplify(const QVector<KomaLineartPoint> &points, qreal tolerance);

    /**
     * Samples the smooth curve through @p points every @p spacing units,
     * so that geometry tests can treat the line as a polyline.
     */
    static QVector<KomaLineartPoint> densify(const QVector<KomaLineartPoint> &points, qreal spacing);

    /**
     * Erases the parts of a (densified) line touched by an eraser that
     * followed @p eraserPath with radius @p eraserRadius.
     * @return the pieces of the line that remain.
     */
    static QVector<QVector<KomaLineartPoint>> eraseTouchedArea(const QVector<KomaLineartPoint> &dense,
                                                              const QVector<QPointF> &eraserPath,
                                                              qreal eraserRadius);

    /**
     * Erases each touched part of a (densified) line up to the nearest
     * crossings with @p otherLines (or the line's ends), as Clip Studio
     * Paint's "erase up to intersection" does.
     * @return the pieces of the line that remain.
     */
    static QVector<QVector<KomaLineartPoint>> eraseUpToIntersection(const QVector<KomaLineartPoint> &dense,
                                                                   const QVector<QPointF> &eraserPath,
                                                                   qreal eraserRadius,
                                                                   const QVector<QVector<QPointF>> &otherLines);

    /**
     * Scales the width of a (densified) line around @p center: points
     * within @p radius are multiplied by @p factor, fading to no change at
     * the edge. Widths never go below a hairline.
     */
    static QVector<KomaLineartPoint> adjustWidth(const QVector<KomaLineartPoint> &dense,
                                                 const QPointF &center, qreal radius, qreal factor);

    /**
     * Joins two lines into one: @p first runs into @p second. @p firstAtEnd
     * says whether the first line is joined at its end (otherwise its start
     * is, and it is reversed); @p secondAtStart likewise for the second.
     */
    static QVector<KomaLineartPoint> connect(const QVector<KomaLineartPoint> &first, bool firstAtEnd,
                                             const QVector<KomaLineartPoint> &second, bool secondAtStart);

    // SvgShape
    bool saveSvg(SvgSavingContext &context) override;
    bool loadSvg(const QDomElement &element, SvgLoadingContext &context) override;

private:
    void rebuildOutline();
    void invalidateRender();
    /// Renders the line with its brush at @p scale device pixels per shape unit.
    QImage renderWithBrush(qreal scale, QRectF *localRect) const;

    QVector<KomaLineartPoint> m_centerline;

    KisPaintOpPresetSP m_brush;
    QString m_brushName;
    QString m_brushMd5;
    qreal m_fullPressureWidth {1.0};

    struct RenderCache {
        RenderCache();
        QMutex mutex;
        quint64 id;
    };
    mutable QMutex m_cacheMutex;
    QSharedPointer<RenderCache> m_renderCache {new RenderCache};
};

/**
 * Creates lineart strokes when vector layers are loaded. It is not offered
 * in the shape collection: lineart is drawn with the brush tool.
 */
class KRITAUI_EXPORT KomaLineartShapeFactory : public KoShapeFactoryBase
{
public:
    KomaLineartShapeFactory();

    KoShape *createDefaultShape(KoDocumentResourceManager *documentResources = nullptr) const override;
    bool supports(const QDomElement &element, KoShapeLoadingContext &context) const override;
};

/**
 * Undoable change of a lineart stroke's centerline (and the transformation
 * that comes with it).
 */
class KRITAUI_EXPORT KomaLineartEditCommand : public KUndo2Command
{
public:
    KomaLineartEditCommand(KomaLineartShape *shape,
                           const QVector<KomaLineartPoint> &oldCenterline, const QTransform &oldTransformation,
                           const QVector<KomaLineartPoint> &newCenterline, const QTransform &newTransformation,
                           const KUndo2MagicString &text, KUndo2Command *parent = nullptr);

    void redo() override;
    void undo() override;

private:
    KomaLineartShape *m_shape;
    QVector<KomaLineartPoint> m_oldCenterline;
    QTransform m_oldTransformation;
    QVector<KomaLineartPoint> m_newCenterline;
    QTransform m_newTransformation;
};

#endif // KOMALINEARTSHAPE_H
