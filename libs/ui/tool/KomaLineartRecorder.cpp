/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KomaLineartRecorder.h"

#include <QHash>
#include <QPainter>
#include <QtMath>
#include <QPainterPathStroker>

#include <kis_config.h>
#include <KisRunnableStrokeJobData.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>
#include <brushengine/kis_paintop.h>
#include <kis_paint_layer.h>
#include <kis_painter.h>
#include <tuple>

#include <kundo2command.h>
#include <klocalizedstring.h>

#include <KoCanvasResourceProvider.h>
#include <KoColor.h>
#include <KoShapeController.h>
#include <KoShapeManager.h>
#include <KoViewConverter.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_settings.h>
#include <kis_canvas2.h>
#include <kis_image.h>

#include "flake/kis_shape_layer.h"

namespace
{

/// Simplification tolerance, in image pixels.
constexpr qreal simplifyTolerancePixels = 0.35;

const QString eraserModeKey = QStringLiteral("KomaPaint/VectorEraserMode");

}

KomaLineartRecorder::EraserMode KomaLineartRecorder::eraserMode()
{
    const int stored = KisConfig(true).readEntry<int>(eraserModeKey, int(EraserMode::UpToIntersection));
    return EraserMode(qBound(0, stored, int(EraserMode::WholeLine)));
}

void KomaLineartRecorder::setEraserMode(EraserMode mode)
{
    KisConfig(false).writeEntry<int>(eraserModeKey, int(mode));
}

KomaLineartRecorder::KomaLineartRecorder()
{
}

KomaLineartRecorder::~KomaLineartRecorder()
{
    endBrushPreview();
}

QPointF KomaLineartRecorder::documentToPixel(const QPointF &documentPoint) const
{
    return QPointF(documentPoint.x() * m_xRes, documentPoint.y() * m_yRes);
}

QRectF KomaLineartRecorder::pixelToDocument(const QRectF &pixelRect) const
{
    return QRectF(pixelRect.x() / m_xRes, pixelRect.y() / m_yRes,
                  pixelRect.width() / m_xRes, pixelRect.height() / m_yRes);
}

bool KomaLineartRecorder::beginBrushPreview()
{
    endBrushPreview();
    if (!m_brush || !m_brush->settings()) {
        return false;
    }

    // A private image and layer give the brush engine what it expects;
    // only the tiles the stroke touches use memory.
    const QRect bounds = m_image->bounds();
    const KoColorSpace *colorSpace = KoColorSpaceRegistry::instance()->rgb8();
    m_brushImage = new KisImage(nullptr, qMax(1, bounds.width()), qMax(1, bounds.height()),
                                colorSpace, QStringLiteral("lineart preview"));
    m_brushLayer = new KisPaintLayer(m_brushImage, QStringLiteral("lineart preview"), OPACITY_OPAQUE_U8, colorSpace);

    KisPaintOpPresetSP preset = m_brush->clone().dynamicCast<KisPaintOpPreset>();
    if (!preset || !preset->settings()) {
        endBrushPreview();
        return false;
    }
    preset->settings()->setPaintOpSize(m_baseWidth * m_xRes);

    m_brushPainter.reset(new KisPainter(m_brushLayer->paintDevice()));
    m_brushPainter->setPaintColor(KoColor(m_color, colorSpace));
    m_brushPainter->setBackgroundColor(KoColor(Qt::white, colorSpace));
    m_brushPainter->setCompositeOpId(COMPOSITE_OVER);
    m_brushPainter->setOpacityF(preset->settings()->paintOpOpacity());
    m_brushPainter->setPaintOpPreset(preset, m_brushLayer, m_brushImage);
    if (!m_brushPainter->paintOp()) {
        endBrushPreview();
        return false;
    }

    m_brushDistance = KisDistanceInformation();
    m_brushStarted = false;
    m_brushPreview.clear();
    return true;
}

void KomaLineartRecorder::paintBrushPreview(const QPointF &documentPoint, qreal pressure)
{
    if (!m_brushPainter) {
        return;
    }

    // Same pressure the finished line will be rendered with.
    const KisPaintInformation info(documentToPixel(documentPoint), qBound(0.0, pressure, 1.0));
    if (!m_brushStarted) {
        m_brushPainter->paintAt(info, &m_brushDistance);
        m_brushStarted = true;
    } else {
        m_brushPainter->paintLine(m_lastBrushInfo, info, &m_brushDistance);
    }
    m_lastBrushInfo = info;

    // Brush engines may queue their dabs; render them now.
    for (int round = 0; round < 10000; ++round) {
        QVector<KisRunnableStrokeJobData*> jobs;
        bool needsMoreUpdates = false;
        std::tie(std::ignore, needsMoreUpdates) = m_brushPainter->paintOp()->doAsynchronousUpdate(jobs);
        for (KisRunnableStrokeJobData *job : jobs) {
            job->run();
            delete job;
        }
        if (jobs.isEmpty() && !needsMoreUpdates) {
            break;
        }
    }

    // Copy only the pixels that changed into the preview image.
    const QVector<QRect> dirty = m_brushPainter->takeDirtyRegion();
    if (dirty.isEmpty()) {
        return;
    }
    QRect changed;
    for (const QRect &rect : dirty) {
        changed |= rect;
    }

    for (const QRect &rect : dirty) {
        const QImage part = m_brushLayer->paintDevice()->convertToQImage(nullptr, rect.x(), rect.y(),
                                                                         rect.width(), rect.height());
        m_brushPreview.update(rect, part);
    }

    requestUpdate(pixelToDocument(changed));
}

void KomaLineartRecorder::endBrushPreview()
{
    m_brushPainter.reset();
    m_brushLayer = nullptr;
    m_brushImage = nullptr;
    m_brushStarted = false;
    m_brushPreview.clear();
}

bool KomaLineartRecorder::isActive() const
{
    return m_active;
}

qreal KomaLineartRecorder::widthForPressure(qreal pressure) const
{
    const qreal p = qBound(0.0, pressure, 1.0);
    return m_baseWidth * (KomaLineartShape::minimumPressureShare
                          + (1.0 - KomaLineartShape::minimumPressureShare) * p);
}

bool KomaLineartRecorder::begin(KisCanvas2 *canvas, KisShapeLayer *layer, const QPointF &documentPoint,
                                qreal pressure, bool erase)
{
    if (!canvas || !layer || !canvas->image() || !canvas->resourceManager()) {
        return false;
    }

    Target target;
    target.image = canvas->image();
    target.layer = layer;
    target.preset = canvas->resourceManager()
            ->resource(KoCanvasResource::CurrentPaintOpPreset).value<KisPaintOpPresetSP>();
    target.color = canvas->resourceManager()->foregroundColor().toQColor();
    target.eraserMode = eraserMode();
    const QPointer<KisCanvas2> guardedCanvas(canvas);
    target.updateCanvas = [guardedCanvas](const QRectF &documentRect) {
        if (guardedCanvas) {
            guardedCanvas->updateCanvas(documentRect);
        }
    };

    if (!begin(target, documentPoint, pressure, erase)) {
        return false;
    }
    m_canvas = canvas;
    return true;
}

bool KomaLineartRecorder::begin(const Target &target, const QPointF &documentPoint, qreal pressure, bool erase)
{
    if (!target.image || !target.layer) {
        return false;
    }

    m_canvas = nullptr;
    m_image = target.image;
    m_updateCanvas = target.updateCanvas;
    m_layer = target.layer;
    m_erase = erase;
    m_raw.clear();
    m_preview = QPainterPath();
    m_erased.clear();
    m_erasedOutlines.clear();
    m_eraserTrail.clear();
    m_eraserMode = target.eraserMode;

    // Brush size is in image pixels; shapes live in document units.
    const qreal pixelsPerUnit = qMax(m_image->xRes(), 1e-6);
    qreal sizeInPixels = 4.0;
    const KisPaintOpPresetSP &preset = target.preset;
    if (preset && preset->settings()) {
        sizeInPixels = qMax<qreal>(0.5, preset->settings()->paintOpSize());
    }
    m_baseWidth = sizeInPixels / pixelsPerUnit;
    m_eraserRadius = m_baseWidth / 2.0;
    m_color = target.color;
    m_color.setAlpha(255);
    // New lines are rendered with the brush they are drawn with, and the
    // stroke previews with it while it is drawn.
    m_brush = erase ? KisPaintOpPresetSP() : KomaLineartShape::brushSnapshot(preset, m_color);
    m_xRes = qMax(m_image->xRes(), 1e-6);
    m_yRes = qMax(m_image->yRes(), 1e-6);
    if (!erase) {
        beginBrushPreview();
    }

    m_active = true;
    add(documentPoint, pressure);
    return true;
}

void KomaLineartRecorder::add(const QPointF &documentPoint, qreal pressure)
{
    if (!m_active) {
        return;
    }

    if (m_erase) {
        collectErasedLines(documentPoint);
        return;
    }

    if (!m_raw.isEmpty() && m_raw.last().position == documentPoint) {
        return;
    }
    m_raw.append({documentPoint, widthForPressure(pressure)});
    if (m_brushPainter) {
        paintBrushPreview(documentPoint, pressure);
    } else {
        const QRectF before = m_preview.boundingRect();
        m_preview = KomaLineartShape::outlineForCenterline(m_raw);
        requestUpdate(before | m_preview.boundingRect());
    }
}

void KomaLineartRecorder::collectErasedLines(const QPointF &documentPoint)
{
    if (!m_layer) {
        return;
    }
    if (m_eraserTrail.isEmpty() || m_eraserTrail.last() != documentPoint) {
        m_eraserTrail.append(documentPoint);
    }
    QPainterPath eraser;
    eraser.addEllipse(documentPoint, m_eraserRadius, m_eraserRadius);
    requestUpdate(eraser.boundingRect());

    const QList<KoShape*> shapes = m_layer->shapeManager()->shapesAt(eraser.boundingRect());
    for (KoShape *shape : shapes) {
        KomaLineartShape *line = dynamic_cast<KomaLineartShape*>(shape);
        if (!line || line->parent() != m_layer || m_erased.contains(line) || !line->isVisible() || !line->isSelectable()) {
            continue;
        }
        const QPainterPath outline = line->absoluteTransformation().map(line->outline());
        if (outline.intersects(eraser)) {
            m_erased.insert(line);
            m_erasedOutlines.append(outline);
            requestUpdate(outline.boundingRect());
        }
    }
}

void KomaLineartRecorder::finish()
{
    if (!m_active) {
        return;
    }
    m_active = false;
    const QRectF dirty = previewBounds();

    if (m_canvas && m_layer) {
        if (m_erase) {
            if (!m_erased.isEmpty()) {
                KUndo2Command *command = new KUndo2Command(kundo2_i18n("Erase Lines"));
                if (m_eraserMode == EraserMode::WholeLine) {
                    QList<KoShape*> shapes;
                    for (KomaLineartShape *line : std::as_const(m_erased)) {
                        shapes.append(line);
                    }
                    m_canvas->shapeController()->removeShapes(shapes, command);
                } else {
                    applyPartialErase(command);
                }
                m_canvas->addCommand(command);
            }
        } else if (!m_raw.isEmpty()) {
            const qreal pixelsPerUnit = qMax(m_image->xRes(), 1e-6);
            const QVector<KomaLineartPoint> centerline =
                    KomaLineartShape::processRawStroke(m_raw, simplifyTolerancePixels / pixelsPerUnit);

            KomaLineartShape *line = new KomaLineartShape();
            line->setLineColor(m_color);
            line->setCenterlineInDocument(centerline);
            line->setBrush(m_brush, m_baseWidth);

            KUndo2Command *command = m_canvas->shapeController()->addShape(line, m_layer);
            command->setText(kundo2_i18n("Draw Line"));
            m_canvas->addCommand(command);
            // Do not wait for the vector layer's update-compression timer.
            m_layer->forceUpdateTimedNode();
        }
    }

    m_raw.clear();
    m_preview = QPainterPath();
    m_erased.clear();
    m_erasedOutlines.clear();
    m_eraserTrail.clear();
    endBrushPreview();
    requestUpdate(dirty);
    m_image = nullptr;
}

QVector<KomaLineartRecorder::PartialErase> KomaLineartRecorder::partialErase() const
{
    QVector<PartialErase> result;
    if (!m_layer || !m_image || m_eraserMode == EraserMode::WholeLine) {
        return result;
    }
    const qreal pixelsPerUnit = qMax(m_image->xRes(), 1e-6);
    const qreal spacing = qMax(m_eraserRadius / 3.0, 0.5 / pixelsPerUnit);
    const qreal tolerance = simplifyTolerancePixels / pixelsPerUnit;

    // Lines crossing the erased ones bound "erase up to intersection".
    QVector<QVector<QPointF>> allLines;
    QHash<KomaLineartShape*, int> lineIndex;
    QVector<QVector<KomaLineartPoint>> denseLines;
    if (m_eraserMode == EraserMode::UpToIntersection) {
        const QList<KoShape*> shapes = m_layer->shapes();
        for (KoShape *shape : shapes) {
            KomaLineartShape *line = dynamic_cast<KomaLineartShape*>(shape);
            if (!line || !line->isVisible()) {
                continue;
            }
            const QVector<KomaLineartPoint> dense = KomaLineartShape::densify(line->centerlineInDocument(), spacing);
            QVector<QPointF> polyline;
            polyline.reserve(dense.size());
            for (const KomaLineartPoint &point : dense) {
                polyline.append(point.position);
            }
            lineIndex.insert(line, allLines.size());
            allLines.append(polyline);
            denseLines.append(dense);
        }
    }

    // One index of every line's segments serves all the erased lines.
    const KomaLineartShape::LineIndex crossingLines(allLines);
    for (KomaLineartShape *line : std::as_const(m_erased)) {
        QVector<KomaLineartPoint> dense;
        QVector<QVector<KomaLineartPoint>> pieces;
        if (m_eraserMode == EraserMode::UpToIntersection) {
            const int own = lineIndex.value(line, -1);
            dense = own >= 0 ? denseLines[own] : KomaLineartShape::densify(line->centerlineInDocument(), spacing);
            pieces = KomaLineartShape::eraseUpToIntersection(dense, m_eraserTrail, m_eraserRadius, crossingLines, own);
        } else {
            dense = KomaLineartShape::densify(line->centerlineInDocument(), spacing);
            pieces = KomaLineartShape::eraseTouchedArea(dense, m_eraserTrail, m_eraserRadius);
        }

        if (pieces.size() == 1 && pieces.first().size() == dense.size()) {
            continue; // the eraser only grazed the outline
        }

        PartialErase erase;
        erase.line = line;
        erase.pieces.reserve(pieces.size());
        for (const QVector<KomaLineartPoint> &piece : pieces) {
            erase.pieces.append(KomaLineartShape::simplify(piece, tolerance));
        }
        result.append(erase);
    }
    return result;
}

void KomaLineartRecorder::applyPartialErase(KUndo2Command *parent)
{
    const QVector<PartialErase> erases = partialErase();
    for (const PartialErase &erase : erases) {
        KomaLineartShape *line = erase.line;
        m_canvas->shapeController()->removeShape(line, parent);
        for (const QVector<KomaLineartPoint> &piece : erase.pieces) {
            KomaLineartShape *remaining = new KomaLineartShape();
            remaining->setLineColor(line->lineColor());
            remaining->setZIndex(line->zIndex());
            remaining->setCenterlineInDocument(piece);
            remaining->setBrush(line->brush(), line->fullPressureWidth() * qSqrt(qAbs(line->absoluteTransformation().determinant())));
            m_canvas->shapeController()->addShape(remaining, m_layer, parent);
        }
    }
}

void KomaLineartRecorder::cancel()
{
    if (!m_active) {
        return;
    }
    const QRectF dirty = previewBounds();
    m_active = false;
    m_raw.clear();
    m_preview = QPainterPath();
    m_erased.clear();
    m_erasedOutlines.clear();
    endBrushPreview();
    requestUpdate(dirty);
    m_image = nullptr;
}

QRectF KomaLineartRecorder::previewBounds() const
{
    QRectF bounds = m_preview.boundingRect();
    if (!m_brushPreview.bounds().isEmpty()) {
        bounds |= pixelToDocument(m_brushPreview.bounds());
    }
    for (const QPainterPath &outline : m_erasedOutlines) {
        bounds |= outline.boundingRect();
    }
    return bounds;
}

void KomaLineartRecorder::requestUpdate(const QRectF &documentRect)
{
    if (m_updateCanvas && documentRect.isValid()) {
        const qreal margin = m_baseWidth;
        m_updateCanvas(documentRect.adjusted(-margin, -margin, margin, margin));
    }
}

void KomaLineartRecorder::paint(QPainter &painter, const KoViewConverter &converter) const
{
    if (!m_active) {
        return;
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    const QTransform toView = converter.documentToView();

    if (m_erase) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(128, 128, 128, 160));
        if (m_eraserMode == EraserMode::WholeLine) {
            // Lines that will be erased are drawn in a muted grey. Only those
            // reaching into the area being repainted are drawn: a stroke can
            // mark dozens of lines, and each frame repaints a small part.
            const QRectF clip = painter.hasClipping() ? painter.clipBoundingRect() : QRectF();
            for (const QPainterPath &outline : m_erasedOutlines) {
                // One view pixel of slack covers antialiased edge pixels.
                if (!clip.isNull()
                    && !toView.mapRect(outline.boundingRect()).adjusted(-1, -1, 1, 1).intersects(clip)) {
                    continue;
                }
                painter.drawPath(toView.map(outline));
            }
        } else {
            // The eraser's path, which decides what is removed.
            QPainterPathStroker stroker;
            stroker.setWidth(2 * m_eraserRadius);
            stroker.setCapStyle(Qt::RoundCap);
            stroker.setJoinStyle(Qt::RoundJoin);
            QPainterPath trail;
            if (!m_eraserTrail.isEmpty()) {
                trail.moveTo(m_eraserTrail.first());
                for (const QPointF &point : m_eraserTrail) {
                    trail.lineTo(point);
                }
                QPainterPath area = stroker.createStroke(trail);
                area.addEllipse(m_eraserTrail.first(), m_eraserRadius, m_eraserRadius);
                area.setFillRule(Qt::WindingFill);
                painter.drawPath(toView.map(area));
            }
        }
    } else if (m_brushPainter) {
        // The stroke as the brush paints it.
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setTransform(toView, true);
        m_brushPreview.paint(painter, m_xRes, m_yRes);
    } else {
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_color);
        painter.drawPath(toView.map(m_preview));
    }
    painter.restore();
}
