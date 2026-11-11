/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SUBTOOLDOCK_H
#define SUBTOOLDOCK_H

#include <QDockWidget>
#include <QPointer>
#include <QVector>

#include <KoCanvasObserverBase.h>
#include <KisTag.h>
#include <kis_canvas2.h>

class QLabel;
class QListView;
class QModelIndex;
class QStackedWidget;
class QStandardItemModel;
class QTabBar;
class KisTagFilterResourceProxyModel;
class KisTagModel;
class KoCanvasController;
class KoToolManager;
class SubToolDelegate;

/**
 * The Sub Tool panel.
 *
 * Follows the active tool. For brush tools it lists the brush presets of the
 * chosen preset tag, with one tab per tag; picking a row makes that preset
 * current. For other tools it lists the tool variants of the tool's group
 * (for example every selection tool); picking a row switches to that tool.
 * All updates are driven by tool, canvas resource and tag model signals.
 */
class SubToolDock : public QDockWidget, public KoCanvasObserverBase
{
    Q_OBJECT
public:
    SubToolDock();
    ~SubToolDock() override;

    QString observerName() override { return QStringLiteral("SubToolDock"); }
    void setCanvas(KoCanvasBase *canvas) override;
    void unsetCanvas() override;

private Q_SLOTS:
    void slotToolChanged();
    void slotCanvasResourceChanged(int key, const QVariant &value);
    void slotTagsChanged();
    void slotTabChanged(int index);
    void slotItemActivated(const QModelIndex &index);

private:
    enum class Mode {
        None,
        Presets,
        ToolVariants,
    };

    void showMode(Mode mode);
    void rebuildPresetTabs();
    void rebuildToolVariants(const QString &groupId);
    void selectCurrentPreset();
    void selectCurrentTool();
    QString currentPresetName() const;
    KoToolManager *toolManager() const;

    QPointer<KisCanvas2> m_canvas;
    QPointer<KoToolManager> m_toolManager;

    QTabBar *m_tabs {nullptr};
    QStackedWidget *m_stack {nullptr};
    QListView *m_list {nullptr};
    QLabel *m_emptyLabel {nullptr};
    SubToolDelegate *m_delegate {nullptr};

    KisTagModel *m_tagModel {nullptr};
    KisTagFilterResourceProxyModel *m_presetModel {nullptr};
    QStandardItemModel *m_toolModel {nullptr};

    Mode m_mode {Mode::None};
    QString m_toolGroupId;
    QVector<KisTagSP> m_tabTags;
    bool m_rebuildingTabs {false};
};

#endif // SUBTOOLDOCK_H
