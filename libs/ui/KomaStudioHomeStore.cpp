/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KomaStudioHomeStore.h"

#include <QGlobalStatic>
#include <QUuid>

#include <KConfigGroup>

namespace
{
const char groupName[] = "KomaStudioHome";
const char pinnedKey[] = "Pinned";
const char collectionIdsKey[] = "CollectionIds";
const char animationWorkspaceKey[] = "SwitchToAnimationWorkspace";

QString collectionGroupName(const QString &id)
{
    return QStringLiteral("Collection %1").arg(id);
}
}

Q_GLOBAL_STATIC_WITH_ARGS(KomaStudioHomeStore, s_instance, (KSharedConfig::openConfig()))

KomaStudioHomeStore::KomaStudioHomeStore(KSharedConfigPtr config, QObject *parent)
    : QObject(parent)
    , m_config(config)
{
    load();
}

KomaStudioHomeStore::~KomaStudioHomeStore()
{
}

KomaStudioHomeStore *KomaStudioHomeStore::instance()
{
    return s_instance;
}

QVector<QColor> KomaStudioHomeStore::collectionColors()
{
    // Readable on both the dark and the light Koma Paint theme, and told
    // apart by lightness as well as hue.
    return {QColor(0xFF6BA3), QColor(0x2FD4A0), QColor(0xFFB020),
            QColor(0x6CA8FF), QColor(0xB58CFF), QColor(0xFF8A5C)};
}

QStringList KomaStudioHomeStore::pinnedFiles() const
{
    return m_pinned;
}

bool KomaStudioHomeStore::isPinned(const QString &path) const
{
    return m_pinned.contains(path);
}

void KomaStudioHomeStore::setPinned(const QString &path, bool pinned)
{
    if (path.isEmpty() || isPinned(path) == pinned) {
        return;
    }
    if (pinned) {
        m_pinned.prepend(path);
    } else {
        m_pinned.removeAll(path);
    }
    save();
}

QVector<KomaStudioHomeStore::Collection> KomaStudioHomeStore::collections() const
{
    return m_collections;
}

KomaStudioHomeStore::Collection KomaStudioHomeStore::collection(const QString &id) const
{
    const int index = indexOf(id);
    return index >= 0 ? m_collections[index] : Collection();
}

QStringList KomaStudioHomeStore::collectionsWithFile(const QString &path) const
{
    QStringList ids;
    for (const Collection &collection : m_collections) {
        if (collection.files.contains(path)) {
            ids.append(collection.id);
        }
    }
    return ids;
}

QString KomaStudioHomeStore::addCollection(const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }
    const QVector<QColor> colors = collectionColors();
    Collection collection;
    collection.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    collection.name = trimmed;
    collection.color = colors[m_collections.size() % colors.size()];
    m_collections.append(collection);
    save();
    return collection.id;
}

void KomaStudioHomeStore::renameCollection(const QString &id, const QString &name)
{
    const int index = indexOf(id);
    const QString trimmed = name.trimmed();
    if (index < 0 || trimmed.isEmpty() || m_collections[index].name == trimmed) {
        return;
    }
    m_collections[index].name = trimmed;
    save();
}

void KomaStudioHomeStore::removeCollection(const QString &id)
{
    const int index = indexOf(id);
    if (index < 0) {
        return;
    }
    m_collections.remove(index);
    save();
}

void KomaStudioHomeStore::addFile(const QString &id, const QString &path)
{
    const int index = indexOf(id);
    if (index < 0 || path.isEmpty() || m_collections[index].files.contains(path)) {
        return;
    }
    m_collections[index].files.prepend(path);
    save();
}

void KomaStudioHomeStore::removeFile(const QString &id, const QString &path)
{
    const int index = indexOf(id);
    if (index < 0 || !m_collections[index].files.removeAll(path)) {
        return;
    }
    save();
}

bool KomaStudioHomeStore::switchToAnimationWorkspace() const
{
    return m_switchToAnimationWorkspace;
}

void KomaStudioHomeStore::setSwitchToAnimationWorkspace(bool value)
{
    if (m_switchToAnimationWorkspace == value) {
        return;
    }
    m_switchToAnimationWorkspace = value;
    save();
}

int KomaStudioHomeStore::indexOf(const QString &id) const
{
    for (int i = 0; i < m_collections.size(); ++i) {
        if (m_collections[i].id == id) {
            return i;
        }
    }
    return -1;
}

void KomaStudioHomeStore::load()
{
    const KConfigGroup group = m_config->group(groupName);
    m_pinned = group.readEntry(pinnedKey, QStringList());
    m_switchToAnimationWorkspace = group.readEntry(animationWorkspaceKey, true);

    m_collections.clear();
    const QStringList ids = group.readEntry(collectionIdsKey, QStringList());
    for (const QString &id : ids) {
        const KConfigGroup entry = group.group(collectionGroupName(id));
        Collection collection;
        collection.id = id;
        collection.name = entry.readEntry("Name", QString());
        collection.color = entry.readEntry("Color", QColor());
        collection.files = entry.readEntry("Files", QStringList());
        if (collection.name.isEmpty()) {
            continue;
        }
        if (!collection.color.isValid()) {
            const QVector<QColor> colors = collectionColors();
            collection.color = colors[m_collections.size() % colors.size()];
        }
        m_collections.append(collection);
    }
}

void KomaStudioHomeStore::save()
{
    KConfigGroup group = m_config->group(groupName);
    // Collections that were removed leave no group behind.
    for (const QString &existing : group.groupList()) {
        group.deleteGroup(existing);
    }
    group.writeEntry(pinnedKey, m_pinned);
    group.writeEntry(animationWorkspaceKey, m_switchToAnimationWorkspace);

    QStringList ids;
    for (const Collection &collection : std::as_const(m_collections)) {
        ids.append(collection.id);
        KConfigGroup entry = group.group(collectionGroupName(collection.id));
        entry.writeEntry("Name", collection.name);
        entry.writeEntry("Color", collection.color);
        entry.writeEntry("Files", collection.files);
    }
    group.writeEntry(collectionIdsKey, ids);
    m_config->sync();
    Q_EMIT changed();
}
