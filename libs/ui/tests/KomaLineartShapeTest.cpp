/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KomaLineartShapeTest.h"

#include <QElapsedTimer>
#include <QPainterPath>
#include <QPainter>
#include <QRandomGenerator>
#include <QPainterPathStroker>

#include <KisGlobalResourcesInterface.h>
#include <KisLocalStrokeResources.h>
#include <KisResourceTypes.h>
#include <resources/KoPattern.h>
#include <brushengine/kis_paintop_settings.h>
#include <cmath>
#include <KoShapeManager.h>
#include <KoAddRemoveShapeCommands.h>
#include <brushengine/kis_paintop_preset.h>
#include <testutil.h>

#include "flake/KomaLineartShape.h"
#include "flake/kis_shape_layer.h"
#include "tool/KomaLineartPreview.h"

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

void KomaLineartShapeTest::testBrushPreviewTiles()
{
    KomaLineartPreview preview;
    const QRect rect(-5, -3, 520, 8);
    QImage source(rect.size(), QImage::Format_ARGB32_Premultiplied);
    source.fill(QColor(210, 40, 70, 180));
    preview.update(rect, source);
    QCOMPARE(preview.bounds(), rect);

    const QRect changed(254, -1, 5, 3);
    QImage replacement(changed.size(), QImage::Format_ARGB32_Premultiplied);
    replacement.fill(QColor(10, 180, 90, 130));
    preview.update(changed, replacement);
    {
        QPainter painter(&source);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(changed.topLeft() - rect.topLeft(), replacement);
    }

    QImage actual(rect.size(), QImage::Format_ARGB32_Premultiplied);
    actual.fill(Qt::transparent);
    {
        QPainter painter(&actual);
        painter.translate(-rect.topLeft());
        preview.paint(painter, 1.0, 1.0);
    }
    QCOMPARE(actual, source);

    QImage clipped(actual.size(), actual.format());
    clipped.fill(Qt::transparent);
    {
        QPainter painter(&clipped);
        painter.translate(-rect.topLeft());
        painter.setClipRect(QRect(252, -3, 10, 8));
        preview.paint(painter, 1.0, 1.0);
    }
    for (int y = 0; y < clipped.height(); ++y) {
        for (int x = 0; x < clipped.width(); ++x) {
            const QPoint documentPoint = QPoint(x, y) + rect.topLeft();
            QCOMPARE(clipped.pixel(x, y),
                     QRect(252, -3, 10, 8).contains(documentPoint) ? source.pixel(x, y) : QRgb(0));
        }
    }

    QImage scaledActual(rect.width() * 2, rect.height() * 2, QImage::Format_ARGB32_Premultiplied);
    QImage scaledExpected(scaledActual.size(), scaledActual.format());
    scaledActual.fill(Qt::transparent);
    scaledExpected.fill(Qt::transparent);
    {
        QPainter painter(&scaledActual);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.scale(2, 2);
        painter.translate(-rect.topLeft());
        preview.paint(painter, 1.0, 1.0);
    }
    {
        QPainter painter(&scaledExpected);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.scale(2, 2);
        painter.drawImage(QPoint(0, 0), source);
    }
    // The tiles have transparent pixels outside the source rectangle. Compare
    // the interior, including both sides of each tile boundary.
    for (int y = 2; y < scaledActual.height() - 2; ++y) {
        for (int x = 2; x < scaledActual.width() - 2; ++x) {
            QCOMPARE(scaledActual.pixel(x, y), scaledExpected.pixel(x, y));
        }
    }

    QImage fractionalActual(700, 20, QImage::Format_ARGB32_Premultiplied);
    QImage fractionalExpected(fractionalActual.size(), fractionalActual.format());
    fractionalActual.fill(Qt::transparent);
    fractionalExpected.fill(Qt::transparent);
    {
        QPainter painter(&fractionalActual);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.scale(1.3, 1.3);
        painter.translate(-rect.topLeft());
        preview.paint(painter, 1.0, 1.0);
    }
    {
        QPainter painter(&fractionalExpected);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.scale(1.3, 1.3);
        painter.drawImage(QPoint(0, 0), source);
    }
    for (int y = 2; y < 9; ++y) {
        for (int x = 2; x < 673; ++x) {
            const QRgb actualPixel = fractionalActual.pixel(x, y);
            const QRgb expectedPixel = fractionalExpected.pixel(x, y);
            QVERIFY(qAbs(qAlpha(actualPixel) - qAlpha(expectedPixel)) <= 2);
            QVERIFY(qAbs(qRed(actualPixel) - qRed(expectedPixel)) <= 2);
            QVERIFY(qAbs(qGreen(actualPixel) - qGreen(expectedPixel)) <= 2);
            QVERIFY(qAbs(qBlue(actualPixel) - qBlue(expectedPixel)) <= 2);
        }
    }

    preview.clear();
    QVERIFY(preview.bounds().isEmpty());
    actual.fill(Qt::transparent);
    {
        QPainter painter(&actual);
        preview.paint(painter, 1.0, 1.0);
    }
    QCOMPARE(actual.pixelColor(5, 3), QColor(Qt::transparent));
}

void KomaLineartShapeTest::testSparseImageRendering()
{
    QImage source(1021, 765, QImage::Format_ARGB32);
    source.fill(Qt::transparent);
    {
        QPainter painter(&source);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(192, 44, 153, 117), 7));
        painter.drawLine(QPoint(0, 0), QPoint(1020, 764));
        painter.drawLine(QPoint(0, 255), QPoint(1020, 255));
        painter.drawPoint(QPoint(1020, 764));
    }
    source.setPixel(10, 650, qRgba(17, 34, 51, 0));
    KomaLineartPreview sparse;
    sparse.setSparseImage(source);
    QVERIFY(sparse.sizeInBytes() < source.sizeInBytes());
    QCOMPARE(sparse.toImage(source.size()), source);

    for (int mode = 0; mode < 7; ++mode) {
        QImage expected(1500, 1200, QImage::Format_ARGB32_Premultiplied);
        QImage actual(expected.size(), expected.format());
        expected.fill(QColor(33, 75, 110, 150));
        actual.fill(QColor(33, 75, 110, 150));
        auto configure = [mode](QPainter &painter) {
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.translate(100.25, 100.75);
            if (mode == 1) painter.scale(1.19, 1.19);
            if (mode == 2) {
                painter.translate(300, 0);
                painter.rotate(15);
                painter.scale(0.8, 0.8);
            }
            if (mode == 3) {
                painter.translate(10.3, 5.7);
                painter.scale(1.11, 0.94);
            }
            if (mode == 4) {
                painter.translate(1100, 0);
                painter.scale(-1, 1);
            }
            if (mode == 5) painter.setCompositionMode(QPainter::CompositionMode_Source);
            if (mode == 6) painter.setClipRect(QRectF(250, 180, 400, 330));
        };
        {
            QPainter painter(&expected);
            configure(painter);
            painter.drawImage(QPointF(0, 0), source);
        }
        {
            QPainter painter(&actual);
            configure(painter);
            if (mode == 2 || mode == 4 || mode == 5) {
                painter.drawImage(QPointF(0, 0), sparse.toImage(source.size()));
            } else {
                painter.setClipRect(QRectF(QPointF(0, 0), source.size()), Qt::IntersectClip);
                sparse.paint(painter, 1, 1);
            }
        }
        for (int y = 0; y < actual.height(); ++y) {
            for (int x = 0; x < actual.width(); ++x) {
                const QRgb a = actual.pixel(x, y);
                const QRgb b = expected.pixel(x, y);
                QVERIFY2(qAbs(qAlpha(a) - qAlpha(b)) <= 2 &&
                         qAbs(qRed(a) - qRed(b)) <= 2 &&
                         qAbs(qGreen(a) - qGreen(b)) <= 2 &&
                         qAbs(qBlue(a) - qBlue(b)) <= 2,
                         qPrintable(QString("Rendering differs at %1,%2 in transform %3").arg(x).arg(y).arg(mode)));
            }
        }
    }
}

void KomaLineartShapeTest::testBrushRenderCloneInvalidation()
{
    KisPaintOpPresetSP preset(new KisPaintOpPreset(TestUtil::fetchDataFileLazy("autobrush_300px.kpp")));
    QVERIFY(preset->load(KisGlobalResourcesInterface::instance()));
    KomaLineartShape shape;
    shape.setLineColor(Qt::red);
    shape.setCenterlineInDocument({{QPointF(10, 10), 8}, {QPointF(50, 10), 8}});
    shape.setBrush(KomaLineartShape::brushSnapshot(preset, Qt::red), 8);

    auto render = [](const KomaLineartShape &line) {
        QImage image(100, 50, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.translate(10, 10);
        line.paint(painter);
        return image;
    };

    const QImage original = render(shape);
    QImage blank(original.size(), original.format());
    blank.fill(Qt::transparent);
    QVERIFY(original != blank);
    QScopedPointer<KoShape> cloneBase(shape.cloneShape());
    auto *clone = dynamic_cast<KomaLineartShape*>(cloneBase.data());
    QVERIFY(clone);
    QCOMPARE(render(*clone), original);

    shape.setLineColor(Qt::blue);
    const QImage changed = render(shape);
    QVERIFY(changed != original);
    QCOMPARE(render(*clone), original);
}

void KomaLineartShapeTest::testSparseRenderCloneInvalidation()
{
    KisPaintOpPresetSP preset(new KisPaintOpPreset(TestUtil::fetchDataFileLazy("autobrush_300px.kpp")));
    QVERIFY(preset->load(KisGlobalResourcesInterface::instance()));
    KomaLineartShape shape;
    shape.setLineColor(Qt::red);
    shape.setCenterlineInDocument({{QPointF(10, 10), 8}, {QPointF(1400, 900), 8}});
    shape.setBrush(KomaLineartShape::brushSnapshot(preset, Qt::red), 8);

    auto render = [](const KomaLineartShape &line) {
        QImage image(1450, 950, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        line.paint(painter);
        return image;
    };
    const QImage original = render(shape);
    QScopedPointer<KoShape> cloneBase(shape.cloneShape());
    auto *clone = dynamic_cast<KomaLineartShape*>(cloneBase.data());
    QVERIFY(clone);
    QCOMPARE(render(*clone), original);
    shape.setLineColor(Qt::blue);
    QVERIFY(render(shape) != original);
    QCOMPARE(render(*clone), original);
    QCOMPARE(render(*clone), original);
}

void KomaLineartShapeTest::testImmediateProjectionUndoRedo()
{
    TestUtil::MaskParent parent(QRect(0, 0, 512, 512));
    parent.image->setResolution(1, 1);
    KisShapeLayerSP layer = new KisShapeLayer(nullptr, parent.image, "lineart", 255);
    parent.image->addNode(layer);
    parent.waitForImageAndShapeLayers();

    KisPaintOpPresetSP preset(new KisPaintOpPreset(TestUtil::fetchDataFileLazy("autobrush_300px.kpp")));
    QVERIFY(preset->load(KisGlobalResourcesInterface::instance()));
    auto *shape = new KomaLineartShape;
    shape->setLineColor(Qt::red);
    shape->setCenterlineInDocument({{QPointF(30, 100), 8}, {QPointF(220, 100), 12}});
    shape->setBrush(KomaLineartShape::brushSnapshot(preset, Qt::red), 12);
    KoAddShapeCommand command(shape, layer.data());
    // forceUpdateTimedNode() may defer the repaint while the image is busy
    // (its contract asks callers to wait), so wait for the pending update to
    // be taken before reading the layer.
    auto pixel = [&] {
        QElapsedTimer waited;
        waited.start();
        while (layer->hasPendingTimedUpdates() && waited.elapsed() < 5000) {
            QTest::qWait(1);
        }
        parent.image->waitForDone();
        return layer->original()->convertToQImage(nullptr, 100, 100, 1, 1).pixel(0, 0);
    };

    command.redo();
    layer->forceUpdateTimedNode();
    const QRgb first = pixel();
    QVERIFY(qAlpha(first) > 0);

    command.undo();
    layer->forceUpdateTimedNode();
    QCOMPARE(qAlpha(pixel()), 0);

    command.redo();
    layer->forceUpdateTimedNode();
    QCOMPARE(pixel(), first);
}

void KomaLineartShapeTest::testEraserSpatialCandidates()
{
    TestUtil::MaskParent image(QRect(0, 0, 500, 500));
    KisShapeLayerSP layer = new KisShapeLayer(nullptr, image.image, "lineart", 255);
    auto *near = new KomaLineartShape;
    near->setCenterlineInDocument({{QPointF(20, 20), 4}, {QPointF(100, 20), 4}});
    layer->addShape(near);
    auto *far = new KomaLineartShape;
    far->setCenterlineInDocument({{QPointF(300, 300), 4}, {QPointF(400, 300), 4}});
    layer->addShape(far);

    const auto candidates = layer->shapeManager()->shapesAt(QRectF(45, 15, 10, 10));
    QVERIFY(candidates.contains(near));
    QVERIFY(!candidates.contains(far));
    QCOMPARE(near->parent(), static_cast<KoShapeContainer*>(layer.data()));
}

void KomaLineartShapeTest::testIndexedEraserMatchesBruteForce()
{
    QRandomGenerator random(0x75192);
    for (int iteration = 0; iteration < 200; ++iteration) {
        QVector<KomaLineartPoint> line;
        QVector<QPointF> trail;
        for (int i = 0; i < 80; ++i) {
            line.append({QPointF(int(random.bounded(101)) - 50, int(random.bounded(101)) - 50),
                         qreal(random.bounded(12))});
        }
        const int trailSize = iteration % 19;
        for (int i = 0; i < trailSize; ++i) {
            trail.append(QPointF(int(random.bounded(101)) - 50, int(random.bounded(101)) - 50));
        }
        if (trail.size() > 2 && iteration % 3 == 0) {
            trail[1] = trail[0];
        }
        const qreal radius = iteration % 7;
        QVector<QVector<KomaLineartPoint>> expected;
        QVector<KomaLineartPoint> piece;
        for (const KomaLineartPoint &point : line) {
            bool touched = false;
            const int count = qMax(0, int(trail.size()) - 1);
            for (int i = 0; i < qMax(count, trail.isEmpty() ? 0 : 1); ++i) {
                const QPointF a = trail[i];
                const QPointF b = count ? trail[i + 1] : a;
                const QPointF ab = b - a;
                const qreal length2 = QPointF::dotProduct(ab, ab);
                const qreal t = length2 > 0 ? qBound(0.0, QPointF::dotProduct(point.position - a, ab) / length2, 1.0) : 0.0;
                const QPointF d = point.position - (a + ab * t);
                if (qSqrt(QPointF::dotProduct(d, d)) <= radius + point.width / 2.0) {
                    touched = true;
                    break;
                }
            }
            if (touched) {
                if (piece.size() >= 2) {
                    expected.append(piece);
                }
                piece.clear();
            } else {
                piece.append(point);
            }
        }
        if (piece.size() >= 2) {
            expected.append(piece);
        }
        QCOMPARE(KomaLineartShape::eraseTouchedArea(line, trail, radius), expected);
    }
}

void KomaLineartShapeTest::testIndexedIntersectionBoundaries()
{
    QVector<KomaLineartPoint> line;
    for (int x = 0; x <= 100; ++x) {
        line.append({QPointF(x, 0), 0});
    }
    const QVector<QVector<QPointF>> crossings = {
        {QPointF(30, -10), QPointF(30, 0), QPointF(30, 10)},
        {QPointF(70, -10), QPointF(70, 10)},
        {QPointF(70, -10), QPointF(70, 10)},
        {QPointF(0, 0), QPointF(100, 0)},
        {QPointF(50, 0), QPointF(50, 0)},
    };
    const auto pieces = KomaLineartShape::eraseUpToIntersection(line, {QPointF(50, 0)}, 0, crossings);
    QCOMPARE(pieces.size(), 2);
    QCOMPARE(pieces[0].last().position, QPointF(30, 0));
    QCOMPARE(pieces[1].first().position, QPointF(70, 0));

    QRandomGenerator random(0x91622);
    for (int iteration = 0; iteration < 200; ++iteration) {
        const int touched = 20 + random.bounded(61);
        QVector<QVector<QPointF>> others;
        qreal previous = 0;
        qreal next = 100;
        for (int i = 0; i < 80; ++i) {
            const qreal x = random.bounded(100) + 0.5;
            const qreal y1 = -qreal(random.bounded(1, 20));
            const qreal y2 = qreal(random.bounded(1, 20));
            others.append({QPointF(x, y1), QPointF(x, y2)});
            if (x < touched) {
                previous = qMax(previous, x);
            } else {
                next = qMin(next, x);
            }
        }
        const auto result = KomaLineartShape::eraseUpToIntersection(line, {QPointF(touched, 0)}, 0, others);
        QCOMPARE(result.size(), int(previous > 0) + int(next < 100));
        if (previous > 0) {
            QCOMPARE(result.first().last().position, QPointF(previous, 0));
        }
        if (next < 100) {
            QCOMPARE(result.last().first().position, QPointF(next, 0));
        }
    }
}

void KomaLineartShapeTest::testEraserOverlayFill()
{
    QVector<QVector<QPointF>> trails = {
        {{10, 10}},
        {{10, 10}, {90, 90}},
        {{10, 10}, {90, 90}, {10, 90}, {90, 10}},
        {{10, 50}, {90, 50}, {10, 50}, {90, 50}},
        {{20, 20}, {80, 20}, {80, 80}, {20, 80}, {20, 20}},
    };
    QRandomGenerator random(0x28213);
    for (int i = 0; i < 40; ++i) {
        QVector<QPointF> points;
        for (int j = 0; j < 20; ++j) {
            points.append(QPointF(10 + random.bounded(80), 10 + random.bounded(80)));
        }
        trails.append(points);
    }
    for (const auto &points : trails) {
        QPainterPath path;
        path.moveTo(points.first());
        for (const QPointF &point : points) {
            path.lineTo(point);
        }
        QPainterPathStroker stroker;
        stroker.setWidth(16);
        stroker.setCapStyle(Qt::RoundCap);
        stroker.setJoinStyle(Qt::RoundJoin);
        QPainterPath area = stroker.createStroke(path);
        area.addEllipse(points.first(), 8, 8);
        const QPainterPath original = area.simplified();
        area.setFillRule(Qt::WindingFill);
        auto render = [](const QPainterPath &fill) {
            QImage image(100, 100, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(128, 128, 128, 160));
            painter.drawPath(fill);
            return image;
        };
        const QImage a = render(area);
        const QImage b = render(original);
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) {
                if (qAbs(qAlpha(a.pixel(x, y)) - qAlpha(b.pixel(x, y))) > 4) {
                    bool hasCovered = false;
                    bool hasUncovered = false;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int px = qBound(0, x + dx, a.width() - 1);
                            const int py = qBound(0, y + dy, a.height() - 1);
                            const int alpha = qAlpha(b.pixel(px, py));
                            hasCovered |= alpha > 0;
                            hasUncovered |= alpha < 160;
                        }
                    }
                    QVERIFY(hasCovered && hasUncovered);
                }
            }
        }
    }
}

SIMPLE_TEST_MAIN(KomaLineartShapeTest)

void KomaLineartShapeTest::testBrushRenderContextReuse_data()
{
    QTest::addColumn<QString>("presetFile");
    QTest::newRow("round") << QStringLiteral("autobrush_300px.kpp");
    QTest::newRow("textured") << QStringLiteral("auto_textured_17.kpp");
    QTest::newRow("hatching") << QStringLiteral("hatching_30px.kpp");
}

void KomaLineartShapeTest::testBrushRenderContextReuse()
{
    // Lines rendered with the brush engine kept from the previous line must
    // come out exactly as lines rendered with a brush engine of their own.
    QFETCH(QString, presetFile);
    QSharedPointer<KisLocalStrokeResources> resources(new KisLocalStrokeResources());
    const QString patternPath = QFINDTESTDATA("../../../krita/data/patterns/Cross03.pat");
    QVERIFY(!patternPath.isEmpty());
    KoPatternSP pattern(new KoPattern(patternPath));
    QVERIFY(pattern->load(resources));
    pattern->setFilename(QStringLiteral("Cross03.pat"));
    resources->addResource(pattern);
    KisPaintOpPresetSP loaded(new KisPaintOpPreset(TestUtil::fetchDataFileLazy(presetFile)));
    QVERIFY(loaded->load(resources));
    loaded->setResourcesInterface(resources);

    auto makeLine = [](bool longLine) {
        QVector<KomaLineartPoint> points;
        const int count = longLine ? 60 : 12;
        for (int i = 0; i <= count; ++i) {
            const qreal t = qreal(i) / count;
            const QPointF position = longLine
                    ? QPointF(20 + 380 * t, 150 + 60 * std::sin(t * 7))
                    : QPointF(60 + 90 * t, 40 + 30 * t * t);
            points.append({position, 4 + 10 * std::sin(M_PI * t)});
        }
        return points;
    };
    auto render = [](const KisPaintOpPresetSP &brush, const QVector<KomaLineartPoint> &centerline) {
        KomaLineartShape shape;
        shape.setLineColor(QColor(30, 20, 90));
        shape.setCenterlineInDocument(centerline);
        shape.setBrush(brush, 14);
        QImage image(900, 600, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.scale(2, 2);
        painter.setTransform(shape.absoluteTransformation(), true);
        shape.paint(painter);
        painter.end();
        return image;
    };

    const QVector<KomaLineartPoint> shortLine = makeLine(false);
    const QVector<KomaLineartPoint> longLine = makeLine(true);
    // Each snapshot gets a brush engine of its own.
    const QImage freshShort = render(KomaLineartShape::brushSnapshot(loaded, Qt::black), shortLine);
    const QImage freshLong = render(KomaLineartShape::brushSnapshot(loaded, Qt::black), longLine);
    QVERIFY(freshShort != QImage(freshShort.size(), freshShort.format()));

    // One snapshot: each render after the first reuses the previous engine.
    const KisPaintOpPresetSP shared = KomaLineartShape::brushSnapshot(loaded, Qt::black);
    QCOMPARE(render(shared, shortLine), freshShort);
    QCOMPARE(render(shared, longLine), freshLong);
    QCOMPARE(render(shared, shortLine), freshShort);
    QCOMPARE(render(shared, longLine), freshLong);
}

void KomaLineartShapeTest::testInteractiveRendering()
{
    KisPaintOpPresetSP loaded(new KisPaintOpPreset(TestUtil::fetchDataFileLazy("autobrush_300px.kpp")));
    QVERIFY(loaded->load(KisGlobalResourcesInterface::instance()));
    const KisPaintOpPresetSP brush = KomaLineartShape::brushSnapshot(loaded, Qt::black);

    QVector<KomaLineartPoint> centerline;
    for (int i = 0; i <= 40; ++i) {
        const qreal t = i / 40.0;
        centerline.append({QPointF(20 + 600 * t, 120 + 50 * std::sin(t * 9)), 3 + 6 * std::sin(M_PI * t)});
    }
    auto paint = [](const KomaLineartShape &shape) {
        QImage image(1400, 500, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.scale(2, 2);
        painter.setTransform(shape.absoluteTransformation(), true);
        shape.paint(painter);
        painter.end();
        return image;
    };
    auto exactRender = [&](const QVector<KomaLineartPoint> &documentCenterline) {
        KomaLineartShape shape;
        shape.setLineColor(Qt::black);
        shape.setCenterlineInDocument(documentCenterline);
        shape.setBrush(brush, 9);
        return paint(shape);
    };

    KomaLineartShape shape;
    shape.setLineColor(Qt::black);
    shape.setCenterlineInDocument(centerline);
    shape.setBrush(brush, 9);
    const QImage before = paint(shape);
    QCOMPARE(before, exactRender(centerline));

    // Starting interactive rendering keeps the exact render of an unchanged line.
    shape.setInteractive(true);
    QCOMPARE(paint(shape), before);

    // Edits are shown from pieces; two edits near different spots of the line.
    QVector<KomaLineartPoint> edited = shape.centerlineInDocument();
    for (int step = 0; step < 2; ++step) {
        edited = KomaLineartShape::adjustWidth(edited, edited[10 + 20 * step].position, 40, 1.5);
        shape.setCenterlineFromDocument(edited);
        const QImage interactive = paint(shape);
        QVERIFY(interactive != before);
        // The preview covers the line to within a pixel (its pieces lie on
        // the pixel grid, the exact render at a sub-pixel offset): every
        // strongly inked pixel of the exact render has ink next to it.
        const QImage exact = exactRender(shape.centerlineInDocument());
        int missing = 0;
        for (int y = 1; y + 1 < exact.height(); ++y) {
            for (int x = 1; x + 1 < exact.width(); ++x) {
                if (qAlpha(exact.pixel(x, y)) <= 128) {
                    continue;
                }
                bool inked = false;
                for (int dy = -1; dy <= 1 && !inked; ++dy) {
                    for (int dx = -1; dx <= 1 && !inked; ++dx) {
                        inked = qAlpha(interactive.pixel(x + dx, y + dy)) > 0;
                    }
                }
                missing += inked ? 0 : 1;
            }
        }
        QCOMPARE(missing, 0);
    }

    // Ending interactive rendering renders the line whole again, exactly.
    const QVector<KomaLineartPoint> finalCenterline = shape.centerlineInDocument();
    shape.setInteractive(false);
    QCOMPARE(paint(shape), exactRender(finalCenterline));
}
