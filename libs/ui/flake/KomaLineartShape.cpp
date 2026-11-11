/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KomaLineartShape.h"

#include <QDomElement>
#include <QStringList>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>

#include <klocalizedstring.h>

#include <KoColorBackground.h>
#include <KoShapeLoadingContext.h>
#include <KoXmlNS.h>
#include <KoXmlWriter.h>
#include <SvgLoadingContext.h>
#include <SvgSavingContext.h>
#include <SvgStyleWriter.h>
#include <SvgUtil.h>
#include <kis_dom_utils.h>

namespace
{
/// Lines never get thinner than this, so a light touch still shows.
constexpr qreal minimumWidth = 0.05;

/// Cubic Bezier constant for a quarter circle.
constexpr qreal kappa = 0.5522847498;

const QString lineartType = QStringLiteral("komapaint-lineart");
const QString lineartAttribute = QStringLiteral("krita:lineart");

QPointF normalized(const QPointF &v)
{
    const qreal length = qSqrt(v.x() * v.x() + v.y() * v.y());
    return length > 0 ? v / length : QPointF(1, 0);
}

/// Smooth curve through @p points (Catmull-Rom as cubic Beziers), starting
/// from the path's current position, which must be points.first().
void appendCurveThrough(QPainterPath &path, const QVector<QPointF> &points)
{
    const int count = points.size();
    for (int i = 0; i + 1 < count; ++i) {
        const QPointF p0 = points[qMax(i - 1, 0)];
        const QPointF p1 = points[i];
        const QPointF p2 = points[i + 1];
        const QPointF p3 = points[qMin(i + 2, count - 1)];
        path.cubicTo(p1 + (p2 - p0) / 6.0, p2 - (p3 - p1) / 6.0, p2);
    }
}

/// Half circle around @p center from @p from (a radius vector) towards @p towards.
void appendRoundCap(QPainterPath &path, const QPointF &center, const QPointF &from, const QPointF &towards)
{
    const QPointF quarters[3] = {from, towards, -from};
    for (int i = 0; i < 2; ++i) {
        const QPointF u = quarters[i];
        const QPointF v = quarters[i + 1];
        path.cubicTo(center + u + v * kappa, center + v + u * kappa, center + v);
    }
}

/// Distance of @p p from the segment a-b.
qreal segmentDistance(const QPointF &p, const QPointF &a, const QPointF &b, qreal *t)
{
    const QPointF ab = b - a;
    const qreal lengthSquared = ab.x() * ab.x() + ab.y() * ab.y();
    *t = lengthSquared > 0 ? qBound(0.0, QPointF::dotProduct(p - a, ab) / lengthSquared, 1.0) : 0.0;
    const QPointF closest = a + ab * *t;
    const QPointF d = p - closest;
    return qSqrt(d.x() * d.x() + d.y() * d.y());
}

/// Ramer-Douglas-Peucker on position and width.
void simplifyRange(const QVector<KomaLineartPoint> &points, int first, int last, qreal tolerance, QVector<bool> &keep)
{
    if (last <= first + 1) {
        return;
    }
    qreal worst = 0;
    int worstIndex = -1;
    for (int i = first + 1; i < last; ++i) {
        qreal t = 0;
        const qreal positionError = segmentDistance(points[i].position, points[first].position, points[last].position, &t);
        const qreal expectedWidth = points[first].width + (points[last].width - points[first].width) * t;
        const qreal error = positionError + 0.5 * qAbs(points[i].width - expectedWidth);
        if (error > worst) {
            worst = error;
            worstIndex = i;
        }
    }
    if (worst > tolerance && worstIndex > 0) {
        keep[worstIndex] = true;
        simplifyRange(points, first, worstIndex, tolerance, keep);
        simplifyRange(points, worstIndex, last, tolerance, keep);
    }
}

void appendPainterPath(KoPathShape *shape, const QPainterPath &path)
{
    for (int i = 0; i < path.elementCount(); ++i) {
        const QPainterPath::Element element = path.elementAt(i);
        switch (element.type) {
        case QPainterPath::MoveToElement:
            shape->moveTo(element);
            break;
        case QPainterPath::LineToElement:
            shape->lineTo(element);
            break;
        case QPainterPath::CurveToElement:
            if (i + 2 < path.elementCount()) {
                shape->curveTo(element, path.elementAt(i + 1), path.elementAt(i + 2));
                i += 2;
            }
            break;
        case QPainterPath::CurveToDataElement:
            break;
        }
    }
    shape->close();
}
}

KomaLineartShape::KomaLineartShape()
{
    setShapeId(KomaLineartShapeId);
    setStroke(KoShapeStrokeModelSP());
}

KomaLineartShape::KomaLineartShape(const KomaLineartShape &rhs)
    : KoPathShape(rhs)
    , SvgShape()
    , m_centerline(rhs.m_centerline)
{
}

KomaLineartShape::~KomaLineartShape()
{
}

KoShape *KomaLineartShape::cloneShape() const
{
    return new KomaLineartShape(*this);
}

QString KomaLineartShape::pathShapeId() const
{
    return KomaLineartShapeId;
}

const QVector<KomaLineartPoint> &KomaLineartShape::centerline() const
{
    return m_centerline;
}

void KomaLineartShape::setLineColor(const QColor &color)
{
    setBackground(QSharedPointer<KoShapeBackground>(new KoColorBackground(color)));
}

QColor KomaLineartShape::lineColor() const
{
    const QSharedPointer<KoColorBackground> fill = qSharedPointerDynamicCast<KoColorBackground>(background());
    return fill ? fill->color() : QColor(Qt::black);
}

QPainterPath KomaLineartShape::outlineForCenterline(const QVector<KomaLineartPoint> &points)
{
    QPainterPath path;
    const int count = points.size();
    if (count == 0) {
        return path;
    }
    if (count == 1) {
        const qreal radius = qMax(points.first().width, minimumWidth) / 2.0;
        path.addEllipse(points.first().position, radius, radius);
        return path;
    }

    QVector<QPointF> left(count);
    QVector<QPointF> right(count);
    QVector<QPointF> offsets(count);
    for (int i = 0; i < count; ++i) {
        const QPointF tangent = normalized(points[qMin(i + 1, count - 1)].position - points[qMax(i - 1, 0)].position);
        const QPointF normal(-tangent.y(), tangent.x());
        offsets[i] = normal * (qMax(points[i].width, minimumWidth) / 2.0);
        left[i] = points[i].position + offsets[i];
        right[i] = points[i].position - offsets[i];
    }

    const QPointF endTangent = normalized(points[count - 1].position - points[count - 2].position);
    const QPointF startTangent = normalized(points[1].position - points[0].position);
    const qreal endRadius = qMax(points.last().width, minimumWidth) / 2.0;
    const qreal startRadius = qMax(points.first().width, minimumWidth) / 2.0;

    path.moveTo(left.first());
    appendCurveThrough(path, left);
    appendRoundCap(path, points.last().position, offsets.last(), endTangent * endRadius);

    QVector<QPointF> rightReversed(right.rbegin(), right.rend());
    appendCurveThrough(path, rightReversed);
    appendRoundCap(path, points.first().position, -offsets.first(), -startTangent * startRadius);
    path.closeSubpath();
    path.setFillRule(Qt::WindingFill);
    return path;
}

QVector<KomaLineartPoint> KomaLineartShape::processRawStroke(const QVector<KomaLineartPoint> &raw, qreal tolerance)
{
    if (raw.size() < 3) {
        return raw;
    }

    // Light smoothing removes hand jitter without moving the line's ends.
    QVector<KomaLineartPoint> smoothed = raw;
    for (int pass = 0; pass < 2; ++pass) {
        const QVector<KomaLineartPoint> source = smoothed;
        for (int i = 1; i + 1 < source.size(); ++i) {
            smoothed[i].position = (source[i - 1].position + 2.0 * source[i].position + source[i + 1].position) / 4.0;
            smoothed[i].width = (source[i - 1].width + 2.0 * source[i].width + source[i + 1].width) / 4.0;
        }
    }

    return simplify(smoothed, tolerance);
}

QVector<KomaLineartPoint> KomaLineartShape::simplify(const QVector<KomaLineartPoint> &points, qreal tolerance)
{
    if (points.size() < 3) {
        return points;
    }
    QVector<bool> keep(points.size(), false);
    keep.first() = true;
    keep.last() = true;
    simplifyRange(points, 0, points.size() - 1, tolerance, keep);

    QVector<KomaLineartPoint> result;
    for (int i = 0; i < points.size(); ++i) {
        if (keep[i]) {
            result.append(points[i]);
        }
    }
    return result;
}

QVector<KomaLineartPoint> KomaLineartShape::densify(const QVector<KomaLineartPoint> &points, qreal spacing)
{
    const int count = points.size();
    if (count < 2 || spacing <= 0) {
        return points;
    }

    QVector<KomaLineartPoint> dense;
    dense.append(points.first());
    for (int i = 0; i + 1 < count; ++i) {
        // The same curve the outline follows (Catmull-Rom).
        const QPointF p0 = points[qMax(i - 1, 0)].position;
        const QPointF p1 = points[i].position;
        const QPointF p2 = points[i + 1].position;
        const QPointF p3 = points[qMin(i + 2, count - 1)].position;
        const QPointF c1 = p1 + (p2 - p0) / 6.0;
        const QPointF c2 = p2 - (p3 - p1) / 6.0;

        const QPointF chord = p2 - p1;
        const qreal length = qSqrt(chord.x() * chord.x() + chord.y() * chord.y());
        const int steps = qMax(1, qCeil(length / spacing));
        for (int s = 1; s <= steps; ++s) {
            const qreal t = qreal(s) / steps;
            const qreal u = 1.0 - t;
            const QPointF position = u * u * u * p1 + 3 * u * u * t * c1 + 3 * u * t * t * c2 + t * t * t * p2;
            const qreal width = points[i].width + (points[i + 1].width - points[i].width) * t;
            dense.append({position, width});
        }
    }
    return dense;
}

namespace
{
/// Distance from @p p to the polyline @p path.
qreal distanceToPolyline(const QPointF &p, const QVector<QPointF> &path)
{
    if (path.isEmpty()) {
        return std::numeric_limits<qreal>::max();
    }
    if (path.size() == 1) {
        const QPointF d = p - path.first();
        return qSqrt(d.x() * d.x() + d.y() * d.y());
    }
    qreal best = std::numeric_limits<qreal>::max();
    for (int i = 0; i + 1 < path.size(); ++i) {
        qreal t = 0;
        best = qMin(best, segmentDistance(p, path[i], path[i + 1], &t));
    }
    return best;
}

/// Indices of the dense samples the eraser touches.
QVector<bool> touchedSamples(const QVector<KomaLineartPoint> &dense, const QVector<QPointF> &eraserPath, qreal radius)
{
    QVector<bool> touched(dense.size(), false);
    for (int i = 0; i < dense.size(); ++i) {
        touched[i] = distanceToPolyline(dense[i].position, eraserPath) <= radius + dense[i].width / 2.0;
    }
    return touched;
}

/// The point at fractional index @p u along a dense line.
KomaLineartPoint pointAt(const QVector<KomaLineartPoint> &dense, qreal u)
{
    const int i = qBound(0, int(std::floor(u)), dense.size() - 1);
    const int j = qMin(i + 1, dense.size() - 1);
    const qreal t = u - i;
    return {dense[i].position + (dense[j].position - dense[i].position) * t,
            dense[i].width + (dense[j].width - dense[i].width) * t};
}

/// The part of a dense line between fractional indices @p from and @p to.
QVector<KomaLineartPoint> pieceBetween(const QVector<KomaLineartPoint> &dense, qreal from, qreal to)
{
    QVector<KomaLineartPoint> piece;
    if (to - from <= 1e-6) {
        return piece;
    }
    piece.append(pointAt(dense, from));
    for (int i = int(std::floor(from)) + 1; i < to; ++i) {
        piece.append(dense[i]);
    }
    piece.append(pointAt(dense, to));
    return piece;
}

/// Fractional index along @p dense where segment i crosses segment q1-q2, if it does.
bool crossing(const QPointF &p1, const QPointF &p2, const QPointF &q1, const QPointF &q2, qreal *t)
{
    const QPointF r = p2 - p1;
    const QPointF s = q2 - q1;
    const qreal denominator = r.x() * s.y() - r.y() * s.x();
    if (qFuzzyIsNull(denominator)) {
        return false;
    }
    const QPointF qp = q1 - p1;
    const qreal a = (qp.x() * s.y() - qp.y() * s.x()) / denominator;
    const qreal b = (qp.x() * r.y() - qp.y() * r.x()) / denominator;
    if (a < 0 || a > 1 || b < 0 || b > 1) {
        return false;
    }
    *t = a;
    return true;
}
}

QVector<QVector<KomaLineartPoint>> KomaLineartShape::eraseTouchedArea(const QVector<KomaLineartPoint> &dense,
                                                                    const QVector<QPointF> &eraserPath,
                                                                    qreal eraserRadius)
{
    const QVector<bool> touched = touchedSamples(dense, eraserPath, eraserRadius);

    QVector<QVector<KomaLineartPoint>> pieces;
    QVector<KomaLineartPoint> current;
    for (int i = 0; i < dense.size(); ++i) {
        if (touched[i]) {
            if (current.size() >= 2) {
                pieces.append(current);
            }
            current.clear();
        } else {
            current.append(dense[i]);
        }
    }
    if (current.size() >= 2) {
        pieces.append(current);
    }
    return pieces;
}

QVector<QVector<KomaLineartPoint>> KomaLineartShape::eraseUpToIntersection(const QVector<KomaLineartPoint> &dense,
                                                                         const QVector<QPointF> &eraserPath,
                                                                         qreal eraserRadius,
                                                                         const QVector<QVector<QPointF>> &otherLines)
{
    const QVector<bool> touched = touchedSamples(dense, eraserPath, eraserRadius);
    if (!touched.contains(true)) {
        return {dense};
    }

    // Where this line crosses the others, as fractional sample indices.
    QVector<qreal> crossings;
    for (int i = 0; i + 1 < dense.size(); ++i) {
        for (const QVector<QPointF> &other : otherLines) {
            for (int k = 0; k + 1 < other.size(); ++k) {
                qreal t = 0;
                if (crossing(dense[i].position, dense[i + 1].position, other[k], other[k + 1], &t)) {
                    crossings.append(i + t);
                }
            }
        }
    }
    std::sort(crossings.begin(), crossings.end());

    // Each touched sample removes the stretch between the crossings around it.
    const qreal lastIndex = dense.size() - 1;
    QVector<QPair<qreal, qreal>> removed;
    for (int i = 0; i < dense.size(); ++i) {
        if (!touched[i]) {
            continue;
        }
        qreal from = 0.0;
        qreal to = lastIndex;
        for (qreal c : crossings) {
            if (c <= i) {
                from = c;
            } else {
                to = c;
                break;
            }
        }
        if (!removed.isEmpty() && from <= removed.last().second) {
            removed.last().second = qMax(removed.last().second, to);
        } else {
            removed.append({from, to});
        }
    }

    QVector<QVector<KomaLineartPoint>> pieces;
    qreal keptFrom = 0.0;
    for (const auto &range : removed) {
        const QVector<KomaLineartPoint> piece = pieceBetween(dense, keptFrom, range.first);
        if (piece.size() >= 2) {
            pieces.append(piece);
        }
        keptFrom = range.second;
    }
    const QVector<KomaLineartPoint> tail = pieceBetween(dense, keptFrom, lastIndex);
    if (tail.size() >= 2) {
        pieces.append(tail);
    }
    return pieces;
}

void KomaLineartShape::rebuildOutline()
{
    clear();
    appendPainterPath(this, outlineForCenterline(m_centerline));
}

void KomaLineartShape::setCenterline(const QVector<KomaLineartPoint> &points)
{
    m_centerline = points;
    rebuildOutline();

    // Keep the path's origin at its bounding box without moving the line.
    const QPointF offset = normalize();
    for (KomaLineartPoint &point : m_centerline) {
        point.position -= offset;
    }
    setTransformation(QTransform::fromTranslate(offset.x(), offset.y()) * transformation());
    notifyChanged();
}

void KomaLineartShape::setCenterlineInDocument(const QVector<KomaLineartPoint> &points)
{
    setTransformation(QTransform());
    setCenterline(points);
}

QVector<KomaLineartPoint> KomaLineartShape::centerlineInDocument() const
{
    const QTransform toDocument = absoluteTransformation();
    const qreal scale = qSqrt(qAbs(toDocument.determinant()));
    QVector<KomaLineartPoint> result;
    result.reserve(m_centerline.size());
    for (const KomaLineartPoint &point : m_centerline) {
        result.append({toDocument.map(point.position), point.width * scale});
    }
    return result;
}

bool KomaLineartShape::saveSvg(SvgSavingContext &context)
{
    const QTransform userSpace = context.userSpaceTransform();
    const qreal widthScale = qSqrt(qAbs(userSpace.determinant()));

    QStringList encoded;
    encoded.reserve(m_centerline.size());
    for (const KomaLineartPoint &point : m_centerline) {
        const QPointF p = userSpace.map(point.position);
        encoded << QStringLiteral("%1 %2 %3").arg(KisDomUtils::toString(p.x()),
                                                  KisDomUtils::toString(p.y()),
                                                  KisDomUtils::toString(point.width * widthScale));
    }

    context.shapeWriter().startElement("path");
    context.shapeWriter().addAttribute("id", context.getID(this));
    context.shapeWriter().addAttribute("krita:type", lineartType);
    SvgUtil::writeTransformAttributeLazy("transform", transformation(), context.shapeWriter());
    SvgStyleWriter::saveSvgStyle(this, context);
    context.shapeWriter().addAttribute("d", toString(userSpace));
    context.shapeWriter().addAttribute(lineartAttribute.toLatin1().constData(), encoded.join(QLatin1Char(';')));
    SvgStyleWriter::saveMetadata(this, context);
    context.shapeWriter().endElement();
    return true;
}

bool KomaLineartShape::loadSvg(const QDomElement &element, SvgLoadingContext &context)
{
    Q_UNUSED(context);
    if (element.attribute("krita:type") != lineartType) {
        return false;
    }

    QVector<KomaLineartPoint> points;
    const QStringList encoded = element.attribute(lineartAttribute).split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &entry : encoded) {
        const QStringList values = entry.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (values.size() != 3) {
            return false;
        }
        points.append({QPointF(SvgUtil::fromUserSpace(KisDomUtils::toDouble(values[0])),
                               SvgUtil::fromUserSpace(KisDomUtils::toDouble(values[1]))),
                       SvgUtil::fromUserSpace(KisDomUtils::toDouble(values[2]))});
    }
    if (points.isEmpty()) {
        return false;
    }

    // Same placement as an ordinary path: the parser applies the element's
    // transform on top of this position.
    m_centerline = points;
    rebuildOutline();
    const QPointF offset = normalize();
    for (KomaLineartPoint &point : m_centerline) {
        point.position -= offset;
    }
    setPosition(offset);
    return true;
}

KomaLineartShapeFactory::KomaLineartShapeFactory()
    : KoShapeFactoryBase(KomaLineartShapeId, i18nc("Shape name", "Lineart Stroke"))
{
    setToolTip(i18nc("@info:tooltip", "An editable ink line drawn on a vector layer"));
    setXmlElementNames(KoXmlNS::svg, QStringList() << QStringLiteral("krita:komapaint-lineart"));
    setLoadingPriority(1);
    setHidden(true);
}

KoShape *KomaLineartShapeFactory::createDefaultShape(KoDocumentResourceManager *documentResources) const
{
    Q_UNUSED(documentResources);
    return new KomaLineartShape();
}

bool KomaLineartShapeFactory::supports(const QDomElement &element, KoShapeLoadingContext &context) const
{
    Q_UNUSED(context);
    return element.attribute("krita:type") == lineartType;
}
