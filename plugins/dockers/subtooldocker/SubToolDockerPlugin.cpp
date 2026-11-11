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

#include "SubToolDock.h"

class SubToolDockFactory : public KoDockFactoryBase
{
public:
    QString id() const override
    {
        return QStringLiteral("SubToolDocker");
    }

    QDockWidget *createDockWidget() override
    {
        SubToolDock *dock = new SubToolDock();
        dock->setObjectName(id());
        return dock;
    }

    DockPosition defaultDockPosition() const override
    {
        return DockLeft;
    }
};

class SubToolDockerPlugin : public QObject
{
    Q_OBJECT
public:
    SubToolDockerPlugin(QObject *parent, const QVariantList &)
        : QObject(parent)
    {
        KoDockRegistry::instance()->add(new SubToolDockFactory());
    }
};

K_PLUGIN_FACTORY_WITH_JSON(SubToolDockerPluginFactory, "krita_subtooldocker.json", registerPlugin<SubToolDockerPlugin>();)

#include "SubToolDockerPlugin.moc"
