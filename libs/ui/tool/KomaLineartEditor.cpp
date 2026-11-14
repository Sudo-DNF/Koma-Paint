/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KomaLineartEditor.h"

#include <QPainterPath>
#include <QtMath>

#include <algorithm>

#include <KoShapeManager.h>
#include <kundo2command.h>

#include "flake/kis_shape_layer.h"

namespace
{
qreal distance(const QPointF &a, const QPointF &b)
{
    const QPointF d = a - b;
    return qSqrt(d.x() * d.x() + d.y() * d.y());
}
}

QList<KomaLineartShape*> KomaLineartEditor::lines(KisShapeLayer *layer)
{
    QList<KomaLineartShape*> result;
    if (!layer) {
        return result;
    }
    const QList<KoShape*> shapes = layer->shapes();
    for (KoShape *shape : shapes) {
        KomaLineartShape *line = dynamic_cast<KomaLineartShape*>(shape);
        if (line && line->isVisible() && line->isSelectable()) {
            result.append(line);
        }
    }
    return result;
}

QList<KomaLineartShape*> KomaLineartEditor::linesTouching(KisShapeLayer *layer, const QPointF &point, qreal radius)
{
    QList<KomaLineartShape*> result;
    if (!layer) {
        return result;
    }
    QPainterPath brush;
    brush.addEllipse(point, radius, radius);
    for (KoShape *shape : layer->shapeManager()->shapesAt(brush.boundingRect())) {
        KomaLineartShape *line = dynamic_cast<KomaLineartShape*>(shape);
        if (line && line->parent() == layer && line->isVisible() && line->isSelectable()
                && line->absoluteTransformation().map(line->outline()).intersects(brush)) {
            result.append(line);
        }
    }
    return result;
}

void KomaLineartEditor::clear()
{
    m_edits.clear();
    m_pointShape = nullptr;
    m_pointIndex = -1;
}

bool KomaLineartEditor::isEmpty() const
{
    return m_edits.isEmpty();
}

KomaLineartEditor::Edit &KomaLineartEditor::startEdit(KomaLineartShape *line, qreal sampleSpacing)
{
    Edit edit;
    edit.oldCenterline = line->centerline();
    edit.oldTransformation = line->transformation();
    edit.working = sampleSpacing > 0
            ? KomaLineartShape::densify(line->centerlineInDocument(), sampleSpacing)
            : line->centerlineInDocument();
    return *m_edits.insert(line, edit);
}

bool KomaLineartEditor::pickPoint(KisShapeLayer *layer, const QPointF &point, qreal pickRadius)
{
    clear();
    qreal best = pickRadius;
    for (KomaLineartShape *line : lines(layer)) {
        const QVector<KomaLineartPoint> points = line->centerlineInDocument();
        for (int i = 0; i < points.size(); ++i) {
            const qreal d = distance(points[i].position, point);
            if (d <= best) {
                best = d;
                m_pointShape = line;
                m_pointIndex = i;
            }
        }
    }
    if (!m_pointShape) {
        m_pointIndex = -1;
        return false;
    }
    startEdit(m_pointShape, 0);
    return true;
}

KomaLineartShape *KomaLineartEditor::pickedLine() const
{
    return m_pointShape;
}

int KomaLineartEditor::pickedIndex() const
{
    return m_pointIndex;
}

void KomaLineartEditor::movePickedPoint(const QPointF &point)
{
    if (!m_pointShape || !m_edits.contains(m_pointShape)) {
        return;
    }
    Edit &edit = m_edits[m_pointShape];
    edit.working[m_pointIndex].position = point;
    m_pointShape->setCenterlineFromDocument(edit.working);
}

bool KomaLineartEditor::deletePickedPoint()
{
    if (!m_pointShape || !m_edits.contains(m_pointShape)) {
        return false;
    }
    Edit &edit = m_edits[m_pointShape];
    if (edit.working.size() <= 2) {
        return false;
    }
    edit.working.remove(m_pointIndex);
    m_pointShape->setCenterlineFromDocument(edit.working);
    return true;
}

void KomaLineartEditor::touchLines(KisShapeLayer *layer, const QPointF &point, qreal radius, qreal sampleSpacing)
{
    for (KomaLineartShape *line : linesTouching(layer, point, radius)) {
        if (!m_edits.contains(line)) {
            startEdit(line, sampleSpacing);
        }
    }
}

void KomaLineartEditor::adjustWidth(KisShapeLayer *layer, const QPointF &point, qreal radius, qreal factor)
{
    touchLines(layer, point, radius, radius / 4.0);
    const qreal radiusSquared = radius * radius;
    for (auto it = m_edits.begin(); it != m_edits.end(); ++it) {
        const bool affectsLine = std::any_of(it->working.cbegin(), it->working.cend(),
                                              [&](const KomaLineartPoint &sample) {
            const QPointF delta = sample.position - point;
            return QPointF::dotProduct(delta, delta) < radiusSquared;
        });
        if (!affectsLine) {
            continue;
        }
        it->working = KomaLineartShape::adjustWidth(it->working, point, radius, factor);
        it.key()->setCenterlineFromDocument(it->working);
    }
}

void KomaLineartEditor::finishAdjustWidth(qreal tolerance)
{
    for (auto it = m_edits.begin(); it != m_edits.end(); ++it) {
        it->working = KomaLineartShape::simplify(it->working, tolerance);
        it.key()->setCenterlineFromDocument(it->working);
    }
}

void KomaLineartEditor::simplifyLines(qreal sampleSpacing, qreal tolerance)
{
    for (auto it = m_edits.begin(); it != m_edits.end(); ++it) {
        it->working = KomaLineartShape::processRawStroke(KomaLineartShape::densify(it->working, sampleSpacing), tolerance);
        it.key()->setCenterlineFromDocument(it->working);
    }
}

KUndo2Command *KomaLineartEditor::takeCommand(const KUndo2MagicString &text)
{
    if (m_edits.isEmpty()) {
        return nullptr;
    }
    KUndo2Command *command = new KUndo2Command(text);
    for (auto it = m_edits.begin(); it != m_edits.end(); ++it) {
        KomaLineartShape *line = it.key();
        if (line->centerline() == it->oldCenterline && line->transformation() == it->oldTransformation) {
            continue;
        }
        new KomaLineartEditCommand(line, it->oldCenterline, it->oldTransformation,
                                   line->centerline(), line->transformation(), text, command);
    }
    if (command->childCount() == 0) {
        delete command;
        return nullptr;
    }
    return command;
}
