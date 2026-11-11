/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QObject>
#include <QVariantList>

#include <kpluginfactory.h>

#include <KoDockFactoryBase.h>
#include <KoDockRegistry.h>

#include "MixingPaletteDock.h"

class MixingPaletteDockFactory : public KoDockFactoryBase
{
public:
    QString id() const override
    {
        return QStringLiteral("MixingPaletteDocker");
    }

    QDockWidget *createDockWidget() override
    {
        MixingPaletteDock *dock = new MixingPaletteDock();
        dock->setObjectName(id());
        return dock;
    }

    DockPosition defaultDockPosition() const override
    {
        return DockRight;
    }
};

class MixingPalettePlugin : public QObject
{
    Q_OBJECT
public:
    MixingPalettePlugin(QObject *parent, const QVariantList &)
        : QObject(parent)
    {
        KoDockRegistry::instance()->add(new MixingPaletteDockFactory());
    }
};

K_PLUGIN_FACTORY_WITH_JSON(MixingPalettePluginFactory, "krita_mixingpalette.json", registerPlugin<MixingPalettePlugin>();)

#include "MixingPalettePlugin.moc"
