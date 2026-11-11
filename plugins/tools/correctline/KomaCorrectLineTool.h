/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KOMACORRECTLINETOOL_H
#define KOMACORRECTLINETOOL_H

#include <QHash>
#include <QPointer>
#include <QTransform>
#include <QVector>

#include <KoToolFactoryBase.h>
#include <kis_tool.h>

#include <flake/KomaLineartShape.h>

class KisShapeLayer;
class QComboBox;
class KisSliderSpinBox;

/**
 * Correct Line: edits lineart strokes on vector layers after they are
 * drawn, as Clip Studio Paint's Correct Line tools do.
 *
 * - Control point: drag a point of a line to reshape it; Shift+click a
 *   point to delete it.
 * - Adjust line width: brush over lines to thicken or narrow them.
 * - Simplify line: brush over lines to smooth them and remove points.
 * - Connect lines: drag from one line end to another to join the lines.
 *
 * Every drag is one undo step.
 */
class KomaCorrectLineTool : public KisTool
{
    Q_OBJECT
public:
    enum class Mode {
        ControlPoint,
        AdjustWidth,
        Simplify,
        Connect,
    };

    KomaCorrectLineTool(KoCanvasBase *canvas, Mode mode);
    ~KomaCorrectLineTool() override;

    void beginPrimaryAction(KoPointerEvent *event) override;
    void continuePrimaryAction(KoPointerEvent *event) override;
    void endPrimaryAction(KoPointerEvent *event) override;
    void mouseMoveEvent(KoPointerEvent *event) override;

    void paint(QPainter &gc, const KoViewConverter &converter) override;
    QWidget *createOptionWidget() override;

private:
    struct Edit {
        QVector<KomaLineartPoint> oldCenterline;
        QTransform oldTransformation;
        QVector<KomaLineartPoint> working; ///< Document coordinates.
    };

    KisShapeLayer *shapeLayer() const;
    QList<KomaLineartShape*> lines() const;
    QList<KomaLineartShape*> linesTouching(const QPointF &documentPoint, qreal radius) const;
    qreal radiusInDocument() const;
    qreal pixelsToDocument(qreal pixels) const;
    void touchLines(const QPointF &documentPoint);
    void commitEdits(const KUndo2MagicString &text);
    void finishConnect(const QPointF &documentPoint);
    void requestRepaint();
    QString configKey(const QString &name) const;

    Mode m_mode;
    int m_size {20};       ///< Brush radius, image pixels.
    int m_amount {30};     ///< Strength, 1-100.
    bool m_thicken {true}; ///< Adjust width direction.

    bool m_dragging {false};
    QPointF m_cursor;      ///< Document coordinates.
    QPointF m_dragStart;

    QHash<KomaLineartShape*, Edit> m_edits;

    // Control point dragging.
    KomaLineartShape *m_pointShape {nullptr};
    int m_pointIndex {-1};
};

class KomaCorrectLineToolFactory : public KoToolFactoryBase
{
public:
    KomaCorrectLineToolFactory(KomaCorrectLineTool::Mode mode);
    KoToolBase *createTool(KoCanvasBase *canvas) override;

    static QString idForMode(KomaCorrectLineTool::Mode mode);

private:
    KomaCorrectLineTool::Mode m_mode;
};

#endif // KOMACORRECTLINETOOL_H
