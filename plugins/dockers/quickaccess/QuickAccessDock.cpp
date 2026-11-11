/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "QuickAccessDock.h"

#include <QCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QToolButton>
#include <QVBoxLayout>

#include <klocalizedstring.h>

#include <KisViewManager.h>
#include <KomaPaintTokens.h>
#include <kis_action.h>
#include <kis_action_manager.h>
#include <kis_icon_utils.h>

#include "QuickAccessGrid.h"
#include "QuickAccessStore.h"

namespace
{
constexpr int popupColumns = 6;
constexpr int popupMaxRows = 6;
constexpr int cell = 40;
}

QuickAccessPopup::QuickAccessPopup(QWidget *parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint)
{
    setAttribute(Qt::WA_TranslucentBackground);

    QVBoxLayout *layout = new QVBoxLayout(this);
    const int margin = KomaPaint::Space::s2;
    layout->setContentsMargins(margin, margin, margin, margin);

    QLabel *title = new QLabel(i18nc("@title:window", "Quick Access"), this);
    QFont titleFont = title->font();
    titleFont.setWeight(QFont::DemiBold);
    title->setFont(titleFont);
    layout->addWidget(title);

    m_grid = new QuickAccessGrid(this);
    m_grid->viewport()->setAutoFillBackground(false);
    layout->addWidget(m_grid);

    connect(m_grid, &QuickAccessGrid::itemTriggered, this, &QWidget::hide);
}

void QuickAccessPopup::setViewManager(KisViewManager *viewManager)
{
    m_grid->setViewManager(viewManager);
}

void QuickAccessPopup::showAtCursor()
{
    const int count = qMax(1, QuickAccessStore::instance()->items().size());
    const int rows = qBound(1, (count + popupColumns - 1) / popupColumns, popupMaxRows);
    m_grid->setFixedSize(popupColumns * cell + 4, rows * cell + 4);
    adjustSize();

    // Centre on the cursor, kept inside the screen.
    QPoint pos = QCursor::pos() - QPoint(width() / 2, height() / 2);
    if (QScreen *screen = QGuiApplication::screenAt(QCursor::pos())) {
        const QRect area = screen->availableGeometry();
        pos.setX(qBound(area.left(), pos.x(), area.right() - width()));
        pos.setY(qBound(area.top(), pos.y(), area.bottom() - height()));
    }
    move(pos);
    show();
    raise();
    activateWindow();
}

void QuickAccessPopup::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    const KomaPaint::Theme t = KomaPaint::themeFor(palette());
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath card;
    card.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), KomaPaint::Radius::md, KomaPaint::Radius::md);
    painter.fillPath(card, t.surface);
    painter.setPen(QPen(t.border, 1.0));
    painter.drawPath(card);
}

QuickAccessDock::QuickAccessDock()
    : QDockWidget(i18nc("@title:window docker", "Quick Access"))
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(KomaPaint::Space::s1, KomaPaint::Space::s1, KomaPaint::Space::s1, KomaPaint::Space::s1);
    layout->setSpacing(KomaPaint::Space::s1);

    m_grid = new QuickAccessGrid(page);
    layout->addWidget(m_grid, 1);

    QHBoxLayout *bar = new QHBoxLayout();
    bar->addStretch(1);
    QToolButton *addButton = new QToolButton(page);
    addButton->setAutoRaise(true);
    addButton->setIcon(KisIconUtils::loadIcon(QStringLiteral("list-add")));
    addButton->setToolTip(i18nc("@info:tooltip", "Add to Quick Access"));
    addButton->setPopupMode(QToolButton::InstantPopup);
    addButton->setMenu(m_grid->createAddMenu(addButton));
    bar->addWidget(addButton);
    layout->addLayout(bar);

    setWidget(page);
    setEnabled(false);
}

void QuickAccessDock::setCanvas(KoCanvasBase *canvas)
{
    if (m_canvas) {
        m_canvas->disconnectCanvasObserver(this);
    }
    m_canvas = dynamic_cast<KisCanvas2*>(canvas);
    setEnabled(m_canvas);
    if (!m_canvas || !m_canvas->viewManager()) {
        return;
    }

    KisViewManager *viewManager = m_canvas->viewManager();
    if (viewManager != m_viewManager) {
        m_viewManager = viewManager;
        m_grid->setViewManager(viewManager);
        if (m_popup) {
            m_popup->setViewManager(viewManager);
        }

        KisActionManager *actions = viewManager->actionManager();
        KisAction *popupAction = actions->actionByName(QStringLiteral("quick_access_popup"));
        if (!popupAction) {
            popupAction = actions->createAction(QStringLiteral("quick_access_popup"));
        }
        connect(popupAction, &QAction::triggered, this, &QuickAccessDock::slotShowPopup, Qt::UniqueConnection);
    }
}

void QuickAccessDock::unsetCanvas()
{
    if (m_canvas) {
        m_canvas->disconnectCanvasObserver(this);
    }
    m_canvas = nullptr;
    setEnabled(false);
}

void QuickAccessDock::slotShowPopup()
{
    if (!m_viewManager) {
        return;
    }
    if (!m_popup) {
        m_popup = new QuickAccessPopup(m_viewManager->mainWindowAsQWidget());
        m_popup->setViewManager(m_viewManager);
    }
    m_popup->showAtCursor();
}
