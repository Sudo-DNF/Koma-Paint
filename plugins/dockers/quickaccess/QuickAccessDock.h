/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef QUICKACCESSDOCK_H
#define QUICKACCESSDOCK_H

#include <QDockWidget>
#include <QFrame>
#include <QPointer>

#include <KoCanvasObserverBase.h>
#include <kis_canvas2.h>

class KisViewManager;
class QuickAccessGrid;

/**
 * Frameless popup that shows the Quick Access grid at the cursor. It closes
 * after an item is used or when the user clicks elsewhere.
 */
class QuickAccessPopup : public QFrame
{
    Q_OBJECT
public:
    explicit QuickAccessPopup(QWidget *parent = nullptr);

    void setViewManager(KisViewManager *viewManager);
    void showAtCursor();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QuickAccessGrid *m_grid {nullptr};
};

/**
 * The Quick Access docker: the user's favourite commands, tools, brushes
 * and colours in one grid. It also provides the "Quick Access" action
 * (Shift+Q) that opens the same grid as a popup at the cursor.
 */
class QuickAccessDock : public QDockWidget, public KoCanvasObserverBase
{
    Q_OBJECT
public:
    QuickAccessDock();

    QString observerName() override { return QStringLiteral("QuickAccessDock"); }
    void setCanvas(KoCanvasBase *canvas) override;
    void unsetCanvas() override;

private Q_SLOTS:
    void slotShowPopup();

private:
    QPointer<KisCanvas2> m_canvas;
    QPointer<KisViewManager> m_viewManager;
    QuickAccessGrid *m_grid {nullptr};
    QPointer<QuickAccessPopup> m_popup;
};

#endif // QUICKACCESSDOCK_H
