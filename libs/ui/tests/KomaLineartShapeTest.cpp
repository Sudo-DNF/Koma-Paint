/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KomaLineartShapeTest.h"

#include <QPainterPath>

#include "flake/KomaLineartShape.h"

namespace
{
/// A horizontal line from x=0 to x=100, two units wide.
QVector<KomaLineartPoint> straightLine()
{
    return {{QPointF(0, 0), 2}, {QPointF(100, 0), 2}};
}
}

void KomaLineartShapeTest::testDensify()
{
    const QVector<KomaLineartPoint> dense = KomaLineartShape::densify(straightLine(), 1.0);
    QVERIFY(dense.size() >= 100);
    QCOMPARE(dense.first().position, QPointF(0, 0));
    QVERIFY(qAbs(dense.last().position.x() - 100.0) < 1e-6);
}

void KomaLineartShapeTest::testOutline()
{
    const QPainterPath outline = KomaLineartShape::outlineForCenterline(straightLine());
    QVERIFY(outline.contains(QPointF(50, 0.5)));
    QVERIFY(!outline.contains(QPointF(50, 3)));
}

void KomaLineartShapeTest::testEraseTouchedArea()
{
    const QVector<KomaLineartPoint> dense = KomaLineartShape::densify(straightLine(), 1.0);

    auto pieces = KomaLineartShape::eraseTouchedArea(dense, {QPointF(50, 0)}, 5.0);
    QCOMPARE(pieces.size(), 2);
    QVERIFY(pieces[0].last().position.x() < 45);
    QVERIFY(pieces[1].first().position.x() > 55);

    pieces = KomaLineartShape::eraseTouchedArea(dense, {QPointF(50, 40)}, 5.0);
    QCOMPARE(pieces.size(), 1);
    QCOMPARE(pieces[0].size(), dense.size());
}

void KomaLineartShapeTest::testEraseUpToIntersection()
{
    const QVector<KomaLineartPoint> dense = KomaLineartShape::densify(straightLine(), 1.0);
    const QVector<QVector<QPointF>> crossing = {
        {QPointF(30, -20), QPointF(30, 20)},
        {QPointF(70, -20), QPointF(70, 20)},
    };

    auto pieces = KomaLineartShape::eraseUpToIntersection(dense, {QPointF(50, 0)}, 2.0, crossing);
    QCOMPARE(pieces.size(), 2);
    QVERIFY(qAbs(pieces[0].last().position.x() - 30) < 0.01);
    QVERIFY(qAbs(pieces[1].first().position.x() - 70) < 0.01);

    pieces = KomaLineartShape::eraseUpToIntersection(dense, {QPointF(10, 0)}, 2.0, crossing);
    QCOMPARE(pieces.size(), 1);
    QVERIFY(qAbs(pieces[0].first().position.x() - 30) < 0.01);
}

void KomaLineartShapeTest::testEraseUpToIntersectionWithoutCrossings()
{
    const QVector<KomaLineartPoint> dense = KomaLineartShape::densify(straightLine(), 1.0);
    const auto pieces = KomaLineartShape::eraseUpToIntersection(dense, {QPointF(50, 0)}, 2.0, {});
    QVERIFY(pieces.isEmpty());
}

SIMPLE_TEST_MAIN(KomaLineartShapeTest)
