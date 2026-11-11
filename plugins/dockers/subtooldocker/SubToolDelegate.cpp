/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "SubToolDelegate.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>

#include <KisResourceModel.h>
#include <KisResourceModelProvider.h>
#include <KomaPaintTokens.h>

namespace
{
constexpr int rowHeight = 44;
constexpr int previewSize = 36;
constexpr int markerWidth = 3;
}

SubToolDelegate::SubToolDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

QString SubToolDelegate::displayName(const QString &resourceName)
{
    static const QRegularExpression sortPrefix(QStringLiteral("^[a-z]\\)[ _]"));
    QString name = resourceName;
    name.remove(sortPrefix);
    name.replace(QLatin1Char('_'), QLatin1Char(' '));
    return name;
}

void SubToolDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    if (!index.isValid()) {
        return;
    }

    const KomaPaint::Theme t = KomaPaint::themeFor(option.palette);
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    const QRectF row = QRectF(option.rect).adjusted(2, 1, -2, -1);
    if (selected || hovered) {
        QColor fill = t.surfaceRaised;
        if (!selected) {
            fill.setAlphaF(0.6);
        }
        painter->setPen(Qt::NoPen);
        painter->setBrush(fill);
        painter->drawRoundedRect(row, KomaPaint::Radius::sm, KomaPaint::Radius::sm);
    }
    if (selected) {
        // The current sub tool gets a small pulse marker, not a pulse fill.
        const QRectF marker(row.left() + 2, row.top() + 8, markerWidth, row.height() - 16);
        painter->setBrush(t.pulse);
        painter->drawRoundedRect(marker, markerWidth / 2.0, markerWidth / 2.0);
    }

    const QRect preview(option.rect.left() + KomaPaint::Space::s3,
                        option.rect.top() + (option.rect.height() - previewSize) / 2,
                        previewSize, previewSize);

    QString name;
    const QVariant toolId = index.data(ToolIdRole);
    if (toolId.isValid()) {
        name = index.data(Qt::DisplayRole).toString();
        const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        const int iconSize = KomaPaint::Space::s5;
        const QRect iconRect(preview.center().x() - iconSize / 2 + 1,
                             preview.center().y() - iconSize / 2 + 1,
                             iconSize, iconSize);
        icon.paint(painter, iconRect, Qt::AlignCenter, option.state & QStyle::State_Enabled ? QIcon::Normal : QIcon::Disabled);
    } else {
        name = displayName(index.data(Qt::UserRole + KisAbstractResourceModel::Name).toString());

        // Thumbnails are looked up on the global resource model.
        const int resourceId = index.data(Qt::UserRole + KisAbstractResourceModel::Id).toInt();
        const QString resourceType = index.data(Qt::UserRole + KisAbstractResourceModel::ResourceType).toString();
        KisAllResourcesModel *model = KisResourceModelProvider::resourceModel(resourceType);
        const QModelIndex globalIndex = model ? model->indexForResourceId(resourceId) : QModelIndex();
        const QImage thumbnail = m_thumbnailPainter.getReadyThumbnail(globalIndex, preview.size() * painter->device()->devicePixelRatioF(), option.palette);

        QPainterPath clip;
        clip.addRoundedRect(QRectF(preview), KomaPaint::Radius::xs, KomaPaint::Radius::xs);
        painter->setClipPath(clip);
        painter->fillRect(preview, t.bg);
        if (!thumbnail.isNull()) {
            painter->drawImage(preview, thumbnail);
        }
        painter->setClipping(false);
        painter->setPen(QPen(t.border, 1.0));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(QRectF(preview).adjusted(0.5, 0.5, -0.5, -0.5), KomaPaint::Radius::xs, KomaPaint::Radius::xs);
    }

    const QRect textRect(preview.right() + KomaPaint::Space::s3, option.rect.top(),
                         option.rect.right() - preview.right() - KomaPaint::Space::s4, option.rect.height());
    painter->setPen(option.palette.color(option.state & QStyle::State_Enabled ? QPalette::Active : QPalette::Disabled,
                                         QPalette::Text));
    painter->setFont(option.font);
    painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                      option.fontMetrics.elidedText(name, Qt::ElideRight, textRect.width()));

    painter->restore();
}

QSize SubToolDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Q_UNUSED(index);
    return QSize(option.rect.width(), rowHeight);
}
