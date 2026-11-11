/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "QuickAccessStore.h"

#include <QGlobalStatic>
#include <QStringList>

#include <kis_config.h>

Q_GLOBAL_STATIC(QuickAccessStore, s_store)

namespace
{
const QString itemsKey = QStringLiteral("QuickAccess/Items");

const struct {
    QuickAccessItem::Type type;
    const char *prefix;
} prefixes[] = {
    {QuickAccessItem::Type::Action, "action:"},
    {QuickAccessItem::Type::Tool, "tool:"},
    {QuickAccessItem::Type::Preset, "preset:"},
    {QuickAccessItem::Type::Color, "color:"},
};

/// A useful starting set: undo, redo, the main tools and canvas commands.
QStringList defaultItems()
{
    return {
        QStringLiteral("action:edit_undo"),
        QStringLiteral("action:edit_redo"),
        QStringLiteral("tool:KritaShape/KisToolBrush"),
        QStringLiteral("tool:KisToolSelectRectangular"),
        QStringLiteral("tool:KritaFill/KisToolFill"),
        QStringLiteral("tool:KisToolTransform"),
        QStringLiteral("action:clear"),
        QStringLiteral("action:mirror_canvas"),
        QStringLiteral("action:zoom_to_100pct"),
        QStringLiteral("action:zoom_to_fit"),
    };
}
}

QString QuickAccessItem::serialize() const
{
    for (const auto &p : prefixes) {
        if (p.type == type) {
            return QLatin1String(p.prefix) + value;
        }
    }
    return QString();
}

bool QuickAccessItem::deserialize(const QString &text, QuickAccessItem *item)
{
    for (const auto &p : prefixes) {
        const QLatin1String prefix(p.prefix);
        if (text.startsWith(prefix) && text.size() > prefix.size()) {
            item->type = p.type;
            item->value = text.mid(prefix.size());
            return true;
        }
    }
    return false;
}

QuickAccessStore *QuickAccessStore::instance()
{
    return s_store;
}

QuickAccessStore::QuickAccessStore()
{
    KisConfig cfg(true);
    const QStringList stored = cfg.readEntry<QStringList>(itemsKey, defaultItems());
    for (const QString &text : stored) {
        QuickAccessItem item;
        if (QuickAccessItem::deserialize(text, &item)) {
            m_items.append(item);
        }
    }
}

const QVector<QuickAccessItem> &QuickAccessStore::items() const
{
    return m_items;
}

void QuickAccessStore::add(const QuickAccessItem &item)
{
    if (item.value.isEmpty() || m_items.contains(item)) {
        return;
    }
    m_items.append(item);
    save();
    Q_EMIT itemsChanged();
}

void QuickAccessStore::remove(int index)
{
    if (index < 0 || index >= m_items.size()) {
        return;
    }
    m_items.remove(index);
    save();
    Q_EMIT itemsChanged();
}

void QuickAccessStore::setItems(const QVector<QuickAccessItem> &items)
{
    if (items == m_items) {
        return;
    }
    m_items = items;
    save();
    Q_EMIT itemsChanged();
}

void QuickAccessStore::save() const
{
    QStringList stored;
    for (const QuickAccessItem &item : m_items) {
        stored << item.serialize();
    }
    KisConfig(false).writeEntry(itemsKey, stored);
}
