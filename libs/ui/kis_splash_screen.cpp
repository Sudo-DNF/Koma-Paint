/*
 *  SPDX-FileCopyrightText: 2014 Boudewijn Rempt <boud@valdyas.org>
 *  SPDX-FileCopyrightText: 2021 Alvin Wong <alvin@alvinhc.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "kis_splash_screen.h"

#include <QApplication>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QSvgWidget>
#include <QWindow>

#include <kis_icon.h>
#include <klocalizedstring.h>

#include <KomaPaintTokens.h>

namespace
{
// The card keeps the same proportions at every size it is shown at.
constexpr qreal cardAspect = 4.0 / 3.0;
constexpr qreal logoHeightRatio = 0.42;
constexpr qreal logoAspect = 424.13 / 282.7;
}

KisSplashScreen::KisSplashScreen(int height, QWidget *parent, Qt::WindowFlags f)
    : QWidget(parent, f)
    , m_versionHtml(qApp->applicationVersion().toHtmlEscaped())
{
    setWindowTitle(i18n("Koma Paint"));
#ifndef Q_OS_MACOS
    setWindowIcon(KisIconUtils::loadIcon("krita-branding"));
#endif

    if (!parent) {
        // Rounded card corners need a transparent window behind them.
        setAttribute(Qt::WA_TranslucentBackground);
    }

    const QString mutedColor = QColor(KomaPaint::Dark::textMuted).name();

    m_logoSvg = new QSvgWidget(QStringLiteral(":/splash/logo-stacked-reversed.svg"), this);

    m_loadingTextLabel = new QLabel(this);
    m_loadingTextLabel->setTextFormat(Qt::RichText);
    m_loadingTextLabel->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_loadingTextLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; background: transparent; }").arg(mutedColor));

    m_footerLabel = new QLabel(this);
    m_footerLabel->setTextFormat(Qt::PlainText);
    m_footerLabel->setAlignment(Qt::AlignHCenter | Qt::AlignBottom);
    m_footerLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; background: transparent; }").arg(mutedColor));
    m_footerLabel->setText(i18nc("splash screen footer", "Koma Paint is based on Krita."));

    QFont uiFont(QStringLiteral("Inter"));
    uiFont.setPixelSize(13);
    uiFont.setWeight(QFont::Medium);
    m_loadingTextLabel->setFont(uiFont);

    QFont labelFont(QStringLiteral("Inter"));
    labelFont.setPixelSize(12);
    labelFont.setWeight(QFont::DemiBold);
    m_footerLabel->setFont(labelFont);

    layoutCard(height);
    setLoadingText(QString()); // the version string is set here too

    connect(&m_timer, SIGNAL(timeout()), SLOT(raise()));

    m_timer.setSingleShot(true);
    m_timer.start(10);
}

void KisSplashScreen::layoutCard(const int height)
{
    const int width = qRound(height * cardAspect);
    const int margin = KomaPaint::Space::s6;

    setFixedSize(width, height);

    const int logoHeight = qRound(height * logoHeightRatio);
    const int logoWidth = qRound(logoHeight * logoAspect);
    const int logoTop = qRound(height * 0.16);
    m_logoSvg->setFixedSize(logoWidth, logoHeight);
    m_logoSvg->move((width - logoWidth) / 2, logoTop);

    const int textTop = logoTop + logoHeight + KomaPaint::Space::s5;
    m_loadingTextLabel->setFixedWidth(width - 2 * margin);
    m_loadingTextLabel->move(margin, textTop);

    m_footerLabel->setFixedSize(width - 2 * margin, KomaPaint::Space::s5);
    m_footerLabel->move(margin, height - margin - m_footerLabel->height());
}

void KisSplashScreen::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Flat card: brand background, 1px border, large radius. No shadow.
    const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.addRoundedRect(card, KomaPaint::Radius::lg, KomaPaint::Radius::lg);

    painter.fillPath(path, QColor(KomaPaint::Dark::bg));
    painter.setPen(QPen(QColor(KomaPaint::Dark::border), 1.0));
    painter.drawPath(path);
}

void KisSplashScreen::setLoadingText(QString text)
{
    const QString textColor = QColor(KomaPaint::Dark::text).name();
    QString htmlText = QStringLiteral("<span style='color: %3;'>%1</span><br>%2")
            .arg(m_versionHtml, text.toHtmlEscaped(), textColor);
    m_loadingTextLabel->setText(htmlText);
}

KisSplashScreen::Source KisSplashScreen::getImageSource()
{
    return Source{QStringLiteral(":/splash/logo-stacked-reversed.png"), QString()};
}

void KisSplashScreen::repaint()
{
    QWidget::repaint();
    qApp->sendPostedEvents();
}

void KisSplashScreen::show()
{
    if (!this->parentWidget()) {
        this->winId(); // Force creation of native window
        QWindow *windowHandle = this->windowHandle();
        QScreen *screen = windowHandle ? windowHandle->screen() : nullptr;
        if (windowHandle && !screen) {
            // At least on Windows, the window may be created on a non-primary
            // screen with a different scale factor. If we don't explicitly
            // move it to the primary screen, the position will be scaled with
            // the wrong factor and the splash will be offset.
            windowHandle->setScreen(QApplication::primaryScreen());
        }
        if (!screen) {
            screen = QApplication::primaryScreen();
        }
        QRect r(QPoint(), size());
        move(screen->availableGeometry().center() - r.center());
    }
    if (isVisible()) {
        repaint();
    }
    m_timer.setSingleShot(true);
    m_timer.start(1);
    QWidget::show();
}
