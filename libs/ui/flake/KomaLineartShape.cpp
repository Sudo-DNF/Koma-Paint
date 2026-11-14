/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KomaLineartShape.h"
#include "tool/KomaLineartPreview.h"

#include <QDomElement>
#include <QCache>
#include <QStringList>
#include <QtMath>

#include <algorithm>
#include <atomic>
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
#include <KisGlobalResourcesInterface.h>
#include <KisResourceTypes.h>
#include <KoCanvasResourcesIds.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoLocalStrokeCanvasResources.h>
#include <brushengine/kis_paint_information.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_settings.h>
#include <kis_distance_information.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include <kis_painter.h>
#include <brushengine/kis_paintop.h>
#include <KisRunnableStrokeJobData.h>
#include <tuple>
#include <KoCompositeOpRegistry.h>
#include <QPainter>

namespace
{
/// Lines never get thinner than this, so a light touch still shows.
constexpr qreal minimumWidth = 0.05;

/// Cubic Bezier constant for a quarter circle.
constexpr qreal kappa = 0.5522847498;

const QString lineartType = QStringLiteral("komapaint-lineart");
const QString lineartAttribute = QStringLiteral("krita:lineart");

struct CachedLineartRender {
    QImage image;
    KomaLineartPreview tiles;
    QSize imageSize;
    QRectF rect;
    qreal scale;
    bool tiled {false};

    qint64 sizeInBytes() const
    {
        return tiled ? tiles.sizeInBytes() : image.sizeInBytes();
    }

    void paint(QPainter &painter) const
    {
        if (!tiled) {
            painter.drawImage(rect, image);
            return;
        }
        const QTransform transform = painter.transform();
        // Rotated tile clips can leave seams; non-SourceOver modes need transparent pixels too.
        if (transform.type() >= QTransform::TxRotate ||
            transform.m11() <= 0 || transform.m22() <= 0 ||
            painter.compositionMode() != QPainter::CompositionMode_SourceOver) {
            painter.drawImage(rect, tiles.toImage(imageSize));
            return;
        }
        painter.save();
        painter.translate(rect.topLeft());
        painter.scale(rect.width() / imageSize.width(), rect.height() / imageSize.height());
        painter.setClipRect(QRectF(QPointF(0, 0), imageSize), Qt::IntersectClip);
        tiles.paint(painter, 1, 1);
        painter.restore();
    }
};

CachedLineartRender makeCachedLineartRender(const QImage &image, const QRectF &rect, qreal scale)
{
    CachedLineartRender result;
    result.image = image;
    result.imageSize = image.size();
    result.rect = rect;
    result.scale = scale;

    if (image.format() != QImage::Format_ARGB32 || image.sizeInBytes() < 1024 * 1024) {
        return result;
    }

    result.tiles.setSparseImage(image);
    if (result.tiles.sizeInBytes() < image.sizeInBytes()) {
        result.tiled = true;
        result.image = QImage();
    } else {
        result.tiles.clear();
    }
    return result;
}

struct LineartRenderStore {
    QMutex mutex;
    // Costs are in KiB; old renders are evicted across all lineart shapes.
    QCache<quint64, CachedLineartRender> renders {128 * 1024};
};

LineartRenderStore &renderStore()
{
    static LineartRenderStore store;
    return store;
}

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

KomaLineartShape::RenderCache::RenderCache()
{
    static std::atomic<quint64> nextId {0};
    id = ++nextId;
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
    , m_brush(rhs.m_brush)
    , m_brushName(rhs.m_brushName)
    , m_brushMd5(rhs.m_brushMd5)
    , m_fullPressureWidth(rhs.m_fullPressureWidth)
{
    QMutexLocker locker(&rhs.m_cacheMutex);
    m_renderCache = rhs.m_renderCache;
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

void KomaLineartShape::setBackground(QSharedPointer<KoShapeBackground> background)
{
    KoPathShape::setBackground(background);
    invalidateRender();
}

void KomaLineartShape::setBrush(KisPaintOpPresetSP preset, qreal fullPressureWidth)
{
    m_brush = preset;
    m_brushName = preset ? preset->name() : QString();
    m_brushMd5 = preset ? preset->md5Sum() : QString();
    m_fullPressureWidth = qMax(fullPressureWidth, minimumWidth);
    invalidateRender();
    update();
}

KisPaintOpPresetSP KomaLineartShape::brush() const
{
    return m_brush;
}

qreal KomaLineartShape::fullPressureWidth() const
{
    return m_fullPressureWidth;
}

KisPaintOpPresetSP KomaLineartShape::brushSnapshot(KisPaintOpPresetSP preset, const QColor &color)
{
    if (!preset) {
        return KisPaintOpPresetSP();
    }
    // Presets may read canvas resources such as the foreground colour.
    KoLocalStrokeCanvasResourcesSP canvasResources(new KoLocalStrokeCanvasResources());
    canvasResources->storeResource(KoCanvasResource::ForegroundColor,
                                   QVariant::fromValue(KoColor(color, KoColorSpaceRegistry::instance()->rgb8())));
    canvasResources->storeResource(KoCanvasResource::BackgroundColor,
                                   QVariant::fromValue(KoColor(Qt::white, KoColorSpaceRegistry::instance()->rgb8())));
    return preset->cloneWithResourcesSnapshot(KisGlobalResourcesInterface::instance(), canvasResources, nullptr);
}

void KomaLineartShape::invalidateRender()
{
    QMutexLocker locker(&m_cacheMutex);
    m_renderCache.reset(new RenderCache);
}

QRectF KomaLineartShape::boundingRect() const
{
    QRectF bounds = KoPathShape::boundingRect();
    if (m_brush) {
        // Brush tips and textures can reach a little past the outline.
        const QTransform toDocument = absoluteTransformation();
        const qreal margin = m_fullPressureWidth * 0.5 * qSqrt(qAbs(toDocument.determinant()));
        bounds.adjust(-margin, -margin, margin, margin);
    }
    return bounds;
}

void KomaLineartShape::paint(QPainter &painter) const
{
    if (!m_brush || m_centerline.isEmpty()) {
        KoPathShape::paint(painter);
        return;
    }

    const qreal scale = qSqrt(qAbs(painter.transform().determinant()));
    if (scale <= 0) {
        return;
    }

    CachedLineartRender render;
    bool haveRender = false;
    QSharedPointer<RenderCache> cache;
    {
        QMutexLocker locker(&m_cacheMutex);
        cache = m_renderCache;
    }
    {
        QMutexLocker locker(&cache->mutex);
        LineartRenderStore &store = renderStore();
        {
            QMutexLocker storeLocker(&store.mutex);
            const CachedLineartRender *cached = store.renders.object(cache->id);
            if (cached && qFuzzyCompare(cached->scale, scale)) {
                render = *cached;
                haveRender = true;
            }
        }
        if (!haveRender) {
            QRectF rect;
            const QImage image = renderWithBrush(scale, &rect);
            if (!image.isNull()) {
                render = makeCachedLineartRender(image, rect, scale);
                haveRender = true;
                const int cost = int(qMax<qint64>(1, (render.sizeInBytes() + 1023) / 1024));
                QMutexLocker storeLocker(&store.mutex);
                store.renders.insert(cache->id, new CachedLineartRender(render), cost);
            }
        }
    }

    if (!haveRender) {
        KoPathShape::paint(painter);
        return;
    }
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    render.paint(painter);
    painter.restore();
}

QImage KomaLineartShape::renderWithBrush(qreal scale, QRectF *localRect) const
{
    const qreal margin = m_fullPressureWidth * 0.75 + 2.0 / scale;
    const QRectF bounds = outline().boundingRect().adjusted(-margin, -margin, margin, margin);

    // Keep very large or very zoomed renders within a sane size.
    constexpr qreal maximumPixels = 4096.0 * 4096.0;
    qreal renderScale = scale;
    const qreal pixels = bounds.width() * bounds.height() * renderScale * renderScale;
    if (pixels > maximumPixels) {
        renderScale *= qSqrt(maximumPixels / pixels);
    }

    const int width = qMax(1, qCeil(bounds.width() * renderScale));
    const int height = qMax(1, qCeil(bounds.height() * renderScale));

    const KoColorSpace *colorSpace = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(nullptr, width, height, colorSpace, QStringLiteral("lineart"));
    KisPaintLayerSP layer = new KisPaintLayer(image, QStringLiteral("lineart"), OPACITY_OPAQUE_U8, colorSpace);
    KisPaintDeviceSP device = layer->paintDevice();

    KisPaintOpPresetSP preset = m_brush->clone().dynamicCast<KisPaintOpPreset>();
    if (!preset || !preset->settings()) {
        return QImage();
    }
    preset->settings()->setPaintOpSize(m_fullPressureWidth * renderScale);

    KisPainter brushPainter(device);
    brushPainter.setPaintColor(KoColor(lineColor(), colorSpace));
    brushPainter.setBackgroundColor(KoColor(Qt::white, colorSpace));
    brushPainter.setCompositeOpId(COMPOSITE_OVER);
    brushPainter.setOpacityF(preset->settings()->paintOpOpacity());
    brushPainter.setPaintOpPreset(preset, layer, image);
    if (!brushPainter.paintOp()) {
        return QImage();
    }

    // Widths map back to the pen pressure the line was drawn with.
    auto paintInfo = [&](const KomaLineartPoint &point) {
        const qreal share = point.width / m_fullPressureWidth;
        const qreal pressure = qBound(0.0, (share - minimumPressureShare) / (1.0 - minimumPressureShare), 1.0);
        return KisPaintInformation((point.position - bounds.topLeft()) * renderScale, pressure);
    };

    const QVector<KomaLineartPoint> dense = densify(m_centerline, qMax(0.5 / renderScale, minimumWidth));
    KisDistanceInformation distance;
    brushPainter.paintAt(paintInfo(dense.first()), &distance);
    for (int i = 1; i < dense.size(); ++i) {
        brushPainter.paintLine(paintInfo(dense[i - 1]), paintInfo(dense[i]), &distance);
    }

    // Brush engines may render their dabs asynchronously; run the pending
    // jobs here until the engine has nothing left to do.
    for (int round = 0; round < 10000; ++round) {
        QVector<KisRunnableStrokeJobData*> jobs;
        bool needsMoreUpdates = false;
        std::tie(std::ignore, needsMoreUpdates) = brushPainter.paintOp()->doAsynchronousUpdate(jobs);
        for (KisRunnableStrokeJobData *job : jobs) {
            job->run();
            delete job;
        }
        if (jobs.isEmpty() && !needsMoreUpdates) {
            break;
        }
    }

    *localRect = QRectF(bounds.topLeft(), QSizeF(width, height) / renderScale);
    return device->convertToQImage(nullptr, 0, 0, width, height);
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
struct LineSegment {
    QPointF a;
    QPointF b;

    QRectF bounds() const
    {
        return QRectF(a, b).normalized();
    }
};

bool overlapsInclusive(const QRectF &a, const QRectF &b)
{
    return a.left() <= b.right() && a.right() >= b.left()
            && a.top() <= b.bottom() && a.bottom() >= b.top();
}

class SegmentIndex
{
public:
    explicit SegmentIndex(QVector<LineSegment> segments)
        : m_segments(std::move(segments))
    {
        if (!m_segments.isEmpty()) {
            m_nodes.reserve(2 * m_segments.size());
            build(0, m_segments.size());
        }
    }

    template<typename Visitor>
    bool visit(const QRectF &bounds, const Visitor &visitor) const
    {
        return !m_nodes.isEmpty() && visitNode(0, bounds, visitor);
    }

private:
    struct Node {
        QRectF bounds;
        int first;
        int last;
        int left {-1};
        int right {-1};
    };

    int build(int first, int last)
    {
        qreal left = std::numeric_limits<qreal>::max();
        qreal top = left;
        qreal right = std::numeric_limits<qreal>::lowest();
        qreal bottom = right;
        for (int i = first; i < last; ++i) {
            const QRectF rect = m_segments[i].bounds();
            left = qMin(left, rect.left());
            right = qMax(right, rect.right());
            top = qMin(top, rect.top());
            bottom = qMax(bottom, rect.bottom());
        }
        const int node = m_nodes.size();
        m_nodes.append({QRectF(QPointF(left, top), QPointF(right, bottom)), first, last});
        if (last - first > 8) {
            const bool xAxis = right - left >= bottom - top;
            const int middle = first + (last - first) / 2;
            std::nth_element(m_segments.begin() + first, m_segments.begin() + middle, m_segments.begin() + last,
                             [xAxis](const LineSegment &a, const LineSegment &b) {
                return xAxis ? a.a.x() + a.b.x() < b.a.x() + b.b.x()
                             : a.a.y() + a.b.y() < b.a.y() + b.b.y();
            });
            const int leftChild = build(first, middle);
            const int rightChild = build(middle, last);
            m_nodes[node].left = leftChild;
            m_nodes[node].right = rightChild;
        }
        return node;
    }

    template<typename Visitor>
    bool visitNode(int index, const QRectF &bounds, const Visitor &visitor) const
    {
        const Node &node = m_nodes[index];
        if (!overlapsInclusive(node.bounds, bounds)) {
            return false;
        }
        if (node.left >= 0) {
            return visitNode(node.left, bounds, visitor) || visitNode(node.right, bounds, visitor);
        }
        for (int i = node.first; i < node.last; ++i) {
            const LineSegment &segment = m_segments[i];
            if (overlapsInclusive(segment.bounds(), bounds) && visitor(segment)) {
                return true;
            }
        }
        return false;
    }

    QVector<LineSegment> m_segments;
    QVector<Node> m_nodes;
};

QVector<LineSegment> lineSegments(const QVector<QPointF> &points)
{
    QVector<LineSegment> segments;
    segments.reserve(qMax<qsizetype>(0, points.size() - 1));
    for (int i = 0; i + 1 < points.size(); ++i) {
        segments.append({points[i], points[i + 1]});
    }
    return segments;
}

/// Indices of the dense samples the eraser touches.
QVector<bool> touchedSamples(const QVector<KomaLineartPoint> &dense, const QVector<QPointF> &eraserPath, qreal radius)
{
    QVector<bool> touched(dense.size(), false);
    if (eraserPath.isEmpty()) {
        return touched;
    }
    QVector<LineSegment> segments = lineSegments(eraserPath);
    if (segments.isEmpty()) {
        segments.append({eraserPath.first(), eraserPath.first()});
    }
    const SegmentIndex index(std::move(segments));
    for (int i = 0; i < dense.size(); ++i) {
        const qreal reach = radius + dense[i].width / 2.0;
        if (reach < 0) {
            continue;
        }
        const QPointF point = dense[i].position;
        const QRectF search(point.x() - reach, point.y() - reach, 2 * reach, 2 * reach);
        touched[i] = index.visit(search, [&](const LineSegment &segment) {
            qreal t = 0;
            return segmentDistance(point, segment.a, segment.b, &t) <= reach;
        });
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
    QVector<LineSegment> otherSegments;
    for (const QVector<QPointF> &other : otherLines) {
        otherSegments += lineSegments(other);
    }
    const SegmentIndex index(std::move(otherSegments));
    QVector<qreal> crossings;
    for (int i = 0; i + 1 < dense.size(); ++i) {
        const QPointF first = dense[i].position;
        const QPointF last = dense[i + 1].position;
        index.visit(QRectF(first, last).normalized(), [&](const LineSegment &segment) {
            qreal t = 0;
            if (crossing(first, last, segment.a, segment.b, &t)) {
                crossings.append(i + t);
            }
            return false;
        });
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
    invalidateRender();
    clear();
    appendPainterPath(this, outlineForCenterline(m_centerline));
}

void KomaLineartShape::setCenterline(const QVector<KomaLineartPoint> &points)
{
    m_centerline = points;
    rebuildOutline();

    // Keep the path's origin at its bounding box. normalize() moves the
    // shape's transformation to compensate, so the line stays in place.
    const QPointF offset = normalize();
    for (KomaLineartPoint &point : m_centerline) {
        point.position -= offset;
    }
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

void KomaLineartShape::setCenterlineFromDocument(const QVector<KomaLineartPoint> &points)
{
    const QTransform toDocument = absoluteTransformation();
    const QTransform toShape = toDocument.inverted();
    const qreal scale = qSqrt(qAbs(toDocument.determinant()));
    const qreal widthToShape = scale > 0 ? 1.0 / scale : 1.0;

    QVector<KomaLineartPoint> local;
    local.reserve(points.size());
    for (const KomaLineartPoint &point : points) {
        local.append({toShape.map(point.position), point.width * widthToShape});
    }
    update();
    setCenterline(local);
    update();
}

void KomaLineartShape::restoreState(const QVector<KomaLineartPoint> &centerline, const QTransform &transformation)
{
    update();
    m_centerline = centerline;
    rebuildOutline();
    // The saved centerline is already normalized, so this keeps it in place.
    const QPointF offset = normalize();
    for (KomaLineartPoint &point : m_centerline) {
        point.position -= offset;
    }
    setTransformation(QTransform::fromTranslate(offset.x(), offset.y()) * transformation);
    notifyChanged();
    update();
}

QVector<KomaLineartPoint> KomaLineartShape::adjustWidth(const QVector<KomaLineartPoint> &dense,
                                                        const QPointF &center, qreal radius, qreal factor)
{
    QVector<KomaLineartPoint> result = dense;
    if (radius <= 0) {
        return result;
    }
    for (KomaLineartPoint &point : result) {
        const QPointF d = point.position - center;
        const qreal distance = qSqrt(d.x() * d.x() + d.y() * d.y());
        if (distance >= radius) {
            continue;
        }
        // Smooth falloff: full effect at the centre, none at the edge.
        const qreal t = distance / radius;
        const qreal weight = 1.0 - t * t * (3.0 - 2.0 * t);
        point.width = qMax(minimumWidth, point.width * (1.0 + (factor - 1.0) * weight));
    }
    return result;
}

QVector<KomaLineartPoint> KomaLineartShape::connect(const QVector<KomaLineartPoint> &first, bool firstAtEnd,
                                                    const QVector<KomaLineartPoint> &second, bool secondAtStart)
{
    QVector<KomaLineartPoint> joined = first;
    if (!firstAtEnd) {
        std::reverse(joined.begin(), joined.end());
    }
    QVector<KomaLineartPoint> tail = second;
    if (!secondAtStart) {
        std::reverse(tail.begin(), tail.end());
    }
    joined += tail;
    return joined;
}

KomaLineartEditCommand::KomaLineartEditCommand(KomaLineartShape *shape,
                                               const QVector<KomaLineartPoint> &oldCenterline, const QTransform &oldTransformation,
                                               const QVector<KomaLineartPoint> &newCenterline, const QTransform &newTransformation,
                                               const KUndo2MagicString &text, KUndo2Command *parent)
    : KUndo2Command(text, parent)
    , m_shape(shape)
    , m_oldCenterline(oldCenterline)
    , m_oldTransformation(oldTransformation)
    , m_newCenterline(newCenterline)
    , m_newTransformation(newTransformation)
{
}

void KomaLineartEditCommand::redo()
{
    KUndo2Command::redo();
    m_shape->restoreState(m_newCenterline, m_newTransformation);
}

void KomaLineartEditCommand::undo()
{
    KUndo2Command::undo();
    m_shape->restoreState(m_oldCenterline, m_oldTransformation);
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
    if (m_brush) {
        context.shapeWriter().addAttribute("krita:lineart-brush", m_brushName);
        context.shapeWriter().addAttribute("krita:lineart-brush-md5", m_brushMd5);
        context.shapeWriter().addAttribute("krita:lineart-full-width", KisDomUtils::toString(m_fullPressureWidth * widthScale));
    }
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

    // The brush is looked up by MD5 (then name); without it the line is
    // drawn as a solid fill.
    const QString brushName = element.attribute("krita:lineart-brush");
    const QString brushMd5 = element.attribute("krita:lineart-brush-md5");
    if (!brushName.isEmpty() || !brushMd5.isEmpty()) {
        KisPaintOpPresetSP preset = KisGlobalResourcesInterface::instance()
                ->source<KisPaintOpPreset>(ResourceType::PaintOpPresets)
                .bestMatch(brushMd5, QString(), brushName);
        const qreal fullWidth = SvgUtil::fromUserSpace(
                    KisDomUtils::toDouble(element.attribute("krita:lineart-full-width", "1")));
        m_fullPressureWidth = qMax(fullWidth, minimumWidth);
        m_brushName = brushName;
        m_brushMd5 = brushMd5;
        if (preset) {
            m_brush = brushSnapshot(preset, Qt::black);
        }
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
