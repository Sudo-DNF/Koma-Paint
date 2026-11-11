/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef QUICKACCESSGRID_H
#define QUICKACCESSGRID_H

#include <QListWidget>
#include <QPointer>

#include <KisResourceModel.h>

#include "QuickAccessStore.h"

class KisViewManager;
class QMenu;

/**
 * Shows the Quick Access set as a grid of icons. Clicking an icon runs the
 * command, switches tool, selects the brush or sets the colour. Items can
 * be dragged to reorder them and removed from the context menu; the "Add"
 * menu adds the current tool, brush or colour, or any command.
 */
class QuickAccessGrid : public QListWidget
{
    Q_OBJECT
public:
    explicit QuickAccessGrid(QWidget *parent = nullptr);

    void setViewManager(KisViewManager *viewManager);

    /// A menu with the "Add ..." entries, owned by @p parent.
    QMenu *createAddMenu(QWidget *parent);

Q_SIGNALS:
    /// Emitted after an item has been used.
    void itemTriggered();

private Q_SLOTS:
    void rebuild();
    void slotItemClicked(QListWidgetItem *item);
    void slotContextMenu(const QPoint &pos);
    void slotRowsMoved();

private:
    QIcon iconFor(const QuickAccessItem &item) const;
    QString labelFor(const QuickAccessItem &item) const;
    KoResourceSP presetFor(const QString &name) const;
    void addCommand();

    QPointer<KisViewManager> m_viewManager;
    KisResourceModel m_presetModel;
    bool m_rebuilding {false};
};

#endif // QUICKACCESSGRID_H
