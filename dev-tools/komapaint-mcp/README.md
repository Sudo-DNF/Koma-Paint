# Koma Paint MCP server

A [Model Context Protocol](https://modelcontextprotocol.io) server for testing and debugging Koma Paint. An MCP client (Claude Code, or any other) can start Koma Paint, open documents, paint with the real tools, click through dockers and dialogs, take screenshots, check pixels and read the console output.

It has two parts:

- **`komapaint_mcp.py`** (this folder): the MCP server. It talks JSON-RPC over stdio to the client, starts and stops Koma Paint, keeps its console output, and forwards the other tool calls to the bridge. It uses only the Python standard library.
- **Koma MCP Bridge** (`plugins/python/koma_mcp`): a Python plugin inside Koma Paint. It listens on `127.0.0.1` for an authenticated connection and runs each request on the GUI thread, through the Krita scripting API and Qt.

## Setup

Build and install Koma Paint as described in the main README. The bridge plugin is installed with it.

The repository has an `.mcp.json` that registers the server for Claude Code when it is started in the source folder. For another client, run:

```bash
python3 dev-tools/komapaint-mcp/komapaint_mcp.py
```

The server looks for `bin/komapaint` in `$KOMA_PAINT_PREFIX`, then in the `install` folder next to the source folder, then on `PATH`.

## Two ways to connect

**Launch a test instance** with `app_launch`. Koma Paint starts with the bridge enabled for that session only. By default it uses a throwaway profile, so every test starts from the default settings, workspace and resources; the profile is deleted on `app_quit`. Options:

- `headless: true` uses Qt's offscreen platform: no window appears, and everything else works, screenshots included. The window starts at 800×800; use `ui_resize_window` for a real screen size.
- `isolated: false` uses your own profile instead.
- `files` opens files at start-up; `env` adds environment variables.

`app_logs` returns Koma Paint's stdout and stderr: warnings, Python tracebacks and crash output. Pass `since` with the last line number to read only new lines.

**Attach to your own session** with `app_attach`. Enable *Koma MCP Bridge* in *Settings › Configure Koma Paint › Python Plugin Manager* and restart. The plugin writes its port and a random token to `$XDG_RUNTIME_DIR/komapaint-mcp.json` (readable only by you), and `app_attach` reads it from there.

## Tools

| Area | Tools |
| --- | --- |
| Session | `app_launch`, `app_attach`, `app_quit`, `app_status`, `app_logs`, `app_state`, `events_read`, `wait`, `python_exec` |
| Documents | `doc_list`, `doc_new`, `doc_open`, `doc_info`, `doc_activate`, `doc_save`, `doc_export`, `doc_close` |
| Layers | `layer_add`, `layer_set`, `layer_select`, `layer_remove`, `layer_move` |
| Pixels | `paint_stroke`, `pixel_sample`, `pixel_stats`, `render_document` |
| Brush and tools | `brush_get`, `brush_set`, `resource_list`, `tool_list`, `tool_activate`, `action_list`, `action_trigger` |
| Canvas | `canvas_drag`, `canvas_view`, `canvas_map` |
| Interface | `ui_screenshot`, `ui_windows`, `ui_tree`, `ui_find`, `ui_click`, `ui_key`, `ui_type`, `ui_set_value`, `ui_close_popups`, `ui_resize_window`, `docker_list`, `docker_show` |

Each tool's description in `tools/list` explains its parameters. Some notes:

- **Two ways to paint.** `paint_stroke` sends a stroke straight to the brush engine, which is useful when testing brushes. `canvas_drag` sends mouse or pen events to the canvas widget. These go through the input manager and the active tool, so they test what a user does: strokes with smoothing, selections, transforms, `Space` + drag panning, `Ctrl + Alt` + drag brush sizing. With `device: "pen"`, each point can carry pressure, tilt and rotation.
- **Widgets** are addressed by path ids such as `KisMainWindow#MainWindow#1/SubToolDock#SubToolDocker/QWidget[0]/QStackedWidget[0]/QListView[0]`. `ui_find` and `ui_tree` return these ids, and `canvas` means the active view's canvas. `ui_click` with `item` clicks the row of a list or tree view by its text, for example a preset in the Sub Tool panel.
- **Dialogs.** Actions and clicks are delivered from the event loop, so a call returns even when it opens a modal dialog. Inspect the dialog with `ui_windows` or `ui_tree` and close it with `ui_click`.
- **Checking results.** `pixel_stats` reports how many pixels in a region (or on one layer) are not transparent, their bounds and their mean colour. It is the quickest way to confirm that a stroke landed, an eraser removed something or an undo restored the layer. `render_document` and `ui_screenshot` return PNG images; `ui_screenshot` with `target: "screen"` draws open dialogs, popups and floating windows over the main window, as the user sees them.
- **`python_exec`** runs code inside Koma Paint, with `Krita`, `krita` and the bridge `session` already defined. Variables persist between calls, and the value of the last expression is returned.

## Example

A test that the brush tool paints on a new layer and that `Ctrl+Z` undoes the stroke:

```text
app_launch      {"headless": true}
doc_new         {"width": 1000, "height": 1000}
layer_add       {"name": "Ink"}
tool_activate   {"id": "KritaShape/KisToolBrush"}
canvas_drag     {"points": [[300, 300], [500, 500], [700, 700]]}
pixel_stats     {"node": "Ink"}             -> non_transparent > 0
ui_key          {"keys": "Ctrl+Z", "widget": "canvas"}
pixel_stats     {"node": "Ink"}             -> non_transparent == 0
app_quit        {}
```

## Security

The bridge runs any Python the MCP client sends. It accepts connections only from `127.0.0.1`, only with the per-session token, and only while the plugin is loaded. The plugin is off unless you enable it or `app_launch` starts Koma Paint with it. Use it only on machines and sessions you develop on.

## Tests

```bash
python3 -m unittest discover -s dev-tools/komapaint-mcp -v
```

The protocol tests run without Koma Paint. To also run the end-to-end test, which launches Koma Paint headless, set `KOMA_MCP_INTEGRATION=1`. It needs an installed build.
