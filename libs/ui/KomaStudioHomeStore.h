/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KOMASTUDIOHOMESTORE_H
#define KOMASTUDIOHOMESTORE_H

#include <QColor>
#include <QObject>
#include <QStringList>
#include <QVector>

#include <KSharedConfig>

#include "kritaui_export.h"

/**
 * How the artist has organised their files on the Studio Home page: pinned
 * files and named, coloured collections of files. Both refer to files by
 * local path and are kept in the application configuration, so they are
 * shared by every main window and survive restarts.
 *
 * Every change is written to the configuration at once and announced with
 * changed().
 */
class KRITAUI_EXPORT KomaStudioHomeStore : public QObject
{
    Q_OBJECT
public:
    struct Collection {
        QString id;
        QString name;
        QColor color;
        QStringList files;
    };

    /// Reads and writes the "KomaStudioHome" group of @p config.
    explicit KomaStudioHomeStore(KSharedConfigPtr config, QObject *parent = nullptr);
    ~KomaStudioHomeStore() override;

    /// The store for the application configuration, shared by all windows.
    static KomaStudioHomeStore *instance();

    QStringList pinnedFiles() const;
    bool isPinned(const QString &path) const;
    void setPinned(const QString &path, bool pinned);

    QVector<Collection> collections() const;
    /// The collection with @p id, or one with an empty id if there is none.
    Collection collection(const QString &id) const;
    /// Ids of the collections that hold @p path, in collection order.
    QStringList collectionsWithFile(const QString &path) const;

    /// Adds an empty collection and returns its id.
    QString addCollection(const QString &name);
    void renameCollection(const QString &id, const QString &name);
    void removeCollection(const QString &id);
    void addFile(const QString &id, const QString &path);
    void removeFile(const QString &id, const QString &path);

    /// Whether a new animation canvas opens the Animation workspace.
    bool switchToAnimationWorkspace() const;
    void setSwitchToAnimationWorkspace(bool value);

    /// The colours given to new collections, in turn.
    static QVector<QColor> collectionColors();

Q_SIGNALS:
    void changed();

private:
    int indexOf(const QString &id) const;
    void load();
    void save();

    KSharedConfigPtr m_config;
    QStringList m_pinned;
    QVector<Collection> m_collections;
    bool m_switchToAnimationWorkspace {true};
};

#endif // KOMASTUDIOHOMESTORE_H
