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

#include <kundo2command.h>
#include <klocalizedstring.h>

#include <KoCanvasResourceProvider.h>
#include <KoColor.h>
#include <KoShapeController.h>
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

    m_canvas = canvas;
    m_layer = layer;
    m_erase = erase;
    m_raw.clear();
    m_preview = QPainterPath();
    m_erased.clear();
    m_eraserTrail.clear();
    m_eraserMode = eraserMode();

    // Brush size is in image pixels; shapes live in document units.
    const qreal pixelsPerUnit = qMax(canvas->image()->xRes(), 1e-6);
    qreal sizeInPixels = 4.0;
    const KisPaintOpPresetSP preset = canvas->resourceManager()
            ->resource(KoCanvasResource::CurrentPaintOpPreset).value<KisPaintOpPresetSP>();
    if (preset && preset->settings()) {
        sizeInPixels = qMax<qreal>(0.5, preset->settings()->paintOpSize());
    }
    m_baseWidth = sizeInPixels / pixelsPerUnit;
    m_eraserRadius = m_baseWidth / 2.0;
    m_color = canvas->resourceManager()->foregroundColor().toQColor();
    m_color.setAlpha(255);
    // New lines are rendered with the brush they are drawn with.
    m_brush = erase ? KisPaintOpPresetSP() : KomaLineartShape::brushSnapshot(preset, m_color);

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
    const QRectF before = m_preview.boundingRect();
    m_raw.append({documentPoint, widthForPressure(pressure)});
    m_preview = KomaLineartShape::outlineForCenterline(m_raw);
    requestUpdate(before | m_preview.boundingRect());
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

    const QList<KoShape*> shapes = m_layer->shapes();
    for (KoShape *shape : shapes) {
        KomaLineartShape *line = dynamic_cast<KomaLineartShape*>(shape);
        if (!line || m_erased.contains(line) || !line->isVisible() || !line->isSelectable()) {
            continue;
        }
        const QPainterPath outline = line->absoluteTransformation().map(line->outline());
        if (outline.intersects(eraser)) {
            m_erased.insert(line);
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
            const qreal pixelsPerUnit = qMax(m_canvas->image()->xRes(), 1e-6);
            const QVector<KomaLineartPoint> centerline =
                    KomaLineartShape::processRawStroke(m_raw, simplifyTolerancePixels / pixelsPerUnit);

            KomaLineartShape *line = new KomaLineartShape();
            line->setLineColor(m_color);
            line->setCenterlineInDocument(centerline);
            line->setBrush(m_brush, m_baseWidth);

            KUndo2Command *command = m_canvas->shapeController()->addShape(line, m_layer);
            command->setText(kundo2_i18n("Draw Line"));
            m_canvas->addCommand(command);
        }
    }

    m_raw.clear();
    m_preview = QPainterPath();
    m_erased.clear();
    m_eraserTrail.clear();
    requestUpdate(dirty);
}

void KomaLineartRecorder::applyPartialErase(KUndo2Command *parent)
{
    const qreal pixelsPerUnit = qMax(m_canvas->image()->xRes(), 1e-6);
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

    for (KomaLineartShape *line : std::as_const(m_erased)) {
        QVector<KomaLineartPoint> dense;
        QVector<QVector<KomaLineartPoint>> pieces;
        if (m_eraserMode == EraserMode::UpToIntersection) {
            const int own = lineIndex.value(line, -1);
            dense = own >= 0 ? denseLines[own] : KomaLineartShape::densify(line->centerlineInDocument(), spacing);
            QVector<QVector<QPointF>> others;
            for (int i = 0; i < allLines.size(); ++i) {
                if (i != own) {
                    others.append(allLines[i]);
                }
            }
            pieces = KomaLineartShape::eraseUpToIntersection(dense, m_eraserTrail, m_eraserRadius, others);
        } else {
            dense = KomaLineartShape::densify(line->centerlineInDocument(), spacing);
            pieces = KomaLineartShape::eraseTouchedArea(dense, m_eraserTrail, m_eraserRadius);
        }

        if (pieces.size() == 1 && pieces.first().size() == dense.size()) {
            continue; // the eraser only grazed the outline
        }

        m_canvas->shapeController()->removeShape(line, parent);
        for (const QVector<KomaLineartPoint> &piece : pieces) {
            KomaLineartShape *remaining = new KomaLineartShape();
            remaining->setLineColor(line->lineColor());
            remaining->setZIndex(line->zIndex());
            remaining->setCenterlineInDocument(KomaLineartShape::simplify(piece, tolerance));
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
    requestUpdate(dirty);
}

QRectF KomaLineartRecorder::previewBounds() const
{
    QRectF bounds = m_preview.boundingRect();
    for (KomaLineartShape *line : m_erased) {
        bounds |= line->absoluteTransformation().map(line->outline()).boundingRect();
    }
    return bounds;
}

void KomaLineartRecorder::requestUpdate(const QRectF &documentRect)
{
    if (m_canvas && documentRect.isValid()) {
        const qreal margin = m_baseWidth;
        m_canvas->updateCanvas(documentRect.adjusted(-margin, -margin, margin, margin));
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
            // Lines that will be erased are drawn in a muted grey.
            for (KomaLineartShape *line : m_erased) {
                painter.drawPath(toView.map(line->absoluteTransformation().map(line->outline())));
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
                painter.drawPath(toView.map(area.simplified()));
            }
        }
    } else {
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_color);
        painter.drawPath(toView.map(m_preview));
    }
    painter.restore();
}
