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

#include "QuickAccessDock.h"

class QuickAccessDockFactory : public KoDockFactoryBase
{
public:
    QString id() const override
    {
        return QStringLiteral("QuickAccessDocker");
    }

    QDockWidget *createDockWidget() override
    {
        QuickAccessDock *dock = new QuickAccessDock();
        dock->setObjectName(id());
        return dock;
    }

    DockPosition defaultDockPosition() const override
    {
        return DockRight;
    }
};

class QuickAccessPlugin : public QObject
{
    Q_OBJECT
public:
    QuickAccessPlugin(QObject *parent, const QVariantList &)
        : QObject(parent)
    {
        KoDockRegistry::instance()->add(new QuickAccessDockFactory());
    }
};

K_PLUGIN_FACTORY_WITH_JSON(QuickAccessPluginFactory, "krita_quickaccess.json", registerPlugin<QuickAccessPlugin>();)

#include "QuickAccessPlugin.moc"
