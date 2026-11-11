/*
 *  SPDX-FileCopyrightText: 2014 Boudewijn Rempt <boud@valdyas.org>
 *  SPDX-FileCopyrightText: 2021 Alvin Wong <alvin@alvinhc.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_SPLASH_SCREEN_H
#define KIS_SPLASH_SCREEN_H

#include <QWidget>
#include <QTimer>

class QSvgWidget;
class QLabel;

#include "kritaui_export.h"

/**
 * The Koma Paint splash card: the stacked logo on the brand background,
 * with the version and the current loading step underneath.
 */
class KRITAUI_EXPORT KisSplashScreen : public QWidget
{
    Q_OBJECT
public:
    struct Source {
        QString resourcePath;
        QString artistCredit;
    };

    explicit KisSplashScreen(int height, QWidget *parent = 0,
        Qt::WindowFlags f = Qt::WindowFlags(Qt::SplashScreen | Qt::FramelessWindowHint));

    void repaint();

    void show();

    void setLoadingText(QString text);

    /// Raster version of the splash logo, for platforms that need a bitmap.
    static Source getImageSource();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void layoutCard(const int height);

private:
    QTimer m_timer;
    QSvgWidget *m_logoSvg;
    QLabel *m_loadingTextLabel;
    QLabel *m_footerLabel;
    QString m_versionHtml;
};

#endif // KIS_SPLASH_SCREEN_H
