/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KomaStudioHome.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QSvgWidget>
#include <QTemporaryFile>
#include <kis_action_registry.h>
#include <QVBoxLayout>

#include <klocalizedstring.h>
#include <kis_debug.h>

#include <KisRecentFilesManager.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KomaPaintTokens.h>

#include "KisDocument.h"
#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisRemoteFileFetcher.h"
#include "KisResourceServerProvider.h"
#include "KisUiFont.h"
#include "KisViewManager.h"
#include "KomaStudioHomeStore.h"
#include "canvas/kis_guides_config.h"
#include "dialogs/KisDlgCreateNewDocument.h"
#include "kis_action.h"
#include "kis_action_manager.h"
#include "kis_clipboard.h"
#include "kis_config.h"
#include "kis_image.h"
#include "kis_image_animation_interface.h"
#include "kis_workspace_resource.h"
#include "utils/KisRecentFileIconCache.h"

namespace
{
constexpr int cardGap = KomaPaint::Space::s4;
constexpr int minimumCardWidth = 208;
constexpr qreal thumbnailAspect = 10.0 / 16.0;
constexpr qreal millimetersPerInch = 25.4;
constexpr qreal pointsPerInch = 72.0;

QColor sidebarColor()
{
    // A step between the window background and the panels, so the sidebar
    // reads as part of the page rather than as another docker.
    return KomaPaint::isDarkPalette(qApp->palette()) ? QColor(0x1A1B21) : QColor(0xEFF0F3);
}

QIcon colorDot(const QColor &color)
{
    QPixmap pixmap(QSize(20, 20) * qApp->devicePixelRatio());
    pixmap.setDevicePixelRatio(qApp->devicePixelRatio());
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(5, 5, 10, 10), 3, 3);
    return QIcon(pixmap);
}

QString keycap(const QString &key, const KomaPaint::Theme &theme)
{
    return QStringLiteral("<span style=\"background-color:%1; color:%2;\">&nbsp;%3&nbsp;</span>")
        .arg(theme.surfaceRaised.name(), theme.text.name(), key.toHtmlEscaped());
}

/// The shortcut the user has for a registered action, whether or not the
/// action has been created yet (some are created only once a view opens).
QString registeredShortcut(const QString &actionName)
{
    KisActionRegistry *registry = KisActionRegistry::instance();
    if (!registry->hasAction(actionName)) {
        return QString();
    }
    QAction probe;
    registry->propertizeAction(actionName, &probe);
    return probe.shortcut().toString(QKeySequence::NativeText);
}

QString modifiedText(const QDateTime &modified)
{
    if (!modified.isValid()) {
        return QString();
    }
    const QDate date = modified.date();
    const QDate today = QDate::currentDate();
    const QString time = QLocale().toString(modified.time(), QLocale::ShortFormat);
    if (date == today) {
        return i18nc("@info file modified today, %1 is a time", "Today %1", time);
    }
    if (date == today.addDays(-1)) {
        return i18nc("@info file modified yesterday, %1 is a time", "Yesterday %1", time);
    }
    return QLocale().toString(date, QLocale::ShortFormat);
}
}

/**
 * A one-click canvas preset: a small picture of the canvas shape, its name
 * and its size. Painted with the theme tokens so it follows the theme.
 */
class KomaStudioHomePresetCard : public QAbstractButton
{
public:
    enum class Picture {
        Page,
        ComicPage,
        Strip,
        Animation,
        Custom,
    };

    KomaStudioHomePresetCard(const QString &title, const QString &description, Picture picture, QWidget *parent)
        : QAbstractButton(parent)
        , m_description(description)
        , m_picture(picture)
    {
        setText(title);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_Hover);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAccessibleName(title);
        setAccessibleDescription(description);
        setToolTip(description);
    }

    QSize sizeHint() const override
    {
        return QSize(184, 172);
    }

    QSize minimumSizeHint() const override
    {
        return QSize(140, 172);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const KomaPaint::Theme theme = KomaPaint::themeFor(palette());
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRectF card = QRectF(rect()).adjusted(1, 1, -1, -1);
        const bool highlighted = underMouse() || hasFocus() || isDown();
        const bool custom = m_picture == Picture::Custom;
        QPen border(highlighted ? theme.pulse : theme.border, highlighted ? 1.5 : 1.0);
        if (custom && !highlighted) {
            border.setStyle(Qt::DashLine);
        }
        painter.setPen(border);
        painter.setBrush(custom ? Qt::transparent : (isDown() ? theme.surfaceRaised : theme.surface));
        painter.drawRoundedRect(card, KomaPaint::Radius::md, KomaPaint::Radius::md);

        const qreal left = card.left() + KomaPaint::Space::s4;
        const QRectF pictureBox(left, card.top() + KomaPaint::Space::s4, card.width() - 2 * KomaPaint::Space::s4, 64);
        paintPicture(painter, pictureBox, theme);

        QFont titleFont = font();
        titleFont.setPixelSize(15);
        titleFont.setWeight(QFont::DemiBold);
        painter.setFont(titleFont);
        painter.setPen(theme.text);
        const qreal titleTop = pictureBox.bottom() + KomaPaint::Space::s3;
        const QRectF titleRect(left, titleTop, pictureBox.width(), 22);
        painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                         QFontMetrics(titleFont).elidedText(text(), Qt::ElideRight, int(titleRect.width())));

        QFont descriptionFont = font();
        descriptionFont.setPixelSize(12);
        painter.setFont(descriptionFont);
        painter.setPen(theme.textMuted);
        painter.drawText(QRectF(left, titleRect.bottom() + 2, pictureBox.width(), card.bottom() - titleRect.bottom() - 6),
                         Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, m_description);
    }

private:
    void paintPicture(QPainter &painter, const QRectF &box, const KomaPaint::Theme &theme) const
    {
        const QColor paper(0xF6F6F8);
        const QColor ink(0x14151A);
        // The outline keeps white paper visible on the light theme's white cards.
        const QPen paperEdge(theme.border, 1.0);
        painter.save();
        switch (m_picture) {
        case Picture::Page:
        case Picture::ComicPage: {
            const QRectF page(box.left(), box.bottom() - 64, 45, 64);
            painter.setPen(paperEdge);
            painter.setBrush(paper);
            painter.drawRoundedRect(page, 3, 3);
            if (m_picture == Picture::ComicPage) {
                painter.setPen(QPen(ink, 1.5));
                painter.setBrush(Qt::NoBrush);
                const QRectF inner = page.adjusted(5, 5, -5, -5);
                const qreal gap = 3;
                const qreal rowHeight = (inner.height() - gap) / 2;
                const qreal halfWidth = (inner.width() - gap) / 2;
                painter.drawRect(QRectF(inner.left(), inner.top(), inner.width(), rowHeight));
                painter.drawRect(QRectF(inner.left(), inner.top() + rowHeight + gap, halfWidth, rowHeight));
                painter.drawRect(QRectF(inner.left() + halfWidth + gap, inner.top() + rowHeight + gap, halfWidth, rowHeight));
            }
            break;
        }
        case Picture::Strip:
            painter.setPen(paperEdge);
            painter.setBrush(paper);
            painter.drawRoundedRect(QRectF(box.left(), box.bottom() - 64, 18, 64), 3, 3);
            break;
        case Picture::Animation: {
            const QRectF frame(box.left(), box.bottom() - 47, 84, 47);
            painter.setPen(paperEdge);
            painter.setBrush(paper);
            painter.drawRoundedRect(frame, 3, 3);
            QPainterPath play;
            const QPointF c = frame.center();
            play.moveTo(c + QPointF(-6, -8));
            play.lineTo(c + QPointF(9, 0));
            play.lineTo(c + QPointF(-6, 8));
            play.closeSubpath();
            painter.setPen(Qt::NoPen);
            painter.setBrush(ink);
            painter.drawPath(play);
            break;
        }
        case Picture::Custom: {
            const QRectF circle(box.left(), box.center().y() - 24, 48, 48);
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme.surfaceRaised);
            painter.drawEllipse(circle);
            painter.setPen(QPen(theme.text, 2.2, Qt::SolidLine, Qt::RoundCap));
            const QPointF c = circle.center();
            painter.drawLine(c + QPointF(-7, 0), c + QPointF(7, 0));
            painter.drawLine(c + QPointF(0, -7), c + QPointF(0, 7));
            break;
        }
        }
        painter.restore();
    }

    QString m_description;
    Picture m_picture;
};

// ---------------------------------------------------------------------------
// File cards

void KomaStudioHomeFileDelegate::setCellSize(const QSize &size)
{
    m_cellSize = size;
}

int KomaStudioHomeFileDelegate::cardHeight(int width)
{
    const int thumbnail = qRound(width * thumbnailAspect);
    return thumbnail + KomaPaint::Space::s3 + 20 + 4 + 18;
}

QSize KomaStudioHomeFileDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    return m_cellSize;
}

void KomaStudioHomeFileDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const KomaPaint::Theme theme = KomaPaint::themeFor(option.palette);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setRenderHint(QPainter::SmoothPixmapTransform);

    // Each cell keeps half the gap on its right and bottom.
    const QRect card = option.rect.adjusted(0, 0, -cardGap, -cardGap);
    const int thumbnailHeight = qRound(card.width() * thumbnailAspect);
    const QRectF thumbnail(card.left() + 0.5, card.top() + 0.5, card.width() - 1, thumbnailHeight);
    const bool exists = index.data(KomaStudioHome::ExistsRole).toBool();
    const bool hovered = option.state & QStyle::State_MouseOver;
    const bool focused = (option.state & QStyle::State_HasFocus) || (option.state & QStyle::State_Selected);

    QPainterPath clip;
    clip.addRoundedRect(thumbnail, 10, 10);
    painter->fillPath(clip, theme.surfaceRaised);

    const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
    if (exists && !icon.isNull()) {
        // The whole page is shown: comic pages and strips are tall, and
        // cropping them to the card would hide most of the page.
        const QPixmap pixmap = icon.pixmap(QSize(300, 300));
        const QRectF inner = thumbnail.adjusted(KomaPaint::Space::s2, KomaPaint::Space::s2,
                                                -KomaPaint::Space::s2, -KomaPaint::Space::s2);
        const QSizeF fitted = QSizeF(pixmap.size()).scaled(inner.size(), Qt::KeepAspectRatio);
        const QRectF target(inner.center() - QPointF(fitted.width() / 2, fitted.height() / 2), fitted);
        painter->save();
        painter->setClipPath(clip);
        painter->drawPixmap(target, pixmap, QRectF(pixmap.rect()));
        painter->restore();
    } else {
        QFont typeFont = option.font;
        typeFont.setPixelSize(13);
        typeFont.setWeight(QFont::DemiBold);
        painter->setFont(typeFont);
        painter->setPen(theme.textMuted);
        const QString suffix = QFileInfo(index.data(KomaStudioHome::PathRole).toString()).suffix().toUpper();
        painter->drawText(thumbnail, Qt::AlignCenter, exists ? suffix : i18nc("@info", "Not found"));
    }

    painter->setPen(QPen((hovered || focused) ? theme.pulse : theme.border, (hovered || focused) ? 1.5 : 1.0));
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(clip);

    if (index.data(KomaStudioHome::PinnedRole).toBool()) {
        QFont badgeFont = option.font;
        badgeFont.setPixelSize(11);
        badgeFont.setWeight(QFont::DemiBold);
        const QString label = i18nc("@label badge on a pinned file", "Pinned");
        const QFontMetrics metrics(badgeFont);
        const QRectF badge(thumbnail.left() + 8, thumbnail.top() + 8, metrics.horizontalAdvance(label) + 14, 20);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0x14, 0x15, 0x1A, 200));
        painter->drawRoundedRect(badge, 10, 10);
        painter->setFont(badgeFont);
        painter->setPen(QColor(0xF6F6F8));
        painter->drawText(badge, Qt::AlignCenter, label);
    }

    const int textTop = qRound(thumbnail.bottom()) + KomaPaint::Space::s3;
    QFont nameFont = option.font;
    nameFont.setPixelSize(14);
    nameFont.setWeight(QFont::DemiBold);
    painter->setFont(nameFont);
    painter->setPen(exists ? theme.text : theme.textMuted);
    const QRect nameRect(card.left(), textTop, card.width(), 20);
    painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(nameFont).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, nameRect.width()));

    QFont metaFont = option.font;
    metaFont.setPixelSize(12);
    painter->setFont(metaFont);
    QRect metaRect(card.left(), nameRect.bottom() + 4, card.width(), 18);
    const QColor collectionColor = index.data(KomaStudioHome::CollectionColorRole).value<QColor>();
    if (collectionColor.isValid()) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(collectionColor);
        painter->drawRoundedRect(QRectF(metaRect.left(), metaRect.center().y() - 4, 8, 8), 2, 2);
        metaRect.setLeft(metaRect.left() + 14);
    }
    QString meta;
    if (exists) {
        meta = index.data(KomaStudioHome::FolderRole).toString();
        const QString modified = modifiedText(index.data(KomaStudioHome::ModifiedRole).toDateTime());
        if (!modified.isEmpty()) {
            meta = meta.isEmpty() ? modified : i18nc("@info folder and date of a file", "%1 · %2", meta, modified);
        }
        painter->setPen(theme.textMuted);
    } else {
        meta = i18nc("@info", "File not found");
        painter->setPen(QColor(KomaPaint::isDarkPalette(option.palette) ? KomaPaint::Dark::danger : KomaPaint::Light::danger));
    }
    painter->drawText(metaRect, Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(metaFont).elidedText(meta, Qt::ElideMiddle, metaRect.width()));
    painter->restore();
}

// ---------------------------------------------------------------------------
// The page

QVector<KomaStudioHome::Preset> KomaStudioHome::presets()
{
    // A4 is 210 × 297 mm; JIS B5, the usual manga page, is 182 × 257 mm.
    auto pixels = [](qreal millimeters, qreal ppi) {
        return qRound(millimeters / millimetersPerInch * ppi);
    };
    QVector<Preset> result;
    result.append({QStringLiteral("illustration"), i18nc("@title new canvas preset", "Illustration"),
                   i18nc("@info new canvas preset", "A4 portrait · 300 ppi"),
                   pixels(210, 300), pixels(297, 300), 300, 0, 0.0, QString()});
    result.append({QStringLiteral("comic"), i18nc("@title new canvas preset", "Comic page"),
                   i18nc("@info new canvas preset", "B5 · 600 ppi · trim guides"),
                   pixels(182, 600), pixels(257, 600), 600, 0, 3.0, QString()});
    result.append({QStringLiteral("webtoon"), i18nc("@title new canvas preset", "Webtoon strip"),
                   i18nc("@info new canvas preset", "800 × 12800 px"),
                   800, 12800, 72, 0, 0.0, QString()});
    result.append({QStringLiteral("animation"), i18nc("@title new canvas preset", "Animation"),
                   i18nc("@info new canvas preset", "1920 × 1080 · 24 fps"),
                   1920, 1080, 72, 24, 0.0, QStringLiteral("Animation")});
    return result;
}

KomaStudioHome::KomaStudioHome(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("KomaStudioHome"));
    setAcceptDrops(true);
    setAttribute(Qt::WA_StyledBackground);

    m_model = new QStandardItemModel(this);
    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setFilterRole(SearchTextRole);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);

    buildUi();

    KomaStudioHomeStore *store = KomaStudioHomeStore::instance();
    connect(store, &KomaStudioHomeStore::changed, this, &KomaStudioHome::slotRebuildSidebar);
    connect(store, &KomaStudioHomeStore::changed, this, &KomaStudioHome::slotRefreshFiles);

    KisRecentFilesManager *recent = KisRecentFilesManager::instance();
    connect(recent, &KisRecentFilesManager::fileAdded, this, &KomaStudioHome::slotRefreshFiles);
    connect(recent, &KisRecentFilesManager::fileRemoved, this, &KomaStudioHome::slotRefreshFiles);
    connect(recent, &KisRecentFilesManager::listRenewed, this, &KomaStudioHome::slotRefreshFiles);
    connect(KisRecentFileIconCache::instance(), &KisRecentFileIconCache::fileIconChanged,
            this, &KomaStudioHome::slotFileIconChanged);
    connect(KisActionRegistry::instance(), &KisActionRegistry::shortcutsUpdated,
            this, &KomaStudioHome::updateShortcutTexts);

    slotRebuildSidebar();
    slotRefreshFiles();
    slotUpdateThemeColors();
}

KomaStudioHome::~KomaStudioHome()
{
}

void KomaStudioHome::buildUi()
{
    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(buildSidebar());
    layout->addWidget(buildMain(), 1);

    // Shown over the whole page while files are dragged onto it.
    m_dropIndicator = new QFrame(this);
    m_dropIndicator->setObjectName(QStringLiteral("studioDropIndicator"));
    m_dropIndicator->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_dropIndicator->hide();
}

QWidget *KomaStudioHome::buildSidebar()
{
    QFrame *sidebar = new QFrame(this);
    sidebar->setObjectName(QStringLiteral("studioSidebar"));
    sidebar->setFixedWidth(256);
    QVBoxLayout *layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(KomaPaint::Space::s4, KomaPaint::Space::s5, KomaPaint::Space::s4, KomaPaint::Space::s5);
    layout->setSpacing(2);

    m_logo = new QSvgWidget(sidebar);
    m_logo->setFixedSize(176, 40);
    m_logo->setAccessibleName(i18nc("@info", "Koma Paint"));
    layout->addWidget(m_logo, 0, Qt::AlignLeft);
    layout->addSpacing(KomaPaint::Space::s5);

    m_libraryButtons = new QButtonGroup(this);
    m_libraryButtons->setExclusive(true);
    auto addLibraryButton = [&](const QString &text, Library library) {
        QPushButton *button = new QPushButton(text, sidebar);
        button->setProperty("studioNav", true);
        button->setCheckable(true);
        button->setFlat(true);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setMinimumHeight(40);
        m_libraryButtons->addButton(button);
        connect(button, &QPushButton::clicked, this, [this, library]() { showLibrary(library); });
        layout->addWidget(button);
        return button;
    };
    addLibraryButton(i18nc("@action:button Studio Home library", "Recent files"), Library::Recent)->setChecked(true);
    addLibraryButton(i18nc("@action:button Studio Home library", "Pinned"), Library::Pinned);

    layout->addSpacing(KomaPaint::Space::s5);
    QLabel *collectionsLabel = new QLabel(i18nc("@title Studio Home sidebar", "Collections"), sidebar);
    collectionsLabel->setObjectName(QStringLiteral("studioSectionLabel"));
    layout->addWidget(collectionsLabel);
    layout->addSpacing(KomaPaint::Space::s1);

    m_collectionsLayout = new QVBoxLayout();
    m_collectionsLayout->setContentsMargins(0, 0, 0, 0);
    m_collectionsLayout->setSpacing(2);
    layout->addLayout(m_collectionsLayout);

    QPushButton *newCollection = new QPushButton(i18nc("@action:button", "New collection…"), sidebar);
    newCollection->setObjectName(QStringLiteral("studioNewCollection"));
    newCollection->setFlat(true);
    newCollection->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    newCollection->setMinimumHeight(36);
    connect(newCollection, &QPushButton::clicked, this, &KomaStudioHome::slotNewCollection);
    layout->addSpacing(KomaPaint::Space::s1);
    layout->addWidget(newCollection);

    layout->addStretch(1);

    m_animationWorkspace = new QCheckBox(i18nc("@option:check", "Open the Animation workspace\nfor new animations"), sidebar);
    m_animationWorkspace->setChecked(KomaStudioHomeStore::instance()->switchToAnimationWorkspace());
    connect(m_animationWorkspace, &QCheckBox::toggled, this, [](bool checked) {
        KomaStudioHomeStore::instance()->setSwitchToAnimationWorkspace(checked);
    });
    layout->addWidget(m_animationWorkspace);
    layout->addSpacing(KomaPaint::Space::s4);

    m_learnLinks = new QLabel(sidebar);
    m_learnLinks->setObjectName(QStringLiteral("studioLearnLinks"));
    m_learnLinks->setOpenExternalLinks(true);
    m_learnLinks->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_learnLinks->setWordWrap(true);
    layout->addWidget(m_learnLinks);
    return sidebar;
}

QWidget *KomaStudioHome::buildMain()
{
    QWidget *main = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(main);
    layout->setContentsMargins(KomaPaint::Space::s6 + KomaPaint::Space::s2, KomaPaint::Space::s6,
                               KomaPaint::Space::s6 + KomaPaint::Space::s2, KomaPaint::Space::s5);
    layout->setSpacing(0);

    QHBoxLayout *header = new QHBoxLayout();
    header->setSpacing(KomaPaint::Space::s3);
    m_title = new QLabel(i18nc("@title Studio Home", "Start something new"), main);
    m_title->setFont(KisUiFont::headingFont(KisUiFont::HeadingLevel::H2));
    header->addWidget(m_title);
    header->addStretch(1);
    m_search = new QLineEdit(main);
    m_search->setObjectName(QStringLiteral("studioSearch"));
    m_search->setPlaceholderText(i18nc("@info:placeholder", "Search these files"));
    m_search->setAccessibleName(i18nc("@info", "Search these files"));
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumWidth(260);
    m_search->setMinimumHeight(38);
    connect(m_search, &QLineEdit::textChanged, this, &KomaStudioHome::slotSearchChanged);
    header->addWidget(m_search);
    m_openButton = new QPushButton(i18nc("@action:button", "Open file…"), main);
    m_openButton->setMinimumHeight(38);
    header->addWidget(m_openButton);
    layout->addLayout(header);
    layout->addSpacing(KomaPaint::Space::s5);

    QLabel *newLabel = new QLabel(i18nc("@title Studio Home section", "New canvas"), main);
    newLabel->setObjectName(QStringLiteral("studioSectionLabel"));
    layout->addWidget(newLabel);
    layout->addSpacing(KomaPaint::Space::s3);

    QHBoxLayout *presetRow = new QHBoxLayout();
    presetRow->setSpacing(cardGap);
    const QVector<Preset> allPresets = presets();
    for (const Preset &preset : allPresets) {
        KomaStudioHomePresetCard::Picture picture = KomaStudioHomePresetCard::Picture::Page;
        if (preset.id == QLatin1String("comic")) {
            picture = KomaStudioHomePresetCard::Picture::ComicPage;
        } else if (preset.id == QLatin1String("webtoon")) {
            picture = KomaStudioHomePresetCard::Picture::Strip;
        } else if (preset.id == QLatin1String("animation")) {
            picture = KomaStudioHomePresetCard::Picture::Animation;
        }
        KomaStudioHomePresetCard *card = new KomaStudioHomePresetCard(preset.title, preset.description, picture, main);
        card->setObjectName(QStringLiteral("studioPreset-%1").arg(preset.id));
        connect(card, &QAbstractButton::clicked, this, [this, preset]() { createPreset(preset); });
        m_presetCards.append(card);
        presetRow->addWidget(card);
    }
    KomaStudioHomePresetCard *custom = new KomaStudioHomePresetCard(
                i18nc("@title new canvas preset", "Custom size…"),
                i18nc("@info new canvas preset", "Any size, colour space or template"),
                KomaStudioHomePresetCard::Picture::Custom, main);
    custom->setObjectName(QStringLiteral("studioPreset-custom"));
    connect(custom, &QAbstractButton::clicked, this, [this]() {
        if (m_mainWindow) {
            m_mainWindow->slotFileNew();
        }
    });
    m_presetCards.append(custom);
    presetRow->addWidget(custom);
    layout->addLayout(presetRow);
    layout->addSpacing(KomaPaint::Space::s6);

    QHBoxLayout *libraryHeader = new QHBoxLayout();
    libraryHeader->setSpacing(KomaPaint::Space::s3);
    m_libraryTitle = new QLabel(main);
    m_libraryTitle->setObjectName(QStringLiteral("studioSectionLabel"));
    libraryHeader->addWidget(m_libraryTitle);
    m_libraryCount = new QLabel(main);
    m_libraryCount->setObjectName(QStringLiteral("studioMutedLabel"));
    libraryHeader->addWidget(m_libraryCount);
    libraryHeader->addStretch(1);
    layout->addLayout(libraryHeader);
    layout->addSpacing(KomaPaint::Space::s3);

    m_files = new QListView(main);
    m_files->setObjectName(QStringLiteral("studioFiles"));
    m_files->setViewMode(QListView::IconMode);
    m_files->setMovement(QListView::Static);
    m_files->setResizeMode(QListView::Adjust);
    m_files->setWrapping(true);
    m_files->setUniformItemSizes(true);
    m_files->setSelectionMode(QAbstractItemView::SingleSelection);
    m_files->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_files->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_files->setFrameShape(QFrame::NoFrame);
    m_files->setMouseTracking(true);
    m_files->setContextMenuPolicy(Qt::CustomContextMenu);
    m_files->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_files->setDragEnabled(false);
    m_files->setSpacing(0);
    m_files->viewport()->setAutoFillBackground(false);
    m_files->viewport()->installEventFilter(this);
    m_files->setModel(m_proxy);
    m_delegate = new KomaStudioHomeFileDelegate(m_files);
    m_files->setItemDelegate(m_delegate);
    connect(m_files, &QListView::clicked, this, &KomaStudioHome::slotOpenIndex);
    connect(m_files, &QListView::activated, this, &KomaStudioHome::slotOpenIndex);
    connect(m_files, &QListView::customContextMenuRequested, this, &KomaStudioHome::slotFileContextMenu);

    m_emptyLabel = new QLabel(main);
    m_emptyLabel->setObjectName(QStringLiteral("studioEmptyLabel"));
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setWordWrap(true);

    m_filesStack = new QStackedWidget(main);
    m_filesStack->addWidget(m_files);
    m_filesStack->addWidget(m_emptyLabel);
    layout->addWidget(m_filesStack, 1);
    layout->addSpacing(KomaPaint::Space::s4);

    QFrame *controlsFrame = new QFrame(main);
    controlsFrame->setObjectName(QStringLiteral("studioControls"));
    QHBoxLayout *controlsLayout = new QHBoxLayout(controlsFrame);
    controlsLayout->setContentsMargins(KomaPaint::Space::s4, KomaPaint::Space::s3, KomaPaint::Space::s4, KomaPaint::Space::s3);
    m_controls = new QLabel(controlsFrame);
    m_controls->setWordWrap(true);
    controlsLayout->addWidget(m_controls);
    layout->addWidget(controlsFrame);
    return main;
}

void KomaStudioHome::setMainWindow(KisMainWindow *mainWindow)
{
    m_mainWindow = mainWindow;
    if (!mainWindow) {
        return;
    }
    connect(m_openButton, &QPushButton::clicked, mainWindow, &KisMainWindow::slotFileOpen);

    KisActionManager *actions = mainWindow->viewManager()->actionManager();
    if (KisAction *paste = actions->actionByName(QStringLiteral("edit_paste"))) {
        connect(paste, &QAction::triggered, this, &KomaStudioHome::slotPaste);
    }
    updateShortcutTexts();
    slotUpdateThemeColors();
}

void KomaStudioHome::updateShortcutTexts()
{
    const KomaPaint::Theme theme = KomaPaint::themeFor(palette());
    const QString openShortcut = registeredShortcut(QStringLiteral("file_open"));
    m_openButton->setToolTip(openShortcut.isEmpty() ? QString()
                                                    : i18nc("@info:tooltip", "Open a file (%1)", openShortcut));
    QString quickAccess;
    const QString quickAccessShortcut = registeredShortcut(QStringLiteral("quick_access_popup"));
    if (!quickAccessShortcut.isEmpty()) {
        quickAccess = i18nc("@info canvas control, %1 is a key", "%1 Quick Access", keycap(quickAccessShortcut, theme));
    }

    // The canvas controls of the Koma Paint input profile.
    QStringList items = {
        i18nc("@info canvas control, %1 is a key", "%1 + drag to pan", keycap(i18nc("@info key", "Space"), theme)),
        i18nc("@info canvas control, %1 is a key", "%1 zoom", keycap(i18nc("@info key", "Ctrl+Space"), theme)),
        i18nc("@info canvas control, %1 is a key", "%1 rotate", keycap(i18nc("@info key", "Shift+Space"), theme)),
        i18nc("@info canvas control, %1 is a key", "%1 brush size", keycap(i18nc("@info key", "Ctrl+Alt"), theme)),
    };
    if (!quickAccess.isEmpty()) {
        items.append(quickAccess);
    }
    m_controls->setText(QStringLiteral("<b style=\"color:%1;\">%2</b>&nbsp;&nbsp;&nbsp;&nbsp;%3")
                        .arg(theme.text.name(), i18nc("@title", "Canvas controls"),
                             items.join(QStringLiteral("&nbsp;&nbsp;&nbsp;&nbsp;"))));
}

void KomaStudioHome::slotUpdateThemeColors()
{
    const KomaPaint::Theme theme = KomaPaint::themeFor(qApp->palette());
    const bool dark = KomaPaint::isDarkPalette(qApp->palette());
    m_logo->load(dark ? QStringLiteral(":/splash/logo-primary-reversed.svg")
                      : QStringLiteral(":/splash/logo-primary.svg"));

    const QString sheet = QStringLiteral(
        "#KomaStudioHome { background: %1; }"
        "#studioSidebar { background: %2; border: none; border-right: 1px solid %3; }"
        "QPushButton[studioNav=\"true\"] { text-align: left; border: none; border-radius: %8px;"
        "  padding: 8px 12px; color: %4; background: transparent; }"
        "QPushButton[studioNav=\"true\"]:hover { background: %3; color: %5; }"
        "QPushButton[studioNav=\"true\"]:checked { background: %6; color: %5; font-weight: 600; }"
        "#studioNewCollection { text-align: left; border: 1px dashed %7; border-radius: %8px;"
        "  padding: 8px 12px; color: %4; background: transparent; }"
        "#studioNewCollection:hover { border-color: %9; color: %5; }"
        "QLabel#studioSectionLabel { color: %4; font-weight: 600; }"
        "QLabel#studioMutedLabel, QLabel#studioEmptyLabel { color: %4; }"
        "#studioFiles { background: transparent; }"
        "#studioControls { background: %2; border: 1px solid %3; border-radius: %10px; }"
        "#studioControls QLabel { color: %4; }"
        "#studioDropIndicator { border: 2px dashed %9; border-radius: %10px; background: transparent; }")
        .arg(theme.bg.name(), sidebarColor().name(), theme.surface.name(), theme.textMuted.name(),
             theme.text.name(), theme.surfaceRaised.name(), theme.border.name())
        .arg(KomaPaint::Radius::sm)
        .arg(theme.pulse.name())
        .arg(KomaPaint::Radius::md);
    setStyleSheet(sheet);

    QFont sectionFont = font();
    sectionFont.setPixelSize(12);
    sectionFont.setWeight(QFont::DemiBold);
    sectionFont.setCapitalization(QFont::AllUppercase);
    sectionFont.setLetterSpacing(QFont::PercentageSpacing, 106);
    const QList<QLabel*> sectionLabels = findChildren<QLabel*>(QStringLiteral("studioSectionLabel"));
    for (QLabel *label : sectionLabels) {
        label->setFont(sectionFont);
    }

    const QColor link = KomaPaint::pulseTextColor(qApp->palette());
    m_learnLinks->setText(QStringLiteral(
        "<p style=\"color:%1; line-height:150%;\">%2<br>"
        "<a style=\"color:%3;\" href=\"https://docs.krita.org/en/user_manual.html\">%4</a><br>"
        "<a style=\"color:%3;\" href=\"https://github.com/Sudo-DNF/Koma-Paint\">%5</a></p>")
        .arg(theme.textMuted.name(), i18nc("@title Studio Home sidebar", "Learn"), link.name(),
             i18nc("@action link", "Painting manual (Krita)"), i18nc("@action link", "Koma Paint on GitHub")));

    updateShortcutTexts();
    m_files->viewport()->update();
    update();
}

void KomaStudioHome::slotRebuildSidebar()
{
    KomaStudioHomeStore *store = KomaStudioHomeStore::instance();
    // The sender may be one of these buttons (renaming from its menu), so
    // they are deleted once control is back in the event loop.
    while (QLayoutItem *item = m_collectionsLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            if (QAbstractButton *button = qobject_cast<QAbstractButton*>(widget)) {
                m_libraryButtons->removeButton(button);
            }
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }

    const QVector<KomaStudioHomeStore::Collection> collections = store->collections();
    for (const KomaStudioHomeStore::Collection &collection : collections) {
        QPushButton *button = new QPushButton(colorDot(collection.color), collection.name,
                                              m_collectionsLayout->parentWidget());
        button->setProperty("studioNav", true);
        button->setIconSize(QSize(20, 20));
        button->setCheckable(true);
        button->setFlat(true);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setMinimumHeight(36);
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        button->setToolTip(i18ncp("@info:tooltip", "%1 file", "%1 files", collection.files.size()));
        m_libraryButtons->addButton(button);
        const QString id = collection.id;
        connect(button, &QPushButton::clicked, this, [this, id]() { showLibrary(Library::Collection, id); });
        connect(button, &QPushButton::customContextMenuRequested, this, [this, id, button](const QPoint &pos) {
            collectionContextMenu(id, button->mapToGlobal(pos));
        });
        if (m_library == Library::Collection && m_collectionId == id) {
            button->setChecked(true);
        }
        m_collectionsLayout->addWidget(button);
    }

    if (m_library == Library::Collection && store->collection(m_collectionId).id.isEmpty()) {
        showLibrary(Library::Recent);
    }
}

void KomaStudioHome::showLibrary(Library library, const QString &collectionId)
{
    m_library = library;
    m_collectionId = library == Library::Collection ? collectionId : QString();

    // Keep the sidebar in step when the library changes from code.
    const QList<QAbstractButton*> buttons = m_libraryButtons->buttons();
    int index = 0;
    const QVector<KomaStudioHomeStore::Collection> collections = KomaStudioHomeStore::instance()->collections();
    for (QAbstractButton *button : buttons) {
        bool current = false;
        if (index == 0) {
            current = library == Library::Recent;
        } else if (index == 1) {
            current = library == Library::Pinned;
        } else if (index - 2 < collections.size()) {
            current = library == Library::Collection && collections[index - 2].id == m_collectionId;
        }
        if (current) {
            button->setChecked(true);
        }
        ++index;
    }
    slotRefreshFiles();
}

QStringList KomaStudioHome::currentPaths() const
{
    KomaStudioHomeStore *store = KomaStudioHomeStore::instance();
    switch (m_library) {
    case Library::Recent: {
        QStringList paths;
        const QList<QUrl> urls = KisRecentFilesManager::instance()->recentUrlsLatestFirst();
        for (const QUrl &url : urls) {
            if (url.isLocalFile()) {
                paths.append(url.toLocalFile());
            }
        }
        return paths;
    }
    case Library::Pinned:
        return store->pinnedFiles();
    case Library::Collection:
        return store->collection(m_collectionId).files;
    }
    return QStringList();
}

void KomaStudioHome::fillItem(QStandardItem *item, const QString &path) const
{
    KomaStudioHomeStore *store = KomaStudioHomeStore::instance();
    const QFileInfo info(path);
    const bool exists = info.exists();
    item->setText(info.fileName());
    item->setToolTip(path);
    item->setData(path, PathRole);
    item->setData(exists, ExistsRole);
    item->setData(info.dir().dirName(), FolderRole);
    item->setData(exists ? info.lastModified() : QDateTime(), ModifiedRole);
    item->setData(store->isPinned(path), PinnedRole);

    QStringList searchText = {info.fileName(), info.absolutePath()};
    const QStringList collectionIds = store->collectionsWithFile(path);
    for (const QString &id : collectionIds) {
        searchText.append(store->collection(id).name);
    }
    // In a collection the other collections a file belongs to are what
    // tells it apart; elsewhere its first collection does.
    QColor color;
    for (const QString &id : collectionIds) {
        if (m_library != Library::Collection || id != m_collectionId) {
            color = store->collection(id).color;
            break;
        }
    }
    item->setData(color, CollectionColorRole);
    item->setData(searchText.join(QLatin1Char('\n')), SearchTextRole);

    if (exists) {
        const QIcon icon = KisRecentFileIconCache::instance()->getOrQueueFileIcon(QUrl::fromLocalFile(path));
        if (!icon.isNull()) {
            item->setIcon(icon);
        }
    }
}

void KomaStudioHome::slotRefreshFiles()
{
    const QStringList paths = currentPaths();
    m_model->clear();
    for (const QString &path : paths) {
        QStandardItem *item = new QStandardItem();
        item->setEditable(false);
        fillItem(item, path);
        m_model->appendRow(item);
    }

    switch (m_library) {
    case Library::Recent:
        m_libraryTitle->setText(i18nc("@title Studio Home section", "Continue where you left off"));
        break;
    case Library::Pinned:
        m_libraryTitle->setText(i18nc("@title Studio Home section", "Pinned"));
        break;
    case Library::Collection:
        m_libraryTitle->setText(KomaStudioHomeStore::instance()->collection(m_collectionId).name);
        break;
    }
    updateEmptyState();
}

void KomaStudioHome::slotFileIconChanged(const QUrl &url, const QIcon &icon)
{
    if (!url.isLocalFile()) {
        return;
    }
    const QString path = url.toLocalFile();
    for (int row = 0; row < m_model->rowCount(); ++row) {
        QStandardItem *item = m_model->item(row);
        if (item->data(PathRole).toString() == path) {
            item->setIcon(icon);
        }
    }
}

void KomaStudioHome::slotSearchChanged(const QString &text)
{
    m_proxy->setFilterFixedString(text.trimmed());
    updateEmptyState();
}

void KomaStudioHome::updateEmptyState()
{
    const int total = m_model->rowCount();
    const int shown = m_proxy->rowCount();
    m_libraryCount->setText(total == 0 ? QString() : i18ncp("@info number of files", "%1 file", "%1 files", shown));

    if (shown > 0) {
        m_filesStack->setCurrentWidget(m_files);
        return;
    }
    QString message;
    if (total > 0) {
        message = i18nc("@info", "No files here match “%1”.", m_search->text().trimmed());
    } else {
        switch (m_library) {
        case Library::Recent:
            message = i18nc("@info", "Files you open or save appear here.\nStart with a canvas above, or drop a file on this page.");
            break;
        case Library::Pinned:
            message = i18nc("@info", "Pin the files you come back to: right-click a file and choose Pin.");
            break;
        case Library::Collection:
            message = i18nc("@info", "This collection is empty.\nRight-click a file and choose Add to collection.");
            break;
        }
    }
    m_emptyLabel->setText(message);
    m_filesStack->setCurrentWidget(m_emptyLabel);
}

void KomaStudioHome::slotOpenIndex(const QModelIndex &index)
{
    if (!index.isValid() || !m_mainWindow) {
        return;
    }
    m_mainWindow->openDocument(index.data(PathRole).toString(), KisMainWindow::None);
}

void KomaStudioHome::slotFileContextMenu(const QPoint &pos)
{
    const QModelIndex index = m_files->indexAt(pos);
    if (!index.isValid() || !m_mainWindow) {
        return;
    }
    KomaStudioHomeStore *store = KomaStudioHomeStore::instance();
    const QString path = index.data(PathRole).toString();
    const bool exists = index.data(ExistsRole).toBool();
    const QString name = index.data(Qt::DisplayRole).toString();

    QMenu menu(this);
    QAction *open = menu.addAction(i18nc("@action:inmenu", "Open"));
    open->setEnabled(exists);
    const bool pinned = store->isPinned(path);
    QAction *pin = menu.addAction(pinned ? i18nc("@action:inmenu", "Unpin") : i18nc("@action:inmenu", "Pin"));

    QMenu *addTo = menu.addMenu(i18nc("@action:inmenu", "Add to collection"));
    const QStringList memberOf = store->collectionsWithFile(path);
    const QVector<KomaStudioHomeStore::Collection> collections = store->collections();
    for (const KomaStudioHomeStore::Collection &collection : collections) {
        QAction *action = addTo->addAction(colorDot(collection.color), collection.name);
        action->setData(collection.id);
        action->setEnabled(!memberOf.contains(collection.id));
    }
    if (!collections.isEmpty()) {
        addTo->addSeparator();
    }
    QAction *addToNew = addTo->addAction(i18nc("@action:inmenu", "New collection…"));

    QAction *removeFromCollection = nullptr;
    if (m_library == Library::Collection) {
        removeFromCollection = menu.addAction(i18nc("@action:inmenu, %1 is a collection name", "Remove from “%1”",
                                                    store->collection(m_collectionId).name));
    }
    menu.addSeparator();
    QAction *showFolder = menu.addAction(i18nc("@action:inmenu", "Show in file manager"));
    showFolder->setEnabled(exists);
    QAction *forget = nullptr;
    if (m_library == Library::Recent) {
        forget = menu.addAction(i18nc("@action:inmenu, %1 is a file name", "Forget “%1”", name));
    }

    QAction *chosen = menu.exec(m_files->viewport()->mapToGlobal(pos));
    if (!chosen) {
        return;
    }
    if (chosen == open) {
        slotOpenIndex(index);
    } else if (chosen == pin) {
        store->setPinned(path, !pinned);
    } else if (chosen == addToNew) {
        const QString collectionName = askCollectionName(i18nc("@title:window", "New Collection"), QString());
        if (!collectionName.isEmpty()) {
            store->addFile(store->addCollection(collectionName), path);
        }
    } else if (chosen->parent() == addTo && !chosen->data().toString().isEmpty()) {
        store->addFile(chosen->data().toString(), path);
    } else if (chosen == removeFromCollection) {
        store->removeFile(m_collectionId, path);
    } else if (chosen == showFolder) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    } else if (chosen == forget) {
        m_mainWindow->removeRecentFile(path);
    }
}

void KomaStudioHome::slotNewCollection()
{
    const QString name = askCollectionName(i18nc("@title:window", "New Collection"), QString());
    if (name.isEmpty()) {
        return;
    }
    const QString id = KomaStudioHomeStore::instance()->addCollection(name);
    showLibrary(Library::Collection, id);
}

QString KomaStudioHome::askCollectionName(const QString &title, const QString &current)
{
    bool accepted = false;
    const QString name = QInputDialog::getText(this, title, i18nc("@label:textbox", "Collection name:"),
                                               QLineEdit::Normal, current, &accepted);
    return accepted ? name.trimmed() : QString();
}

void KomaStudioHome::collectionContextMenu(const QString &collectionId, const QPoint &globalPos)
{
    KomaStudioHomeStore *store = KomaStudioHomeStore::instance();
    const KomaStudioHomeStore::Collection collection = store->collection(collectionId);
    if (collection.id.isEmpty()) {
        return;
    }
    QMenu menu(this);
    QAction *rename = menu.addAction(i18nc("@action:inmenu", "Rename…"));
    QAction *remove = menu.addAction(i18nc("@action:inmenu", "Delete collection…"));
    QAction *chosen = menu.exec(globalPos);
    if (chosen == rename) {
        const QString name = askCollectionName(i18nc("@title:window", "Rename Collection"), collection.name);
        if (!name.isEmpty()) {
            store->renameCollection(collectionId, name);
        }
    } else if (chosen == remove) {
        const QMessageBox::StandardButton answer = QMessageBox::question(
                    this, i18nc("@title:window", "Delete Collection"),
                    i18nc("@info", "Delete the collection “%1”? The files in it are not deleted.", collection.name),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer == QMessageBox::Yes) {
            store->removeCollection(collectionId);
        }
    }
}

void KomaStudioHome::createPreset(const Preset &preset)
{
    if (!m_mainWindow) {
        return;
    }
    const KoColorSpace *colorSpace = KoColorSpaceRegistry::instance()->rgb8();
    const qreal resolution = preset.pixelsPerInch / pointsPerInch; // pixels per point
    KisDocument *document = KisPart::instance()->createDocument();
    // The same start as the New dialog: a white Paper fill layer with a
    // layer to draw on above it.
    if (!document->newImage(preset.title, preset.width, preset.height, colorSpace,
                            KoColor(Qt::white, colorSpace), KisConfig::FILL_LAYER, 2, QString(), resolution)) {
        delete document;
        return;
    }

    if (preset.framesPerSecond > 0) {
        document->image()->animationInterface()->setFramerate(preset.framesPerSecond);
    }
    if (preset.trimGuideMillimeters > 0) {
        // Guide positions are in points.
        const qreal inset = preset.trimGuideMillimeters / millimetersPerInch * pointsPerInch;
        const qreal width = preset.width / resolution;
        const qreal height = preset.height / resolution;
        KisGuidesConfig guides = document->guidesConfig();
        guides.setHorizontalGuideLines({inset, height - inset});
        guides.setVerticalGuideLines({inset, width - inset});
        guides.setShowGuides(true);
        document->setGuidesConfig(guides);
    }
    document->setModified(false);

    KisPart::instance()->addDocument(document);
    m_mainWindow->addViewAndNotifyLoadingCompleted(document);

    // Applied once the view is open: while this page shows, the window keeps
    // its docker layout aside and puts it back when the first view opens,
    // which would undo a workspace applied earlier.
    if (!preset.workspace.isEmpty() && KomaStudioHomeStore::instance()->switchToAnimationWorkspace()) {
        KisWorkspaceResourceSP workspace = KisResourceServerProvider::instance()->workspaceServer()
                ->resource(QString(), QString(), preset.workspace);
        if (workspace) {
            KisConfig(false).writeEntry("CurrentWorkspace", workspace->name());
            m_mainWindow->restoreWorkspace(workspace);
        }
    }
}

void KomaStudioHome::slotPaste()
{
    if (!isVisible() || !KisClipboard::instance()->hasImage()) {
        return;
    }
    KisDlgCreateNewDocument *dialog = new KisDlgCreateNewDocument(this);
    dialog->SelectPage(KisDlgCreateNewDocument::Page::CreateFromClipboard);
    dialog->exec();
    dialog->deleteLater();
}

void KomaStudioHome::updateGridSize()
{
    const int available = m_files->viewport()->width();
    if (available <= 0) {
        return;
    }
    const int columns = qMax(1, (available + cardGap) / (minimumCardWidth + cardGap));
    const int cellWidth = (available + cardGap) / columns;
    const QSize cell(cellWidth, KomaStudioHomeFileDelegate::cardHeight(cellWidth - cardGap) + cardGap);
    if (m_files->gridSize() == cell) {
        return;
    }
    m_delegate->setCellSize(cell);
    m_files->setGridSize(cell);
}

void KomaStudioHome::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    m_dropIndicator->setGeometry(rect().adjusted(KomaPaint::Space::s2, KomaPaint::Space::s2,
                                                 -KomaPaint::Space::s2, -KomaPaint::Space::s2));
}

bool KomaStudioHome::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_files->viewport() && event->type() == QEvent::Resize) {
        updateGridSize();
    }
    return QWidget::eventFilter(watched, event);
}

void KomaStudioHome::showDropAreaIndicator(bool show)
{
    m_dropIndicator->setVisible(show);
    if (show) {
        m_dropIndicator->raise();
    }
}

void KomaStudioHome::dragEnterEvent(QDragEnterEvent *event)
{
    const QMimeData *data = event->mimeData();
    if (data->hasUrls() || data->hasFormat(QStringLiteral("application/x-krita-node-internal-pointer"))
            || data->hasFormat(QStringLiteral("application/x-qt-image"))) {
        showDropAreaIndicator(true);
        event->accept();
        return;
    }
    event->ignore();
}

void KomaStudioHome::dragMoveEvent(QDragMoveEvent *event)
{
    if (m_mainWindow) {
        m_mainWindow->dragMoveEvent(event);
    }
    const QMimeData *data = event->mimeData();
    if (data->hasUrls() || data->hasFormat(QStringLiteral("application/x-krita-node-internal-pointer"))
            || data->hasFormat(QStringLiteral("application/x-qt-image"))) {
        event->accept();
        return;
    }
    event->ignore();
}

void KomaStudioHome::dragLeaveEvent(QDragLeaveEvent *)
{
    showDropAreaIndicator(false);
    if (m_mainWindow) {
        m_mainWindow->dragLeave();
    }
}

void KomaStudioHome::dropEvent(QDropEvent *event)
{
    showDropAreaIndicator(false);
    if (!m_mainWindow || !event->mimeData()->hasUrls()) {
        return;
    }
    const QList<QUrl> urls = event->mimeData()->urls();
    for (const QUrl &url : urls) {
        if (url.toLocalFile().endsWith(QStringLiteral(".bundle"), Qt::CaseInsensitive)) {
            if (!m_mainWindow->installBundle(url.toLocalFile())) {
                qWarning() << "Could not install bundle" << url.toLocalFile();
            }
        } else if (!url.isLocalFile()) {
            QScopedPointer<QTemporaryFile> temporary(new QTemporaryFile());
            temporary->setFileName(url.fileName());
            KisRemoteFileFetcher fetcher;
            if (!fetcher.fetchFile(url, temporary.data())) {
                qWarning() << "Fetching" << url << "failed";
                continue;
            }
            m_mainWindow->openDocument(temporary->fileName(), KisMainWindow::None);
        } else {
            m_mainWindow->openDocument(url.toLocalFile(), KisMainWindow::None);
        }
    }
}
