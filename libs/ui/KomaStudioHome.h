/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KOMASTUDIOHOME_H
#define KOMASTUDIOHOME_H

#include <QPointer>
#include <QSize>
#include <QStyledItemDelegate>
#include <QWidget>

#include "kritaui_export.h"

class QButtonGroup;
class QCheckBox;
class QFrame;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QSortFilterProxyModel;
class QStackedWidget;
class QStandardItem;
class QStandardItemModel;
class QSvgWidget;
class QVBoxLayout;
class KisMainWindow;
class KomaStudioHomePresetCard;

/**
 * Paints a file on the Studio Home page as a card: its thumbnail, name,
 * folder and date, the colour of its first collection and a pinned mark.
 */
class KomaStudioHomeFileDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    /// Size of one grid cell, set by the page as the view resizes.
    void setCellSize(const QSize &size);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    /// Height of a card whose thumbnail is @p width wide.
    static int cardHeight(int width);

private:
    QSize m_cellSize {240, 240};
};

/**
 * The page a Koma Paint window shows when no document is open.
 *
 * - New canvas presets (Illustration, Comic page, Webtoon strip, Animation)
 *   create a document in one click; Custom size opens the New dialog.
 * - The file grid shows recent files, pinned files or one collection,
 *   chosen in the sidebar, and can be searched.
 * - Files can be pinned and gathered into named, coloured collections
 *   (KomaStudioHomeStore keeps them).
 * - Files dropped on the page are opened; pasting an image offers to make
 *   a document from it.
 */
class KRITAUI_EXPORT KomaStudioHome : public QWidget
{
    Q_OBJECT
public:
    explicit KomaStudioHome(QWidget *parent = nullptr);
    ~KomaStudioHome() override;

    void setMainWindow(KisMainWindow *mainWindow);

    /// What the file grid shows.
    enum class Library {
        Recent,
        Pinned,
        Collection,
    };

    /// Roles of the file model, beyond the standard ones.
    enum Role {
        PathRole = Qt::UserRole + 1,
        FolderRole,
        ModifiedRole,
        ExistsRole,
        PinnedRole,
        CollectionColorRole,
        SearchTextRole,
    };

    /// A canvas the page can create in one click.
    struct Preset {
        QString id;
        QString title;
        QString description;
        int width {0};
        int height {0};
        qreal pixelsPerInch {72.0};
        int framesPerSecond {0};     ///< Non-zero for an animation.
        qreal trimGuideMillimeters {0.0}; ///< Non-zero to add trim guides.
        QString workspace;            ///< Workspace to switch to, if any.
    };
    static QVector<Preset> presets();

public Q_SLOTS:
    void slotUpdateThemeColors();
    void showDropAreaIndicator(bool show);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private Q_SLOTS:
    void updateShortcutTexts();
    void slotRebuildSidebar();
    void slotRefreshFiles();
    void slotFileIconChanged(const QUrl &url, const QIcon &icon);
    void slotSearchChanged(const QString &text);
    void slotOpenIndex(const QModelIndex &index);
    void slotFileContextMenu(const QPoint &pos);
    void slotNewCollection();
    void slotPaste();

private:
    void buildUi();
    QWidget *buildSidebar();
    QWidget *buildMain();
    void createPreset(const Preset &preset);
    void showLibrary(Library library, const QString &collectionId = QString());
    void collectionContextMenu(const QString &collectionId, const QPoint &globalPos);
    QString askCollectionName(const QString &title, const QString &current);
    void updateGridSize();
    void updateEmptyState();
    QStringList currentPaths() const;
    void fillItem(QStandardItem *item, const QString &path) const;

    QPointer<KisMainWindow> m_mainWindow;
    Library m_library {Library::Recent};
    QString m_collectionId;

    QSvgWidget *m_logo {nullptr};
    QButtonGroup *m_libraryButtons {nullptr};
    QVBoxLayout *m_collectionsLayout {nullptr};
    QCheckBox *m_animationWorkspace {nullptr};
    QLabel *m_learnLinks {nullptr};

    QLabel *m_title {nullptr};
    QLineEdit *m_search {nullptr};
    QPushButton *m_openButton {nullptr};
    QVector<KomaStudioHomePresetCard*> m_presetCards;
    QLabel *m_libraryTitle {nullptr};
    QLabel *m_libraryCount {nullptr};
    QStackedWidget *m_filesStack {nullptr};
    QListView *m_files {nullptr};
    QLabel *m_emptyLabel {nullptr};
    QLabel *m_controls {nullptr};
    QFrame *m_dropIndicator {nullptr};

    QStandardItemModel *m_model {nullptr};
    QSortFilterProxyModel *m_proxy {nullptr};
    KomaStudioHomeFileDelegate *m_delegate {nullptr};
};

#endif // KOMASTUDIOHOME_H
