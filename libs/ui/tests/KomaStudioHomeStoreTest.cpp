/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KomaStudioHomeStoreTest.h"

#include <QSignalSpy>

#include <KConfigGroup>
#include <KSharedConfig>

#include "KomaStudioHomeStore.h"

namespace
{
KSharedConfigPtr openConfig(const QString &path)
{
    return KSharedConfig::openConfig(path, KConfig::SimpleConfig);
}
}

void KomaStudioHomeStoreTest::init()
{
    // Each test starts on its own empty configuration file.
    ++m_round;
}

QString KomaStudioHomeStoreTest::configPath() const
{
    return m_dir.filePath(QStringLiteral("studiohome-%1rc").arg(m_round));
}

void KomaStudioHomeStoreTest::testPinning()
{
    KomaStudioHomeStore store(openConfig(configPath()));
    QVERIFY(store.pinnedFiles().isEmpty());

    store.setPinned(QStringLiteral("/art/a.kra"), true);
    store.setPinned(QStringLiteral("/art/b.kra"), true);
    store.setPinned(QStringLiteral("/art/a.kra"), true);
    QCOMPARE(store.pinnedFiles(), QStringList({QStringLiteral("/art/b.kra"), QStringLiteral("/art/a.kra")}));
    QVERIFY(store.isPinned(QStringLiteral("/art/a.kra")));

    store.setPinned(QStringLiteral("/art/a.kra"), false);
    QCOMPARE(store.pinnedFiles(), QStringList({QStringLiteral("/art/b.kra")}));
    QVERIFY(!store.isPinned(QStringLiteral("/art/a.kra")));

    store.setPinned(QString(), true);
    QCOMPARE(store.pinnedFiles().size(), 1);
}

void KomaStudioHomeStoreTest::testCollections()
{
    KomaStudioHomeStore store(openConfig(configPath()));
    QVERIFY(store.addCollection(QStringLiteral("   ")).isEmpty());

    const QString comic = store.addCollection(QStringLiteral("  Harbour Tales  "));
    const QString sketches = store.addCollection(QStringLiteral("Sketchbook"));
    QVERIFY(!comic.isEmpty());
    QVERIFY(comic != sketches);
    QCOMPARE(store.collection(comic).name, QStringLiteral("Harbour Tales"));
    QCOMPARE(store.collections().size(), 2);

    store.addFile(comic, QStringLiteral("/art/page1.kra"));
    store.addFile(comic, QStringLiteral("/art/page2.kra"));
    store.addFile(comic, QStringLiteral("/art/page1.kra"));
    store.addFile(sketches, QStringLiteral("/art/page1.kra"));
    QCOMPARE(store.collection(comic).files,
             QStringList({QStringLiteral("/art/page2.kra"), QStringLiteral("/art/page1.kra")}));
    QCOMPARE(store.collectionsWithFile(QStringLiteral("/art/page1.kra")), QStringList({comic, sketches}));

    store.removeFile(comic, QStringLiteral("/art/page1.kra"));
    QCOMPARE(store.collectionsWithFile(QStringLiteral("/art/page1.kra")), QStringList({sketches}));

    store.renameCollection(sketches, QStringLiteral("Studies"));
    QCOMPARE(store.collection(sketches).name, QStringLiteral("Studies"));
    store.renameCollection(sketches, QStringLiteral("  "));
    QCOMPARE(store.collection(sketches).name, QStringLiteral("Studies"));

    store.removeCollection(comic);
    QCOMPARE(store.collections().size(), 1);
    QVERIFY(store.collection(comic).id.isEmpty());
    store.addFile(comic, QStringLiteral("/art/page3.kra"));
    QVERIFY(store.collectionsWithFile(QStringLiteral("/art/page3.kra")).isEmpty());
}

void KomaStudioHomeStoreTest::testCollectionColorsCycle()
{
    KomaStudioHomeStore store(openConfig(configPath()));
    const QVector<QColor> colors = KomaStudioHomeStore::collectionColors();
    QVector<QString> ids;
    for (int i = 0; i <= colors.size(); ++i) {
        ids.append(store.addCollection(QStringLiteral("Collection %1").arg(i)));
    }
    for (int i = 0; i <= colors.size(); ++i) {
        QCOMPARE(store.collection(ids[i]).color, colors[i % colors.size()]);
    }
}

void KomaStudioHomeStoreTest::testPersistence()
{
    const QString path = configPath();
    QString id;
    QColor color;
    {
        KomaStudioHomeStore store(openConfig(path));
        store.setPinned(QStringLiteral("/art/cover.kra"), true);
        id = store.addCollection(QStringLiteral("Commissions"));
        store.addFile(id, QStringLiteral("/art/client.kra"));
        color = store.collection(id).color;
    }

    // A store on a re-read configuration finds the same organisation.
    KSharedConfigPtr fresh = openConfig(path);
    fresh->reparseConfiguration();
    KomaStudioHomeStore store(fresh);
    QCOMPARE(store.pinnedFiles(), QStringList({QStringLiteral("/art/cover.kra")}));
    QCOMPARE(store.collections().size(), 1);
    QCOMPARE(store.collection(id).name, QStringLiteral("Commissions"));
    QCOMPARE(store.collection(id).color, color);
    QCOMPARE(store.collection(id).files, QStringList({QStringLiteral("/art/client.kra")}));
}

void KomaStudioHomeStoreTest::testRemovedCollectionLeavesNoGroup()
{
    KSharedConfigPtr config = openConfig(configPath());
    KomaStudioHomeStore store(config);
    const QString keep = store.addCollection(QStringLiteral("Keep"));
    const QString drop = store.addCollection(QStringLiteral("Drop"));
    store.removeCollection(drop);

    const QStringList groups = config->group(QStringLiteral("KomaStudioHome")).groupList();
    QCOMPARE(groups, QStringList({QStringLiteral("Collection %1").arg(keep)}));
}

void KomaStudioHomeStoreTest::testAnimationWorkspacePreference()
{
    const QString path = configPath();
    {
        KomaStudioHomeStore store(openConfig(path));
        QVERIFY(store.switchToAnimationWorkspace());
        store.setSwitchToAnimationWorkspace(false);
    }
    KSharedConfigPtr fresh = openConfig(path);
    fresh->reparseConfiguration();
    KomaStudioHomeStore store(fresh);
    QVERIFY(!store.switchToAnimationWorkspace());
}

void KomaStudioHomeStoreTest::testChangedSignal()
{
    KomaStudioHomeStore store(openConfig(configPath()));
    QSignalSpy spy(&store, &KomaStudioHomeStore::changed);

    store.setPinned(QStringLiteral("/art/a.kra"), true);
    QCOMPARE(spy.count(), 1);
    store.setPinned(QStringLiteral("/art/a.kra"), true);
    QCOMPARE(spy.count(), 1);

    const QString id = store.addCollection(QStringLiteral("Inks"));
    QCOMPARE(spy.count(), 2);
    store.renameCollection(id, QStringLiteral("Inks"));
    QCOMPARE(spy.count(), 2);
    store.removeFile(id, QStringLiteral("/art/missing.kra"));
    QCOMPARE(spy.count(), 2);
}

SIMPLE_TEST_MAIN(KomaStudioHomeStoreTest)
