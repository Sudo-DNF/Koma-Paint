/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KomaLineartPreview.h"

#include <QPainter>
#include <QtMath>
#include <cstring>
#include <limits>

namespace
{
constexpr int tileSize = 256;
constexpr int gutter = 1;

int tileIndex(int coordinate)
{
    return coordinate >= 0 ? coordinate / tileSize : -1 - (-(coordinate + 1) / tileSize);
}

QRect tileRect(int x, int y)
{
    return QRect(x * tileSize, y * tileSize, tileSize, tileSize);
}
}

void KomaLineartPreview::clear()
{
    m_tiles.clear();
    m_bounds = QRect();
}

void KomaLineartPreview::update(const QRect &rect, const QImage &image)
{
    if (rect.isEmpty() || image.isNull() || image.size() != rect.size()) {
        return;
    }

    m_bounds |= rect;
    const QRect expanded = rect.adjusted(-gutter, -gutter, gutter, gutter);
    for (int y = tileIndex(expanded.top()); y <= tileIndex(expanded.bottom()); ++y) {
        for (int x = tileIndex(expanded.left()); x <= tileIndex(expanded.right()); ++x) {
            const QRect storage = tileRect(x, y).adjusted(-gutter, -gutter, gutter, gutter);
            const QRect part = storage.intersected(rect);
            if (part.isEmpty()) {
                continue;
            }
            QImage &tile = m_tiles[qMakePair(x, y)];
            if (tile.isNull()) {
                tile = QImage(storage.size(), QImage::Format_ARGB32_Premultiplied);
                tile.fill(Qt::transparent);
            }
            QPainter writer(&tile);
            writer.setCompositionMode(QPainter::CompositionMode_Source);
            writer.drawImage(part.topLeft() - storage.topLeft(), image,
                             QRect(part.topLeft() - rect.topLeft(), part.size()));
        }
    }
}

void KomaLineartPreview::setSparseImage(const QImage &image)
{
    clear();
    if (image.isNull() || image.format() != QImage::Format_ARGB32) {
        return;
    }

    m_bounds = image.rect();
    for (int y = 0; y < image.height(); y += tileSize) {
        for (int x = 0; x < image.width(); x += tileSize) {
            const QRect core = tileRect(x / tileSize, y / tileSize);
            const QRect storage = core.adjusted(-gutter, -gutter, gutter, gutter);
            const QRect source = storage.intersected(image.rect());
            bool visible = false;
            for (int row = source.top(); row <= source.bottom() && !visible; ++row) {
                const QRgb *pixels = reinterpret_cast<const QRgb*>(image.constScanLine(row));
                for (int column = source.left(); column <= source.right(); ++column) {
                    if (pixels[column] != 0) {
                        visible = true;
                        break;
                    }
                }
            }
            if (visible) {
                QImage tile(storage.size(), QImage::Format_ARGB32);
                tile.fill(Qt::transparent);
                for (int row = source.top(); row <= source.bottom(); ++row) {
                    std::memcpy(tile.scanLine(row - storage.y()) + (source.x() - storage.x()) * 4,
                                image.constScanLine(row) + source.x() * 4, source.width() * 4);
                }
                const auto copyEdge = [&](int sx, int sy) {
                    if (storage.contains(sx, sy)) {
                        tile.setPixel(sx - storage.x(), sy - storage.y(),
                                      image.pixel(qBound(0, sx, image.width() - 1),
                                                  qBound(0, sy, image.height() - 1)));
                    }
                };
                if (source.left() == 0) {
                    for (int sy = source.top(); sy <= source.bottom(); ++sy) copyEdge(-1, sy);
                }
                if (source.right() == image.width() - 1) {
                    for (int sy = source.top(); sy <= source.bottom(); ++sy) copyEdge(image.width(), sy);
                }
                if (source.top() == 0) {
                    for (int sx = source.left() - 1; sx <= source.right() + 1; ++sx) copyEdge(sx, -1);
                }
                if (source.bottom() == image.height() - 1) {
                    for (int sx = source.left() - 1; sx <= source.right() + 1; ++sx) copyEdge(sx, image.height());
                }
                m_tiles.insert(qMakePair(x / tileSize, y / tileSize), tile);
            }
        }
    }
}

void KomaLineartPreview::paint(QPainter &painter, qreal xRes, qreal yRes) const
{
    const auto toDocument = [xRes, yRes](const QRect &rect) {
        return QRectF(rect.x() / xRes, rect.y() / yRes,
                      rect.width() / xRes, rect.height() / yRes);
    };
    const bool hasClip = painter.hasClipping();
    const QRectF clip = hasClip ? painter.clipBoundingRect().intersected(
                                      toDocument(m_bounds.adjusted(-gutter, -gutter, gutter, gutter))) : QRectF();
    if (hasClip && clip.isEmpty()) {
        return;
    }
    auto it = m_tiles.cbegin();
    const auto end = m_tiles.cend();
    int lastColumn = std::numeric_limits<int>::max();
    if (hasClip) {
        const int firstColumn = tileIndex(qFloor(clip.left() * xRes));
        lastColumn = tileIndex(qFloor(clip.right() * xRes));
        it = m_tiles.lowerBound(qMakePair(firstColumn, std::numeric_limits<int>::min()));
    }
    for (; it != end && it.key().first <= lastColumn; ++it) {
        const QRect core = tileRect(it.key().first, it.key().second);
        const QRectF target = toDocument(core);
        if (hasClip && !clip.intersects(target)) {
            continue;
        }
        painter.save();
        painter.setClipRect(target, Qt::IntersectClip);
        painter.drawImage(toDocument(core.adjusted(-gutter, -gutter, gutter, gutter)), it.value());
        painter.restore();
    }
}

QImage KomaLineartPreview::toImage(const QSize &size) const
{
    QImage image(size, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    for (auto it = m_tiles.cbegin(); it != m_tiles.cend(); ++it) {
        const QRect core = tileRect(it.key().first, it.key().second);
        const QRect part = core.intersected(image.rect());
        const QRect storage = core.adjusted(-gutter, -gutter, gutter, gutter);
        for (int row = part.top(); row <= part.bottom(); ++row) {
            std::memcpy(image.scanLine(row) + part.x() * 4,
                        it.value().constScanLine(row - storage.y()) + (part.x() - storage.x()) * 4,
                        part.width() * 4);
        }
    }
    return image;
}

QRect KomaLineartPreview::bounds() const
{
    return m_bounds;
}

qint64 KomaLineartPreview::sizeInBytes() const
{
    qint64 size = 0;
    for (auto it = m_tiles.cbegin(); it != m_tiles.cend(); ++it) {
        size += it.value().sizeInBytes();
    }
    return size;
}
