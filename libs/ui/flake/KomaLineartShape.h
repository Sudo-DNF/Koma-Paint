/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KOMALINEARTSHAPE_H
#define KOMALINEARTSHAPE_H

#include <QColor>
#include <QPainterPath>
#include <QPointF>
#include <QVector>

#include <KoPathShape.h>
#include <KoShapeFactoryBase.h>
#include <SvgShape.h>

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
 * In SVG the stroke is a normal filled path (any SVG reader can display
 * it) with the centerline stored in a "krita:lineart" attribute.
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

    void setLineColor(const QColor &color);
    QColor lineColor() const;

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

    // SvgShape
    bool saveSvg(SvgSavingContext &context) override;
    bool loadSvg(const QDomElement &element, SvgLoadingContext &context) override;

private:
    void rebuildOutline();

    QVector<KomaLineartPoint> m_centerline;
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

#endif // KOMALINEARTSHAPE_H
