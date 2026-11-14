# SPDX-FileCopyrightText: 2026 Koma Paint contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Widget inspection, synthetic input, screenshots and canvas control.

Widgets are addressed by path ids built from the top-level window down, one
segment per level: "ClassName#objectName" when the object name is unique
among its siblings, otherwise "ClassName[n]" (the n-th sibling of that class).
For example: "KisMainWindow#MainWindow#0/QDockWidget#LayerBox/...".
"""

from PyQt6.QtCore import QEvent, QPoint, QPointF, Qt, QTimer
from PyQt6.QtGui import QImage, QKeyEvent, QKeySequence, QMouseEvent, QPainter, QPointingDevice, QTabletEvent
from PyQt6 import sip
from PyQt6.QtTest import QTest
from PyQt6.QtWidgets import (QAbstractButton, QAbstractItemView, QAbstractScrollArea,
                             QAbstractSlider, QAbstractSpinBox, QApplication, QComboBox,
                             QDialog, QDockWidget, QDoubleSpinBox, QGroupBox, QLabel, QLineEdit, QListView,
                             QMdiArea, QSpinBox, QTabBar, QTabWidget,
                             QTextEdit, QPlainTextEdit, QWidget)

from . import imaging
from .commands import (CommandError, Deferred, active_view, active_window, app, command, current_tool_id,
                       param, wait_for_document)

# -- widget ids --------------------------------------------------------------


def _class_name(widget):
    return widget.metaObject().className()


def _child_widgets(widget):
    return [child for child in widget.children() if isinstance(child, QWidget)]


def _segment(widget, siblings):
    class_name = _class_name(widget)
    name = widget.objectName()
    if name and sum(1 for sibling in siblings if sibling.objectName() == name
                    and _class_name(sibling) == class_name) == 1:
        return "%s#%s" % (class_name, name)
    same_class = [sibling for sibling in siblings if _class_name(sibling) == class_name]
    return "%s[%d]" % (class_name, same_class.index(widget))


def widget_id(widget):
    segments = []
    current = widget
    while current is not None:
        parent = current.parentWidget()
        siblings = _child_widgets(parent) if parent is not None else QApplication.topLevelWidgets()
        segments.append(_segment(current, siblings))
        current = parent
    return "/".join(reversed(segments))


def _match_segment(segment, candidates):
    if segment.endswith("]") and "[" in segment:
        class_name, index = segment[:-1].rsplit("[", 1)
        same_class = [widget for widget in candidates if _class_name(widget) == class_name]
        position = int(index)
        return same_class[position] if position < len(same_class) else None
    class_name, _, name = segment.partition("#")
    for widget in candidates:
        if _class_name(widget) == class_name and widget.objectName() == name:
            return widget
    return None


def resolve_widget(selector):
    """A widget from a path id, or from a bare object name (first visible match)."""
    if not isinstance(selector, str) or not selector:
        raise CommandError("widget selectors are path ids from ui.tree / ui.find, or object names")
    if selector == "canvas":
        return canvas_widget()
    if "/" in selector or "#" in selector or "[" in selector:
        candidates = QApplication.topLevelWidgets()
        widget = None
        for segment in selector.split("/"):
            widget = _match_segment(segment, candidates)
            if widget is None:
                raise CommandError("no widget at '%s' (failed at '%s')" % (selector, segment))
            candidates = _child_widgets(widget)
        return widget
    for top_level in QApplication.topLevelWidgets():
        for widget in [top_level] + top_level.findChildren(QWidget):
            if widget.objectName() == selector and widget.isVisible():
                return widget
    raise CommandError("no visible widget named '%s'" % selector)


# -- describing widgets ------------------------------------------------------

def _display_column(view):
    """The model column a view shows: list views can show any column (resource models show Name)."""
    return view.modelColumn() if isinstance(view, QListView) else 0


def _item_text(model, row, column, root):
    """The text a user would read for a row.

    Views with custom delegates often show a column whose display data is not
    text (Krita's resource models show the numeric id column and paint the
    name themselves); fall back to the tooltip, then the row's first text column.
    """
    value = model.data(model.index(row, column, root), Qt.ItemDataRole.DisplayRole)
    if isinstance(value, str) and value:
        return value
    tooltip = model.data(model.index(row, column, root), Qt.ItemDataRole.ToolTipRole)
    if isinstance(tooltip, str) and tooltip:
        return tooltip
    for other in range(min(model.columnCount(root), 8)):
        candidate = model.data(model.index(row, other, root), Qt.ItemDataRole.DisplayRole)
        if isinstance(candidate, str) and candidate:
            return candidate
    return "" if value is None else str(value)


def _item_texts(view, limit=60):
    model = view.model()
    if model is None:
        return None
    root = view.rootIndex()
    column = _display_column(view)
    rows = []
    for row in range(min(model.rowCount(root), limit)):
        rows.append(_item_text(model, row, column, root))
    return {"rows": model.rowCount(root), "items": rows,
            "current": view.currentIndex().row() if view.currentIndex().isValid() else None}


def describe_widget(widget):
    info = {"id": widget_id(widget), "class": _class_name(widget)}
    if widget.objectName():
        info["name"] = widget.objectName()
    window = widget.window()
    top_left = widget.mapTo(window, QPoint(0, 0)) if window is not widget else QPoint(0, 0)
    info["geometry"] = [top_left.x(), top_left.y(), widget.width(), widget.height()]
    info["visible"] = widget.isVisible()
    if not widget.isEnabled():
        info["enabled"] = False
    if widget.hasFocus():
        info["focus"] = True
    if widget.toolTip():
        info["tooltip"] = widget.toolTip()

    if isinstance(widget, QAbstractButton):
        info["text"] = widget.text().replace("&", "")
        if widget.isCheckable():
            info["checked"] = widget.isChecked()
    elif isinstance(widget, QLabel):
        info["text"] = widget.text()
    elif isinstance(widget, QLineEdit):
        info["text"] = widget.text()
    elif isinstance(widget, (QTextEdit, QPlainTextEdit)):
        info["text"] = widget.toPlainText()[:2000]
    elif isinstance(widget, QComboBox):
        info["text"] = widget.currentText()
        info["items"] = [widget.itemText(index) for index in range(min(widget.count(), 60))]
    elif isinstance(widget, (QSpinBox, QDoubleSpinBox)):
        info["value"] = widget.value()
        info["range"] = [widget.minimum(), widget.maximum()]
    elif isinstance(widget, QAbstractSpinBox):
        info["text"] = widget.text()
    elif isinstance(widget, QAbstractSlider):
        info["value"] = widget.value()
        info["range"] = [widget.minimum(), widget.maximum()]
    elif isinstance(widget, QTabBar):
        info["tabs"] = [widget.tabText(index) for index in range(widget.count())]
        info["current"] = widget.currentIndex()
    elif isinstance(widget, QTabWidget):
        info["tabs"] = [widget.tabText(index) for index in range(widget.count())]
        info["current"] = widget.currentIndex()
    elif isinstance(widget, (QDockWidget, QGroupBox)):
        info["title"] = widget.windowTitle() if isinstance(widget, QDockWidget) else widget.title()
    elif isinstance(widget, QAbstractItemView):
        texts = _item_texts(widget)
        if texts is not None:
            info.update(texts)
    if widget.isWindow():
        info["title"] = widget.windowTitle()
        if widget.isModal():
            info["modal"] = True
    return info


def _tree(widget, depth, visible_only, budget):
    info = describe_widget(widget)
    budget[0] -= 1
    children = [child for child in _child_widgets(widget)
                if not child.isWindow() and (child.isVisible() or not visible_only)]
    if children and depth > 0 and budget[0] > 0:
        info["children"] = []
        for child in children:
            if budget[0] <= 0:
                info["truncated"] = True
                break
            info["children"].append(_tree(child, depth - 1, visible_only, budget))
    elif children:
        info["child_count"] = len(children)
    return info


def modal_dialog():
    """The open modal dialog, if any.

    activeModalWidget() follows window activation, which some platforms (Qt's
    offscreen platform, or an unfocused session) never report, so fall back to
    the most recently shown visible modal top-level window.
    """
    modal = QApplication.activeModalWidget()
    if modal is not None:
        return modal
    candidates = [widget for widget in QApplication.topLevelWidgets() if widget.isVisible() and widget.isModal()]
    return candidates[-1] if candidates else None


def _default_root():
    popup = QApplication.activePopupWidget()
    if popup is not None:
        return popup
    modal = modal_dialog()
    if modal is not None:
        return modal
    return active_window().qwindow()


@command("ui.windows")
def cmd_ui_windows(session, params):
    windows = [describe_widget(widget) for widget in QApplication.topLevelWidgets() if widget.isVisible()]
    modal = modal_dialog()
    popup = QApplication.activePopupWidget()
    focus = QApplication.focusWidget()
    return {"windows": windows,
            "active_modal": widget_id(modal) if modal is not None else None,
            "active_popup": widget_id(popup) if popup is not None else None,
            "focus": widget_id(focus) if focus is not None else None}


@command("ui.tree")
def cmd_ui_tree(session, params):
    root_selector = params.get("root")
    root = resolve_widget(root_selector) if root_selector else _default_root()
    budget = [param(params, "max_nodes", int, 400)]
    return _tree(root, param(params, "depth", int, 4), param(params, "visible_only", bool, True), budget)


@command("ui.find")
def cmd_ui_find(session, params):
    class_name = param(params, "class", str, None)
    name = param(params, "name", str, None)
    text = param(params, "text", str, "").lower()
    visible_only = param(params, "visible_only", bool, True)
    limit = param(params, "limit", int, 50)
    if not (class_name or name or text):
        raise CommandError("give at least one of class, name or text")
    matches = []
    seen = set()
    for top_level in QApplication.topLevelWidgets():
        for widget in [top_level] + top_level.findChildren(QWidget):
            # Dialogs are top-level windows and children of their parent window.
            address = sip.unwrapinstance(widget)
            if address in seen:
                continue
            seen.add(address)
            if visible_only and not widget.isVisible():
                continue
            if class_name and not widget.inherits(class_name):
                continue
            if name and name != widget.objectName():
                continue
            info = describe_widget(widget)
            if text:
                haystack = " ".join(str(info.get(key, "")) for key in ("text", "title", "tooltip", "items", "tabs"))
                if text not in haystack.lower():
                    continue
            matches.append(info)
            if len(matches) >= limit:
                return {"matches": matches, "truncated": True}
    return {"matches": matches}


# -- synthetic input ---------------------------------------------------------

_BUTTONS = {"left": Qt.MouseButton.LeftButton, "right": Qt.MouseButton.RightButton,
            "middle": Qt.MouseButton.MiddleButton, "none": Qt.MouseButton.NoButton}
_MODIFIERS = {"shift": Qt.KeyboardModifier.ShiftModifier, "ctrl": Qt.KeyboardModifier.ControlModifier,
              "control": Qt.KeyboardModifier.ControlModifier, "alt": Qt.KeyboardModifier.AltModifier,
              "meta": Qt.KeyboardModifier.MetaModifier}
_MODIFIER_KEYS = {"shift": Qt.Key.Key_Shift, "ctrl": Qt.Key.Key_Control, "control": Qt.Key.Key_Control,
                  "alt": Qt.Key.Key_Alt, "meta": Qt.Key.Key_Meta}


def _button(name):
    if name not in _BUTTONS:
        raise CommandError("button must be one of %s" % sorted(_BUTTONS))
    return _BUTTONS[name]


def _modifiers(names):
    combined = Qt.KeyboardModifier.NoModifier
    for name in names or []:
        key = name.lower()
        if key not in _MODIFIERS:
            raise CommandError("modifiers must be among %s" % sorted(_MODIFIERS))
        combined |= _MODIFIERS[key]
    return combined


def _key_from_name(name):
    if name.lower() in _MODIFIER_KEYS:
        return _MODIFIER_KEYS[name.lower()]
    sequence = QKeySequence(name)
    if sequence.count() != 1:
        raise CommandError("cannot parse key '%s'" % name)
    return sequence[0].key()


def _input_target(widget):
    """Item views and scroll areas receive mouse input on their viewport."""
    if isinstance(widget, QAbstractScrollArea) and widget.viewport() is not None:
        return widget.viewport()
    return widget


def _item_position(view, text):
    model = view.model()
    root = view.rootIndex()
    column = _display_column(view)
    for row in range(model.rowCount(root)):
        if _item_text(model, row, column, root) == text:
            index = model.index(row, column, root)
            view.scrollTo(index)
            return view.visualRect(index).center()
    raise CommandError("no item '%s' in %s" % (text, _class_name(view)))


@command("ui.click")
def cmd_ui_click(session, params):
    widget = resolve_widget(param(params, "widget", str))
    if not widget.isVisible():
        raise CommandError("widget is not visible")
    target = _input_target(widget)
    item = param(params, "item", str, None)
    if item is not None:
        if not isinstance(widget, QAbstractItemView):
            raise CommandError("item clicks need an item view widget")
        position = _item_position(widget, item)
    elif "x" in params and "y" in params:
        position = QPoint(param(params, "x", int), param(params, "y", int))
    else:
        position = target.rect().center()
    button = _button(param(params, "button", str, "left"))
    modifiers = _modifiers(params.get("modifiers"))
    double = param(params, "double", bool, False)

    def run():
        if double:
            QTest.mouseDClick(target, button, modifiers, position)
        else:
            QTest.mouseClick(target, button, modifiers, position)

    return Deferred([(0, run)], lambda: describe_widget(widget) if _alive(widget) else {"clicked": True},
                    settle_ms=param(params, "settle_ms", int, 100))


def _alive(widget):
    try:
        widget.objectName()
        return True
    except RuntimeError:
        return False


def _key_target(params):
    selector = params.get("widget")
    if selector:
        return resolve_widget(selector)
    return QApplication.focusWidget() or _default_root()


@command("ui.key")
def cmd_ui_key(session, params):
    """Press a key sequence such as 'Ctrl+Z', 'B' or 'Ctrl+Shift+N, Return'."""
    sequence = QKeySequence(param(params, "keys", str))
    if sequence.isEmpty():
        raise CommandError("cannot parse key sequence '%s'" % params["keys"])
    target = _key_target(params)
    combinations = [sequence[index] for index in range(sequence.count())]

    def run():
        for combination in combinations:
            QTest.keyClick(target, combination.key(), combination.keyboardModifiers())

    return Deferred([(0, run)], lambda: {"sent": sequence.toString()},
                    settle_ms=param(params, "settle_ms", int, 100))


@command("ui.type")
def cmd_ui_type(session, params):
    text = param(params, "text", str)
    target = _key_target(params)
    return Deferred([(0, lambda: QTest.keyClicks(target, text))], lambda: {"typed": len(text)},
                    settle_ms=param(params, "settle_ms", int, 50))


@command("ui.set_value")
def cmd_ui_set_value(session, params):
    widget = resolve_widget(param(params, "widget", str))
    value = params.get("value")
    if isinstance(widget, QComboBox):
        index = widget.findText(str(value)) if isinstance(value, str) else int(value)
        if index < 0:
            raise CommandError("combo box has no item '%s'" % value)
        widget.setCurrentIndex(index)
    elif isinstance(widget, (QSpinBox, QAbstractSlider)):
        widget.setValue(int(value))
    elif isinstance(widget, QDoubleSpinBox):
        widget.setValue(float(value))
    elif isinstance(widget, QLineEdit):
        widget.setText(str(value))
        widget.editingFinished.emit()
    elif isinstance(widget, QAbstractButton) and widget.isCheckable():
        if widget.isChecked() != bool(value):
            widget.click()
    elif isinstance(widget, (QTabBar, QTabWidget)):
        widget.setCurrentIndex(int(value))
    elif hasattr(widget, "setValue"):
        widget.setValue(value)
    else:
        raise CommandError("%s has no settable value" % _class_name(widget))
    return describe_widget(widget)


@command("ui.close_popups")
def cmd_ui_close_popups(session, params):
    closed = []
    popup = QApplication.activePopupWidget()
    while popup is not None and len(closed) < 10:
        closed.append(widget_id(popup))
        popup.close()
        popup = QApplication.activePopupWidget()
    return {"closed": closed}


@command("ui.resize_window")
def cmd_ui_resize_window(session, params):
    """Resize the active main window, e.g. to test docker layouts at a given screen size."""
    window = active_window().qwindow()
    if window.isMaximized() or window.isFullScreen():
        window.showNormal()
    window.resize(param(params, "width", int), param(params, "height", int))
    return Deferred([], lambda: {"geometry": [window.x(), window.y(), window.width(), window.height()]},
                    settle_ms=param(params, "settle_ms", int, 150))


def _dismiss_innermost():
    """Close the open popup or the innermost modal dialog; True if something was closed."""
    popup = QApplication.activePopupWidget()
    if popup is not None:
        popup.close()
        return True
    modal = modal_dialog()
    if modal is None:
        return False
    if isinstance(modal, QDialog):
        modal.reject()
    else:
        modal.close()
    return True


@command("app.quit")
def cmd_app_quit(session, params):
    """Quit Koma Paint, discarding unsaved changes.

    Popups and modal dialogs are dismissed first, one per event loop turn, so
    that every nested exec() loop unwinds before the main window is destroyed.
    """
    for document in app().documents():
        document.setModified(False)
    steps = [(0 if index == 0 else 50, _dismiss_innermost) for index in range(10)]

    def finish():
        QTimer.singleShot(0, lambda: app().action("file_quit").trigger())
        return {"quitting": True, "dialogs_left": modal_dialog() is not None}

    return Deferred(steps, finish, settle_ms=50)


# -- dockers -----------------------------------------------------------------

@command("dockers.list")
def cmd_dockers_list(session, params):
    window = active_window().qwindow()
    dockers = []
    for docker in window.findChildren(QDockWidget):
        dockers.append({"name": docker.objectName(), "title": docker.windowTitle(),
                        "visible": docker.isVisible(), "floating": docker.isFloating(),
                        "area": window.dockWidgetArea(docker).name if not docker.isFloating() else None,
                        "id": widget_id(docker)})
    dockers.sort(key=lambda info: info["name"])
    return {"dockers": dockers}


@command("dockers.show")
def cmd_dockers_show(session, params):
    window = active_window().qwindow()
    name = param(params, "name", str)
    for docker in window.findChildren(QDockWidget):
        if name in (docker.objectName(), docker.windowTitle()):
            visible = param(params, "visible", bool, True)
            docker.setVisible(visible)
            if visible:
                docker.raise_()
            return {"name": docker.objectName(), "visible": docker.isVisible()}
    raise CommandError("no docker '%s'; use dockers.list" % name)


# -- screenshots -------------------------------------------------------------

def canvas_widget():
    """The canvas widget of the active view: the viewport of its canvas controller."""
    window = active_window().qwindow()
    active_view()
    mdi = window.findChild(QMdiArea)
    scope = window
    if mdi is not None and mdi.activeSubWindow() is not None:
        scope = mdi.activeSubWindow()
    for class_name in ("KisOpenGLCanvas2", "KisQPainterCanvas"):
        for widget in scope.findChildren(QWidget):
            if _class_name(widget) == class_name and widget.isVisible():
                return widget
    raise CommandError("cannot find the canvas widget of the active view")


def _composite_screen():
    """The active main window with every other visible top-level window drawn over it.

    Dialogs, popups and floating palettes are separate windows, so grabbing the
    main window alone leaves them out; this shows what the user sees.
    """
    main = active_window().qwindow()
    image = main.grab().toImage().convertToFormat(QImage.Format.Format_ARGB32_Premultiplied)
    ratio = image.devicePixelRatio()
    painter = QPainter(image)
    origin = main.frameGeometry().topLeft()
    for widget in QApplication.topLevelWidgets():
        if widget is main or not widget.isVisible() or widget.windowType() == Qt.WindowType.Desktop:
            continue
        offset = widget.geometry().topLeft() - origin
        painter.drawPixmap(QPoint(round(offset.x()), round(offset.y())), widget.grab())
    painter.end()
    image.setDevicePixelRatio(ratio)
    return main, image


@command("ui.screenshot")
def cmd_ui_screenshot(session, params):
    target = param(params, "target", str, "window")
    if target == "screen":
        main, image = _composite_screen()
        return imaging.image_reply(image, param(params, "max_size", int, 1600), param(params, "path", str, None),
                                   {"widget": widget_id(main), "composited": True})
    if target == "window":
        widget = _default_root()
    elif target == "canvas":
        widget = canvas_widget()
    else:
        widget = resolve_widget(target)
    image = widget.grab().toImage()
    return imaging.image_reply(image, param(params, "max_size", int, 1600), param(params, "path", str, None),
                               {"widget": widget_id(widget)})


# -- canvas ------------------------------------------------------------------

def image_to_widget_transform(view):
    """Image pixel coordinates to canvas widget coordinates for the view's current zoom and rotation."""
    image_to_flake, invertible = view.flakeToImageTransform().inverted()
    if not invertible:
        raise CommandError("the view transform is not invertible")
    return image_to_flake * view.flakeToCanvasTransform()


@command("canvas.map")
def cmd_canvas_map(session, params):
    """Convert points between image and canvas widget coordinates."""
    view = active_view()
    transform = image_to_widget_transform(view)
    if param(params, "to", str, "widget") == "image":
        transform, _ = transform.inverted()
    return {"points": [[round(point.x(), 3), round(point.y(), 3)]
                       for point in (transform.map(QPointF(float(x), float(y)))
                                     for x, y in param(params, "points", list))]}


@command("canvas.view")
def cmd_canvas_view(session, params):
    view = active_view()
    canvas = view.canvas()
    if param(params, "reset", bool, False):
        canvas.resetZoom()
        canvas.resetRotation()
        canvas.setMirror(False)
    if "zoom" in params:
        canvas.setZoomLevel(param(params, "zoom", float))
    if "rotation" in params:
        canvas.setRotation(param(params, "rotation", float))
    if "mirror" in params:
        canvas.setMirror(param(params, "mirror", bool))
    if "center" in params:
        x, y = params["center"]
        canvas.setPreferredCenter(QPointF(float(x), float(y)))
    if "wrap_around" in params:
        canvas.setWrapAroundMode(param(params, "wrap_around", bool))
    center = canvas.preferredCenter()
    return {"zoom": canvas.zoomLevel(), "rotation": canvas.rotation(), "mirror": canvas.mirror(),
            "center": [round(center.x(), 2), round(center.y(), 2)],
            "wrap_around": canvas.wrapAroundMode()}


class _CanvasInput:
    """Delivers mouse or pen events to the canvas widget, as the windowing system would."""

    def __init__(self, widget, device, button, modifiers):
        self.widget = widget
        self.device = device
        self.button = button
        self.modifiers = modifiers
        self.pressed = False
        self._pen = None
        if device == "pen":
            self._pen = QPointingDevice(
                "Koma MCP pen", 0x4b4d43, QPointingDevice.DeviceType.Stylus,
                QPointingDevice.PointerType.Pen,
                QPointingDevice.Capability.Position | QPointingDevice.Capability.Pressure
                | QPointingDevice.Capability.XTilt | QPointingDevice.Capability.YTilt
                | QPointingDevice.Capability.Rotation,
                1, 3)

    def _send(self, event):
        # PyQt6 cannot set event timestamps; steps are paced in real time instead.
        QApplication.sendEvent(self.widget, event)

    def _mouse(self, event_type, position, buttons, button):
        local = QPointF(position)
        global_position = QPointF(self.widget.mapToGlobal(local))
        self._send(QMouseEvent(event_type, local, global_position, button, buttons, self.modifiers))

    def _tablet(self, event_type, position, pressure, buttons, button, tilt=(0.0, 0.0), rotation=0.0):
        local = QPointF(position)
        global_position = QPointF(self.widget.mapToGlobal(local))
        event = QTabletEvent(event_type, self._pen, local, global_position, pressure,
                             tilt[0], tilt[1], 0.0, rotation, 0.0, self.modifiers, button, buttons)
        self._send(event)
        if not event.isAccepted():
            # Qt follows an unaccepted tablet event with a synthesized mouse
            # event. Krita relies on it: on Linux it leaves tablet events
            # unaccepted and swallows the next mouse press, so without this the
            # first real mouse press after a pen stroke would be lost.
            mouse_type = {QEvent.Type.TabletPress: QEvent.Type.MouseButtonPress,
                          QEvent.Type.TabletMove: QEvent.Type.MouseMove,
                          QEvent.Type.TabletRelease: QEvent.Type.MouseButtonRelease}[event_type]
            self._mouse(mouse_type, position, buttons, button)

    def proximity(self, entering):
        """Pen proximity enter/leave, which Qt delivers to the application object.

        Krita treats a lifted pen as hovering (and discards mouse input) until
        the pen leaves proximity, so every pen gesture is wrapped in these.
        """
        if self.device != "pen":
            return
        event_type = QEvent.Type.TabletEnterProximity if entering else QEvent.Type.TabletLeaveProximity
        QApplication.sendEvent(QApplication.instance(), QTabletEvent(
            event_type, self._pen, QPointF(), QPointF(), 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
            Qt.KeyboardModifier.NoModifier, Qt.MouseButton.NoButton, Qt.MouseButton.NoButton))

    def hover(self, sample):
        """Move to the start point without buttons, as a real pointer arrives before it presses."""
        position, _pressure, tilt, rotation = sample
        if self.device == "pen":
            self._tablet(QEvent.Type.TabletMove, position, 0.0, Qt.MouseButton.NoButton,
                         Qt.MouseButton.NoButton, tilt, rotation)
        else:
            self._mouse(QEvent.Type.MouseMove, position, Qt.MouseButton.NoButton, Qt.MouseButton.NoButton)

    def press(self, sample):
        self.pressed = True
        self._event("press", sample)

    def move(self, sample):
        self._event("move", sample)

    def release(self, sample):
        self.pressed = False
        self._event("release", sample)

    def _event(self, phase, sample):
        position, pressure, tilt, rotation = sample
        held = self.button if phase != "release" else Qt.MouseButton.NoButton
        changed = self.button if phase != "move" else Qt.MouseButton.NoButton
        if self.device == "pen":
            event_type = {"press": QEvent.Type.TabletPress, "move": QEvent.Type.TabletMove,
                          "release": QEvent.Type.TabletRelease}[phase]
            self._tablet(event_type, position, pressure if phase != "release" else 0.0,
                         held, changed, tilt, rotation)
        else:
            event_type = {"press": QEvent.Type.MouseButtonPress, "move": QEvent.Type.MouseMove,
                          "release": QEvent.Type.MouseButtonRelease}[phase]
            self._mouse(event_type, position, held, changed)

    def key(self, key, pressed):
        event_type = QEvent.Type.KeyPress if pressed else QEvent.Type.KeyRelease
        QApplication.sendEvent(self.widget, QKeyEvent(event_type, key, self.modifiers))


def _interpolate(samples, spacing):
    """Insert samples so consecutive ones are at most `spacing` widget pixels apart.

    Real pointing devices report many positions along a drag; tools such as
    the freehand brush with smoothing only paint a segment once the next event
    arrives, so a drag given as a few far-apart points must be filled in.
    """
    if spacing <= 0 or len(samples) < 2:
        return samples
    result = [samples[0]]
    for (start, p0, t0, r0), (end, p1, t1, r1) in zip(samples, samples[1:]):
        distance = (end - start).manhattanLength()
        count = max(1, int(distance / spacing + 0.999))
        for step in range(1, count + 1):
            f = step / count
            result.append((start + (end - start) * f, p0 + (p1 - p0) * f,
                           (t0[0] + (t1[0] - t0[0]) * f, t0[1] + (t1[1] - t0[1]) * f), r0 + (r1 - r0) * f))
    return result


@command("canvas.drag")
def cmd_canvas_drag(session, params):
    """Drag across the canvas with synthetic mouse or pen input, through Koma Paint's input manager.

    This exercises the real input path: the active tool, canvas shortcuts
    (Space to pan, Ctrl+Space to zoom, ...), stabilizers and pressure curves.
    """
    view = active_view()
    widget = canvas_widget()
    coordinates = param(params, "coordinates", str, "image")
    transform = image_to_widget_transform(view) if coordinates == "image" else None
    raw_points = param(params, "points", list)
    if len(raw_points) < 1:
        raise CommandError("points must hold at least one point")
    step_ms = param(params, "step_ms", int, 8)
    samples = []
    for point in raw_points:
        if isinstance(point, dict):
            x, y = float(point["x"]), float(point["y"])
            pressure = float(point.get("pressure", 1.0))
            tilt = (float(point.get("x_tilt", 0.0)), float(point.get("y_tilt", 0.0)))
            rotation = float(point.get("rotation", 0.0))
        else:
            x, y = float(point[0]), float(point[1])
            pressure = float(point[2]) if len(point) > 2 else 1.0
            tilt, rotation = (0.0, 0.0), 0.0
        position = transform.map(QPointF(x, y)) if transform is not None else QPointF(x, y)
        samples.append((position, pressure, tilt, rotation))
    samples = _interpolate(samples, param(params, "spacing", float, 6.0))

    device = param(params, "device", str, "mouse")
    if device not in ("mouse", "pen"):
        raise CommandError("device must be 'mouse' or 'pen'")
    held_keys = [_key_from_name(name) for name in param(params, "hold_keys", list, [])]
    driver = _CanvasInput(widget, device, _button(param(params, "button", str, "left")),
                          _modifiers(params.get("modifiers")))
    document = view.document()

    # Input goes to the canvas as if the user had moved there: it gets focus
    # (with a reason other than a mouse click, which makes Krita swallow the
    # next stroke as the click that focused it), the pen enters
    # proximity and the pointer hovers at the start point before pressing.
    widget.setFocus(Qt.FocusReason.OtherFocusReason)
    steps = [(0, lambda: driver.proximity(True)), (0, lambda: driver.hover(samples[0]))]
    if held_keys:
        steps.append((step_ms, lambda: [driver.key(key, True) for key in held_keys]))
    steps.append((step_ms, lambda: driver.press(samples[0])))
    for sample in samples[1:]:
        steps.append((step_ms, lambda sample=sample: driver.move(sample)))
    steps.append((step_ms, lambda: driver.release(samples[-1])))
    steps.append((0, lambda: driver.proximity(False)))
    if held_keys:
        steps.append((0, lambda: [driver.key(key, False) for key in reversed(held_keys)]))

    def abort():
        if driver.pressed:
            driver.release(samples[-1])
        driver.proximity(False)
        for key in reversed(held_keys):
            driver.key(key, False)

    def finish():
        if document is not None:
            wait_for_document(document)
        widget_points = [[round(sample[0].x(), 1), round(sample[0].y(), 1)] for sample in (samples[0], samples[-1])]
        return {"events": len(samples) + 2, "device": device, "widget_start_end": widget_points,
                "current_tool": current_tool_id()}

    return Deferred(steps, finish, settle_ms=param(params, "settle_ms", int, 50), on_abort=abort)
