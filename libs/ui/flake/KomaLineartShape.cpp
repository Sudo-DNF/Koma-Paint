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
#include <memory>
#include <vector>
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
#include <QtConcurrentMap>
#include <KisRunnableStrokeJobsInterface.h>
#include <KisRunnableStrokeJobDataBase.h>
#include <kis_stroke_job_strategy.h>
#include <QCoreApplication>
#include <QThread>
#include <QThreadPool>

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
    /// An interactive render: pieces, each with its rect in shape units.
    QVector<QPair<QRectF, QImage>> pieces;

    qint64 sizeInBytes() const
    {
        if (!pieces.isEmpty()) {
            qint64 size = 0;
            for (const auto &piece : pieces) {
                size += piece.second.sizeInBytes();
            }
            return size;
        }
        return tiled ? tiles.sizeInBytes() : image.sizeInBytes();
    }

    void paint(QPainter &painter) const
    {
        if (!pieces.isEmpty()) {
            for (const auto &piece : pieces) {
                painter.drawImage(piece.first, piece.second);
            }
            return;
        }
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

/**
 * What rendering a line with its brush needs besides the line: an image, a
 * layer and a painter holding the brush engine. Creating them takes about a
 * third of a typical line's render (the paint op builds its brush masks), so
 * they are kept for the next line rendered with the same brush snapshot, at
 * the same size and in the same colour. Lines drawn in one stroke share their
 * snapshot, and an edited line keeps its own.
 */
/**
 * Runs the jobs a brush engine hands out (dab rendering, and blitting the
 * dabs into parts of the device) as a stroke would: jobs marked concurrent
 * that follow one another run in parallel, every other job runs alone, in
 * order. Each job writes its own dab or its own part of the device, so the
 * pixels do not depend on how the jobs are spread over threads.
 *
 * The jobs run on a thread pool of their own: lines are rendered from the
 * global pool's threads (a vector layer repaints in parallel), and jobs
 * queued behind those waiting threads would never run.
 */
class ConcurrentStrokeJobsExecutor : public KisRunnableStrokeJobsInterface
{
public:
    using KisRunnableStrokeJobsInterface::addRunnableJobs;

    void addRunnableJobs(const QVector<KisRunnableStrokeJobDataBase*> &list) override
    {
        QVector<KisRunnableStrokeJobDataBase*> batch;
        auto runBatch = [&batch]() {
            if (batch.size() == 1) {
                batch.first()->run();
            } else if (!batch.isEmpty()) {
                QtConcurrent::blockingMap(&jobPool(), batch, [](KisRunnableStrokeJobDataBase *job) { job->run(); });
            }
            batch.clear();
        };
        for (KisRunnableStrokeJobDataBase *job : list) {
            if (job->sequentiality() == KisStrokeJobData::CONCURRENT
                    && job->exclusivity() == KisStrokeJobData::NORMAL) {
                batch.append(job);
            } else {
                runBatch();
                job->run();
            }
        }
        runBatch();
        qDeleteAll(list);
    }

private:
    static QThreadPool &jobPool()
    {
        static QThreadPool pool;
        return pool;
    }
};

/**
 * The thread pool interactive pieces render on. Lines are rendered from the
 * global pool's threads and their brush jobs run on the executor's pool, so
 * pieces need a third pool: tasks waiting on a pool they occupy would never
 * finish.
 */
QThreadPool &piecePool()
{
    static QThreadPool pool;
    return pool;
}

struct BrushRenderContext {
    KisPaintOpPresetSP source;   ///< The snapshot the context paints with.
    qreal paintOpSize {0};
    QRgb color {0};
    KisImageSP image;
    KisPaintLayerSP layer;
    ConcurrentStrokeJobsExecutor jobs;
    std::unique_ptr<KisPainter> painter;

    bool matches(const KisPaintOpPresetSP &brush, qreal size, QRgb rgba) const
    {
        return source == brush && paintOpSize == size && color == rgba;
    }
};

/**
 * Keeps a few idle render contexts. A render takes one for itself and gives
 * it back afterwards, so contexts are never shared between threads.
 */
class BrushRenderContextPool
{
public:
    static BrushRenderContextPool &instance()
    {
        static BrushRenderContextPool pool;
        return pool;
    }

    std::unique_ptr<BrushRenderContext> take(const KisPaintOpPresetSP &brush, qreal size, QRgb color)
    {
        QMutexLocker locker(&m_mutex);
        for (auto it = m_idle.begin(); it != m_idle.end(); ++it) {
            if ((*it)->matches(brush, size, color)) {
                std::unique_ptr<BrushRenderContext> context = std::move(*it);
                m_idle.erase(it);
                return context;
            }
        }
        return nullptr;
    }

    void giveBack(std::unique_ptr<BrushRenderContext> context)
    {
        QMutexLocker locker(&m_mutex);
        if (!m_releaseRegistered) {
            // Release the contexts while the application still runs: images
            // and brush engines must not outlive the resource registries.
            qAddPostRoutine(&BrushRenderContextPool::releaseAll);
            m_releaseRegistered = true;
        }
        m_idle.push_back(std::move(context));
        // Enough for every rendering thread to keep its most recent brush.
        const size_t limit = size_t(qMax(2, QThread::idealThreadCount()) * 2);
        while (m_idle.size() > limit) {
            m_idle.erase(m_idle.begin());
        }
    }

private:
    static void releaseAll()
    {
        BrushRenderContextPool &pool = instance();
        std::vector<std::unique_ptr<BrushRenderContext>> idle;
        {
            QMutexLocker locker(&pool.m_mutex);
            std::swap(idle, pool.m_idle);
        }
    }

    QMutex m_mutex;
    std::vector<std::unique_ptr<BrushRenderContext>> m_idle;
    bool m_releaseRegistered {false};
};

/**
 * A render context for @p brush at @p paintOpSize in @p color: an idle one
 * from the pool with its device cleared, or a new one. Null if the brush
 * cannot paint.
 */
std::unique_ptr<BrushRenderContext> acquireBrushContext(const KisPaintOpPresetSP &brush, qreal paintOpSize,
                                                        const QColor &color, int width, int height)
{
    const KoColorSpace *colorSpace = KoColorSpaceRegistry::instance()->rgb8();
    std::unique_ptr<BrushRenderContext> context =
            BrushRenderContextPool::instance().take(brush, paintOpSize, color.rgba());
    if (context) {
        context->layer->paintDevice()->clear();
        return context;
    }
    context.reset(new BrushRenderContext);
    context->source = brush;
    context->paintOpSize = paintOpSize;
    context->color = color.rgba();
    // The layer's paint device is unbounded; the image only gives the
    // brush engine the image and node it expects.
    context->image = new KisImage(nullptr, width, height, colorSpace, QStringLiteral("lineart"));
    context->layer = new KisPaintLayer(context->image, QStringLiteral("lineart"), OPACITY_OPAQUE_U8, colorSpace);

    KisPaintOpPresetSP preset;
    {
        // Lines drawn in one stroke share their brush snapshot, and lines
        // are rendered from several threads when a vector layer repaints.
        static QMutex cloneMutex;
        QMutexLocker locker(&cloneMutex);
        preset = brush->clone().dynamicCast<KisPaintOpPreset>();
    }
    if (!preset || !preset->settings()) {
        return nullptr;
    }
    preset->settings()->setPaintOpSize(paintOpSize);

    context->painter.reset(new KisPainter(context->layer->paintDevice()));
    KisPainter &painter = *context->painter;
    painter.setPaintColor(KoColor(color, colorSpace));
    painter.setBackgroundColor(KoColor(Qt::white, colorSpace));
    painter.setCompositeOpId(COMPOSITE_OVER);
    painter.setOpacityF(preset->settings()->paintOpOpacity());
    painter.setRunnableStrokeJobsInterface(&context->jobs);
    painter.setPaintOpPreset(preset, context->layer, context->image);
    if (!painter.paintOp()) {
        return nullptr;
    }
    return context;
}

/// Brush engines may render their dabs asynchronously; runs the pending jobs until none are left.
void finishBrushJobs(BrushRenderContext &context)
{
    for (int round = 0; round < 10000; ++round) {
        QVector<KisRunnableStrokeJobData*> jobs;
        bool needsMoreUpdates = false;
        std::tie(std::ignore, needsMoreUpdates) = context.painter->paintOp()->doAsynchronousUpdate(jobs);
        const bool noJobs = jobs.isEmpty();
        context.jobs.addRunnableJobs(jobs);
        if (noJobs && !needsMoreUpdates) {
            break;
        }
    }
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

/// Samples per piece of an interactive render (about 64 px at 100%).
static constexpr int interactivePieceSamples = 128;

struct KomaLineartShape::InteractivePieces {
    struct Piece {
        QVector<KomaLineartPoint> samples;
        qreal scale {0};
        QRectF rect;
        QImage image;
    };
    QMutex mutex;
    QVector<Piece> pieces;
};

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
    m_interactivePieces = rhs.m_interactivePieces;
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

void KomaLineartShape::setInteractive(bool interactive)
{
    QSharedPointer<InteractivePieces> previous;
    {
        QMutexLocker locker(&m_cacheMutex);
        if (bool(m_interactivePieces) == interactive) {
            return;
        }
        previous = m_interactivePieces;
        m_interactivePieces = interactive
                ? QSharedPointer<InteractivePieces>(new InteractivePieces)
                : QSharedPointer<InteractivePieces>();
    }
    // Starting keeps the current render: the line is rendered in pieces
    // once it changes. Ending renders it whole again if pieces were shown.
    if (!interactive) {
        bool renderedInPieces = false;
        {
            QMutexLocker locker(&previous->mutex);
            renderedInPieces = !previous->pieces.isEmpty();
        }
        if (renderedInPieces) {
            invalidateRender();
            update();
        }
    }
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
        bool interactive = false;
        {
            QMutexLocker interactiveLocker(&m_cacheMutex);
            interactive = bool(m_interactivePieces);
        }
        if (!haveRender && interactive) {
            render.pieces = renderInteractive(scale);
            render.scale = scale;
            haveRender = !render.pieces.isEmpty();
            if (haveRender) {
                const int cost = int(qMax<qint64>(1, (render.sizeInBytes() + 1023) / 1024));
                QMutexLocker storeLocker(&store.mutex);
                store.renders.insert(cache->id, new CachedLineartRender(render), cost);
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

    const qreal paintOpSize = m_fullPressureWidth * renderScale;
    const QColor color = lineColor();
    BrushRenderContextPool &pool = BrushRenderContextPool::instance();
    std::unique_ptr<BrushRenderContext> context = acquireBrushContext(m_brush, paintOpSize, color, width, height);
    if (!context) {
        return QImage();
    }
    KisPainter &brushPainter = *context->painter;
    KisPaintDeviceSP device = context->layer->paintDevice();

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

    finishBrushJobs(*context);

    *localRect = QRectF(bounds.topLeft(), QSizeF(width, height) / renderScale);
    const QImage result = device->convertToQImage(nullptr, 0, 0, width, height);
    brushPainter.takeDirtyRegion();
    pool.giveBack(std::move(context));
    return result;
}

QVector<QPair<QRectF, QImage>> KomaLineartShape::renderInteractive(qreal scale) const
{
    QVector<QPair<QRectF, QImage>> result;
    QSharedPointer<InteractivePieces> pieces;
    {
        QMutexLocker locker(&m_cacheMutex);
        pieces = m_interactivePieces;
    }
    if (!pieces) {
        return result;
    }

    // The same samples a full render paints; pieces split them.
    const QVector<KomaLineartPoint> dense = densify(m_centerline, qMax(0.5 / scale, minimumWidth));
    if (dense.isEmpty()) {
        return result;
    }
    const qreal margin = m_fullPressureWidth * 0.75 + 2.0 / scale;
    const qreal paintOpSize = m_fullPressureWidth * scale;
    const QColor color = lineColor();
    auto paintInfo = [&](const KomaLineartPoint &point, const QPoint &origin) {
        const qreal share = point.width / m_fullPressureWidth;
        const qreal pressure = qBound(0.0, (share - minimumPressureShare) / (1.0 - minimumPressureShare), 1.0);
        return KisPaintInformation(point.position * scale - origin, pressure);
    };

    QMutexLocker locker(&pieces->mutex);
    const int count = qMax(1, (dense.size() - 1 + interactivePieceSamples - 1) / interactivePieceSamples);
    pieces->pieces.resize(count);

    // The pieces whose samples changed since they were last rendered.
    struct PieceJob {
        int index;
        QVector<KomaLineartPoint> samples;
        QRect pixelRect;
    };
    QVector<PieceJob> stale;
    for (int k = 0; k < count; ++k) {
        const int first = k * interactivePieceSamples;
        const int last = qMin(first + interactivePieceSamples, int(dense.size()) - 1);
        // Consecutive pieces share their boundary sample.
        QVector<KomaLineartPoint> samples = dense.mid(first, last - first + 1);
        const InteractivePieces::Piece &piece = pieces->pieces[k];
        if (piece.scale == scale && piece.samples == samples && !piece.image.isNull()) {
            continue;
        }
        // Pieces lie on the pixel grid, so unchanged ones fit wherever the line's bounds move.
        QRectF sampleBounds(samples.first().position, QSizeF());
        for (const KomaLineartPoint &sample : samples) {
            sampleBounds |= QRectF(sample.position, QSizeF());
        }
        sampleBounds.adjust(-margin, -margin, margin, margin);
        const QRect pixelRect = QRectF(sampleBounds.topLeft() * scale, sampleBounds.bottomRight() * scale).toAlignedRect();
        stale.append({k, std::move(samples), pixelRect});
    }

    // Each piece is painted on its own, so pieces render in parallel: a line
    // that starts being edited has all of its pieces rendered at once.
    std::atomic<bool> failed {false};
    auto renderPiece = [&](const PieceJob &job) {
        std::unique_ptr<BrushRenderContext> context =
                acquireBrushContext(m_brush, paintOpSize, color, job.pixelRect.width(), job.pixelRect.height());
        if (!context) {
            failed = true;
            return;
        }
        KisPainter &brushPainter = *context->painter;
        KisDistanceInformation distance;
        brushPainter.paintAt(paintInfo(job.samples.first(), job.pixelRect.topLeft()), &distance);
        for (int i = 1; i < job.samples.size(); ++i) {
            brushPainter.paintLine(paintInfo(job.samples[i - 1], job.pixelRect.topLeft()),
                                   paintInfo(job.samples[i], job.pixelRect.topLeft()), &distance);
        }
        finishBrushJobs(*context);
        brushPainter.takeDirtyRegion();

        InteractivePieces::Piece &piece = pieces->pieces[job.index];
        piece.samples = job.samples;
        piece.scale = scale;
        piece.rect = QRectF(QPointF(job.pixelRect.topLeft()) / scale, QSizeF(job.pixelRect.size()) / scale);
        piece.image = context->layer->paintDevice()->convertToQImage(nullptr, 0, 0,
                                                                     job.pixelRect.width(), job.pixelRect.height());
        BrushRenderContextPool::instance().giveBack(std::move(context));
    };
    if (stale.size() == 1) {
        renderPiece(stale.first());
    } else if (!stale.isEmpty()) {
        QtConcurrent::blockingMap(&piecePool(), stale, renderPiece);
    }
    if (failed) {
        return result;
    }

    result.reserve(count);
    for (const InteractivePieces::Piece &piece : std::as_const(pieces->pieces)) {
        result.append({piece.rect, piece.image});
    }
    return result;
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
    int line {-1}; ///< The polyline the segment belongs to, if indexed with others.

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

QVector<LineSegment> lineSegments(const QVector<QPointF> &points, int line = -1)
{
    QVector<LineSegment> segments;
    segments.reserve(qMax<qsizetype>(0, points.size() - 1));
    for (int i = 0; i + 1 < points.size(); ++i) {
        segments.append({points[i], points[i + 1], line});
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

struct KomaLineartShape::LineIndex::Private {
    explicit Private(QVector<LineSegment> segments)
        : index(std::move(segments))
    {
    }
    SegmentIndex index;
};

KomaLineartShape::LineIndex::LineIndex(const QVector<QVector<QPointF>> &lines)
{
    QVector<LineSegment> segments;
    for (int line = 0; line < lines.size(); ++line) {
        segments += lineSegments(lines[line], line);
    }
    d.reset(new Private(std::move(segments)));
}

KomaLineartShape::LineIndex::~LineIndex()
{
}

QVector<QVector<KomaLineartPoint>> KomaLineartShape::eraseUpToIntersection(const QVector<KomaLineartPoint> &dense,
                                                                         const QVector<QPointF> &eraserPath,
                                                                         qreal eraserRadius,
                                                                         const QVector<QVector<QPointF>> &otherLines)
{
    return eraseUpToIntersection(dense, eraserPath, eraserRadius, LineIndex(otherLines), -1);
}

QVector<QVector<KomaLineartPoint>> KomaLineartShape::eraseUpToIntersection(const QVector<KomaLineartPoint> &dense,
                                                                         const QVector<QPointF> &eraserPath,
                                                                         qreal eraserRadius,
                                                                         const LineIndex &lines, int ownLine)
{
    const QVector<bool> touched = touchedSamples(dense, eraserPath, eraserRadius);
    if (!touched.contains(true)) {
        return {dense};
    }

    // Where this line crosses the others, as fractional sample indices.
    QVector<qreal> crossings;
    for (int i = 0; i + 1 < dense.size(); ++i) {
        const QPointF first = dense[i].position;
        const QPointF last = dense[i + 1].position;
        lines.d->index.visit(QRectF(first, last).normalized(), [&](const LineSegment &segment) {
            qreal t = 0;
            if (segment.line != ownLine && crossing(first, last, segment.a, segment.b, &t)) {
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
