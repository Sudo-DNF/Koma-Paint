/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KOMALINEARTSHAPETEST_H
#define KOMALINEARTSHAPETEST_H

#include <simpletest.h>

class KomaLineartShapeTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testDensify();
    void testOutline();
    void testEraseTouchedArea();
    void testEraseUpToIntersection();
    void testEraseUpToIntersectionWithoutCrossings();
    void testAdjustWidth();
    void testConnect();
    void testEditTransformedLine();
    void testNewLineStaysWhereDrawn();
};

#endif // KOMALINEARTSHAPETEST_H
