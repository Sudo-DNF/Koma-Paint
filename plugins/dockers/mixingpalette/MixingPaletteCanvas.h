/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MIXINGPALETTECANVAS_H
#define MIXINGPALETTECANVAS_H

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QWidget>

/**
 * A small paint surface for mixing colour by hand.
 *
 * - Brush mode lays down the current colour with a soft round dab. The
 *   brush picks up some of the paint it passes over, so colours mix as
 *   they would with a wet brush.
 * - Blend mode smudges: it carries the paint under the stroke and drags it
 *   along, blending neighbouring colours.
 * - Pick mode (or Ctrl+click in any mode) reads the colour under the cursor
 *   and emits colorPicked().
 *
 * The surface is kept as an image in the user's data folder, so the mixed
 * colours survive between sessions. It is saved when a stroke ends.
 */
class MixingPaletteCanvas : public QWidget
{
    Q_OBJECT
public:
    enum class Mode {
        Brush,
        Blend,
        Pick,
    };

    explicit MixingPaletteCanvas(QWidget *parent = nullptr);
    ~MixingPaletteCanvas() override;

    void setMode(Mode mode);
    Mode mode() const;

    /// Brush diameter in logical pixels.
    void setBrushSize(int size);
    int brushSize() const;

    /// The colour Brush mode paints with.
    void setPaintColor(const QColor &color);

    /// Wipes the surface back to clean paper.
    void clear();

    QSize sizeHint() const override;

Q_SIGNALS:
    void colorPicked(const QColor &color);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void tabletEvent(QTabletEvent *event) override;

private:
    void beginStroke(const QPointF &pos, qreal pressure, bool pick);
    void continueStroke(const QPointF &pos, qreal pressure);
    void endStroke();
    void dab(const QPointF &pos, qreal pressure);
    QColor sampleAverage(const QPointF &pos, qreal radius) const;
    void ensureSurface();
    QString storagePath() const;
    void load();
    void save() const;

    QImage m_surface;      ///< Paint, in device pixels.
    Mode m_mode {Mode::Brush};
    int m_brushSize {24};
    QColor m_paintColor {Qt::black};

    bool m_stroking {false};
    bool m_pickStroke {false};
    QPointF m_lastPos;      ///< Last dab position, in device pixels.
    qreal m_lastPressure {1.0};
    qreal m_distanceToNextDab {0.0};
    QColor m_loadedColor;   ///< Paint currently on the brush.
};

#endif // MIXINGPALETTECANVAS_H
