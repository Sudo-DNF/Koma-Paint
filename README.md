# Koma Paint

Koma Paint is a digital painting and comic studio. Its workspace is built around how illustrators work: tools and their variants on the left, layers and navigation on the right, and the canvas in the middle with nothing in the way.

Koma Paint is based on Krita. It is not affiliated with or endorsed by the Krita Foundation.

Development used AI coding assistants; the app itself contains no generative AI features or MCP support.

## What's different from Krita

- **Koma Studio workspace.** A slim tool strip sits on the far left. Next to it is a left panel with sub tools, tool properties and a colour wheel. The right panel holds the navigator, layers and a colour mixer.
- **Sub Tool panel.** Brush tools show their brushes in groups such as Pencil, Ink, Watercolor, Thick Paint, Gouache, Airbrush, Blend and Decoration, each with a stroke preview. Other tools list their variants. For example, the selection tool holds rectangle, ellipse, lasso, magic wand and more.
- **Layers panel.** Each row shows the layer's opacity and blending mode above its name. One-click toggles above the list:
  - clip to the layer below;
  - lock transparent pixels;
  - lock the layer;
  - draft layer;
  - layer colour.
- **Draft layers.** A draft layer shows on the canvas but is left out when you export, so sketches, guides and notes stay out of the final image.
- **Paper.** New canvases start on a white Paper layer, with an empty layer above it to draw on.
- **Canvas controls.**
  - Space + drag pans.
  - Ctrl + Space + drag zooms smoothly.
  - Shift + Space + drag rotates.
  - Ctrl + Alt + drag resizes the brush.

  The default keyboard shortcuts follow Clip Studio Paint.
- **Look and feel.** Dark and light Koma Paint themes use flat panels, rounded controls and the Inter and Poppins typefaces. The pink accent is kept for what matters: the active tool, the primary action and the current selection.

## Status

Koma Paint is in active development. These are planned next:

- a tactile colour mixing palette;
- a Quick Access palette and a redesigned pop-up palette;
- an icon and wording pass;
- vector lineart layers, where every ink stroke stays editable.

Files are saved in Krita's `.kra` format, so documents open in both applications. Draft layers are a Koma Paint addition; Krita keeps them as normal layers.

## Building

Koma Paint builds the same way as Krita, with Qt 6 and KDE Frameworks 6. On Fedora:

```bash
sudo dnf builddep krita
sudo dnf install extra-cmake-modules ccache
```

Koma Paint also needs the header-only libraries [Immer](https://github.com/arximboldi/immer), [Zug](https://github.com/arximboldi/zug) and [Lager](https://github.com/arximboldi/lager). Install them to a local prefix and pass that prefix as `CMAKE_PREFIX_PATH`. Then configure and build:

```bash
cmake -S . -B ../build -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX="$PWD/../install" \
    -DCMAKE_PREFIX_PATH=/path/to/deps/prefix \
    -DBUILD_WITH_QT6=ON -DALLOW_UNSTABLE=QT6 \
    -DBUILD_TESTING=OFF
ninja -C ../build install
```

To run Koma Paint from the install prefix, point the library and data paths at it:

```bash
PREFIX="$PWD/../install"
LD_LIBRARY_PATH="$PREFIX/lib64" \
XDG_DATA_DIRS="$PREFIX/share:/usr/local/share:/usr/share" \
KRITA_PLUGIN_PATH="$PREFIX/lib64/kritaplugins" \
"$PREFIX/bin/komapaint"
```

Settings and resources are shared with Krita's locations (`kritarc` and the Krita resource folder). The first time Koma Paint starts, it switches to its own theme, workspace and controls.

For everything else about building, see the [Krita build documentation](https://docs.krita.org/en/untranslatable_pages/building_krita.html). It applies to Koma Paint as well.

## Learning to paint with it

Koma Paint uses Krita's painting engine, so most of the [Krita manual](https://docs.krita.org/en/user_manual.html) applies. That covers brushes, layers, filters, selections and animation.

## Licence

Koma Paint is free software, released under the GNU General Public License, version 3 or later, like Krita. Individual files carry their own SPDX headers, and the licence texts are in [LICENSES](LICENSES) and [COPYING](COPYING). Bundled fonts (Inter and Poppins) are under the SIL Open Font License; see `krita/data/uifonts`.

## Credits

Koma Paint stands on the work of the Krita developers and the KDE community. The full list of Krita contributors is in Help > About Koma Paint. Krita lives at [krita.org](https://krita.org) and [invent.kde.org/graphics/krita](https://invent.kde.org/graphics/krita).
