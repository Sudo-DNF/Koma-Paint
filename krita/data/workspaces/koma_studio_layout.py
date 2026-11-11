#!/usr/bin/env python3
#
#  SPDX-FileCopyrightText: 2026 Koma Paint contributors
#
#  SPDX-License-Identifier: GPL-3.0-or-later
#
"""
Generates the Koma Studio workspace (KomaStudio.kws) and the matching
first-run window state in krita/data/kritarc.

Krita workspaces store QMainWindow::saveState() output. This script builds a
stand-in main window whose dock widgets and toolbars carry the same object
names as Koma Paint's, arranges them in the Koma Studio layout, and saves the
resulting state, so the layout is reproducible instead of hand-recorded.

Layout (left to right):
  * Toolbox: a slim single-column tool strip.
  * Left panel: sub tools on top, tool properties beneath, colour wheel at
    the bottom.
  * Canvas.
  * Right panel: navigator on top, layers in the middle, colour mixing
    palette at the bottom.

Usage (from the repository root):
    python3 krita/data/workspaces/koma_studio_layout.py

Requires PyQt6. Runs with the offscreen Qt platform, so no display is needed.
"""

import base64
import os
import re
import sys
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PyQt6.QtCore import Qt  # noqa: E402
from PyQt6.QtWidgets import QApplication, QDockWidget, QMainWindow, QToolBar, QWidget  # noqa: E402

WORKSPACE_NAME = "Koma Studio"

# Reference window size the dock sizes are designed for.
WINDOW_WIDTH = 1920
WINDOW_HEIGHT = 1080

TOOLBOX_WIDTH = 36
LEFT_PANEL_WIDTH = 280
RIGHT_PANEL_WIDTH = 300

# Object names of the docks shown in Koma Studio.
TOOLBOX = "ToolBox"
SUB_TOOLS = "PresetDocker"
TOOL_PROPERTIES = "sharedtooldocker"
COLOR_WHEEL = "WideGamutColorSelector"
NAVIGATOR = "OverviewDocker"
LAYERS = "KisLayerBox"
MIXING_PALETTE = "DigitalMixer"

# Heights within each panel column, top to bottom.
LEFT_COLUMN = [(SUB_TOOLS, 380), (TOOL_PROPERTIES, 360), (COLOR_WHEEL, 260)]
RIGHT_COLUMN = [(NAVIGATOR, 220), (LAYERS, 520), (MIXING_PALETTE, 260)]

# Docks that exist in Koma Paint but start hidden. Listing them keeps a
# workspace switch from leaving docks of the previous layout on screen.
HIDDEN_RIGHT = [
    "ArrangeDocker", "ArtisticColorSelector", "BrushHudDocker", "ChannelDocker",
    "ColorSelectorNg", "ColorSlider", "CompositionDocker", "GamutMask",
    "GridDocker", "HistogramDocker", "History", "ImageDocker", "KisBirdeyeBox",
    "KisHistogramDock", "KisHistogramDocker", "KisPainterlyMixerDocker",
    "KisPaletteDocker", "KisTriangleColorSelector", "KoColorDocker",
    "KoShapeCollectionDocker", "LogDocker", "LutDocker", "PagerDocker",
    "PaletteDocker", "PatternDocker", "PresetHistory", "RecorderDocker",
    "Scripting", "Shadow Properties", "Shape Properties", "ShapeCollectionDocker",
    "ShapeSelector", "Simple Text Editor", "SmallColorSelector",
    "SnapGuide Properties", "SnapGuideConfigWidget", "Snapshot",
    "SpecificColorSelector", "Stroke Properties", "StyleDocker",
    "SvgSymbolCollectionDocker", "TasksetDocker", "TextDocumentInspectionDocker",
    "TextProperties", "ThrottleDocker", "ToolBarDocker", "TouchDocker",
    "comics_project_manager_docker", "lastdocumentsdocker", "palette_docker",
    "pykrita_workflow_buttons", "quick_settings_docker", "scriptdocker",
]
HIDDEN_BOTTOM = [
    "AnimationCurvesDocker", "AnimationDocker", "FlipbookDocker",
    "OnionSkinsDocker", "StoryboardDocker", "TimelineDocker",
]

TOOLBARS = ["mainToolBar", "BrushesAndStuff"]


def make_dock(window, name):
    dock = QDockWidget(name, window)
    dock.setObjectName(name)
    # Without a title bar the stand-in has no minimum width of its own, so
    # the saved widths are the ones designed here.
    dock.setTitleBarWidget(QWidget(dock))
    content = QWidget(dock)
    content.setMinimumSize(0, 0)
    dock.setWidget(content)
    return dock


def build_state():
    app = QApplication.instance() or QApplication(sys.argv)

    window = QMainWindow()
    window.setObjectName("MainWindow")
    window.setCentralWidget(QWidget(window))
    window.resize(WINDOW_WIDTH, WINDOW_HEIGHT)

    for name in TOOLBARS:
        toolbar = QToolBar(name, window)
        toolbar.setObjectName(name)
        window.addToolBar(Qt.ToolBarArea.TopToolBarArea, toolbar)

    docks = {}

    def dock(name):
        if name not in docks:
            docks[name] = make_dock(window, name)
        return docks[name]

    # Left side: toolbox strip, then the panel column beside it.
    window.addDockWidget(Qt.DockWidgetArea.LeftDockWidgetArea, dock(TOOLBOX))
    window.addDockWidget(Qt.DockWidgetArea.LeftDockWidgetArea, dock(LEFT_COLUMN[0][0]))
    window.splitDockWidget(dock(TOOLBOX), dock(LEFT_COLUMN[0][0]), Qt.Orientation.Horizontal)
    for (above, _), (below, _) in zip(LEFT_COLUMN, LEFT_COLUMN[1:]):
        window.splitDockWidget(dock(above), dock(below), Qt.Orientation.Vertical)

    # Right side: one column.
    window.addDockWidget(Qt.DockWidgetArea.RightDockWidgetArea, dock(RIGHT_COLUMN[0][0]))
    for (above, _), (below, _) in zip(RIGHT_COLUMN, RIGHT_COLUMN[1:]):
        window.splitDockWidget(dock(above), dock(below), Qt.Orientation.Vertical)

    for name in HIDDEN_RIGHT:
        window.addDockWidget(Qt.DockWidgetArea.RightDockWidgetArea, dock(name))
        docks[name].hide()
    for name in HIDDEN_BOTTOM:
        window.addDockWidget(Qt.DockWidgetArea.BottomDockWidgetArea, dock(name))
        docks[name].hide()

    window.show()
    app.processEvents()

    window.resizeDocks(
        [dock(TOOLBOX), dock(LEFT_COLUMN[0][0]), dock(RIGHT_COLUMN[0][0])],
        [TOOLBOX_WIDTH, LEFT_PANEL_WIDTH, RIGHT_PANEL_WIDTH],
        Qt.Orientation.Horizontal,
    )
    window.resizeDocks([dock(n) for n, _ in LEFT_COLUMN], [h for _, h in LEFT_COLUMN], Qt.Orientation.Vertical)
    window.resizeDocks([dock(n) for n, _ in RIGHT_COLUMN], [h for _, h in RIGHT_COLUMN], Qt.Orientation.Vertical)
    app.processEvents()

    return bytes(window.saveState())


def write_workspace(path, state):
    encoded = base64.b64encode(state).decode("ascii")
    path.write_text(
        f'<Workspace version="1" name="{WORKSPACE_NAME}">\n'
        f"    <state><![CDATA[{encoded}]]></state>\n"
        "    <settings>\n"
        "    </settings>\n"
        "</Workspace>\n",
        encoding="utf-8",
    )


def update_kritarc(path, state):
    """Replaces the [MainWindow] first-run State in the bundled kritarc."""
    encoded = base64.b64encode(state).decode("ascii")
    text = path.read_text(encoding="utf-8")
    pattern = re.compile(r"(^\[MainWindow\]\n(?:(?!\[).*\n)*?)State=.*$", re.MULTILINE)
    if not pattern.search(text):
        raise SystemExit(f"no [MainWindow] State entry found in {path}")
    text = pattern.sub(lambda m: f"{m.group(1)}State={encoded}", text, count=1)
    path.write_text(text, encoding="utf-8")


def main():
    data_dir = Path(__file__).resolve().parent.parent
    state = build_state()
    write_workspace(data_dir / "workspaces" / "KomaStudio.kws", state)
    update_kritarc(data_dir / "kritarc", state)
    print(f"Wrote {WORKSPACE_NAME} workspace ({len(state)} bytes of window state).")


if __name__ == "__main__":
    main()
