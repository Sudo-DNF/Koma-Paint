/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "QuickAccessGrid.h"

#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QVBoxLayout>

#include <klocalizedstring.h>

#include <KisResourceTypes.h>
#include <KisViewManager.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoToolManager.h>
#include <KomaPaintTokens.h>
#include <brushengine/kis_paintop_preset.h>
#include <kactioncollection.h>
#include <kis_canvas_resource_provider.h>
#include <kis_icon_utils.h>
#include <kis_paintop_box.h>

namespace
{
constexpr int ItemIndexRole = Qt::UserRole + 1;
constexpr int cellSize = 40;
constexpr int itemIconSize = 24;
}

QuickAccessGrid::QuickAccessGrid(QWidget *parent)
    : QListWidget(parent)
    , m_presetModel(ResourceType::PaintOpPresets)
{
    setViewMode(QListView::IconMode);
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Snap);
    setDragDropMode(QAbstractItemView::InternalMove);
    setDefaultDropAction(Qt::MoveAction);
    setGridSize(QSize(cellSize, cellSize));
    setIconSize(QSize(itemIconSize, itemIconSize));
    setUniformItemSizes(true);
    setWrapping(true);
    setFrameShape(QFrame::NoFrame);
    setSelectionMode(QAbstractItemView::NoSelection);
    setMouseTracking(true);
    setContextMenuPolicy(Qt::CustomContextMenu);

    connect(this, &QListWidget::itemClicked, this, &QuickAccessGrid::slotItemClicked);
    connect(this, &QWidget::customContextMenuRequested, this, &QuickAccessGrid::slotContextMenu);
    connect(model(), &QAbstractItemModel::rowsMoved, this, &QuickAccessGrid::slotRowsMoved);
    connect(QuickAccessStore::instance(), &QuickAccessStore::itemsChanged, this, &QuickAccessGrid::rebuild);

    rebuild();
}

void QuickAccessGrid::setViewManager(KisViewManager *viewManager)
{
    m_viewManager = viewManager;
    rebuild();
}

KoResourceSP QuickAccessGrid::presetFor(const QString &name) const
{
    const QVector<KoResourceSP> found = m_presetModel.resourcesForName(name);
    return found.isEmpty() ? KoResourceSP() : found.first();
}

QString QuickAccessGrid::labelFor(const QuickAccessItem &item) const
{
    switch (item.type) {
    case QuickAccessItem::Type::Action:
        if (m_viewManager) {
            if (QAction *action = m_viewManager->actionCollection()->action(item.value)) {
                return action->text().remove(QLatin1Char('&'));
            }
        }
        break;
    case QuickAccessItem::Type::Tool:
        if (m_viewManager && m_viewManager->toolManager()) {
            for (KoToolAction *action : m_viewManager->toolManager()->toolActionList()) {
                if (action->id() == item.value) {
                    return action->iconText().isEmpty() ? action->toolTip() : action->iconText();
                }
            }
        }
        break;
    case QuickAccessItem::Type::Preset:
        return item.value;
    case QuickAccessItem::Type::Color:
        return item.value.toUpper();
    }
    return item.value;
}

QIcon QuickAccessGrid::iconFor(const QuickAccessItem &item) const
{
    switch (item.type) {
    case QuickAccessItem::Type::Action:
        if (m_viewManager) {
            if (QAction *action = m_viewManager->actionCollection()->action(item.value)) {
                if (!action->icon().isNull()) {
                    return action->icon();
                }
            }
        }
        return KisIconUtils::loadIcon(QStringLiteral("system-run"));
    case QuickAccessItem::Type::Tool:
        if (m_viewManager && m_viewManager->toolManager()) {
            for (KoToolAction *action : m_viewManager->toolManager()->toolActionList()) {
                if (action->id() == item.value) {
                    return KisIconUtils::loadIcon(action->iconName());
                }
            }
        }
        break;
    case QuickAccessItem::Type::Preset:
        if (KoResourceSP preset = presetFor(item.value)) {
            return QIcon(QPixmap::fromImage(preset->image()));
        }
        break;
    case QuickAccessItem::Type::Color: {
        const qreal dpr = devicePixelRatioF();
        QPixmap swatch(QSize(itemIconSize, itemIconSize) * dpr);
        swatch.setDevicePixelRatio(dpr);
        swatch.fill(Qt::transparent);
        QPainter painter(&swatch);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(KomaPaint::themeFor(palette()).border, 1.0));
        painter.setBrush(QColor(item.value));
        painter.drawRoundedRect(QRectF(1.5, 1.5, itemIconSize - 3, itemIconSize - 3),
                                KomaPaint::Radius::xs, KomaPaint::Radius::xs);
        return QIcon(swatch);
    }
    }
    return QIcon();
}

void QuickAccessGrid::rebuild()
{
    m_rebuilding = true;
    clear();
    const QVector<QuickAccessItem> &items = QuickAccessStore::instance()->items();
    for (int i = 0; i < items.size(); ++i) {
        QListWidgetItem *entry = new QListWidgetItem(iconFor(items[i]), QString(), this);
        entry->setToolTip(labelFor(items[i]));
        entry->setData(ItemIndexRole, i);
        entry->setSizeHint(QSize(cellSize, cellSize));
        entry->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDragEnabled);
    }
    m_rebuilding = false;
}

void QuickAccessGrid::slotItemClicked(QListWidgetItem *entry)
{
    if (!entry || !m_viewManager) {
        return;
    }
    const QVector<QuickAccessItem> &items = QuickAccessStore::instance()->items();
    const int index = entry->data(ItemIndexRole).toInt();
    if (index < 0 || index >= items.size()) {
        return;
    }
    const QuickAccessItem item = items[index];

    switch (item.type) {
    case QuickAccessItem::Type::Action:
        if (QAction *action = m_viewManager->actionCollection()->action(item.value)) {
            if (action->isEnabled()) {
                action->trigger();
            }
        }
        break;
    case QuickAccessItem::Type::Tool:
        if (m_viewManager->toolManager()) {
            m_viewManager->toolManager()->switchToolRequested(item.value);
        }
        break;
    case QuickAccessItem::Type::Preset:
        if (KoResourceSP preset = presetFor(item.value)) {
            m_viewManager->paintOpBox()->resourceSelected(preset);
        }
        break;
    case QuickAccessItem::Type::Color:
        m_viewManager->canvasResourceProvider()->setFGColor(
                    KoColor(QColor(item.value), KoColorSpaceRegistry::instance()->rgb8()));
        break;
    }
    Q_EMIT itemTriggered();
}

void QuickAccessGrid::slotContextMenu(const QPoint &pos)
{
    QMenu menu(this);
    QListWidgetItem *entry = itemAt(pos);
    if (entry) {
        const int index = entry->data(ItemIndexRole).toInt();
        menu.addAction(KisIconUtils::loadIcon(QStringLiteral("edit-delete")),
                       i18nc("@action:inmenu", "Remove from Quick Access"),
                       this, [index]() { QuickAccessStore::instance()->remove(index); });
        menu.addSeparator();
    }
    QMenu *addMenu = createAddMenu(&menu);
    menu.addActions(addMenu->actions());
    menu.exec(viewport()->mapToGlobal(pos));
}

void QuickAccessGrid::slotRowsMoved()
{
    if (m_rebuilding) {
        return;
    }
    const QVector<QuickAccessItem> &items = QuickAccessStore::instance()->items();
    QVector<QuickAccessItem> reordered;
    for (int row = 0; row < count(); ++row) {
        const int index = item(row)->data(ItemIndexRole).toInt();
        if (index >= 0 && index < items.size()) {
            reordered.append(items[index]);
        }
    }
    if (reordered.size() == items.size()) {
        QuickAccessStore::instance()->setItems(reordered);
    }
}

QMenu *QuickAccessGrid::createAddMenu(QWidget *parent)
{
    QMenu *menu = new QMenu(parent);

    menu->addAction(i18nc("@action:inmenu", "Add Current Tool"), this, [this]() {
        if (m_viewManager && m_viewManager->toolManager()) {
            QuickAccessStore::instance()->add({QuickAccessItem::Type::Tool, m_viewManager->toolManager()->activeToolId()});
        }
    });
    menu->addAction(i18nc("@action:inmenu", "Add Current Brush"), this, [this]() {
        if (m_viewManager) {
            if (KisPaintOpPresetSP preset = m_viewManager->canvasResourceProvider()->currentPreset()) {
                QuickAccessStore::instance()->add({QuickAccessItem::Type::Preset, preset->name()});
            }
        }
    });
    menu->addAction(i18nc("@action:inmenu", "Add Current Color"), this, [this]() {
        if (m_viewManager) {
            const QColor color = m_viewManager->canvasResourceProvider()->fgColor().toQColor();
            QuickAccessStore::instance()->add({QuickAccessItem::Type::Color, color.name()});
        }
    });
    menu->addAction(i18nc("@action:inmenu", "Add Command..."), this, &QuickAccessGrid::addCommand);
    return menu;
}

void QuickAccessGrid::addCommand()
{
    if (!m_viewManager) {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(i18nc("@title:window", "Add Command to Quick Access"));
    QVBoxLayout *layout = new QVBoxLayout(&dialog);

    QLineEdit *search = new QLineEdit(&dialog);
    search->setPlaceholderText(i18nc("@info:placeholder", "Search commands"));
    search->setClearButtonEnabled(true);
    layout->addWidget(search);

    QListWidget *list = new QListWidget(&dialog);
    list->setIconSize(QSize(16, 16));
    layout->addWidget(list, 1);

    QList<QAction*> actions = m_viewManager->actionCollection()->actions();
    std::sort(actions.begin(), actions.end(), [](QAction *a, QAction *b) {
        return a->text().remove(QLatin1Char('&')).localeAwareCompare(b->text().remove(QLatin1Char('&'))) < 0;
    });
    for (QAction *action : actions) {
        const QString text = action->text().remove(QLatin1Char('&'));
        if (text.isEmpty() || action->objectName().isEmpty() || action->objectName() == QLatin1String("quick_access_popup")) {
            continue;
        }
        QListWidgetItem *entry = new QListWidgetItem(action->icon(), text, list);
        entry->setData(Qt::UserRole, action->objectName());
        entry->setToolTip(action->toolTip());
    }

    connect(search, &QLineEdit::textChanged, list, [list](const QString &text) {
        for (int row = 0; row < list->count(); ++row) {
            QListWidgetItem *entry = list->item(row);
            entry->setHidden(!text.isEmpty() && !entry->text().contains(text, Qt::CaseInsensitive));
        }
    });

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);

    dialog.resize(360, 480);
    if (dialog.exec() == QDialog::Accepted && list->currentItem()) {
        QuickAccessStore::instance()->add({QuickAccessItem::Type::Action, list->currentItem()->data(Qt::UserRole).toString()});
    }
}
