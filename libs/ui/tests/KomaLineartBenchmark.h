/*
 * SPDX-FileCopyrightText: 2026 Koma Paint contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KOMALINEARTBENCHMARK_H
#define KOMALINEARTBENCHMARK_H

#include <simpletest.h>

class KomaLineartBenchmark : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void eraseIntersections();
    void eraseLongTrail();
    void previewUpdates();
    void previewPaintClipped();
    void eraserOverlay();
    void brushReplay_data();
    void brushReplay();
    void finishedVectorRender_data();
    void finishedVectorRender();
    void vectorProjection_data();
    void vectorProjection();
    void rasterFinish_data();
    void rasterFinish();
    void populatedVectorRender();

    // Interactive work on a vector layer that already holds 220 lines.
    void denseInkStroke();
    void denseEraser_data();
    void denseEraser();
    void denseControlPointDrag();
    void denseAdjustWidth();
    void denseLayerRepaint();
};

#endif
