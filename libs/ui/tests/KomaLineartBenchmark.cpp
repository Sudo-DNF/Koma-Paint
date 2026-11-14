/*
 * SPDX-FileCopyrightText: 2026 Koma Paint contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KomaLineartBenchmark.h"

#include "flake/KomaLineartShape.h"
#include "flake/kis_shape_layer.h"
#include "tool/KomaLineartPreview.h"
#include "tool/KisAsynchronousStrokeUpdateHelper.h"
#include "tool/kis_resources_snapshot.h"
#include "tool/strokes/KisFreehandStrokeInfo.h"
#include "tool/strokes/freehand_stroke.h"
#include <QEventLoop>
#include <QPainter>
#include <QPainterPathStroker>
#include <QElapsedTimer>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <tuple>

#include <KisLocalStrokeResources.h>
#include <KisResourceTypes.h>
#include <KisRunnableStrokeJobData.h>
#include <KoCanvasResourceProvider.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>
#include <brushengine/kis_paint_information.h>
#include <brushengine/kis_paintop.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_settings.h>
#include <kis_distance_information.h>
#include <kis_image.h>
#include <kis_paint_layer.h>
#include <kis_painter.h>
#include <resources/KoPattern.h>
#include <testutil.h>

namespace
{
bool containsInk(const QImage &image)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 0) {
                return true;
            }
        }
    }
    return false;
}
}

void KomaLineartBenchmark::eraseIntersections()
{
    QVector<KomaLineartPoint> line;
    for (int x = 0; x <= 2048; ++x) {
        line.append({QPointF(x, 1024), 4});
    }
    QVector<QVector<QPointF>> others;
    for (int n = 0; n < 32; ++n) {
        QVector<QPointF> other;
        for (int y = 0; y <= 2048; ++y) {
            other.append(QPointF(64 * n, y));
        }
        others.append(other);
    }
    QBENCHMARK {
        const auto result = KomaLineartShape::eraseUpToIntersection(line, {QPointF(1030, 1024)}, 5, others);
        Q_UNUSED(result);
    }
}

void KomaLineartBenchmark::eraseLongTrail()
{
    QVector<KomaLineartPoint> line;
    QVector<QPointF> trail;
    for (int x = 0; x <= 3840; ++x) {
        line.append({QPointF(x, 1080), 4});
        trail.append(QPointF(x, 1082));
    }
    QBENCHMARK {
        const auto result = KomaLineartShape::eraseTouchedArea(line, trail, 5);
        Q_UNUSED(result);
    }
}

void KomaLineartBenchmark::previewUpdates()
{
    QImage part(32, 32, QImage::Format_ARGB32_Premultiplied);
    part.fill(Qt::black);
    QBENCHMARK {
        KomaLineartPreview preview;
        for (int i = 0; i < 1000; ++i) {
            preview.update(QRect(i * 3, i * 2, 32, 32), part);
        }
    }
}

void KomaLineartBenchmark::previewPaintClipped()
{
    KomaLineartPreview preview;
    QImage part(16, 16, QImage::Format_ARGB32_Premultiplied);
    part.fill(Qt::black);
    for (int i = 0; i < 256; ++i) {
        preview.update(QRect((i % 16) * 256 + 8, (i / 16) * 256 + 8, 16, 16), part);
    }
    QImage target(256, 256, QImage::Format_ARGB32_Premultiplied);
    QBENCHMARK {
        QPainter painter(&target);
        painter.setClipRect(QRect(0, 0, 256, 256));
        preview.paint(painter, 1, 1);
    }
}

void KomaLineartBenchmark::eraserOverlay()
{
    QPainterPath trail;
    trail.moveTo(0, 1000);
    for (int i = 1; i < 3840; ++i) {
        trail.lineTo(i, 1000 + 40 * std::sin(i / 50.0));
    }
    QPainterPathStroker stroker;
    stroker.setWidth(20);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    QBENCHMARK {
        QPainterPath area = stroker.createStroke(trail);
        area.addEllipse(trail.pointAtPercent(0), 10, 10);
        area.setFillRule(Qt::WindingFill);
        QImage image(3840, 100, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.translate(0, -950);
        painter.setBrush(QColor(128, 128, 128, 160));
        painter.setPen(Qt::NoPen);
        painter.drawPath(area);
    }
}

void KomaLineartBenchmark::brushReplay_data()
{
    QTest::addColumn<QString>("presetFile");
    QTest::addColumn<int>("brushSize");
    QTest::addColumn<QSize>("canvasSize");
    QTest::newRow("solid-4k") << QStringLiteral("autobrush_300px.kpp") << 40 << QSize(3840, 2160);
    QTest::newRow("textured-2k") << QStringLiteral("auto_textured_17.kpp") << 40 << QSize(2048, 2048);
    QTest::newRow("hatching-4k") << QStringLiteral("hatching_30px.kpp") << 40 << QSize(3840, 2160);
    QTest::newRow("large-textured-4k") << QStringLiteral("auto_textured_17.kpp") << 300 << QSize(3840, 2160);
}

void KomaLineartBenchmark::brushReplay()
{
    QFETCH(QString, presetFile);
    QFETCH(int, brushSize);
    QFETCH(QSize, canvasSize);
    QSharedPointer<KisLocalStrokeResources> resources(new KisLocalStrokeResources());
    if (presetFile == QStringLiteral("auto_textured_17.kpp")) {
        const QString path = QFINDTESTDATA("../../../krita/data/patterns/Cross03.pat");
        QVERIFY(!path.isEmpty());
        KoPatternSP pattern(new KoPattern(path));
        QVERIFY(pattern->load(resources));
        pattern->setFilename(QStringLiteral("Cross03.pat"));
        resources->addResource(pattern);
    }
    KisPaintOpPresetSP loaded(new KisPaintOpPreset(TestUtil::fetchDataFileLazy(presetFile)));
    QVERIFY(loaded->load(resources));
    loaded->setResourcesInterface(resources);
    KisPaintOpPresetSP snapshot = KomaLineartShape::brushSnapshot(loaded, Qt::black);
    QVERIFY(snapshot && snapshot->settings());
    if (presetFile == QStringLiteral("auto_textured_17.kpp")) {
        QVERIFY(snapshot->resourcesInterface()->source<KoPattern>(ResourceType::Patterns)
                .bestMatch(QString(), QStringLiteral("Cross03.pat"), QString()));
    }

    QVector<qint64> sampleTimes;
    sampleTimes.reserve(400);
    QBENCHMARK {
        sampleTimes.clear();
        const KoColorSpace *colorSpace = KoColorSpaceRegistry::instance()->rgb8();
        KisImageSP image = new KisImage(nullptr, canvasSize.width(), canvasSize.height(), colorSpace, QStringLiteral("benchmark"));
        KisPaintLayerSP layer = new KisPaintLayer(image, QStringLiteral("benchmark"), OPACITY_OPAQUE_U8, colorSpace);
        KisPaintOpPresetSP preset = snapshot->clone().dynamicCast<KisPaintOpPreset>();
        preset->settings()->setPaintOpSize(brushSize);
        KisPainter painter(layer->paintDevice());
        painter.setPaintColor(KoColor(Qt::black, colorSpace));
        painter.setBackgroundColor(KoColor(Qt::white, colorSpace));
        painter.setCompositeOpId(COMPOSITE_OVER);
        painter.setOpacityF(preset->settings()->paintOpOpacity());
        painter.setPaintOpPreset(preset, layer, image);
        QVERIFY(painter.paintOp());
        KisDistanceInformation distance;
        KisPaintInformation previous(QPointF(100, 1000), 0.6);
        KomaLineartPreview preview;
        painter.paintAt(previous, &distance);
        for (int i = 1; i <= 400; ++i) {
            QElapsedTimer timer;
            timer.start();
            const KisPaintInformation current(QPointF(100 + i * 2, 1000 + 30 * std::sin(i / 30.0)), 0.6);
            painter.paintLine(previous, current, &distance);
            previous = current;
            for (int round = 0; round < 10000; ++round) {
                QVector<KisRunnableStrokeJobData*> jobs;
                bool needsMoreUpdates = false;
                std::tie(std::ignore, needsMoreUpdates) = painter.paintOp()->doAsynchronousUpdate(jobs);
                for (KisRunnableStrokeJobData *job : jobs) {
                    job->run();
                    delete job;
                }
                if (jobs.isEmpty() && !needsMoreUpdates) {
                    break;
                }
            }
            for (const QRect &rect : painter.takeDirtyRegion()) {
                preview.update(rect, layer->paintDevice()->convertToQImage(nullptr, rect.x(), rect.y(),
                                                                          rect.width(), rect.height()));
            }
            sampleTimes.append(timer.nsecsElapsed());
        }
    }
    std::sort(sampleTimes.begin(), sampleTimes.end());
    const auto milliseconds = [&](int percentile) {
        return sampleTimes[(sampleTimes.size() - 1) * percentile / 100] / 1000000.0;
    };
    std::printf("Brush sample processing, ms: p50 %.3f p95 %.3f p99 %.3f max %.3f\n",
                milliseconds(50), milliseconds(95), milliseconds(99), sampleTimes.last() / 1000000.0);
}

void KomaLineartBenchmark::finishedVectorRender_data()
{
    QTest::addColumn<QString>("presetFile");
    QTest::addColumn<QSize>("canvasSize");
    QTest::addColumn<bool>("diagonal");
    QTest::newRow("solid-4k") << QStringLiteral("autobrush_300px.kpp") << QSize(3840, 2160) << false;
    QTest::newRow("textured-2k") << QStringLiteral("auto_textured_17.kpp") << QSize(2048, 2048) << false;
    QTest::newRow("textured-diagonal-4k") << QStringLiteral("auto_textured_17.kpp") << QSize(3840, 2160) << true;
}

void KomaLineartBenchmark::finishedVectorRender()
{
    QFETCH(QString, presetFile);
    QFETCH(QSize, canvasSize);
    QFETCH(bool, diagonal);
    QSharedPointer<KisLocalStrokeResources> resources(new KisLocalStrokeResources());
    if (presetFile == QStringLiteral("auto_textured_17.kpp")) {
        const QString path = QFINDTESTDATA("../../../krita/data/patterns/Cross03.pat");
        QVERIFY(!path.isEmpty());
        KoPatternSP pattern(new KoPattern(path));
        QVERIFY(pattern->load(resources));
        pattern->setFilename(QStringLiteral("Cross03.pat"));
        resources->addResource(pattern);
    }
    KisPaintOpPresetSP loaded(new KisPaintOpPreset(TestUtil::fetchDataFileLazy(presetFile)));
    QVERIFY(loaded->load(resources));
    loaded->setResourcesInterface(resources);
    KisPaintOpPresetSP preset = KomaLineartShape::brushSnapshot(loaded, Qt::black);
    QVERIFY(preset && preset->settings());

    QVector<KomaLineartPoint> raw;
    for (int i = 0; i <= 400; ++i) {
        const qreal pressure = 0.6 + 0.25 * std::sin(i / 50.0);
        const qreal width = 40 * (KomaLineartShape::minimumPressureShare
                                  + (1 - KomaLineartShape::minimumPressureShare) * pressure);
        raw.append({QPointF(100 + i * 2, 1000 + (diagonal ? i * 2 : 0) + 30 * std::sin(i / 30.0)), width});
    }
    QImage target(canvasSize, QImage::Format_ARGB32_Premultiplied);
    target.fill(Qt::transparent);
    QVector<qint64> processTimes;
    QVector<qint64> renderTimes;
    for (int iteration = 0; iteration < 110; ++iteration) {
        QElapsedTimer timer;
        timer.start();
        const QVector<KomaLineartPoint> centerline = KomaLineartShape::processRawStroke(raw, 0.35);
        KomaLineartShape shape;
        shape.setLineColor(Qt::black);
        shape.setCenterlineInDocument(centerline);
        shape.setBrush(preset, 40);
        const qint64 processed = timer.nsecsElapsed();
        {
            QPainter painter(&target);
            shape.paint(painter);
        }
        if (iteration >= 10) {
            processTimes.append(processed);
            renderTimes.append(timer.nsecsElapsed() - processed);
        }
    }
    auto report = [](const char *name, QVector<qint64> times) {
        std::sort(times.begin(), times.end());
        auto ms = [&](int percentile) { return times[(times.size() - 1) * percentile / 100] / 1000000.0; };
        std::printf("%s, ms: p50 %.3f p95 %.3f p99 %.3f max %.3f\n",
                    name, ms(50), ms(95), ms(99), times.last() / 1000000.0);
    };
    report("Vector processing and shape setup", processTimes);
    report("Vector first paint (cold cache)", renderTimes);
}

void KomaLineartBenchmark::vectorProjection_data()
{
    QTest::addColumn<bool>("expedite");
    QTest::addColumn<QString>("presetFile");
    QTest::addColumn<QSize>("canvasSize");
    QTest::newRow("solid-4k-batched") << false << QStringLiteral("autobrush_300px.kpp") << QSize(3840, 2160);
    QTest::newRow("solid-4k-expedited") << true << QStringLiteral("autobrush_300px.kpp") << QSize(3840, 2160);
    QTest::newRow("textured-2k-batched") << false << QStringLiteral("auto_textured_17.kpp") << QSize(2048, 2048);
    QTest::newRow("textured-2k-expedited") << true << QStringLiteral("auto_textured_17.kpp") << QSize(2048, 2048);
}

void KomaLineartBenchmark::vectorProjection()
{
    QFETCH(bool, expedite);
    QFETCH(QString, presetFile);
    QFETCH(QSize, canvasSize);
    QSharedPointer<KisLocalStrokeResources> resources(new KisLocalStrokeResources());
    if (presetFile == QStringLiteral("auto_textured_17.kpp")) {
        const QString path = QFINDTESTDATA("../../../krita/data/patterns/Cross03.pat");
        QVERIFY(!path.isEmpty());
        KoPatternSP pattern(new KoPattern(path));
        QVERIFY(pattern->load(resources));
        pattern->setFilename(QStringLiteral("Cross03.pat"));
        resources->addResource(pattern);
    }
    KisPaintOpPresetSP loaded(new KisPaintOpPreset(TestUtil::fetchDataFileLazy(presetFile)));
    QVERIFY(loaded->load(resources));
    loaded->setResourcesInterface(resources);
    KisPaintOpPresetSP preset = KomaLineartShape::brushSnapshot(loaded, Qt::black);
    QVERIFY(preset && preset->settings());

    QVector<qint64> firstUpdateTimes;
    QVector<qint64> completedTimes;
    std::unique_ptr<TestUtil::MaskParent> parent;
    KisShapeLayerSP layer;
    for (int i = 0; i < 110; ++i) {
        if (i % 20 == 0) {
            layer = nullptr;
            parent.reset(new TestUtil::MaskParent(QRect(QPoint(0, 0), canvasSize)));
            parent->image->setResolution(1, 1);
            layer = new KisShapeLayer(nullptr, parent->image, "lineart", 255);
            parent->image->addNode(layer);
            parent->image->waitForDone();
            QCoreApplication::processEvents();
            layer->forceUpdateTimedNode();
            parent->image->waitForDone();
            QCoreApplication::processEvents();
        }
        const int y = 40 + (i % 20) * 90;
        QVector<KomaLineartPoint> raw;
        for (int j = 0; j <= 400; ++j) {
            const qreal pressure = 0.6 + 0.25 * std::sin(j / 50.0);
            const qreal width = 40 * (KomaLineartShape::minimumPressureShare
                                      + (1 - KomaLineartShape::minimumPressureShare) * pressure);
            raw.append({QPointF(100 + j * 2, y + 3 * std::sin(j / 30.0)), width});
        }

        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QElapsedTimer timer;
        connect(parent->image.data(), &KisImage::sigImageUpdated, &loop,
                [&](const QRect &rect) {
                    if (rect.contains(QPoint(500, y))) loop.quit();
                }, Qt::QueuedConnection);
        timer.start();
        const auto centerline = KomaLineartShape::processRawStroke(raw, 0.35);
        auto *shape = new KomaLineartShape;
        shape->setLineColor(Qt::black);
        shape->setCenterlineInDocument(centerline);
        shape->setBrush(preset, 40);
        layer->addShape(shape);
        if (expedite) {
            layer->forceUpdateTimedNode();
        }
        timeout.start(10000);
        loop.exec();
        QVERIFY2(timeout.isActive(), "Timed out waiting for the layer projection update");
        const qint64 elapsed = timer.nsecsElapsed();
        parent->image->waitForDone();
        const qint64 completed = timer.nsecsElapsed();
        const QImage sample = layer->original()->convertToQImage(nullptr, 490, y - 10, 20, 20);
        QVERIFY2(containsInk(sample), "Projection did not contain the completed stroke");
        QCOMPARE(parent->image->projection()->convertToQImage(nullptr, 490, y - 10, 20, 20), sample);
        if (i >= 10) {
            firstUpdateTimes.append(elapsed);
            completedTimes.append(completed);
        }
        QTest::qWait(5);
    }
    auto report = [](const char *name, QVector<qint64> times) {
        std::sort(times.begin(), times.end());
        auto ms = [&](int percentile) { return times[(times.size() - 1) * percentile / 100] / 1000000.0; };
        std::printf("%s, ms: p50 %.3f p95 %.3f p99 %.3f max %.3f\n",
                    name, ms(50), ms(95), ms(99), times.last() / 1000000.0);
    };
    report("Vector finish to first image update", firstUpdateTimes);
    report("Vector finish to image jobs complete", completedTimes);
}

void KomaLineartBenchmark::rasterFinish_data()
{
    QTest::addColumn<QString>("presetFile");
    QTest::addColumn<QSize>("canvasSize");
    QTest::newRow("solid-4k") << QStringLiteral("autobrush_300px.kpp") << QSize(3840, 2160);
    QTest::newRow("textured-2k") << QStringLiteral("auto_textured_17.kpp") << QSize(2048, 2048);
}

void KomaLineartBenchmark::rasterFinish()
{
    QFETCH(QString, presetFile);
    QFETCH(QSize, canvasSize);
    TestUtil::MaskParent parent(QRect(QPoint(0, 0), canvasSize));
    KoCanvasResourceProvider manager;
    manager.setResource(KoCanvasResource::ForegroundColor,
                        QVariant::fromValue(KoColor(Qt::black, parent.image->colorSpace())));
    manager.setResource(KoCanvasResource::BackgroundColor,
                        QVariant::fromValue(KoColor(Qt::white, parent.image->colorSpace())));
    manager.setResource(KoCanvasResource::CurrentKritaNode, QVariant::fromValue(KisNodeSP(parent.layer)));
    manager.setResource(KoCanvasResource::CurrentEffectiveCompositeOp, COMPOSITE_OVER);
    manager.setResource(KoCanvasResource::Opacity, 1.0);
    QSharedPointer<KisLocalStrokeResources> resources(new KisLocalStrokeResources());
    if (presetFile == QStringLiteral("auto_textured_17.kpp")) {
        const QString path = QFINDTESTDATA("../../../krita/data/patterns/Cross03.pat");
        QVERIFY(!path.isEmpty());
        KoPatternSP pattern(new KoPattern(path));
        QVERIFY(pattern->load(resources));
        pattern->setFilename(QStringLiteral("Cross03.pat"));
        resources->addResource(pattern);
    }
    KisPaintOpPresetSP preset(new KisPaintOpPreset(TestUtil::fetchDataFileLazy(presetFile)));
    QVERIFY(preset->load(resources));
    preset->setResourcesInterface(resources);
    preset->settings()->setPaintOpSize(40);
    manager.setResource(KoCanvasResource::CurrentPaintOpPreset, QVariant::fromValue(preset));
    parent.image->waitForDone();

    QVector<qint64> times;
    for (int iteration = 0; iteration < 110; ++iteration) {
        parent.layer->paintDevice()->clear();
        parent.layer->setDirty(QRect(60, 0, 900, canvasSize.height()));
        parent.image->waitForDone();
        KisResourcesSnapshotSP resources = new KisResourcesSnapshot(parent.image, parent.layer, &manager);
        auto *stroke = new FreehandStrokeStrategy(resources, new KisFreehandStrokeInfo(),
                                                  kundo2_noi18n("Benchmark stroke"));
        KisStrokeId id = parent.image->startStroke(stroke);
        const int y = 30 + iteration * 15;
        KisPaintInformation previous(QPointF(100, y), 0.6);
        for (int i = 1; i <= 400; ++i) {
            const KisPaintInformation current(QPointF(100 + i * 2, y + 3 * std::sin(i / 30.0)), 0.6);
            parent.image->addJob(id, new FreehandStrokeStrategy::Data(0, previous, current));
            previous = current;
            if (i % 10 == 0) QTest::qWait(1);
        }
        QElapsedTimer timer;
        timer.start();
        parent.image->addJob(id, new KisAsynchronousStrokeUpdateHelper::UpdateData(true));
        parent.image->endStroke(id);
        parent.image->waitForDone();
        const qint64 elapsed = timer.nsecsElapsed();
        const QImage sample = parent.layer->paintDevice()->convertToQImage(nullptr, 490, y - 10, 20, 20);
        QVERIFY2(containsInk(sample), "Raster stroke did not reach its target layer");
        QCOMPARE(parent.image->projection()->convertToQImage(nullptr, 490, y - 10, 20, 20), sample);
        if (iteration >= 10) times.append(elapsed);
    }
    std::sort(times.begin(), times.end());
    auto ms = [&](int percentile) { return times[(times.size() - 1) * percentile / 100] / 1000000.0; };
    std::printf("Raster release to stroke jobs complete, ms: p50 %.3f p95 %.3f p99 %.3f max %.3f\n",
                ms(50), ms(95), ms(99), times.last() / 1000000.0);
}

void KomaLineartBenchmark::populatedVectorRender()
{
    QSharedPointer<KisLocalStrokeResources> resources(new KisLocalStrokeResources());
    const QString path = QFINDTESTDATA("../../../krita/data/patterns/Cross03.pat");
    QVERIFY(!path.isEmpty());
    KoPatternSP pattern(new KoPattern(path));
    QVERIFY(pattern->load(resources));
    pattern->setFilename(QStringLiteral("Cross03.pat"));
    resources->addResource(pattern);
    KisPaintOpPresetSP loaded(new KisPaintOpPreset(TestUtil::fetchDataFileLazy("auto_textured_17.kpp")));
    QVERIFY(loaded->load(resources));
    loaded->setResourcesInterface(resources);
    const KisPaintOpPresetSP preset = KomaLineartShape::brushSnapshot(loaded, Qt::black);
    QVERIFY(preset && preset->settings());

    std::vector<std::unique_ptr<KomaLineartShape>> lines;
    for (int i = 0; i < 8; ++i) {
        std::unique_ptr<KomaLineartShape> shape(new KomaLineartShape);
        shape->setLineColor(Qt::black);
        shape->setCenterlineInDocument({{QPointF(100, 100 + i * 5), 28},
                                       {QPointF(3700, 2000 + i * 5), 28}});
        shape->setBrush(preset, 40);
        lines.push_back(std::move(shape));
    }

    QImage target(3840, 2160, QImage::Format_ARGB32_Premultiplied);
    target.fill(Qt::transparent);
    QVector<qint64> times;
    for (int iteration = 0; iteration < 110; ++iteration) {
        QElapsedTimer timer;
        timer.start();
        {
            QPainter painter(&target);
            for (const auto &line : lines) {
                line->paint(painter);
            }
        }
        if (iteration >= 10) times.append(timer.nsecsElapsed());
    }
    std::sort(times.begin(), times.end());
    auto ms = [&](int percentile) { return times[(times.size() - 1) * percentile / 100] / 1000000.0; };
    std::printf("Eight overlapping 4K vector renders, ms: p50 %.3f p95 %.3f p99 %.3f max %.3f\n",
                ms(50), ms(95), ms(99), times.last() / 1000000.0);
}

SIMPLE_TEST_MAIN(KomaLineartBenchmark)
