/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef QUICKACCESSSTORE_H
#define QUICKACCESSSTORE_H

#include <QObject>
#include <QString>
#include <QVector>

/**
 * One Quick Access entry.
 */
struct QuickAccessItem {
    enum class Type {
        Action, ///< A menu command, by action name.
        Tool,   ///< A tool, by tool id.
        Preset, ///< A brush preset, by preset name.
        Color,  ///< A colour, as #rrggbb.
    };

    Type type {Type::Action};
    QString value;

    bool operator==(const QuickAccessItem &other) const
    {
        return type == other.type && value == other.value;
    }

    /// "action:edit_undo", "tool:KisToolSelectRectangular", ...
    QString serialize() const;
    static bool deserialize(const QString &text, QuickAccessItem *item);
};

/**
 * The user's Quick Access set, shared by every window's Quick Access
 * docker and popup and kept in the configuration file.
 */
class QuickAccessStore : public QObject
{
    Q_OBJECT
public:
    static QuickAccessStore *instance();

    QuickAccessStore();

    const QVector<QuickAccessItem> &items() const;

    /// Adds @p item at the end, unless it is already in the set.
    void add(const QuickAccessItem &item);
    void remove(int index);
    void setItems(const QVector<QuickAccessItem> &items);

Q_SIGNALS:
    void itemsChanged();

private:
    void save() const;

    QVector<QuickAccessItem> m_items;
};

#endif // QUICKACCESSSTORE_H
