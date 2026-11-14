/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KOMASTUDIOHOMESTORETEST_H
#define KOMASTUDIOHOMESTORETEST_H

#include <QTemporaryDir>

#include <simpletest.h>

class KomaStudioHomeStoreTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void init();
    void testPinning();
    void testCollections();
    void testCollectionColorsCycle();
    void testPersistence();
    void testRemovedCollectionLeavesNoGroup();
    void testAnimationWorkspacePreference();
    void testChangedSignal();

private:
    QString configPath() const;

    QTemporaryDir m_dir;
    int m_round {0};
};

#endif // KOMASTUDIOHOMESTORETEST_H
