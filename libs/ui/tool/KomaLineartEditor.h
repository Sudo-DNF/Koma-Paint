/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KOMALINEARTEDITOR_H
#define KOMALINEARTEDITOR_H

#include <QHash>
#include <QList>
#include <QPointF>
#include <QTransform>
#include <QVector>

#include <kundo2magicstring.h>

#include "flake/KomaLineartShape.h"
#include "kritaui_export.h"

class KisShapeLayer;
class KUndo2Command;

/**
 * Edits the centerlines of lineart strokes already on a vector layer: the
 * geometry behind the Correct Line tools. One drag is one edit session;
 * takeCommand() turns the changes into one undo step.
 *
 * Positions and radii are in document coordinates.
 */
class KRITAUI_EXPORT KomaLineartEditor
{
public:
    /// The visible, selectable lineart strokes on @p layer.
    static QList<KomaLineartShape*> lines(KisShapeLayer *layer);

    /// The lines on @p layer whose outline a circle at @p point touches.
    static QList<KomaLineartShape*> linesTouching(KisShapeLayer *layer, const QPointF &point, qreal radius);

    /// Forgets the session without changing anything further.
    void clear();

    bool isEmpty() const;

    /**
     * Starts dragging the centerline point nearest @p point within
     * @p pickRadius on @p layer.
     * @return false if no point is that close.
     */
    bool pickPoint(KisShapeLayer *layer, const QPointF &point, qreal pickRadius);

    /// The line and point index picked by pickPoint(), or null and -1.
    KomaLineartShape *pickedLine() const;
    int pickedIndex() const;

    /// Moves the picked point to @p point.
    void movePickedPoint(const QPointF &point);

    /**
     * Deletes the picked point, if its line keeps at least two points.
     * @return true if the line changed.
     */
    bool deletePickedPoint();

    /**
     * Adds the lines a circle at @p point touches to the session.
     * @param sampleSpacing when positive, the lines are worked on as
     *        samples this far apart (width changes need samples along the
     *        line, not just its points).
     */
    void touchLines(KisShapeLayer *layer, const QPointF &point, qreal radius, qreal sampleSpacing);

    /**
     * One Adjust Line Width step: touches the lines under the brush and
     * scales their width around @p point by @p factor.
     */
    void adjustWidth(KisShapeLayer *layer, const QPointF &point, qreal radius, qreal factor);

    /// Drops the extra samples adjustWidth() worked with.
    void finishAdjustWidth(qreal tolerance);

    /// Smooths and simplifies every line in the session.
    void simplifyLines(qreal sampleSpacing, qreal tolerance);

    /**
     * An undo command for the changes made in this session, or null if no
     * line changed. The changes are already applied; the command's first
     * redo() reapplies the same state.
     */
    KUndo2Command *takeCommand(const KUndo2MagicString &text);

private:
    struct Edit {
        QVector<KomaLineartPoint> oldCenterline;
        QTransform oldTransformation;
        QVector<KomaLineartPoint> working; ///< Document coordinates.
    };

    Edit &startEdit(KomaLineartShape *line, qreal sampleSpacing);

    QHash<KomaLineartShape*, Edit> m_edits;
    KomaLineartShape *m_pointShape {nullptr};
    int m_pointIndex {-1};
};

#endif // KOMALINEARTEDITOR_H
