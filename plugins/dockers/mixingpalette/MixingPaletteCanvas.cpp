/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "MixingPaletteCanvas.h"

#include <QDir>
#include <QFileInfo>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QTabletEvent>
#include <QtMath>

#include <KoResourcePaths.h>
#include <KomaPaintTokens.h>

namespace
{
const QColor paperColor(Qt::white);

/// How much of the paint under a dab the brush picks up, per dab.
constexpr qreal brushPickup = 0.10;
/// How much fresh paint a smudge picks up, per dab.
constexpr qreal blendPickup = 0.35;
/// Opacity of a smudge dab.
constexpr qreal blendOpacity = 0.55;
/// Dab spacing as a fraction of the dab diameter.
constexpr qreal spacing = 0.15;

QColor mix(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t,
                            a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}
}

MixingPaletteCanvas::MixingPaletteCanvas(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(false);
    setCursor(Qt::CrossCursor);
    setMinimumSize(120, 80);
    load();
}

MixingPaletteCanvas::~MixingPaletteCanvas()
{
}

void MixingPaletteCanvas::setMode(Mode mode)
{
    m_mode = mode;
}

MixingPaletteCanvas::Mode MixingPaletteCanvas::mode() const
{
    return m_mode;
}

void MixingPaletteCanvas::setBrushSize(int size)
{
    m_brushSize = qMax(1, size);
}

int MixingPaletteCanvas::brushSize() const
{
    return m_brushSize;
}

void MixingPaletteCanvas::setPaintColor(const QColor &color)
{
    m_paintColor = color.toRgb();
    m_paintColor.setAlpha(255);
}

void MixingPaletteCanvas::clear()
{
    m_surface.fill(paperColor);
    update();
    save();
}

QSize MixingPaletteCanvas::sizeHint() const
{
    return QSize(260, 180);
}

QString MixingPaletteCanvas::storagePath() const
{
    return KoResourcePaths::getAppDataLocation() + QStringLiteral("/komapaint/mixing-palette.png");
}

void MixingPaletteCanvas::load()
{
    QImage stored(storagePath());
    if (!stored.isNull()) {
        m_surface = stored.convertToFormat(QImage::Format_RGB32);
    }
}

void MixingPaletteCanvas::save() const
{
    if (m_surface.isNull()) {
        return;
    }
    const QString path = storagePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    m_surface.save(path, "PNG");
}

void MixingPaletteCanvas::ensureSurface()
{
    const qreal dpr = devicePixelRatioF();
    const QSize needed = (QSizeF(size()) * dpr).toSize();
    if (needed.isEmpty()) {
        return;
    }
    if (!m_surface.isNull() && m_surface.width() >= needed.width() && m_surface.height() >= needed.height()) {
        return;
    }

    // Grow without losing paint: the old surface stays in the top left.
    QImage grown(needed.expandedTo(m_surface.size()), QImage::Format_RGB32);
    grown.fill(paperColor);
    if (!m_surface.isNull()) {
        QPainter painter(&grown);
        painter.drawImage(0, 0, m_surface);
    }
    m_surface = grown;
}

void MixingPaletteCanvas::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    ensureSurface();
}

void MixingPaletteCanvas::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    ensureSurface();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const KomaPaint::Theme t = KomaPaint::themeFor(palette());
    painter.fillRect(rect(), palette().color(QPalette::Window));

    // The paper is an inset card with rounded corners.
    const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath clip;
    clip.addRoundedRect(card, KomaPaint::Radius::sm, KomaPaint::Radius::sm);
    painter.save();
    painter.setClipPath(clip);
    const qreal dpr = devicePixelRatioF();
    painter.drawImage(QRectF(QPointF(0, 0), QSizeF(m_surface.size()) / dpr), m_surface);
    painter.restore();

    painter.setPen(QPen(t.border, 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(card, KomaPaint::Radius::sm, KomaPaint::Radius::sm);
}

QColor MixingPaletteCanvas::sampleAverage(const QPointF &pos, qreal radius) const
{
    const int r = qMax(1, qRound(radius));
    const int cx = qRound(pos.x());
    const int cy = qRound(pos.y());
    const int step = qMax(1, r / 6);

    qreal red = 0, green = 0, blue = 0;
    int count = 0;
    for (int y = cy - r; y <= cy + r; y += step) {
        if (y < 0 || y >= m_surface.height()) continue;
        const QRgb *line = reinterpret_cast<const QRgb*>(m_surface.constScanLine(y));
        for (int x = cx - r; x <= cx + r; x += step) {
            if (x < 0 || x >= m_surface.width()) continue;
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > r * r) continue;
            red += qRed(line[x]);
            green += qGreen(line[x]);
            blue += qBlue(line[x]);
            ++count;
        }
    }
    if (count == 0) {
        return paperColor;
    }
    return QColor(qRound(red / count), qRound(green / count), qRound(blue / count));
}

void MixingPaletteCanvas::dab(const QPointF &pos, qreal pressure)
{
    const qreal dpr = devicePixelRatioF();
    const qreal radius = qMax<qreal>(1.0, m_brushSize * dpr * 0.5 * (0.35 + 0.65 * pressure));
    const QColor under = sampleAverage(pos, radius * 0.6);

    QColor dabColor;
    qreal opacity = 1.0;
    if (m_mode == Mode::Blend) {
        dabColor = m_loadedColor;
        opacity = blendOpacity;
        m_loadedColor = mix(m_loadedColor, under, blendPickup);
    } else {
        dabColor = m_loadedColor;
        opacity = 0.35 + 0.65 * pressure;
        // A wet brush slowly takes on the colour it passes through.
        m_loadedColor = mix(m_loadedColor, under, brushPickup);
    }

    QRadialGradient gradient(pos, radius);
    QColor core = dabColor;
    core.setAlphaF(opacity);
    QColor edge = dabColor;
    edge.setAlphaF(0.0);
    gradient.setColorAt(0.0, core);
    gradient.setColorAt(0.55, core);
    gradient.setColorAt(1.0, edge);

    QPainter painter(&m_surface);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(gradient);
    painter.drawEllipse(pos, radius, radius);

    const QRectF dirty(pos.x() - radius - 1, pos.y() - radius - 1, 2 * radius + 2, 2 * radius + 2);
    update(QRectF(dirty.topLeft() / dpr, dirty.size() / dpr).toAlignedRect());
}

void MixingPaletteCanvas::beginStroke(const QPointF &pos, qreal pressure, bool pick)
{
    ensureSurface();
    const qreal dpr = devicePixelRatioF();
    const QPointF devicePos = pos * dpr;

    m_pickStroke = pick || m_mode == Mode::Pick;
    m_stroking = true;
    m_lastPos = devicePos;
    m_lastPressure = pressure;
    m_distanceToNextDab = 0.0;

    if (m_pickStroke) {
        Q_EMIT colorPicked(sampleAverage(devicePos, 2.0 * dpr));
        return;
    }

    m_loadedColor = (m_mode == Mode::Blend) ? sampleAverage(devicePos, m_brushSize * dpr * 0.3) : m_paintColor;
    dab(devicePos, pressure);
}

void MixingPaletteCanvas::continueStroke(const QPointF &pos, qreal pressure)
{
    if (!m_stroking) {
        return;
    }
    const qreal dpr = devicePixelRatioF();
    const QPointF devicePos = pos * dpr;

    if (m_pickStroke) {
        Q_EMIT colorPicked(sampleAverage(devicePos, 2.0 * dpr));
        return;
    }

    const QPointF delta = devicePos - m_lastPos;
    const qreal distance = qSqrt(delta.x() * delta.x() + delta.y() * delta.y());
    const qreal step = qMax<qreal>(1.0, m_brushSize * dpr * spacing);

    qreal travelled = m_distanceToNextDab;
    while (travelled + step <= distance) {
        travelled += step;
        const qreal t = travelled / distance;
        dab(m_lastPos + delta * t, m_lastPressure + (pressure - m_lastPressure) * t);
    }
    m_distanceToNextDab = travelled - distance;
    m_lastPos = devicePos;
    m_lastPressure = pressure;
}

void MixingPaletteCanvas::endStroke()
{
    if (!m_stroking) {
        return;
    }
    m_stroking = false;
    if (!m_pickStroke) {
        save();
    }
}

void MixingPaletteCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }
    beginStroke(event->position(), 1.0, event->modifiers() & Qt::ControlModifier);
}

void MixingPaletteCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton) {
        continueStroke(event->position(), 1.0);
    }
}

void MixingPaletteCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        endStroke();
    }
}

void MixingPaletteCanvas::tabletEvent(QTabletEvent *event)
{
    switch (event->type()) {
    case QEvent::TabletPress:
        beginStroke(event->position(), event->pressure(), event->modifiers() & Qt::ControlModifier);
        break;
    case QEvent::TabletMove:
        if (m_stroking) {
            continueStroke(event->position(), event->pressure());
        }
        break;
    case QEvent::TabletRelease:
        endStroke();
        break;
    default:
        break;
    }
    event->accept();
}
