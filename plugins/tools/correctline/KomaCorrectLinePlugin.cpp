/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QObject>
#include <QVariantList>

#include <kpluginfactory.h>

#include <KoToolRegistry.h>

#include "KomaCorrectLineTool.h"

class KomaCorrectLinePlugin : public QObject
{
    Q_OBJECT
public:
    KomaCorrectLinePlugin(QObject *parent, const QVariantList &)
        : QObject(parent)
    {
        KoToolRegistry::instance()->add(new KomaCorrectLineToolFactory(KomaCorrectLineTool::Mode::ControlPoint));
        KoToolRegistry::instance()->add(new KomaCorrectLineToolFactory(KomaCorrectLineTool::Mode::AdjustWidth));
        KoToolRegistry::instance()->add(new KomaCorrectLineToolFactory(KomaCorrectLineTool::Mode::Simplify));
        KoToolRegistry::instance()->add(new KomaCorrectLineToolFactory(KomaCorrectLineTool::Mode::Connect));
    }
};

K_PLUGIN_FACTORY_WITH_JSON(KomaCorrectLinePluginFactory, "kritacorrectline.json", registerPlugin<KomaCorrectLinePlugin>();)

#include "KomaCorrectLinePlugin.moc"
