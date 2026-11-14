# SPDX-FileCopyrightText: 2026 Koma Paint contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bridge commands: documents, layers, painting, brushes, tools and actions.

Every command takes a dict of JSON parameters and returns a JSON-compatible
value, or a Deferred when its effect has to be delivered from the event loop
(actions and input that may open modal dialogs, timed input, waits).
"""

import ast
import contextlib
import io
import time
import traceback
from collections import deque

import krita
from krita import InfoObject, Krita, ManagedColor, StrokePoint
from PyQt6.QtCore import QPointF, QUuid
from PyQt6.QtGui import QColor
from PyQt6.QtWidgets import QAbstractButton

from . import imaging


class CommandError(Exception):
    """An error caused by the request, reported to the client without a traceback."""


class Deferred:
    """Work that has to run from the event loop rather than inside the request.

    steps:     list of (delay_ms, callable); each delay is measured from the
               previous step.
    finish:    callable returning the reply, run settle_ms after the last step.
    on_abort:  callable run when a step raises, to undo partial input state
               (for example releasing a held mouse button).
    """

    def __init__(self, steps, finish, settle_ms=0, on_abort=None):
        self.steps = steps
        self.finish = finish
        self.settle_ms = settle_ms
        self._on_abort = on_abort

    def abort(self):
        if self._on_abort is not None:
            try:
                self._on_abort()
            except Exception:
                traceback.print_exc()


COMMANDS = {}


def command(name):
    def register(function):
        COMMANDS[name] = function
        return function
    return register


# -- parameter helpers -------------------------------------------------------

_MISSING = object()


def param(params, key, kind=None, default=_MISSING):
    if key not in params or params[key] is None:
        if default is _MISSING:
            raise CommandError("missing parameter '%s'" % key)
        return default
    value = params[key]
    if kind is float and isinstance(value, int) and not isinstance(value, bool):
        value = float(value)
    if kind is not None and not isinstance(value, kind):
        raise CommandError("parameter '%s' must be %s" % (key, getattr(kind, "__name__", kind)))
    return value


def app():
    return Krita.instance()


def active_window():
    window = app().activeWindow()
    if window is None:
        windows = app().windows()
        if not windows:
            raise CommandError("Koma Paint has no main window")
        window = windows[0]
    return window


def active_view(required=True):
    window = app().activeWindow()
    view = window.activeView() if window is not None else None
    if view is None and required:
        raise CommandError("no active view; open or create a document first")
    return view


def resolve_document(params):
    """The document named by params['document'] (name, file name or index), else the active one."""
    selector = params.get("document")
    documents = app().documents()
    if selector is None:
        document = app().activeDocument()
        if document is None:
            raise CommandError("no active document; open or create a document first")
        return document
    if isinstance(selector, int) and not isinstance(selector, bool):
        if 0 <= selector < len(documents):
            return documents[selector]
        raise CommandError("document index %d out of range (%d open)" % (selector, len(documents)))
    for document in documents:
        if selector in (document.name(), document.fileName()):
            return document
    raise CommandError("no open document named '%s'" % selector)


def walk_nodes(node):
    for child in node.childNodes():
        yield child
        yield from walk_nodes(child)


def resolve_node(document, selector, required=True):
    """Find a node by unique id, by 'Group/Child' path or by name; None selects the active node."""
    if selector is None:
        node = document.activeNode()
        if node is None and required:
            raise CommandError("the document has no active node")
        return node
    if not isinstance(selector, str):
        raise CommandError("node selectors are strings (unique id, path or name)")
    uuid = QUuid.fromString("{%s}" % selector.strip("{}"))
    if not uuid.isNull():
        node = document.nodeByUniqueID(uuid)
        if node is not None:
            return node
    if "/" in selector:
        node = document.rootNode()
        for part in selector.split("/"):
            node = next((child for child in node.childNodes() if child.name() == part), None)
            if node is None:
                break
        if node is not None:
            return node
    for node in walk_nodes(document.rootNode()):
        if node.name() == selector:
            return node
    if required:
        raise CommandError("no node '%s' in document '%s'" % (selector, document.name()))
    return None


def rect_json(rect):
    return [rect.x(), rect.y(), rect.width(), rect.height()]


def node_json(node, active_id=None, recursive=True):
    info = {
        "name": node.name(),
        "id": node.uniqueId().toString(),
        "type": node.type(),
        "visible": node.visible(),
        "locked": node.locked(),
        "opacity": round(node.opacity() * 100.0 / 255.0, 1),
        "blending_mode": node.blendingMode(),
        "alpha_locked": node.alphaLocked(),
        "clipped": node.inheritAlpha(),
        "color_label": node.colorLabel(),
        "bounds": rect_json(node.bounds()),
        "color_model": node.colorModel(),
        "color_depth": node.colorDepth(),
    }
    if active_id is not None and info["id"] == active_id:
        info["active"] = True
    children = node.childNodes()
    if children:
        info["collapsed"] = node.collapsed()
        if recursive:
            # Krita lists children bottom to top; report them top to bottom like the Layers panel.
            info["children"] = [node_json(child, active_id) for child in reversed(children)]
    return info


def document_json(document, with_layers=False):
    info = {
        "name": document.name(),
        "file": document.fileName(),
        "width": document.width(),
        "height": document.height(),
        "resolution": document.resolution(),
        "color_model": document.colorModel(),
        "color_depth": document.colorDepth(),
        "color_profile": document.colorProfile(),
        "modified": document.modified(),
    }
    active = document.activeNode()
    info["active_node"] = active.name() if active is not None else None
    if with_layers:
        active_id = active.uniqueId().toString() if active is not None else None
        info["layers"] = [node_json(node, active_id) for node in reversed(document.topLevelNodes())]
    return info


def color_json(managed_color, canvas=None):
    if managed_color is None:
        return None
    qcolor = managed_color.colorForCanvas(canvas) if canvas is not None else managed_color.toQColor()
    return {
        "hex": qcolor.name(QColor.NameFormat.HexArgb),
        "model": managed_color.colorModel(),
        "depth": managed_color.colorDepth(),
        "components": [round(c, 5) for c in managed_color.components()],
    }


def parse_color(value):
    color = QColor(value) if isinstance(value, str) else None
    if isinstance(value, (list, tuple)) and len(value) in (3, 4):
        channels = [float(c) for c in value]
        scale = 255.0 if all(c <= 1.0 for c in channels) else 1.0
        color = QColor(*[max(0, min(255, round(c * scale))) for c in channels])
    if color is None or not color.isValid():
        raise CommandError("colors are '#rrggbb' / '#aarrggbb' strings or [r, g, b(, a)] lists")
    return color


def wait_for_document(document):
    document.waitForDone()
    document.refreshProjection()


class Session:
    """State shared by all connections: event log, Python console and tracked tool."""

    def __init__(self):
        self.events = deque(maxlen=1000)
        self._event_sequence = 0
        self._views = []
        self.console_namespace = {"Krita": Krita, "krita": krita, "session": self}
        notifier = app().notifier()
        notifier.setActive(True)
        notifier.imageCreated.connect(lambda document: self.log_event("image_created", name=document.name()))
        notifier.imageSaved.connect(lambda name: self.log_event("image_saved", file=name))
        notifier.imageClosed.connect(lambda name: self.log_event("image_closed", file=name))
        notifier.viewCreated.connect(self._on_view_created)
        notifier.viewClosed.connect(self._on_view_closed)
        notifier.windowCreated.connect(lambda: self.log_event("window_created"))
        notifier.applicationClosing.connect(lambda: self.log_event("application_closing"))

    # -- events ---------------------------------------------------------

    def log_event(self, kind, **data):
        self._event_sequence += 1
        entry = {"seq": self._event_sequence, "time": round(time.time(), 3), "event": kind}
        entry.update(data)
        self.events.append(entry)

    def _on_view_created(self, view):
        # The wrapper is kept alive so its signals stay connected for the view's lifetime.
        self._views.append(view)
        document = view.document()
        self.log_event("view_created", document=document.name() if document is not None else None)
        view.currentToolChanged.connect(self._on_tool_changed)
        view.currentBrushPresetChanged.connect(
            lambda: self.log_event("brush_preset_changed", preset=self._preset_name()))

    def _on_view_closed(self, view):
        self.log_event("view_closed")
        remaining = []
        for kept in self._views:
            try:
                if kept == view:
                    continue
            except RuntimeError:
                continue  # Krita already deleted this wrapper's view.
            remaining.append(kept)
        self._views = remaining

    def _on_tool_changed(self, tool_id):
        self.log_event("tool_changed", tool=tool_id)

    def _preset_name(self):
        view = active_view(required=False)
        preset = view.currentBrushPreset() if view is not None else None
        return preset.name() if preset is not None else None

    # -- dispatch -------------------------------------------------------

    def dispatch(self, method, params):
        handler = COMMANDS.get(method)
        if handler is None:
            raise CommandError("unknown method '%s'" % method)
        return handler(self, params)


# -- application -------------------------------------------------------------

@command("ping")
def cmd_ping(session, params):
    return {"pong": True, "time": time.time()}


@command("state")
def cmd_state(session, params):
    krita = app()
    windows = []
    for window in krita.windows():
        qwindow = window.qwindow()
        views = []
        for view in window.views():
            document = view.document()
            views.append({"document": document.name() if document is not None else None,
                          "visible": view.visible()})
        windows.append({"title": qwindow.windowTitle(), "views": views,
                        "geometry": rect_json(qwindow.geometry()),
                        "active": window == krita.activeWindow()})
    state = {
        "version": krita.version(),
        "windows": windows,
        "documents": [document_json(document) for document in krita.documents()],
        "current_tool": current_tool_id(),
    }
    document = krita.activeDocument()
    state["active_document"] = document.name() if document is not None else None
    view = active_view(required=False)
    canvas = view.canvas() if view is not None else None
    if canvas is not None:
        state["brush"] = brush_json(view)
        state["canvas"] = {"zoom": canvas.zoomLevel(), "rotation": canvas.rotation(),
                           "mirror": canvas.mirror(), "wrap_around": canvas.wrapAroundMode()}
    return state


@command("events")
def cmd_events(session, params):
    since = param(params, "since", int, 0)
    return {"events": [entry for entry in session.events if entry["seq"] > since],
            "last_seq": session._event_sequence}


@command("wait")
def cmd_wait(session, params):
    milliseconds = param(params, "ms", int, 250)
    return Deferred([], lambda: {"waited_ms": milliseconds}, settle_ms=milliseconds)


@command("python")
def cmd_python(session, params):
    """Run code in a persistent namespace; the value of a final expression is returned."""
    code = param(params, "code", str)
    tree = ast.parse(code, "<koma-mcp>", "exec")
    trailing = None
    if tree.body and isinstance(tree.body[-1], ast.Expr):
        trailing = ast.Expression(tree.body.pop().value)
    output = io.StringIO()
    value = None
    with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
        exec(compile(tree, "<koma-mcp>", "exec"), session.console_namespace)
        if trailing is not None:
            value = eval(compile(trailing, "<koma-mcp>", "eval"), session.console_namespace)
    session.console_namespace["_"] = value
    return {"output": output.getvalue(), "value": None if value is None else repr(value)}


# -- documents ---------------------------------------------------------------

@command("documents.list")
def cmd_documents_list(session, params):
    active = app().activeDocument()
    documents = []
    for index, document in enumerate(app().documents()):
        info = document_json(document)
        info["index"] = index
        info["active"] = active is not None and document.rootNode() == active.rootNode()
        documents.append(info)
    return {"documents": documents}


@command("documents.new")
def cmd_documents_new(session, params):
    document = app().createDocument(
        param(params, "width", int, 1920), param(params, "height", int, 1080),
        param(params, "name", str, "Untitled"), param(params, "color_model", str, "RGBA"),
        param(params, "color_depth", str, "U8"), param(params, "profile", str, ""),
        param(params, "resolution", float, 300.0))
    if document is None:
        raise CommandError("Koma Paint could not create the document")
    active_window().addView(document)
    app().setActiveDocument(document)
    wait_for_document(document)
    return document_json(document, with_layers=True)


@command("documents.open")
def cmd_documents_open(session, params):
    path = param(params, "path", str)
    document = app().openDocument(path)
    if document is None:
        raise CommandError("Koma Paint could not open '%s'" % path)
    active_window().addView(document)
    app().setActiveDocument(document)
    wait_for_document(document)
    return document_json(document, with_layers=True)


@command("documents.info")
def cmd_documents_info(session, params):
    return document_json(resolve_document(params), with_layers=True)


@command("documents.activate")
def cmd_documents_activate(session, params):
    document = resolve_document(params)
    window = active_window()
    for view in window.views():
        if view.document() is not None and view.document().rootNode() == document.rootNode():
            window.showView(view)
            break
    else:
        window.addView(document)
    app().setActiveDocument(document)
    return document_json(document)


@command("documents.save")
def cmd_documents_save(session, params):
    document = resolve_document(params)
    path = param(params, "path", str, None)
    document.setBatchmode(True)
    try:
        saved = document.saveAs(path) if path else document.save()
    finally:
        document.setBatchmode(False)
    if not saved:
        raise CommandError("saving '%s' failed" % (path or document.fileName()))
    return {"saved": path or document.fileName()}


@command("documents.export")
def cmd_documents_export(session, params):
    document = resolve_document(params)
    path = param(params, "path", str)
    wait_for_document(document)
    document.setBatchmode(True)
    try:
        exported = document.exportImage(path, InfoObject())
    finally:
        document.setBatchmode(False)
    if not exported:
        raise CommandError("exporting to '%s' failed" % path)
    return {"exported": path}


@command("documents.close")
def cmd_documents_close(session, params):
    document = resolve_document(params)
    name = document.name()
    if document.modified() and not param(params, "discard", bool, False):
        raise CommandError("'%s' has unsaved changes; pass discard=true to close it anyway" % name)
    document.setModified(False)
    if not document.close():
        raise CommandError("closing '%s' failed" % name)
    return {"closed": name}


# -- layers ------------------------------------------------------------------

LAYER_TYPES = {"paint": "paintlayer", "group": "grouplayer", "vector": "vectorlayer",
               "fill": "filllayer", "clone": "clonelayer", "file": "filelayer",
               "filter": "filterlayer", "transparency_mask": "transparencymask",
               "selection_mask": "selectionmask", "colorize_mask": "colorizemask",
               "transform_mask": "transformmask"}


@command("layers.add")
def cmd_layers_add(session, params):
    document = resolve_document(params)
    kind = param(params, "type", str, "paint")
    node_type = LAYER_TYPES.get(kind, kind)
    node = document.createNode(param(params, "name", str, "Layer"), node_type)
    if node is None:
        raise CommandError("cannot create a node of type '%s'" % kind)
    parent_selector = params.get("parent")
    parent = resolve_node(document, parent_selector) if parent_selector else document.rootNode()
    above_selector = params.get("above")
    above = resolve_node(document, above_selector) if above_selector else None
    if above is None and not parent_selector:
        # Like the Layers panel: insert above the active layer when no position is given.
        active = document.activeNode()
        if active is not None and active.parentNode() is not None:
            parent, above = active.parentNode(), active
    if not parent.addChildNode(node, above):
        raise CommandError("could not insert the node")
    document.setActiveNode(node)
    wait_for_document(document)
    return node_json(node)


@command("layers.set")
def cmd_layers_set(session, params):
    document = resolve_document(params)
    node = resolve_node(document, params.get("node"))
    setters = {
        "name": lambda value: node.setName(str(value)),
        "visible": lambda value: node.setVisible(bool(value)),
        "locked": lambda value: node.setLocked(bool(value)),
        "opacity": lambda value: node.setOpacity(max(0, min(255, round(float(value) * 255.0 / 100.0)))),
        "blending_mode": lambda value: node.setBlendingMode(str(value)),
        "alpha_locked": lambda value: node.setAlphaLocked(bool(value)),
        "clipped": lambda value: node.setInheritAlpha(bool(value)),
        "color_label": lambda value: node.setColorLabel(int(value)),
        "collapsed": lambda value: node.setCollapsed(bool(value)),
    }
    properties = param(params, "properties", dict)
    unknown = sorted(set(properties) - set(setters))
    if unknown:
        raise CommandError("unknown layer properties %s; known: %s" % (unknown, sorted(setters)))
    for key, value in properties.items():
        setters[key](value)
    wait_for_document(document)
    return node_json(node, recursive=False)


@command("layers.select")
def cmd_layers_select(session, params):
    document = resolve_document(params)
    node = resolve_node(document, param(params, "node", str))
    document.setActiveNode(node)
    return node_json(node, recursive=False)


@command("layers.remove")
def cmd_layers_remove(session, params):
    document = resolve_document(params)
    node = resolve_node(document, param(params, "node", str))
    name = node.name()
    if not node.remove():
        raise CommandError("could not remove '%s'" % name)
    wait_for_document(document)
    return {"removed": name}


@command("layers.move")
def cmd_layers_move(session, params):
    document = resolve_document(params)
    node = resolve_node(document, param(params, "node", str))
    parent_selector = params.get("parent")
    parent = resolve_node(document, parent_selector) if parent_selector else node.parentNode()
    above_selector = params.get("above")
    above = resolve_node(document, above_selector) if above_selector else None
    clone = node.duplicate()
    clone.setName(node.name())
    if not parent.addChildNode(clone, above):
        raise CommandError("could not move the node")
    node.remove()
    document.setActiveNode(clone)
    wait_for_document(document)
    return node_json(clone, recursive=False)


# -- painting and pixels -----------------------------------------------------

def _stroke_points(points):
    if not isinstance(points, list) or not points:
        raise CommandError("points must be a non-empty list of [x, y] or {x, y, pressure, ...}")
    result = []
    for index, point in enumerate(points):
        if isinstance(point, (list, tuple)):
            x, y = float(point[0]), float(point[1])
            pressure = float(point[2]) if len(point) > 2 else 1.0
            extra = {}
        elif isinstance(point, dict):
            x, y = float(point["x"]), float(point["y"])
            pressure = float(point.get("pressure", 1.0))
            extra = point
        else:
            raise CommandError("point %d is neither a list nor an object" % index)
        result.append(StrokePoint(
            QPointF(x, y), pressure,
            float(extra.get("x_tilt", 0.0)), float(extra.get("y_tilt", 0.0)),
            float(extra.get("rotation", 0.0)), float(extra.get("tangential_pressure", 0.0)),
            float(extra.get("time", index * 8.0)), float(extra.get("speed", 0.5))))
    return result


@command("paint.stroke")
def cmd_paint_stroke(session, params):
    """Paint one brush stroke with the current preset through the engine (no UI input)."""
    document = resolve_document(params)
    node = resolve_node(document, params.get("node"))
    ability = node.paintAbility()
    if ability != "PAINT":
        raise CommandError("'%s' cannot be painted with the current preset (%s)" % (node.name(), ability))
    before = node.bounds()
    node.paintPolylineStroke(_stroke_points(param(params, "points", list)),
                             param(params, "style", str, "ForegroundColor"))
    wait_for_document(document)
    return {"node": node.name(), "bounds_before": rect_json(before), "bounds_after": rect_json(node.bounds())}


@command("pixels.sample")
def cmd_pixels_sample(session, params):
    document = resolve_document(params)
    x, y = param(params, "x", int), param(params, "y", int)
    wait_for_document(document)
    selector = params.get("node")
    if selector is None:
        image = document.projection(x, y, 1, 1)
        color = image.pixelColor(0, 0)
        return {"source": "projection", "hex": color.name(QColor.NameFormat.HexArgb),
                "rgba8": [color.red(), color.green(), color.blue(), color.alpha()]}
    node = resolve_node(document, selector)
    data = bytes(node.pixelData(x, y, 1, 1))
    return {"source": node.name(),
            "pixel": imaging.decode_pixel(data, node.colorModel(), node.colorDepth())}


@command("pixels.stats")
def cmd_pixels_stats(session, params):
    """Coverage statistics for a region of the composite image or of one node."""
    document = resolve_document(params)
    wait_for_document(document)
    region = params.get("rect") or [0, 0, document.width(), document.height()]
    x, y, width, height = [int(value) for value in region]
    selector = params.get("node")
    if selector is None:
        image = document.projection(x, y, width, height)
        return dict(imaging.qimage_stats(image, x, y), source="projection")
    node = resolve_node(document, selector)
    image = imaging.node_region_image(node, x, y, width, height)
    return dict(imaging.qimage_stats(image, x, y), source=node.name(),
                node_bounds=rect_json(node.bounds()))


@command("render.document")
def cmd_render_document(session, params):
    document = resolve_document(params)
    wait_for_document(document)
    region = params.get("rect") or [0, 0, document.width(), document.height()]
    x, y, width, height = [int(value) for value in region]
    selector = params.get("node")
    if selector is None:
        image = document.projection(x, y, width, height)
    else:
        image = imaging.node_region_image(resolve_node(document, selector), x, y, width, height)
    return imaging.image_reply(image, param(params, "max_size", int, 1024),
                               param(params, "path", str, None), {"rect": [x, y, width, height]})


# -- brush, colors and resources ---------------------------------------------

def brush_json(view):
    preset = view.currentBrushPreset()
    canvas = view.canvas()
    return {
        "preset": preset.name() if preset is not None else None,
        "size": view.brushSize(),
        "opacity": view.paintingOpacity(),
        "flow": view.paintingFlow(),
        "rotation": view.brushRotation(),
        "blending_mode": view.currentBlendingMode(),
        "eraser_mode": view.eraserMode(),
        "alpha_lock": view.globalAlphaLock(),
        "foreground": color_json(view.foregroundColor(), canvas),
        "background": color_json(view.backgroundColor(), canvas),
    }


@command("brush.get")
def cmd_brush_get(session, params):
    return brush_json(active_view())


@command("brush.set")
def cmd_brush_set(session, params):
    view = active_view()
    if "preset" in params:
        presets = app().resources("preset")
        name = param(params, "preset", str)
        if name not in presets:
            matches = sorted(key for key in presets if name.lower() in key.lower())[:10]
            raise CommandError("no brush preset '%s'; similar: %s" % (name, matches))
        view.setCurrentBrushPreset(presets[name])
    if "size" in params:
        view.setBrushSize(param(params, "size", float))
    if "opacity" in params:
        view.setPaintingOpacity(param(params, "opacity", float))
    if "flow" in params:
        view.setPaintingFlow(param(params, "flow", float))
    if "rotation" in params:
        view.setBrushRotation(param(params, "rotation", float))
    if "blending_mode" in params:
        view.setCurrentBlendingMode(param(params, "blending_mode", str))
    if "eraser_mode" in params:
        view.setEraserMode(param(params, "eraser_mode", bool))
    if "alpha_lock" in params:
        view.setGlobalAlphaLock(param(params, "alpha_lock", bool))
    canvas = view.canvas()
    if "foreground" in params:
        view.setForeGroundColor(ManagedColor.fromQColor(parse_color(params["foreground"]), canvas))
    if "background" in params:
        view.setBackGroundColor(ManagedColor.fromQColor(parse_color(params["background"]), canvas))
    return brush_json(view)


@command("resources.list")
def cmd_resources_list(session, params):
    kind = param(params, "type", str, "preset")
    needle = param(params, "filter", str, "").lower()
    limit = param(params, "limit", int, 200)
    names = sorted(name for name in app().resources(kind) if needle in name.lower())
    return {"type": kind, "count": len(names), "names": names[:limit]}


# -- actions and tools -------------------------------------------------------

def action_json(action):
    info = {"name": action.objectName(), "text": action.text().replace("&", ""),
            "enabled": action.isEnabled()}
    shortcuts = [text for text in (shortcut.toString() for shortcut in action.shortcuts()) if text]
    if shortcuts:
        info["shortcuts"] = shortcuts
    if action.isCheckable():
        info["checked"] = action.isChecked()
    return info


def tool_buttons():
    """The toolbox buttons of the active window; each is named after its tool id."""
    window = active_window().qwindow()
    return [button for button in window.findChildren(QAbstractButton)
            if button.metaObject().className() == "KoToolBoxButton" and button.objectName()]


def current_tool_id():
    """The tool whose toolbox button is checked: the toolbox follows KoToolManager's active tool."""
    for button in tool_buttons():
        if button.isChecked():
            return button.objectName()
    return None


@command("actions.list")
def cmd_actions_list(session, params):
    needle = param(params, "filter", str, "").lower()
    limit = param(params, "limit", int, 300)
    actions = [action_json(action) for action in app().actions()
               if action.objectName() and (needle in action.objectName().lower()
                                           or needle in action.text().replace("&", "").lower())]
    actions.sort(key=lambda info: info["name"])
    return {"count": len(actions), "actions": actions[:limit]}


@command("actions.trigger")
def cmd_actions_trigger(session, params):
    name = param(params, "name", str)
    action = app().action(name)
    if action is None:
        raise CommandError("no action '%s'; use actions.list to search" % name)
    if not action.isEnabled():
        raise CommandError("action '%s' is disabled in the current state" % name)
    checked = params.get("checked")

    def run():
        if checked is not None and action.isCheckable():
            if action.isChecked() != bool(checked):
                action.trigger()
        else:
            action.trigger()

    return Deferred([(0, run)], lambda: action_json(action), settle_ms=param(params, "settle_ms", int, 100))


@command("tools.list")
def cmd_tools_list(session, params):
    tools = []
    seen = set()
    for button in tool_buttons():
        tool_id = button.objectName()
        if tool_id in seen:
            continue
        seen.add(tool_id)
        info = {"id": tool_id, "name": button.toolTip().split(" (")[0], "enabled": button.isEnabled()}
        action = app().action(tool_id)
        if action is not None:
            info.update({key: value for key, value in action_json(action).items() if key == "shortcuts"})
        if button.isChecked():
            info["active"] = True
        tools.append(info)
    return {"current_tool": current_tool_id(), "tools": tools}


@command("tools.activate")
def cmd_tools_activate(session, params):
    tool_id = param(params, "id", str)
    action = app().action(tool_id)
    if action is None:
        raise CommandError("no tool '%s'; use tools.list" % tool_id)
    return Deferred([(0, action.trigger)],
                    lambda: {"requested": tool_id, "current_tool": current_tool_id()},
                    settle_ms=param(params, "settle_ms", int, 50))
