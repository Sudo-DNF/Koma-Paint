#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Koma Paint contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Model Context Protocol server for testing and debugging Koma Paint.

Speaks MCP (JSON-RPC 2.0 over stdio) to the client and forwards tool calls to
the koma_mcp bridge plugin running inside Koma Paint. It can launch Koma Paint
itself (in a throwaway profile, optionally headless) and keeps the process
output so it can be searched with the app_logs tool, or attach to a Koma Paint
that is already running with the bridge plugin enabled.

Only the Python standard library is used.

Environment:
    KOMA_PAINT_PREFIX   install prefix holding bin/komapaint (default: the
                        `install` directory next to the source checkout, then
                        komapaint on PATH)
"""

import itertools
import json
import os
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from collections import deque

PROTOCOL_VERSIONS = ("2025-06-18", "2025-03-26", "2024-11-05")
SERVER_INFO = {"name": "komapaint", "version": "1.0.0"}
DISCOVERY_FILE_NAME = "komapaint-mcp.json"
SOURCE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOG_LINES = 20000


class ToolError(Exception):
    pass


# -- bridge connection -------------------------------------------------------

class Bridge:
    """A JSON-lines connection to the koma_mcp plugin inside Koma Paint."""

    def __init__(self):
        self._socket = None
        self._reader = None
        self._ids = itertools.count(1)
        self._lock = threading.Lock()
        self.endpoint = None

    @property
    def connected(self):
        return self._socket is not None

    def connect(self, host, port, token, timeout=5.0):
        self.close()
        connection = socket.create_connection((host, port), timeout=timeout)
        self._socket = connection
        self._reader = connection.makefile("rb")
        try:
            self.endpoint = {"host": host, "port": port}
            return self.call("auth", {"token": token}, timeout=timeout)
        except Exception:
            self.close()
            raise

    def close(self):
        if self._reader is not None:
            self._reader.close()
        if self._socket is not None:
            self._socket.close()
        self._socket = self._reader = None
        self.endpoint = None

    def call(self, method, params=None, timeout=60.0):
        if self._socket is None:
            raise ToolError("not connected to Koma Paint; call app_launch or app_attach first")
        with self._lock:
            request_id = next(self._ids)
            line = json.dumps({"id": request_id, "method": method, "params": params or {}}) + "\n"
            try:
                self._socket.settimeout(timeout)
                self._socket.sendall(line.encode("utf-8"))
                while True:
                    raw = self._reader.readline()
                    if not raw:
                        self.close()
                        raise ToolError("Koma Paint closed the bridge connection (did it crash? see app_logs)")
                    reply = json.loads(raw)
                    if reply.get("id") == request_id:
                        break
            except socket.timeout:
                # A late reply would desynchronize the stream, so start over on the next call.
                self.close()
                raise ToolError("Koma Paint did not answer '%s' within %.0f s; the connection was reset. "
                                "A modal dialog or a long operation may be blocking the GUI thread." % (method, timeout))
            except OSError as error:
                self.close()
                raise ToolError("bridge connection failed: %s" % error)
        if "error" in reply:
            error = reply["error"]
            message = error.get("message", "unknown error")
            if error.get("traceback"):
                message += "\n" + error["traceback"]
            raise ToolError(message)
        return reply.get("result")


def runtime_directory():
    return os.environ.get("XDG_RUNTIME_DIR") or tempfile.gettempdir()


def read_discovery_file():
    path = os.path.join(runtime_directory(), DISCOVERY_FILE_NAME)
    try:
        with open(path) as handle:
            info = json.load(handle)
    except (OSError, ValueError):
        return None
    try:
        os.kill(int(info["pid"]), 0)
    except (OSError, KeyError, ValueError):
        return None
    return info


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


# -- the Koma Paint process --------------------------------------------------

class AppProcess:
    def __init__(self):
        self.process = None
        self.profile_dir = None
        self.logs = deque(maxlen=LOG_LINES)
        self._log_counter = itertools.count(1)
        self._log_lock = threading.Lock()
        self.command = None
        self.pump = None

    def running(self):
        return self.process is not None and self.process.poll() is None

    def _pump(self, stream):
        for raw in iter(stream.readline, b""):
            text = raw.decode("utf-8", "replace").rstrip("\n")
            with self._log_lock:
                self.logs.append((next(self._log_counter), time.time(), text))
        stream.close()

    def log_lines(self, since=0, grep=None, limit=200):
        with self._log_lock:
            lines = [entry for entry in self.logs if entry[0] > since]
        if grep:
            needle = grep.lower()
            lines = [entry for entry in lines if needle in entry[2].lower()]
        return lines[-limit:]

    def last_log_number(self):
        with self._log_lock:
            return self.logs[-1][0] if self.logs else 0

    def start(self, prefix, args, env_overrides, isolated, headless):
        executable = os.path.join(prefix, "bin", "komapaint")
        if not os.access(executable, os.X_OK):
            raise ToolError("no executable at %s; set KOMA_PAINT_PREFIX or pass prefix" % executable)
        env = dict(os.environ)
        lib_dir = os.path.join(prefix, "lib64")
        env["PATH"] = os.path.join(prefix, "bin") + os.pathsep + env.get("PATH", "")
        env["LD_LIBRARY_PATH"] = lib_dir + (os.pathsep + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
        # Same environment as run.sh: only Koma Paint's own data and plugins.
        env["XDG_DATA_DIRS"] = os.path.join(prefix, "share")
        env["KRITA_PLUGIN_PATH"] = os.path.join(lib_dir, "kritaplugins")
        env["QT_PLUGIN_PATH"] = os.path.join(lib_dir, "plugins") + (
            os.pathsep + env["QT_PLUGIN_PATH"] if env.get("QT_PLUGIN_PATH") else "")
        enabled = [name for name in env.get("KRITA_ENABLE_PYTHON_PLUGINS", "").split(",") if name]
        env["KRITA_ENABLE_PYTHON_PLUGINS"] = ",".join(sorted(set(enabled + ["koma_mcp"])))
        env["PYTHONUNBUFFERED"] = "1"
        port, token = free_port(), secrets.token_hex(16)
        env["KOMA_MCP_PORT"], env["KOMA_MCP_TOKEN"] = str(port), token
        if isolated:
            self.profile_dir = tempfile.mkdtemp(prefix="komapaint-mcp-profile-")
            for name in ("config", "data", "cache", "state"):
                os.makedirs(os.path.join(self.profile_dir, name))
            env["XDG_CONFIG_HOME"] = os.path.join(self.profile_dir, "config")
            env["XDG_DATA_HOME"] = os.path.join(self.profile_dir, "data")
            env["XDG_CACHE_HOME"] = os.path.join(self.profile_dir, "cache")
            env["XDG_STATE_HOME"] = os.path.join(self.profile_dir, "state")
            # Krita's single-instance check keeps its lock file and local
            # socket in QDir::tempPath(). A private TMPDIR keeps this instance
            # from handing its arguments to another running Koma Paint and
            # exiting straight away.
            tmp_dir = os.path.join(self.profile_dir, "tmp")
            os.makedirs(tmp_dir)
            env["TMPDIR"] = tmp_dir
        if headless:
            # Some start-up code talks to X11 directly whenever DISPLAY is set,
            # so a headless session must not see the desktop's display at all.
            env["QT_QPA_PLATFORM"] = "offscreen"
            for name in ("DISPLAY", "WAYLAND_DISPLAY"):
                env.pop(name, None)
        env.update({str(key): str(value) for key, value in (env_overrides or {}).items()})

        self.logs.clear()
        self.command = [executable, "--nosplash"] + list(args or [])
        self.process = subprocess.Popen(self.command, env=env, stdin=subprocess.DEVNULL,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        start_new_session=True)
        self.pump = threading.Thread(target=self._pump, args=(self.process.stdout,), daemon=True)
        self.pump.start()
        return port, token

    def stop(self, timeout=10.0):
        if self.process is None:
            return None
        if self.process.poll() is None:
            try:
                os.killpg(self.process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                self.process.wait(timeout)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGKILL)
                self.process.wait(5)
        code = self.process.returncode
        self.process = None
        self.cleanup_profile()
        return code

    def cleanup_profile(self):
        if self.profile_dir:
            shutil.rmtree(self.profile_dir, ignore_errors=True)
            self.profile_dir = None


def default_prefix():
    configured = os.environ.get("KOMA_PAINT_PREFIX")
    if configured:
        return configured
    beside_source = os.path.join(os.path.dirname(SOURCE_ROOT), "install")
    if os.path.exists(os.path.join(beside_source, "bin", "komapaint")):
        return beside_source
    found = shutil.which("komapaint")
    if found:
        return os.path.dirname(os.path.dirname(os.path.realpath(found)))
    return beside_source


# -- tools -------------------------------------------------------------------

def schema(properties=None, required=None):
    result = {"type": "object", "properties": properties or {}, "additionalProperties": False}
    if required:
        result["required"] = required
    return result


STR = {"type": "string"}
INT = {"type": "integer"}
NUM = {"type": "number"}
BOOL = {"type": "boolean"}
DOC = {"description": "Document name, file path or index in documents (default: the active document).",
       "anyOf": [STR, INT]}
NODE = {"type": "string", "description": "Layer unique id, 'Group/Layer' path or name (default: the active layer)."}
RECT = {"type": "array", "items": INT, "minItems": 4, "maxItems": 4, "description": "[x, y, width, height] in image pixels"}
POINTS = {"type": "array", "description": "[x, y] or [x, y, pressure] lists, or objects with x, y, pressure, "
                                          "x_tilt, y_tilt, rotation (and time/speed for paint_stroke).",
          "items": {"anyOf": [{"type": "array", "items": NUM, "minItems": 2, "maxItems": 3}, {"type": "object"}]}}
COLOR = {"description": "'#rrggbb', '#aarrggbb', a color name, or [r, g, b(, a)] in 0-1 or 0-255.",
         "anyOf": [STR, {"type": "array", "items": NUM}]}
SETTLE = {"type": "integer", "description": "Milliseconds to let the UI settle before replying (default ~100)."}
WIDGET = {"type": "string", "description": "Widget path id from ui_tree / ui_find, a unique object name, or "
                                          "'canvas' for the active view's canvas."}

# name -> (bridge method or None for local tools, description, input schema)
TOOLS = {
    "app_launch": (None, "Start Koma Paint with the MCP bridge enabled and connect to it. By default it runs "
                   "in a fresh throwaway profile (with its own TMPDIR, so it starts as a separate instance even "
                   "when another Koma Paint is open) so tests start from default settings; pass isolated=false "
                   "to use the user's own profile, which fails while another Koma Paint is running (use "
                   "app_attach then). headless=true uses Qt's offscreen platform (no window).",
                   schema({"isolated": BOOL, "headless": BOOL, "files": {"type": "array", "items": STR},
                           "args": {"type": "array", "items": STR}, "env": {"type": "object"},
                           "prefix": STR, "timeout": NUM})),
    "app_attach": (None, "Connect to a Koma Paint that is already running with the 'Koma MCP Bridge' Python "
                   "plugin enabled (found through its discovery file), or to an explicit port and token.",
                   schema({"port": INT, "token": STR})),
    "app_quit": (None, "Close the Koma Paint started by app_launch (unsaved changes are discarded) and remove "
                 "its throwaway profile. For an attached instance this only disconnects.",
                 schema({"force": BOOL})),
    "app_status": (None, "Whether Koma Paint is running and connected, its pid, exit code and profile.", schema()),
    "app_logs": (None, "Console output (stdout/stderr) of the Koma Paint started by app_launch: warnings, "
                 "Python tracebacks, crash backtraces. Use since=<last line number> to read only new lines.",
                 schema({"since": INT, "grep": STR, "limit": INT})),
    "app_state": ("state", "Overview: version, windows and their views, open documents, the active document, "
                  "current tool, brush (preset, size, opacity, flow, colors) and canvas zoom/rotation.", schema()),
    "events_read": ("events", "Events Koma Paint reported since the bridge started (documents created/saved/"
                    "closed, views, tool and brush preset changes). Pass since=<last seq> to read only new ones.",
                    schema({"since": INT})),
    "wait": ("wait", "Let Koma Paint's event loop run for a while (for animations, delayed updates).",
             schema({"ms": INT})),
    "python_exec": ("python", "Run Python inside Koma Paint (Krita scripting API plus PyQt6) in a persistent "
                    "namespace with Krita, krita and session predefined. Returns printed output and the repr of "
                    "a final expression. The escape hatch for anything the other tools do not cover.",
                    schema({"code": STR}, ["code"])),

    "doc_list": ("documents.list", "List open documents.", schema()),
    "doc_new": ("documents.new", "Create a document and open it in the active window. Koma Paint starts new "
                "documents with a Paper layer and an empty layer above it.",
                schema({"width": INT, "height": INT, "name": STR, "color_model": STR, "color_depth": STR,
                        "profile": STR, "resolution": NUM})),
    "doc_open": ("documents.open", "Open an image file in the active window.", schema({"path": STR}, ["path"])),
    "doc_info": ("documents.info", "A document's properties and full layer tree (top to bottom).",
                 schema({"document": DOC})),
    "doc_activate": ("documents.activate", "Make a document the active one, showing its view.",
                     schema({"document": DOC}, ["document"])),
    "doc_save": ("documents.save", "Save the document, or save it as path (.kra keeps layers).",
                 schema({"document": DOC, "path": STR})),
    "doc_export": ("documents.export", "Export a flattened copy to path (format from the extension) without "
                   "changing the document's file name. Draft layers are left out.",
                   schema({"document": DOC, "path": STR}, ["path"])),
    "doc_close": ("documents.close", "Close a document. Refuses when it has unsaved changes unless discard=true.",
                  schema({"document": DOC, "discard": BOOL})),

    "layer_add": ("layers.add", "Add a layer. type: paint, group, vector, fill, clone, file, filter, "
                  "transparency_mask, selection_mask, colorize_mask, transform_mask. Inserted above the active "
                  "layer unless parent/above are given; the new layer becomes active.",
                  schema({"document": DOC, "type": STR, "name": STR, "parent": NODE, "above": NODE})),
    "layer_set": ("layers.set", "Change layer properties: name, visible, locked, opacity (0-100), blending_mode "
                  "(e.g. normal, multiply, add), alpha_locked, clipped (clip to layer below), color_label (0-8), "
                  "collapsed.", schema({"document": DOC, "node": NODE, "properties": {"type": "object"}},
                                       ["properties"])),
    "layer_select": ("layers.select", "Make a layer the active layer.",
                     schema({"document": DOC, "node": NODE}, ["node"])),
    "layer_remove": ("layers.remove", "Delete a layer.", schema({"document": DOC, "node": NODE}, ["node"])),
    "layer_move": ("layers.move", "Move a layer under parent (default: its current parent), above another "
                   "layer (default: at the bottom).",
                   schema({"document": DOC, "node": NODE, "parent": NODE, "above": NODE}, ["node"])),

    "paint_stroke": ("paint.stroke", "Paint one stroke on a layer with the current brush preset, straight "
                     "through the brush engine (no tool or canvas input involved). Coordinates are image pixels. "
                     "Returns the layer bounds before and after.",
                     schema({"document": DOC, "node": NODE, "points": POINTS, "style": STR}, ["points"])),
    "pixel_sample": ("pixels.sample", "Read one pixel of the composite image, or of a layer when node is given.",
                     schema({"document": DOC, "node": NODE, "x": INT, "y": INT}, ["x", "y"])),
    "pixel_stats": ("pixels.stats", "Coverage statistics of a region (default: whole image) of the composite "
                    "image or of one layer: non-transparent pixel count, coverage, content bounds, mean color.",
                    schema({"document": DOC, "node": NODE, "rect": RECT})),
    "render_document": ("render.document", "Render the composite image or one layer (optionally a region) as "
                        "a PNG image. max_size limits the long edge of the returned image (default 1024); path "
                        "also writes the full-size PNG to disk.",
                        schema({"document": DOC, "node": NODE, "rect": RECT, "max_size": INT, "path": STR})),

    "brush_get": ("brush.get", "The current brush preset, size, opacity, flow, rotation, blending mode, eraser "
                  "mode and foreground/background colors.", schema()),
    "brush_set": ("brush.set", "Change the brush preset (by name), size, opacity (0-1), flow (0-1), rotation, "
                  "blending_mode, eraser_mode, alpha_lock, foreground and background colors.",
                  schema({"preset": STR, "size": NUM, "opacity": NUM, "flow": NUM, "rotation": NUM,
                          "blending_mode": STR, "eraser_mode": BOOL, "alpha_lock": BOOL,
                          "foreground": COLOR, "background": COLOR})),
    "resource_list": ("resources.list", "List resource names of a type: preset, brush, pattern, gradient, "
                      "palette, workspace, ...", schema({"type": STR, "filter": STR, "limit": INT})),

    "action_list": ("actions.list", "Search Koma Paint's actions (menu commands, toggles, shortcuts) by id or "
                    "text.", schema({"filter": STR, "limit": INT})),
    "action_trigger": ("actions.trigger", "Trigger an action by id, e.g. edit_undo, clear, "
                       "koma_quick_access. checked=true/false sets a toggle action to that state. If the "
                       "action opens a dialog the call still returns; inspect it with ui_windows / ui_tree.",
                       schema({"name": STR, "checked": BOOL, "settle_ms": SETTLE}, ["name"])),
    "tool_list": ("tools.list", "List tool actions and the current tool id.", schema()),
    "tool_activate": ("tools.activate", "Switch to a tool by id, e.g. KritaShape/KisToolBrush.",
                      schema({"id": STR, "settle_ms": SETTLE}, ["id"])),

    "canvas_drag": ("canvas.drag", "Drag across the canvas with synthetic mouse or pen input that goes "
                    "through Koma Paint's input manager and the active tool, like a real user (strokes, "
                    "selections, transforms, Space-pan etc.). Points are image pixels by default "
                    "(coordinates='widget' for canvas widget pixels). hold_keys keeps keys such as 'Space' or "
                    "'Ctrl' pressed during the drag. device='pen' sends tablet events with pressure and tilt.",
                    schema({"points": POINTS, "coordinates": {"type": "string", "enum": ["image", "widget"]},
                            "device": {"type": "string", "enum": ["mouse", "pen"]},
                            "button": {"type": "string", "enum": ["left", "right", "middle"]},
                            "modifiers": {"type": "array", "items": STR},
                            "hold_keys": {"type": "array", "items": STR},
                            "spacing": {"type": "number", "description": "Fill in events so consecutive ones "
                                        "are at most this many canvas widget pixels apart, like a real "
                                        "pointer (default 6; 0 sends only the given points)."},
                            "step_ms": INT, "settle_ms": SETTLE}, ["points"])),
    "canvas_view": ("canvas.view", "Read or change the active view: zoom (1.0 = 100%), rotation (degrees), "
                    "mirror, center (image pixels), wrap_around; reset=true restores zoom, rotation and mirror.",
                    schema({"zoom": NUM, "rotation": NUM, "mirror": BOOL, "center": {"type": "array", "items": NUM},
                            "wrap_around": BOOL, "reset": BOOL})),
    "canvas_map": ("canvas.map", "Convert points between image pixels and canvas widget pixels (to='widget' "
                   "or 'image').", schema({"points": {"type": "array", "items": {"type": "array", "items": NUM}},
                                           "to": {"type": "string", "enum": ["widget", "image"]}}, ["points"])),

    "ui_screenshot": ("ui.screenshot", "Capture the UI as a PNG image: target='window' (active window, or the "
                      "open dialog/popup), 'screen' (the main window with open dialogs, popups and floating "
                      "windows drawn over it, as the user sees it), 'canvas', or a widget id. max_size limits "
                      "the long edge (default 1600).",
                      schema({"target": STR, "max_size": INT, "path": STR})),
    "ui_windows": ("ui.windows", "Visible top-level windows and dialogs, the active modal dialog, popup and "
                   "focus widget.", schema()),
    "ui_tree": ("ui.tree", "The widget tree under root (default: the open popup, dialog or active window) with "
                "classes, object names, geometry, texts, values, list items and tab names.",
                schema({"root": WIDGET, "depth": INT, "visible_only": BOOL, "max_nodes": INT})),
    "ui_find": ("ui.find", "Find widgets by class (inherits, e.g. QToolButton), exact object "
                "name and/or text (substring of text, title, tooltip, items or tabs).",
                schema({"class": STR, "name": STR, "text": STR, "visible_only": BOOL, "limit": INT})),
    "ui_click": ("ui.click", "Click a widget (default: its center; x/y are widget-local pixels). For list and "
                 "tree views, item clicks the row whose text matches.",
                 schema({"widget": WIDGET, "x": INT, "y": INT, "item": STR,
                         "button": {"type": "string", "enum": ["left", "right", "middle"]},
                         "modifiers": {"type": "array", "items": STR}, "double": BOOL, "settle_ms": SETTLE},
                        ["widget"])),
    "ui_key": ("ui.key", "Press keys as a QKeySequence string: 'B', 'Ctrl+Z', 'Shift+Q', 'Ctrl+Shift+N, Return'. "
               "Goes to widget, else the focus widget.", schema({"keys": STR, "widget": WIDGET, "settle_ms": SETTLE},
                                                                ["keys"])),
    "ui_type": ("ui.type", "Type text into widget (default: the focus widget).",
                schema({"text": STR, "widget": WIDGET, "settle_ms": SETTLE}, ["text"])),
    "ui_set_value": ("ui.set_value", "Set a widget's value directly: combo box (item text or index), spin box, "
                     "slider, line edit, checkable button, tab bar.",
                     schema({"widget": WIDGET, "value": {}}, ["widget", "value"])),
    "ui_resize_window": ("ui.resize_window", "Resize the active main window (headless windows start at "
                         "800x800; use e.g. 1920x1080 to test the docker layout at a real screen size).",
                         schema({"width": INT, "height": INT, "settle_ms": SETTLE}, ["width", "height"])),
    "ui_close_popups": ("ui.close_popups", "Close open popup menus and popup palettes.", schema()),
    "docker_list": ("dockers.list", "List the dockers (panels) of the active window with visibility and area.",
                    schema()),
    "docker_show": ("dockers.show", "Show (or hide with visible=false) a docker by object name or title.",
                    schema({"name": STR, "visible": BOOL}, ["name"])),
}


class Server:
    def __init__(self):
        self.bridge = Bridge()
        self.app = AppProcess()
        self._write_lock = threading.Lock()

    # -- local tools ----------------------------------------------------

    def _wait_for_bridge(self, port, token, timeout):
        deadline = time.time() + timeout
        last_error = None
        while time.time() < deadline:
            if not self.app.running():
                code = self.app.process.returncode if self.app.process else None
                if self.app.pump:
                    self.app.pump.join(2.0)  # read the last output before quoting it
                output = "\n".join(line for _, _, line in self.app.log_lines(limit=40))
                if code == 0:
                    raise ToolError("Koma Paint exited with code 0 before the bridge came up. Another Koma Paint "
                                    "is probably already running for this user: Krita's single-instance check "
                                    "handed the arguments to it and quit. Use app_attach to drive the running "
                                    "instance, or app_launch with isolated=true (the default) to start a "
                                    "separate one. Last output:\n%s" % output)
                raise ToolError("Koma Paint exited with code %s before the bridge came up. Last output:\n%s"
                                % ("?" if code is None else code, output))
            try:
                return self.bridge.connect("127.0.0.1", port, token)
            except (OSError, ToolError) as error:
                last_error = error
                time.sleep(0.25)
        raise ToolError("the bridge did not come up within %.0f s (%s). Last output:\n%s"
                        % (timeout, last_error, "\n".join(line for _, _, line in self.app.log_lines(limit=40))))

    def tool_app_launch(self, args):
        if self.app.running():
            raise ToolError("Koma Paint is already running (pid %d); call app_quit first" % self.app.process.pid)
        self.bridge.close()
        prefix = args.get("prefix") or default_prefix()
        port, token = self.app.start(prefix, list(args.get("args", [])) + list(args.get("files", [])),
                                     args.get("env"), args.get("isolated", True), args.get("headless", False))
        try:
            hello = self._wait_for_bridge(port, token, float(args.get("timeout", 180)))
        except ToolError:
            self.app.stop()
            raise
        return {"pid": self.app.process.pid, "version": hello.get("version"), "port": port,
                "profile": self.app.profile_dir, "command": self.app.command,
                "log_lines": self.app.last_log_number()}

    def tool_app_attach(self, args):
        if args.get("port"):
            if not args.get("token"):
                raise ToolError("token is required with port")
            port, token = int(args["port"]), args["token"]
        else:
            info = read_discovery_file()
            if info is None:
                raise ToolError("no running Koma Paint with the bridge found. Enable 'Koma MCP Bridge' in "
                                "Settings > Configure Koma Paint > Python Plugin Manager and restart, or use "
                                "app_launch.")
            port, token = int(info["port"]), info["token"]
        hello = self.bridge.connect("127.0.0.1", port, token)
        return {"attached": True, "pid": hello.get("pid"), "version": hello.get("version"), "port": port}

    def tool_app_quit(self, args):
        if not self.app.running():
            self.bridge.close()
            return {"stopped": False, "reason": "no Koma Paint started by this server is running"}
        if self.bridge.connected and not args.get("force"):
            try:
                self.bridge.call("app.quit", {}, timeout=15)
            except ToolError:
                pass
        self.bridge.close()
        if not args.get("force"):
            try:
                self.app.process.wait(20)
            except subprocess.TimeoutExpired:
                pass
        code = self.app.stop()
        return {"stopped": True, "exit_code": code}

    def tool_app_status(self, args):
        process = self.app.process
        return {"running": self.app.running(), "connected": self.bridge.connected,
                "endpoint": self.bridge.endpoint,
                "pid": process.pid if process else None,
                "exit_code": process.poll() if process else None,
                "profile": self.app.profile_dir, "prefix": default_prefix(),
                "log_lines": self.app.last_log_number()}

    def tool_app_logs(self, args):
        lines = self.app.log_lines(int(args.get("since", 0)), args.get("grep"), int(args.get("limit", 200)))
        return {"lines": [{"n": number, "text": text} for number, _, text in lines],
                "last": self.app.last_log_number(),
                "running": self.app.running()}

    # -- MCP ------------------------------------------------------------

    def call_tool(self, name, arguments):
        if name not in TOOLS:
            raise ToolError("unknown tool '%s'" % name)
        method = TOOLS[name][0]
        if method is None:
            return getattr(self, "tool_" + name)(arguments)
        timeout = 60.0
        if name == "wait":
            timeout += arguments.get("ms", 0) / 1000.0
        elif name == "canvas_drag":
            # Points are interpolated inside Koma Paint, so allow for long drags.
            timeout += 60.0
        return self.bridge.call(method, arguments, timeout=timeout)

    @staticmethod
    def to_content(result):
        content = []
        if isinstance(result, dict) and "_images" in result:
            images = result.pop("_images")
            content.append({"type": "text", "text": json.dumps(result, indent=1, ensure_ascii=False)})
            for image in images:
                content.append({"type": "image", "data": image["data"], "mimeType": image["mime"]})
            return content
        return [{"type": "text", "text": json.dumps(result, indent=1, ensure_ascii=False)}]

    def handle(self, message):
        method = message.get("method")
        request_id = message.get("id")
        params = message.get("params") or {}
        if method == "initialize":
            requested = params.get("protocolVersion")
            version = requested if requested in PROTOCOL_VERSIONS else PROTOCOL_VERSIONS[0]
            return {"protocolVersion": version, "serverInfo": SERVER_INFO,
                    "capabilities": {"tools": {"listChanged": False}},
                    "instructions": (
                        "Tools for testing and debugging Koma Paint, a Clip Studio Paint style fork of Krita. "
                        "Start with app_launch (fresh profile) or app_attach (running instance), then app_state. "
                        "Verify visually with ui_screenshot / render_document and numerically with pixel_stats, "
                        "doc_info and ui_tree. app_logs shows console output and Python tracebacks; "
                        "python_exec reaches the full Krita scripting API.")}
        if method == "ping":
            return {}
        if method == "tools/list":
            return {"tools": [{"name": name, "description": description, "inputSchema": input_schema}
                              for name, (_, description, input_schema) in TOOLS.items()]}
        if method == "tools/call":
            name = params.get("name")
            try:
                result = self.call_tool(name, params.get("arguments") or {})
            except ToolError as error:
                return {"content": [{"type": "text", "text": str(error)}], "isError": True}
            except Exception as error:  # A bug in this server; report it to the client.
                return {"content": [{"type": "text", "text": "%s: %s" % (type(error).__name__, error)}],
                        "isError": True}
            return {"content": self.to_content(result)}
        if request_id is None:
            return None  # Notifications such as notifications/initialized need no reply.
        raise LookupError(method)

    def send(self, message):
        with self._write_lock:
            sys.stdout.write(json.dumps(message, ensure_ascii=False) + "\n")
            sys.stdout.flush()

    def serve(self):
        for raw in sys.stdin:
            raw = raw.strip()
            if not raw:
                continue
            try:
                message = json.loads(raw)
            except ValueError as error:
                self.send({"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": str(error)}})
                continue
            request_id = message.get("id")
            try:
                result = self.handle(message)
            except LookupError as error:
                self.send({"jsonrpc": "2.0", "id": request_id,
                           "error": {"code": -32601, "message": "method not found: %s" % error}})
                continue
            if request_id is not None and result is not None:
                self.send({"jsonrpc": "2.0", "id": request_id, "result": result})
        self.shutdown()

    def shutdown(self):
        self.bridge.close()
        if self.app.running():
            self.app.stop()
        self.app.cleanup_profile()


def main():
    server = Server()
    signal.signal(signal.SIGTERM, lambda *_: (server.shutdown(), sys.exit(0)))
    try:
        server.serve()
    except KeyboardInterrupt:
        server.shutdown()


if __name__ == "__main__":
    main()
