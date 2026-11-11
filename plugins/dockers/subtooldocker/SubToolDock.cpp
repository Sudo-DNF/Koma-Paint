/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "SubToolDock.h"

#include <QLabel>
#include <QListView>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTabBar>
#include <QVBoxLayout>

#include <klocalizedstring.h>

#include <KisResourceTypes.h>
#include <KisTagFilterResourceProxyModel.h>
#include <KisTagModel.h>
#include <KoCanvasResourceProvider.h>
#include <KoResourceServer.h>
#include <KoToolManager.h>
#include <KisUiFont.h>
#include <KisViewManager.h>
#include <KomaPaintTokens.h>
#include <brushengine/kis_paintop_preset.h>
#include <kis_config.h>
#include <kis_icon_utils.h>
#include <kis_paintop_box.h>

#include "SubToolDelegate.h"
#include "SubToolGroups.h"

namespace
{
const QString lastPresetTagKey = QStringLiteral("SubToolDocker/LastPresetTag");
}

SubToolDock::SubToolDock()
    : QDockWidget(i18nc("@title:window docker", "Sub Tool"))
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(KomaPaint::Space::s1);

    m_tabs = new QTabBar(page);
    m_tabs->setDocumentMode(true);
    m_tabs->setExpanding(false);
    m_tabs->setUsesScrollButtons(true);
    m_tabs->setElideMode(Qt::ElideNone);
    m_tabs->setDrawBase(true);
    m_tabs->setFont(KisUiFont::dockFont());
    layout->addWidget(m_tabs);

    m_stack = new QStackedWidget(page);

    m_list = new QListView(m_stack);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setMouseTracking(true);
    m_list->setUniformItemSizes(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_delegate = new SubToolDelegate(m_list);
    m_list->setItemDelegate(m_delegate);
    m_stack->addWidget(m_list);

    m_emptyLabel = new QLabel(i18n("This tool has no sub tools."), m_stack);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->setForegroundRole(QPalette::PlaceholderText);
    m_stack->addWidget(m_emptyLabel);

    layout->addWidget(m_stack, 1);
    setWidget(page);

    m_tagModel = new KisTagModel(ResourceType::PaintOpPresets, this);
    m_presetModel = new KisTagFilterResourceProxyModel(ResourceType::PaintOpPresets, this);
    m_toolModel = new QStandardItemModel(this);

    connect(m_tagModel, &QAbstractItemModel::modelReset, this, &SubToolDock::slotTagsChanged);
    connect(m_tagModel, &QAbstractItemModel::rowsInserted, this, &SubToolDock::slotTagsChanged);
    connect(m_tagModel, &QAbstractItemModel::rowsRemoved, this, &SubToolDock::slotTagsChanged);
    connect(m_tagModel, &QAbstractItemModel::dataChanged, this, &SubToolDock::slotTagsChanged);

    connect(m_presetModel, &QAbstractItemModel::modelReset, this, &SubToolDock::selectCurrentPreset);
    connect(m_tabs, &QTabBar::currentChanged, this, &SubToolDock::slotTabChanged);
    connect(m_list, &QListView::clicked, this, &SubToolDock::slotItemActivated);
    connect(m_list, &QListView::activated, this, &SubToolDock::slotItemActivated);

    slotToolChanged();
}

SubToolDock::~SubToolDock()
{
}

void SubToolDock::setCanvas(KoCanvasBase *canvas)
{
    if (m_canvas) {
        m_canvas->disconnectCanvasObserver(this);
    }

    if (m_toolManager) {
        disconnect(m_toolManager, nullptr, this, nullptr);
    }

    m_canvas = dynamic_cast<KisCanvas2*>(canvas);
    setEnabled(m_canvas);

    // Each main window has its own tool manager, owned by its view manager.
    m_toolManager = (m_canvas && m_canvas->viewManager()) ? m_canvas->viewManager()->toolManager() : nullptr;
    if (m_toolManager) {
        connect(m_toolManager, &KoToolManager::changedTool, this, &SubToolDock::slotToolChanged);
    }

    if (m_canvas && m_canvas->resourceManager()) {
        connect(m_canvas->resourceManager(), &KoCanvasResourceProvider::canvasResourceChanged,
                this, &SubToolDock::slotCanvasResourceChanged);
    }

    slotToolChanged();
}

void SubToolDock::unsetCanvas()
{
    if (m_canvas) {
        m_canvas->disconnectCanvasObserver(this);
    }
    if (m_toolManager) {
        disconnect(m_toolManager, nullptr, this, nullptr);
    }
    m_canvas = nullptr;
    m_toolManager = nullptr;
    setEnabled(false);
}

KoToolManager *SubToolDock::toolManager() const
{
    return m_toolManager;
}

void SubToolDock::slotToolChanged()
{
    const QString toolId = toolManager() ? toolManager()->activeToolId() : QString();

    if (SubToolGroups::isBrushTool(toolId)) {
        if (m_mode != Mode::Presets) {
            showMode(Mode::Presets);
            rebuildPresetTabs();
        }
        selectCurrentPreset();
        return;
    }

    if (const SubToolGroups::ToolGroup *group = SubToolGroups::groupForTool(toolId)) {
        if (m_mode != Mode::ToolVariants || m_toolGroupId != group->id) {
            showMode(Mode::ToolVariants);
            rebuildToolVariants(group->id);
        }
        selectCurrentTool();
        return;
    }

    showMode(Mode::None);
}

void SubToolDock::showMode(Mode mode)
{
    m_mode = mode;

    m_rebuildingTabs = true;
    while (m_tabs->count() > 0) {
        m_tabs->removeTab(0);
    }
    m_rebuildingTabs = false;
    m_tabTags.clear();

    switch (mode) {
    case Mode::Presets:
        m_list->setModel(m_presetModel);
        m_stack->setCurrentWidget(m_list);
        m_tabs->setVisible(true);
        break;
    case Mode::ToolVariants:
        m_list->setModel(m_toolModel);
        m_stack->setCurrentWidget(m_list);
        m_tabs->setVisible(true);
        break;
    case Mode::None:
        m_toolGroupId.clear();
        m_stack->setCurrentWidget(m_emptyLabel);
        m_tabs->setVisible(false);
        break;
    }
}

void SubToolDock::rebuildPresetTabs()
{
    QVector<KisTagSP> tags;
    KisTagSP allTag;
    for (int row = 0; row < m_tagModel->rowCount(); ++row) {
        KisTagSP tag = m_tagModel->tagForIndex(m_tagModel->index(row, 0));
        if (!tag) {
            continue;
        }
        if (tag->id() == KisAllTagsModel::All) {
            allTag = tag;
        } else if (tag->id() != KisAllTagsModel::AllUntagged) {
            tags.append(tag);
        }
    }

    // Clip Studio style groups first, in their usual order; then the rest.
    const QStringList &preferred = SubToolGroups::preferredTagOrder();
    std::stable_sort(tags.begin(), tags.end(), [&preferred](const KisTagSP &a, const KisTagSP &b) {
        const int ia = preferred.indexOf(a->url());
        const int ib = preferred.indexOf(b->url());
        if (ia >= 0 || ib >= 0) {
            if (ia < 0) return false;
            if (ib < 0) return true;
            return ia < ib;
        }
        return a->name().localeAwareCompare(b->name()) < 0;
    });
    if (allTag) {
        tags.append(allTag);
    }

    const QString remembered = KisConfig(true).readEntry<QString>(lastPresetTagKey, QString());

    m_rebuildingTabs = true;
    while (m_tabs->count() > 0) {
        m_tabs->removeTab(0);
    }
    m_tabTags = tags;
    int currentTab = 0;
    for (int i = 0; i < tags.size(); ++i) {
        m_tabs->addTab(tags[i]->name());
        m_tabs->setTabToolTip(i, tags[i]->comment());
        if (tags[i]->url() == remembered) {
            currentTab = i;
        }
    }
    m_tabs->setCurrentIndex(currentTab);
    m_rebuildingTabs = false;

    slotTabChanged(m_tabs->currentIndex());
}

void SubToolDock::rebuildToolVariants(const QString &groupId)
{
    m_toolGroupId = groupId;
    m_toolModel->clear();

    const SubToolGroups::ToolGroup *group = nullptr;
    for (const SubToolGroups::ToolGroup &candidate : SubToolGroups::toolGroups()) {
        if (candidate.id == groupId) {
            group = &candidate;
            break;
        }
    }
    if (!group) {
        return;
    }

    // Only list tools that are actually installed.
    if (!toolManager()) {
        return;
    }
    const QList<KoToolAction*> actions = toolManager()->toolActionList();
    for (const QString &toolId : group->toolIds) {
        for (KoToolAction *action : actions) {
            if (action->id() != toolId) {
                continue;
            }
            QString label = action->iconText();
            if (label.isEmpty()) {
                label = action->toolTip();
            }
            QStandardItem *item = new QStandardItem(KisIconUtils::loadIcon(action->iconName()), label);
            item->setData(toolId, SubToolDelegate::ToolIdRole);
            item->setToolTip(action->toolTip());
            item->setEditable(false);
            m_toolModel->appendRow(item);
            break;
        }
    }

    m_rebuildingTabs = true;
    m_tabs->addTab(group->title);
    m_tabs->setCurrentIndex(0);
    m_rebuildingTabs = false;
}

void SubToolDock::slotTagsChanged()
{
    if (m_mode == Mode::Presets) {
        rebuildPresetTabs();
    }
}

void SubToolDock::slotTabChanged(int index)
{
    if (m_rebuildingTabs || m_mode != Mode::Presets) {
        return;
    }
    if (index < 0 || index >= m_tabTags.size()) {
        return;
    }

    const KisTagSP tag = m_tabTags[index];
    m_presetModel->setTagFilter(tag);
    KisConfig(false).writeEntry(lastPresetTagKey, tag->url());
    selectCurrentPreset();
}

void SubToolDock::slotCanvasResourceChanged(int key, const QVariant &value)
{
    Q_UNUSED(value);
    if (key == KoCanvasResource::CurrentPaintOpPreset && m_mode == Mode::Presets) {
        selectCurrentPreset();
    }
}

QString SubToolDock::currentPresetName() const
{
    if (!m_canvas || !m_canvas->resourceManager()) {
        return QString();
    }
    const KisPaintOpPresetSP preset = m_canvas->resourceManager()
            ->resource(KoCanvasResource::CurrentPaintOpPreset).value<KisPaintOpPresetSP>();
    return preset ? preset->name() : QString();
}

void SubToolDock::selectCurrentPreset()
{
    if (m_mode != Mode::Presets) {
        return;
    }

    const QString name = currentPresetName();
    QModelIndex current;
    if (!name.isEmpty()) {
        for (int row = 0; row < m_presetModel->rowCount(); ++row) {
            const QModelIndex index = m_presetModel->index(row, 0);
            if (index.data(Qt::UserRole + KisAbstractResourceModel::Name).toString() == name) {
                current = index;
                break;
            }
        }
    }

    if (current.isValid()) {
        m_list->selectionModel()->setCurrentIndex(current, QItemSelectionModel::ClearAndSelect);
        m_list->scrollTo(current);
    } else {
        m_list->clearSelection();
    }
}

void SubToolDock::selectCurrentTool()
{
    const QString toolId = toolManager() ? toolManager()->activeToolId() : QString();
    for (int row = 0; row < m_toolModel->rowCount(); ++row) {
        const QModelIndex index = m_toolModel->index(row, 0);
        if (index.data(SubToolDelegate::ToolIdRole).toString() == toolId) {
            m_list->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
            return;
        }
    }
    m_list->clearSelection();
}

void SubToolDock::slotItemActivated(const QModelIndex &index)
{
    if (!index.isValid()) {
        return;
    }

    if (m_mode == Mode::ToolVariants) {
        const QString toolId = index.data(SubToolDelegate::ToolIdRole).toString();
        if (toolManager() && !toolId.isEmpty() && toolId != toolManager()->activeToolId()) {
            toolManager()->switchToolRequested(toolId);
        }
        return;
    }

    if (m_mode == Mode::Presets && m_canvas && m_canvas->viewManager()) {
        const KoResourceSP resource = m_presetModel->resourceForIndex(index);
        if (resource && resource->name() != currentPresetName()) {
            m_canvas->viewManager()->paintOpBox()->resourceSelected(resource);
        }
    }
}
