/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KomaLineartRecorder.h"

#include <QPainter>

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
/// Share of the full width kept at the lightest pressure.
constexpr qreal minimumPressureWidth = 0.15;

/// Simplification tolerance, in image pixels.
constexpr qreal simplifyTolerancePixels = 0.35;
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
    return m_baseWidth * (minimumPressureWidth + (1.0 - minimumPressureWidth) * p);
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
    QPainterPath eraser;
    eraser.addEllipse(documentPoint, m_eraserRadius, m_eraserRadius);

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
                QList<KoShape*> shapes;
                for (KomaLineartShape *line : std::as_const(m_erased)) {
                    shapes.append(line);
                }
                KUndo2Command *command = m_canvas->shapeController()->removeShapes(shapes);
                command->setText(kundo2_i18n("Erase Lines"));
                m_canvas->addCommand(command);
            }
        } else if (!m_raw.isEmpty()) {
            const qreal pixelsPerUnit = qMax(m_canvas->image()->xRes(), 1e-6);
            const QVector<KomaLineartPoint> centerline =
                    KomaLineartShape::processRawStroke(m_raw, simplifyTolerancePixels / pixelsPerUnit);

            KomaLineartShape *line = new KomaLineartShape();
            line->setLineColor(m_color);
            line->setCenterlineInDocument(centerline);

            KUndo2Command *command = m_canvas->shapeController()->addShape(line, m_layer);
            command->setText(kundo2_i18n("Draw Line"));
            m_canvas->addCommand(command);
        }
    }

    m_raw.clear();
    m_preview = QPainterPath();
    m_erased.clear();
    requestUpdate(dirty);
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
        // Lines that will be erased are drawn in a muted grey.
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(128, 128, 128, 160));
        for (KomaLineartShape *line : m_erased) {
            painter.drawPath(toView.map(line->absoluteTransformation().map(line->outline())));
        }
    } else {
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_color);
        painter.drawPath(toView.map(m_preview));
    }
    painter.restore();
}
