/*
 * SPDX-FileCopyrightText: 2026 Koma Paint contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KomaLineartBenchmark.h"

#include "flake/KomaLineartShape.h"
#include "flake/kis_shape_layer.h"
#include "tool/KomaLineartEditor.h"
#include "tool/KomaLineartPreview.h"
#include "tool/KomaLineartRecorder.h"
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
#include <mutex>
#include <chrono>
#include <tuple>

#include <KisLocalStrokeResources.h>
#include <KisResourceTypes.h>
#include <KisRunnableStrokeJobData.h>
#include <KoCanvasResourceProvider.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>
#include <KoShapeManager.h>
#include <KoViewConverter.h>
#include <KisGlobalResourcesInterface.h>
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


namespace
{
/// Pixels per document unit of a 300 ppi image.
constexpr qreal denseResolution = 300.0 / 72.0;
constexpr int denseLineCount = 220;
constexpr qreal denseBrushPixels = 8.0;

/// Prints p50/p95/p99/max of @p times (nanoseconds) in milliseconds and returns p95.
double printPercentiles(const char *name, QVector<qint64> times)
{
    if (times.isEmpty()) {
        std::printf("%s: no samples\n", name);
        return 0;
    }
    std::sort(times.begin(), times.end());
    auto ms = [&](int percentile) { return times[(times.size() - 1) * percentile / 100] / 1000000.0; };
    std::printf("%s, ms: p50 %.3f p95 %.3f p99 %.3f max %.3f (n=%lld)\n",
                name, ms(50), ms(95), ms(99), times.last() / 1000000.0, qint64(times.size()));
    std::fflush(stdout);
    return ms(95);
}

QPointF wavePoint(const QPointF &start, const QPointF &end, qreal amplitude, qreal phase, qreal t)
{
    const QPointF chord = end - start;
    const qreal length = std::hypot(chord.x(), chord.y());
    const QPointF normal(-chord.y() / length, chord.x() / length);
    return start + chord * t + normal * amplitude * std::sin(t * M_PI * 2 + phase);
}

/**
 * A 2400x1600 px image at 300 ppi with a vector layer holding 220 crossing
 * lineart strokes, each rendered with a round brush and a pressure taper,
 * built the way KomaLineartRecorder builds them.
 */
struct DenseLineart {
    std::unique_ptr<TestUtil::MaskParent> parent;
    KisShapeLayerSP layer;
    KisPaintOpPresetSP preset;   ///< The current brush, 8 px.
    QVector<KomaLineartShape*> lines;

    bool create()
    {
        parent.reset(new TestUtil::MaskParent(QRect(0, 0, 2400, 1600)));
        parent->image->setResolution(denseResolution, denseResolution);
        layer = new KisShapeLayer(nullptr, parent->image, "lineart", 255);
        parent->image->addNode(layer);
        parent->waitForImageAndShapeLayers();

        preset = KisPaintOpPresetSP(new KisPaintOpPreset(TestUtil::fetchDataFileLazy("autobrush_300px.kpp")));
        if (!preset->load(KisGlobalResourcesInterface::instance())) {
            return false;
        }
        preset->settings()->setPaintOpSize(denseBrushPixels);
        const KisPaintOpPresetSP snapshot = KomaLineartShape::brushSnapshot(preset, Qt::black);

        std::srand(7);
        auto random = [](qreal low, qreal high) { return low + (high - low) * (std::rand() / qreal(RAND_MAX)); };
        for (int index = 0; index < denseLineCount; ++index) {
            QPointF start;
            QPointF end;
            if (index < denseLineCount / 2) {
                const qreal y = 60 + index * 13.2;
                const qreal x = random(40, 900);
                start = QPointF(x, y);
                end = QPointF(x + random(600, 1400), y + random(-60, 60));
            } else {
                const qreal x = 60 + (index - denseLineCount / 2) * 20.5;
                const qreal y = random(40, 600);
                start = QPointF(x, y);
                end = QPointF(x + random(-80, 80), y + random(500, 950));
            }
            const qreal amplitude = random(5, 30);
            const qreal phase = random(0, 6.28);
            const int samples = qMax(20, int(std::hypot(end.x() - start.x(), end.y() - start.y()) / 6.0));
            QVector<KomaLineartPoint> raw;
            for (int k = 0; k <= samples; ++k) {
                const qreal t = qreal(k) / samples;
                const qreal pressure = 0.2 + 0.8 * std::sin(M_PI * t);
                const qreal width = denseBrushPixels / denseResolution
                        * (KomaLineartShape::minimumPressureShare
                           + (1 - KomaLineartShape::minimumPressureShare) * pressure);
                raw.append({wavePoint(start, end, amplitude, phase, t) / denseResolution, width});
            }
            auto *shape = new KomaLineartShape;
            shape->setLineColor(Qt::black);
            shape->setCenterlineInDocument(KomaLineartShape::processRawStroke(raw, 0.35 / denseResolution));
            shape->setBrush(snapshot, denseBrushPixels / denseResolution);
            layer->addShape(shape);
            lines.append(shape);
        }
        settle();
        return true;
    }

    /// Lets the vector layer finish every pending repaint.
    void settle()
    {
        QCoreApplication::processEvents();
        layer->forceUpdateTimedNode();
        parent->image->waitForDone();
        QCoreApplication::processEvents();
    }

    KomaLineartRecorder::Target target(KomaLineartRecorder::EraserMode mode,
                                       std::function<void(const QRectF &)> updateCanvas)
    {
        KomaLineartRecorder::Target result;
        result.image = parent->image;
        result.layer = layer.data();
        result.preset = preset;
        result.color = Qt::black;
        result.eraserMode = mode;
        result.updateCanvas = updateCanvas;
        return result;
    }
};

/**
 * Paints what the canvas paints for the recorder after an input event: the
 * live preview or eraser overlay, clipped to the rect the event updated,
 * into a 1920x1080 view at @p zoom (1 = 100%) that follows @p focus.
 */
qint64 paintRecorderFrame(const KomaLineartRecorder &recorder, const QRectF &dirtyDocumentRect,
                          const QPointF &focus, qreal zoom, QImage &view)
{
    KoViewConverter converter;
    converter.setZoom(denseResolution * zoom);
    const QPointF focusInView = converter.documentToView(focus);
    const QPointF offset = QPointF(view.width() / 2.0, view.height() / 2.0) - focusInView;
    QElapsedTimer timer;
    timer.start();
    QPainter painter(&view);
    painter.translate(offset);
    painter.setClipRect(converter.documentToView(dirtyDocumentRect));
    recorder.paint(painter, converter);
    painter.end();
    return timer.nsecsElapsed();
}

/// The image updates reported while it is alive: their total area and when they arrived.
class ImageUpdateArea
{
public:
    explicit ImageUpdateArea(KisImageSP image)
    {
        m_connection = QObject::connect(image.data(), &KisImage::sigImageUpdated, image.data(),
                                        [this](const QRect &rect) {
            const qint64 now = std::chrono::steady_clock::now().time_since_epoch().count();
            std::lock_guard<std::mutex> lock(m_mutex);
            m_area += qint64(rect.width()) * rect.height();
            m_times.append(now);
        }, Qt::DirectConnection);
    }
    ~ImageUpdateArea()
    {
        QObject::disconnect(m_connection);
    }
    qint64 takeArea()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const qint64 area = m_area;
        m_area = 0;
        return area;
    }
    /// Steady-clock times (ns) of the updates since the last call.
    QVector<qint64> takeTimes()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        QVector<qint64> times;
        std::swap(times, m_times);
        return times;
    }

private:
    QMetaObject::Connection m_connection;
    std::mutex m_mutex;
    qint64 m_area {0};
    QVector<qint64> m_times;
};

/**
 * Measures @p count edits made as during a drag, in three ways:
 *
 * - a continuous drag at 120 Hz: the time each edit takes; the intervals
 *   between the updates the layer's projection receives (its frame rate
 *   while the user drags, including stretches where no edit changed
 *   anything); and how long the oldest change not shown yet waits for the
 *   update that shows it;
 * - single edits: the time until the projection shows one edit;
 * - the repaint alone: the cost of repainting the layer's dirty region after
 *   one edit, and the share of the layer one edit marks dirty.
 */
void measureLayerEdits(const char *name, DenseLineart &dense, int count, const std::function<bool(int)> &edit)
{
    constexpr qint64 cadence = 8333333; // 120 Hz
    KisImageSP image = dense.parent->image;
    QVector<qint64> eventTimes;
    QVector<qint64> frameIntervals;
    QVector<qint64> frameWaits;
    QVector<qint64> latencies;
    QVector<qint64> repaintTimes;
    qint64 largestUpdate = 0;
    int next = 0;
    {
        ImageUpdateArea updates(image);
        QElapsedTimer clock;
        clock.start();
        QVector<qint64> changes; // Steady-clock times of the edits that changed the layer.
        for (int i = 0; i < count; ++i, ++next) {
            while (clock.nsecsElapsed() < i * cadence) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
            }
            const qint64 start = clock.nsecsElapsed();
            const bool changed = edit(next);
            eventTimes.append(clock.nsecsElapsed() - start);
            if (changed) {
                changes.append(std::chrono::steady_clock::now().time_since_epoch().count());
            }
        }
        const qint64 dragEnd = std::chrono::steady_clock::now().time_since_epoch().count();
        while (dense.layer->hasPendingTimedUpdates()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        }
        image->waitForDone();
        QCoreApplication::processEvents();
        // One repaint reports several rects; updates within 1 ms are one frame.
        QVector<qint64> frames;
        for (qint64 time : updates.takeTimes()) {
            if (time <= dragEnd && (frames.isEmpty() || time - frames.last() > 1000000)) {
                frames.append(time);
            }
        }
        for (int i = 1; i < frames.size(); ++i) {
            frameIntervals.append(frames[i] - frames[i - 1]);
        }
        // An edit that changed nothing (the brush between lines) needs no
        // frame: only the time a change waits to be shown counts.
        int change = 0;
        for (int i = 0; i < frames.size(); ++i) {
            while (change < changes.size() && changes[change] <= (i > 0 ? frames[i - 1] : 0)) {
                ++change;
            }
            if (change < changes.size() && changes[change] < frames[i]) {
                frameWaits.append(frames[i] - changes[change]);
            }
        }
    }
    dense.settle();
    {
        ImageUpdateArea updates(image);
        QElapsedTimer clock;
        clock.start();
        for (int i = 0; i < 60; ++i, ++next) {
            const qint64 start = clock.nsecsElapsed();
            edit(next);
            // Wait, with the event loop running, until the edit reached the projection.
            while (dense.layer->hasPendingTimedUpdates()) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
            }
            image->waitForDone();
            latencies.append(clock.nsecsElapsed() - start);
            // Pause between edits without letting the CPU idle: after even a
            // short sleep a laptop CPU runs the next repaint at lower clocks,
            // which would measure its power management rather than the edit.
            QElapsedTimer pause;
            pause.start();
            while (pause.elapsed() < 30) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
            }
        }
        // The repaint work alone: one more edit each, repainted immediately.
        for (int i = 0; i < 60; ++i, ++next) {
            edit(next);
            updates.takeArea();
            QElapsedTimer timer;
            timer.start();
            dense.layer->forceUpdateTimedNode();
            image->waitForDone();
            repaintTimes.append(timer.nsecsElapsed());
            QCoreApplication::processEvents();
            largestUpdate = qMax(largestUpdate, updates.takeArea());
        }
    }
    QByteArray label(name);
    printPercentiles(QByteArray(label + ": per input event").constData(), eventTimes);
    printPercentiles(QByteArray(label + ": projection update interval during a 120 Hz drag").constData(), frameIntervals);
    printPercentiles(QByteArray(label + ": oldest change to projection updated during a 120 Hz drag").constData(), frameWaits);
    printPercentiles(QByteArray(label + ": single edit to projection updated").constData(), latencies);
    printPercentiles(QByteArray(label + ": dirty-region layer repaint").constData(), repaintTimes);
    std::printf("%s: largest projection update after one edit %.1f%% of the layer\n",
                name, 100.0 * largestUpdate / (image->width() * qreal(image->height())));
    std::fflush(stdout);
    dense.settle();
}
}

void KomaLineartBenchmark::denseInkStroke()
{
    DenseLineart dense;
    QVERIFY(dense.create());

    QRectF dirty;
    KomaLineartRecorder recorder;
    QVector<qint64> eventTimes;
    QVector<qint64> frames100;
    QVector<qint64> frames400;
    QImage view(1920, 1080, QImage::Format_ARGB32_Premultiplied);
    const int samples = 640;
    auto point = [](int k) {
        return QPointF(100 + k * 3.5, 800 + 500 * std::sin(k / 70.0)) / denseResolution;
    };
    auto pressure = [](int k) { return 0.25 + 0.75 * std::abs(std::sin(k / 45.0)); };
    QVERIFY(recorder.begin(dense.target(KomaLineartRecorder::EraserMode::UpToIntersection,
                                        [&dirty](const QRectF &rect) { dirty |= rect; }),
                           point(0), pressure(0), false));
    for (int k = 1; k < samples; ++k) {
        dirty = QRectF();
        QElapsedTimer timer;
        timer.start();
        recorder.add(point(k), pressure(k));
        eventTimes.append(timer.nsecsElapsed());
        view.fill(Qt::transparent);
        frames100.append(paintRecorderFrame(recorder, dirty, point(k), 1.0, view));
        frames400.append(paintRecorderFrame(recorder, dirty, point(k), 4.0, view));
    }
    recorder.cancel();
    printPercentiles("Ink stroke over 220 lines (640 samples): per input event", eventTimes);
    printPercentiles("Ink stroke over 220 lines: preview frame at 100%", frames100);
    printPercentiles("Ink stroke over 220 lines: preview frame at 400%", frames400);
}

void KomaLineartBenchmark::denseEraser_data()
{
    QTest::addColumn<int>("mode");
    QTest::newRow("touched-area") << int(KomaLineartRecorder::EraserMode::TouchedArea);
    QTest::newRow("up-to-intersection") << int(KomaLineartRecorder::EraserMode::UpToIntersection);
    QTest::newRow("whole-line") << int(KomaLineartRecorder::EraserMode::WholeLine);
}

void KomaLineartBenchmark::denseEraser()
{
    QFETCH(int, mode);
    DenseLineart dense;
    QVERIFY(dense.create());
    // A 24 px eraser, as in the reference scenario.
    dense.preset->settings()->setPaintOpSize(24);

    QRectF dirty;
    KomaLineartRecorder recorder;
    QVector<qint64> eventTimes;
    QVector<qint64> frames100;
    QVector<qint64> frames400;
    QImage view(1920, 1080, QImage::Format_ARGB32_Premultiplied);
    const int samples = 400;
    auto point = [](int k) {
        return QPointF(150 + k * 5.2, 150 + k * 3.2 + 40 * std::sin(k / 12.0)) / denseResolution;
    };
    QVERIFY(recorder.begin(dense.target(KomaLineartRecorder::EraserMode(mode),
                                        [&dirty](const QRectF &rect) { dirty |= rect; }),
                           point(0), 1.0, true));
    for (int k = 1; k < samples; ++k) {
        dirty = QRectF();
        QElapsedTimer timer;
        timer.start();
        recorder.add(point(k), 1.0);
        eventTimes.append(timer.nsecsElapsed());
        view.fill(Qt::transparent);
        frames100.append(paintRecorderFrame(recorder, dirty, point(k), 1.0, view));
        frames400.append(paintRecorderFrame(recorder, dirty, point(k), 4.0, view));
    }
    QVector<qint64> releaseTimes;
    int changedLines = 0;
    for (int i = 0; i < 5; ++i) {
        QElapsedTimer timer;
        timer.start();
        changedLines = recorder.partialErase().size();
        releaseTimes.append(timer.nsecsElapsed());
    }
    recorder.cancel();
    const QByteArray name = QByteArray("Eraser ") + QTest::currentDataTag() + " over 220 lines (400 samples)";
    printPercentiles(QByteArray(name + ": per input event").constData(), eventTimes);
    printPercentiles(QByteArray(name + ": overlay frame at 100%").constData(), frames100);
    printPercentiles(QByteArray(name + ": overlay frame at 400%").constData(), frames400);
    if (KomaLineartRecorder::EraserMode(mode) != KomaLineartRecorder::EraserMode::WholeLine) {
        std::printf("%s: release geometry for %d changed lines\n", name.constData(), changedLines);
        printPercentiles(QByteArray(name + ": release geometry").constData(), releaseTimes);
    }
}

void KomaLineartBenchmark::denseControlPointDrag()
{
    DenseLineart dense;
    QVERIFY(dense.create());
    KomaLineartShape *line = dense.lines[150];
    const QVector<KomaLineartPoint> points = line->centerlineInDocument();
    const QPointF grab = points[points.size() / 2].position;

    KomaLineartEditor editor;
    QVERIFY(editor.pickPoint(dense.layer.data(), grab, 8.0 / denseResolution));
    QCOMPARE(editor.pickedLine(), line);
    measureLayerEdits("Control point drag over 220 lines", dense, 240, [&](int i) {
        const qreal angle = i / 20.0;
        editor.movePickedPoint(grab + QPointF(60 * std::sin(angle), 40 * (1 - std::cos(angle))) / denseResolution);
        return true;
    });
}

void KomaLineartBenchmark::denseAdjustWidth()
{
    DenseLineart dense;
    QVERIFY(dense.create());
    KomaLineartEditor editor;
    const qreal radius = 20.0 / denseResolution;
    const qreal factor = 1.0 + 0.12 * 30 / 100.0;
    measureLayerEdits("Adjust Line Width over 220 lines", dense, 240, [&](int i) {
        // Sweep back and forth across the dense lines.
        const int phase = i % 480;
        const qreal t = (phase < 240 ? phase : 479 - phase) / 240.0;
        const QPointF point = QPointF(1500 + 400 * t, 300 + 800 * t + 30 * std::sin(i / 7.0)) / denseResolution;
        return editor.adjustWidth(dense.layer.data(), point, radius, factor);
    });
}

void KomaLineartBenchmark::denseLayerRepaint()
{
    DenseLineart dense;
    QVERIFY(dense.create());
    KisImageSP image = dense.parent->image;

    // The whole vector layer regenerated at image resolution (as after
    // loading), first with every line's brush render cache cold, then warm.
    QVector<qint64> cold;
    QVector<qint64> warm;
    for (int i = 0; i < 6; ++i) {
        const bool coldRun = i % 2 == 0;
        if (coldRun) {
            // Drop every line's cached render; the repaint below renders them all.
            for (KomaLineartShape *line : dense.lines) {
                line->setBrush(line->brush(), line->fullPressureWidth());
            }
        }
        QElapsedTimer timer;
        timer.start();
        dense.layer->forceUpdateHiddenAreaOnOriginal();
        image->waitForDone();
        (coldRun ? cold : warm).append(timer.nsecsElapsed());
    }
    printPercentiles("Full vector layer repaint, 220 lines, cold render cache", cold);
    printPercentiles("Full vector layer repaint, 220 lines, warm render cache", warm);

    // The lines painted straight into a 1920x1080 view panning across the
    // image, at 1x and 4x the image resolution (as when a document is
    // rendered at a higher resolution): first pass with cold caches, then
    // warm. The canvas itself shows the layer's projection at any zoom; this
    // is the cost of rendering the lines themselves at a larger scale.
    for (const qreal zoom : {1.0, 4.0}) {
        QImage view(1920, 1080, QImage::Format_ARGB32_Premultiplied);
        QVector<qint64> coldFrames;
        QVector<qint64> warmFrames;
        const QSizeF viewInImage(1920 / zoom, 1080 / zoom);
        for (int pass = 0; pass < 2; ++pass) {
            for (int frame = 0; frame < 24; ++frame) {
                const QPointF origin((2400 - viewInImage.width()) * (frame % 6) / 5.0,
                                     (1600 - viewInImage.height()) * (frame / 6) / 3.0);
                const QRectF visible = QRectF(origin, viewInImage);
                const QRectF visibleDocument(visible.topLeft() / denseResolution, visible.size() / denseResolution);
                QElapsedTimer timer;
                timer.start();
                view.fill(Qt::transparent);
                QPainter painter(&view);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.scale(zoom, zoom);
                painter.translate(-origin);
                painter.scale(denseResolution, denseResolution);
                for (KoShape *shape : dense.layer->shapeManager()->shapesAt(visibleDocument)) {
                    painter.save();
                    painter.setTransform(shape->absoluteTransformation(), true);
                    shape->paint(painter);
                    painter.restore();
                }
                painter.end();
                (pass == 0 ? coldFrames : warmFrames).append(timer.nsecsElapsed());
            }
        }
        const QByteArray label = QByteArray("Lines painted directly at ")
                + QByteArray::number(int(zoom * 100)) + "% of image resolution into a 1920x1080 view";
        printPercentiles(QByteArray(label + ", first pass").constData(), coldFrames);
        printPercentiles(QByteArray(label + ", repeated").constData(), warmFrames);
    }
}

SIMPLE_TEST_MAIN(KomaLineartBenchmark)
