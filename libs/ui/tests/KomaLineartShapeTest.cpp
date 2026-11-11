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

void KomaLineartShapeTest::testAdjustWidth()
{
    const QVector<KomaLineartPoint> dense = KomaLineartShape::densify(straightLine(), 1.0);
    const int middle = dense.size() / 2;

    const auto wider = KomaLineartShape::adjustWidth(dense, QPointF(50, 0), 10.0, 2.0);
    QVERIFY(wider[middle].width > 3.9);
    QCOMPARE(wider.first().width, 2.0);

    const auto thinner = KomaLineartShape::adjustWidth(dense, QPointF(50, 0), 10.0, 0.001);
    QVERIFY(thinner[middle].width > 0.0);
    QVERIFY(thinner[middle].width < 0.1);
}

void KomaLineartShapeTest::testConnect()
{
    const QVector<KomaLineartPoint> first = {{QPointF(0, 0), 1}, {QPointF(10, 0), 1}};
    const QVector<KomaLineartPoint> second = {{QPointF(30, 0), 3}, {QPointF(20, 0), 3}};
    const auto joined = KomaLineartShape::connect(first, true, second, false);
    QCOMPARE(joined.size(), 4);
    QCOMPARE(joined[0].position, QPointF(0, 0));
    QCOMPARE(joined[2].position, QPointF(20, 0));
    QCOMPARE(joined[3].position, QPointF(30, 0));
}

void KomaLineartShapeTest::testEditTransformedLine()
{
    KomaLineartShape shape;
    shape.setCenterlineInDocument({{QPointF(10, 10), 2}, {QPointF(60, 10), 2}});
    QTransform rotateAndScale;
    rotateAndScale.rotate(30);
    rotateAndScale.scale(2, 2);
    shape.setTransformation(shape.transformation() * rotateAndScale);

    const auto before = shape.centerlineInDocument();
    const auto oldCenterline = shape.centerline();
    const QTransform oldTransformation = shape.transformation();

    auto edited = before;
    edited[1].position += QPointF(5, 7);
    edited[1].width = 9;
    shape.setCenterlineFromDocument(edited);

    const auto after = shape.centerlineInDocument();
    QVERIFY((after[1].position - edited[1].position).manhattanLength() < 1e-3);
    QVERIFY(qAbs(after[1].width - 9) < 1e-3);
    QVERIFY((after[0].position - before[0].position).manhattanLength() < 1e-3);

    KomaLineartEditCommand command(&shape, oldCenterline, oldTransformation,
                                   shape.centerline(), shape.transformation(), kundo2_noi18n("edit"));
    command.undo();
    QVERIFY((shape.centerlineInDocument()[1].position - before[1].position).manhattanLength() < 1e-3);
    command.redo();
    QVERIFY((shape.centerlineInDocument()[1].position - edited[1].position).manhattanLength() < 1e-3);
}

void KomaLineartShapeTest::testNewLineStaysWhereDrawn()
{
    // A line drawn away from the document origin must stay where it was drawn.
    const QVector<KomaLineartPoint> drawn = {{QPointF(300, 200), 4}, {QPointF(420, 260), 6}, {QPointF(500, 240), 2}};
    KomaLineartShape shape;
    shape.setCenterlineInDocument(drawn);
    const auto placed = shape.centerlineInDocument();
    QCOMPARE(placed.size(), drawn.size());
    for (int i = 0; i < drawn.size(); ++i) {
        QVERIFY((placed[i].position - drawn[i].position).manhattanLength() < 1e-3);
    }
    QVERIFY(shape.absoluteTransformation().map(shape.outline()).boundingRect().contains(QPointF(420, 260)));
}

SIMPLE_TEST_MAIN(KomaLineartShapeTest)
