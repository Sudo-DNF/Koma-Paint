/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KomaPaintToolGroups.h"

#include <klocalizedstring.h>

namespace KomaPaintToolGroups
{

const QStringList &brushToolIds()
{
    static const QStringList ids = {
        QStringLiteral("KritaShape/KisToolBrush"),
        QStringLiteral("KritaShape/KisToolDyna"),
        QStringLiteral("KritaShape/KisToolMultiBrush"),
    };
    return ids;
}

bool isBrushTool(const QString &toolId)
{
    return brushToolIds().contains(toolId);
}

const QVector<ToolGroup> &toolGroups()
{
    static const QVector<ToolGroup> groups = {
        {QStringLiteral("selection"),
         i18nc("Sub tool group", "Selection"),
         {QStringLiteral("KisToolSelectRectangular"),
          QStringLiteral("KisToolSelectElliptical"),
          QStringLiteral("KisToolSelectPolygonal"),
          QStringLiteral("KisToolSelectOutline"),
          QStringLiteral("KisToolSelectPath"),
          QStringLiteral("KisToolSelectMagnetic"),
          QStringLiteral("KisToolSelectContiguous"),
          QStringLiteral("KisToolSelectSimilar")}},
        {QStringLiteral("figure"),
         i18nc("Sub tool group", "Figure"),
         {QStringLiteral("KritaShape/KisToolLine"),
          QStringLiteral("KritaShape/KisToolRectangle"),
          QStringLiteral("KritaShape/KisToolEllipse"),
          QStringLiteral("KisToolPolygon"),
          QStringLiteral("KisToolPolyline"),
          QStringLiteral("KisToolPath"),
          QStringLiteral("KisToolPencil"),
          QStringLiteral("KarbonCalligraphyTool")}},
        {QStringLiteral("correct-line"),
         i18nc("Sub tool group", "Correct Line"),
         {QStringLiteral("KomaCorrectLineControlPoint"),
          QStringLiteral("KomaCorrectLineWidth"),
          QStringLiteral("KomaCorrectLineSimplify"),
          QStringLiteral("KomaCorrectLineConnect")}},
        {QStringLiteral("fill"),
         i18nc("Sub tool group", "Fill"),
         {QStringLiteral("KritaFill/KisToolFill"),
          QStringLiteral("KisToolEncloseAndFill"),
          QStringLiteral("KritaFill/KisToolGradient"),
          QStringLiteral("KritaShape/KisToolLazyBrush"),
          QStringLiteral("KritaShape/KisToolSmartPatch")}},
        {QStringLiteral("operation"),
         i18nc("Sub tool group", "Operation"),
         {QStringLiteral("InteractionTool"),
          QStringLiteral("PathTool"),
          QStringLiteral("KritaTransform/KisToolMove"),
          QStringLiteral("KisToolTransform"),
          QStringLiteral("KisToolCrop"),
          QStringLiteral("KritaShape/KisToolKnife")}},
        {QStringLiteral("text"),
         i18nc("Sub tool group", "Text"),
         {QStringLiteral("SvgTextTool")}},
        {QStringLiteral("ruler"),
         i18nc("Sub tool group", "Ruler"),
         {QStringLiteral("KisAssistantTool"),
          QStringLiteral("KisToolPerspectiveGrid"),
          QStringLiteral("KritaShape/KisToolMeasure"),
          QStringLiteral("ToolReferenceImages")}},
        {QStringLiteral("eyedropper"),
         i18nc("Sub tool group", "Eyedropper"),
         {QStringLiteral("KritaSelected/KisToolColorSampler")}},
        {QStringLiteral("view"),
         i18nc("Sub tool group", "View"),
         {QStringLiteral("ZoomTool"),
          QStringLiteral("PanTool")}},
    };
    return groups;
}

const ToolGroup *groupForTool(const QString &toolId)
{
    for (const ToolGroup &group : toolGroups()) {
        if (group.toolIds.contains(toolId)) {
            return &group;
        }
    }
    return nullptr;
}

const QStringList &preferredTagOrder()
{
    static const QStringList order = {
        QStringLiteral("Pencil"),
        QStringLiteral("Ink"),
        QStringLiteral("Watercolor"),
        QStringLiteral("Thick_Paint"),
        QStringLiteral("Gouache"),
        QStringLiteral("Airbrush"),
        QStringLiteral("Blend"),
        QStringLiteral("Decoration"),
        QStringLiteral("Erasers"),
    };
    return order;
}

const ToolGroup &brushGroup()
{
    static const ToolGroup group = {QStringLiteral("brush"), i18nc("Sub tool group", "Brush"), brushToolIds()};
    return group;
}

const ToolGroup *toolboxGroupForTool(const QString &toolId)
{
    if (isBrushTool(toolId)) {
        return &brushGroup();
    }
    return groupForTool(toolId);
}

}
