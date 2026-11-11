/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "MixingPaletteDock.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QToolButton>
#include <QVBoxLayout>

#include <klocalizedstring.h>

#include <KisMainWindow.h>
#include <KisViewManager.h>
#include <KoCanvasResourceProvider.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KomaPaintTokens.h>
#include <kis_config.h>
#include <kis_icon_utils.h>
#include <kis_slider_spin_box.h>

#include "MixingPaletteCanvas.h"

namespace
{
const QString brushSizeKey = QStringLiteral("MixingPalette/BrushSize");

QToolButton *makeToolButton(QWidget *parent, const QString &toolTip, bool checkable)
{
    QToolButton *button = new QToolButton(parent);
    button->setAutoRaise(true);
    button->setCheckable(checkable);
    button->setIconSize(QSize(16, 16));
    button->setToolTip(toolTip);
    return button;
}
}

MixingPaletteDock::MixingPaletteDock()
    : QDockWidget(i18nc("@title:window docker", "Color Mixing"))
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(KomaPaint::Space::s1, KomaPaint::Space::s1, KomaPaint::Space::s1, KomaPaint::Space::s1);
    layout->setSpacing(KomaPaint::Space::s1);

    QHBoxLayout *tools = new QHBoxLayout();
    tools->setSpacing(KomaPaint::Space::s1);

    m_brushButton = makeToolButton(page, i18nc("@info:tooltip", "Brush: paint with the current color"), true);
    m_blendButton = makeToolButton(page, i18nc("@info:tooltip", "Blend: smudge colors together"), true);
    m_pickButton = makeToolButton(page, i18nc("@info:tooltip", "Pick: use a mixed color (Ctrl+click picks in any mode)"), true);
    m_clearButton = makeToolButton(page, i18nc("@info:tooltip", "Clear the mixing palette"), false);

    QButtonGroup *modes = new QButtonGroup(this);
    modes->setExclusive(true);
    modes->addButton(m_brushButton, int(MixingPaletteCanvas::Mode::Brush));
    modes->addButton(m_blendButton, int(MixingPaletteCanvas::Mode::Blend));
    modes->addButton(m_pickButton, int(MixingPaletteCanvas::Mode::Pick));
    m_brushButton->setChecked(true);

    m_sizeSlider = new KisSliderSpinBox(page);
    m_sizeSlider->setRange(2, 120);
    m_sizeSlider->setPrefix(i18nc("Mixing palette brush size, followed by a number", "Size: "));
    m_sizeSlider->setSuffix(i18nc("Pixel unit", " px"));
    m_sizeSlider->setToolTip(i18nc("@info:tooltip", "Brush size on the mixing palette"));

    tools->addWidget(m_brushButton);
    tools->addWidget(m_blendButton);
    tools->addWidget(m_pickButton);
    tools->addWidget(m_sizeSlider, 1);
    tools->addWidget(m_clearButton);
    layout->addLayout(tools);

    m_surface = new MixingPaletteCanvas(page);
    layout->addWidget(m_surface, 1);
    setWidget(page);

    const int size = KisConfig(true).readEntry<int>(brushSizeKey, 24);
    m_sizeSlider->setValue(size);
    m_surface->setBrushSize(size);

    connect(modes, &QButtonGroup::idClicked, this, [this](int id) {
        m_surface->setMode(MixingPaletteCanvas::Mode(id));
    });
    connect(m_sizeSlider, QOverload<int>::of(&KisSliderSpinBox::valueChanged), this, [this](int value) {
        m_surface->setBrushSize(value);
        KisConfig(false).writeEntry(brushSizeKey, value);
    });
    connect(m_clearButton, &QToolButton::clicked, m_surface, &MixingPaletteCanvas::clear);
    connect(m_surface, &MixingPaletteCanvas::colorPicked, this, &MixingPaletteDock::slotColorPicked);

    slotUpdateIcons();
    setEnabled(false);
}

void MixingPaletteDock::slotUpdateIcons()
{
    m_brushButton->setIcon(KisIconUtils::loadIcon(QStringLiteral("krita_tool_freehand")));
    m_blendButton->setIcon(KisIconUtils::loadIcon(QStringLiteral("colorsmudge")));
    m_pickButton->setIcon(KisIconUtils::loadIcon(QStringLiteral("krita_tool_color_sampler")));
    m_clearButton->setIcon(KisIconUtils::loadIcon(QStringLiteral("edit-delete")));
}

void MixingPaletteDock::setCanvas(KoCanvasBase *canvas)
{
    if (m_canvas) {
        m_canvas->disconnectCanvasObserver(this);
    }

    m_canvas = dynamic_cast<KisCanvas2*>(canvas);
    setEnabled(m_canvas);
    if (!m_canvas || !m_canvas->resourceManager()) {
        return;
    }

    connect(m_canvas->resourceManager(), &KoCanvasResourceProvider::canvasResourceChanged,
            this, &MixingPaletteDock::slotCanvasResourceChanged);
    if (m_canvas->viewManager() && m_canvas->viewManager()->mainWindow()) {
        connect(m_canvas->viewManager()->mainWindow(), &KisMainWindow::themeChanged,
                this, &MixingPaletteDock::slotUpdateIcons, Qt::UniqueConnection);
    }

    m_surface->setPaintColor(m_canvas->resourceManager()->foregroundColor().toQColor());
}

void MixingPaletteDock::unsetCanvas()
{
    if (m_canvas) {
        m_canvas->disconnectCanvasObserver(this);
    }
    m_canvas = nullptr;
    setEnabled(false);
}

void MixingPaletteDock::slotCanvasResourceChanged(int key, const QVariant &value)
{
    if (key == KoCanvasResource::ForegroundColor) {
        m_surface->setPaintColor(value.value<KoColor>().toQColor());
    }
}

void MixingPaletteDock::slotColorPicked(const QColor &color)
{
    if (!m_canvas || !m_canvas->resourceManager()) {
        return;
    }
    m_canvas->resourceManager()->setForegroundColor(KoColor(color, KoColorSpaceRegistry::instance()->rgb8()));
}
