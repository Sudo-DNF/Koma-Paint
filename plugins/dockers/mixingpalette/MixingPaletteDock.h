/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MIXINGPALETTEDOCK_H
#define MIXINGPALETTEDOCK_H

#include <QDockWidget>
#include <QPointer>

#include <KoCanvasObserverBase.h>
#include <kis_canvas2.h>

class QToolButton;
class KisSliderSpinBox;
class MixingPaletteCanvas;

/**
 * The Color Mixing Palette docker: a paint surface for dabbing and blending
 * colours by hand. Brush mode paints with the current foreground colour;
 * picking a colour from the surface makes it the foreground colour.
 */
class MixingPaletteDock : public QDockWidget, public KoCanvasObserverBase
{
    Q_OBJECT
public:
    MixingPaletteDock();

    QString observerName() override { return QStringLiteral("MixingPaletteDock"); }
    void setCanvas(KoCanvasBase *canvas) override;
    void unsetCanvas() override;

private Q_SLOTS:
    void slotCanvasResourceChanged(int key, const QVariant &value);
    void slotColorPicked(const QColor &color);
    void slotUpdateIcons();

private:
    QPointer<KisCanvas2> m_canvas;
    MixingPaletteCanvas *m_surface {nullptr};
    QToolButton *m_brushButton {nullptr};
    QToolButton *m_blendButton {nullptr};
    QToolButton *m_pickButton {nullptr};
    QToolButton *m_clearButton {nullptr};
    KisSliderSpinBox *m_sizeSlider {nullptr};
};

#endif // MIXINGPALETTEDOCK_H
