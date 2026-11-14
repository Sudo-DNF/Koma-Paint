/*
 *  SPDX-FileCopyrightText: 2010 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_doc2_test.h"

#include <KisMainWindow.h>

#include <simpletest.h>

#include "KisDocument.h"
#include "KisPart.h"
#include "kis_image.h"
#include "kis_undo_store.h"
#include "util.h"
#include <KisView.h>
#include <KisViewManager.h>
#include <kis_config.h>
#include <testui.h>
#include "KritaTransformMaskStubs.h"
#include <QImage>
#include <QTemporaryDir>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <kis_layer_utils.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>


void silenceReignsSupreme(QtMsgType /*type*/, const QMessageLogContext &/*context*/, const QString &/*msg*/)
{
}

void KisDocumentTest::initTestCase()
{
    TestUtil::registerTransformMaskStubs();
    qInstallMessageHandler(silenceReignsSupreme);
}

void KisDocumentTest::testOpenImageTwiceInSameDoc()
{
    QString fname2 = QString(FILES_DATA_DIR) + '/' + "load_test.kra";
    QString fname = QString(FILES_DATA_DIR) + '/' + "load_test2.kra";


    Q_ASSERT(!fname.isEmpty());
    Q_ASSERT(!fname2.isEmpty());

    QScopedPointer<KisDocument> doc(KisPart::instance()->createDocument());

    doc->loadNativeFormat(fname);
    doc->loadNativeFormat(fname2);
}

void KisDocumentTest::testSyncExportLeavesOutDraftLayers()
{
    const QRect bounds(0, 0, 64, 64);
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();

    QScopedPointer<KisDocument> doc(KisPart::instance()->createDocument());
    doc->setFileBatchMode(true);
    KisImageSP image = new KisImage(doc->createUndoStore(), bounds.width(), bounds.height(), cs, "draft test");
    doc->setCurrentImage(image);

    KisPaintLayerSP base = new KisPaintLayer(image, "base", OPACITY_OPAQUE_U8);
    base->paintDevice()->fill(bounds, KoColor(Qt::red, cs));
    image->addNode(base, image->root());

    KisPaintLayerSP draft = new KisPaintLayer(image, "draft", OPACITY_OPAQUE_U8);
    draft->paintDevice()->fill(QRect(0, 0, 32, 64), KoColor(Qt::blue, cs));
    draft->setDraft(true);
    image->addNode(draft, image->root());

    image->initialRefreshGraph();
    image->waitForDone();
    QCOMPARE(image->projection()->pixel(QPoint(8, 8)).toQColor(), QColor(Qt::blue));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // A PNG export leaves the draft layer out...
    const QString pngPath = dir.filePath("draft.png");
    QVERIFY(doc->exportDocumentSync(pngPath, "image/png"));
    const QImage png(pngPath);
    QVERIFY(!png.isNull());
    QCOMPARE(png.pixelColor(8, 8), QColor(Qt::red));
    QCOMPARE(png.pixelColor(48, 8), QColor(Qt::red));

    // ...without touching the open document.
    QVERIFY(draft->visible());
    QVERIFY(draft->isDraft());
    QCOMPARE(image->projection()->pixel(QPoint(8, 8)).toQColor(), QColor(Qt::blue));

    // A .kra keeps it, visible and marked as a draft.
    const QString kraPath = dir.filePath("draft.kra");
    QVERIFY(doc->exportDocumentSync(kraPath, KisDocument::nativeFormatMimeType()));

    QScopedPointer<KisDocument> reloaded(KisPart::instance()->createDocument());
    reloaded->setFileBatchMode(true);
    QVERIFY(reloaded->openPath(kraPath));
    reloaded->image()->waitForDone();

    KisLayerSP reloadedDraft = qobject_cast<KisLayer*>(
        KisLayerUtils::findNodeByName(reloaded->image()->root(), "draft").data());
    QVERIFY(reloadedDraft);
    QVERIFY(reloadedDraft->isDraft());
    QVERIFY(reloadedDraft->visible());
    QCOMPARE(reloaded->image()->projection()->pixel(QPoint(8, 8)).toQColor(), QColor(Qt::blue));
}


KISTEST_MAIN(KisDocumentTest)

