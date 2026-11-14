# SPDX-FileCopyrightText: 2026 Koma Paint contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Localhost JSON-lines server that executes bridge commands on the GUI thread.

Wire format: one JSON object per line in both directions.

    -> {"id": 1, "method": "auth", "params": {"token": "..."}}
    <- {"id": 1, "result": {"pid": 1234, "version": "..."}}
    -> {"id": 2, "method": "state", "params": {}}
    <- {"id": 2, "result": {...}}
    <- {"id": 3, "error": {"message": "...", "traceback": "..."}}

A connection must authenticate before any other method is accepted.
Requests are executed one at a time. Commands that can open modal dialogs or
otherwise spin a nested event loop return a Deferred, so the reply is sent from
whichever event loop is running when the command settles and later requests
(for example inspecting the dialog that just opened) are still served.
"""

import json
import os
import secrets
import traceback

from krita import Krita
from PyQt6.QtCore import QObject, QStandardPaths, QTimer
from PyQt6.QtNetwork import QHostAddress, QTcpServer, QTcpSocket
from PyQt6.QtWidgets import QApplication

from . import ui  # noqa: F401  (registers the ui.*, dockers.* and canvas.* commands)
from .commands import CommandError, Deferred, Session

DISCOVERY_FILE_NAME = "komapaint-mcp.json"
MAX_LINE_BYTES = 64 * 1024 * 1024


def _runtime_directory():
    location = QStandardPaths.writableLocation(QStandardPaths.StandardLocation.RuntimeLocation)
    if not location:
        location = QStandardPaths.writableLocation(QStandardPaths.StandardLocation.TempLocation)
    return location


class _Connection(QObject):
    def __init__(self, server, socket):
        super().__init__(server)
        self.server = server
        self.socket = socket
        self.buffer = bytearray()
        self.authenticated = False
        socket.setParent(self)
        socket.readyRead.connect(self._on_ready_read)
        socket.disconnected.connect(self._on_disconnected)

    def _on_ready_read(self):
        self.buffer += bytes(self.socket.readAll())
        if len(self.buffer) > MAX_LINE_BYTES and b"\n" not in self.buffer:
            self.socket.abort()
            return
        while True:
            newline = self.buffer.find(b"\n")
            if newline < 0:
                break
            line = bytes(self.buffer[:newline]).strip()
            del self.buffer[:newline + 1]
            if line:
                self.server.enqueue(self, line)

    def _on_disconnected(self):
        self.server.drop_connection(self)

    def send(self, message):
        if self.socket.state() != QTcpSocket.SocketState.ConnectedState:
            return
        payload = json.dumps(message, ensure_ascii=False, default=str).encode("utf-8") + b"\n"
        self.socket.write(payload)
        self.socket.flush()


class BridgeServer(QObject):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._tcp = QTcpServer(self)
        self._tcp.newConnection.connect(self._on_new_connection)
        self._connections = []
        self._queue = []
        self._busy = False
        self._token = os.environ.get("KOMA_MCP_TOKEN") or secrets.token_hex(16)
        self._discovery_path = os.path.join(_runtime_directory(), DISCOVERY_FILE_NAME)
        self._session = Session()

    # -- lifecycle -----------------------------------------------------

    def start(self):
        port = int(os.environ.get("KOMA_MCP_PORT", "0") or 0)
        if not self._tcp.listen(QHostAddress(QHostAddress.SpecialAddress.LocalHost), port):
            print("koma_mcp: cannot listen on 127.0.0.1:%d: %s" % (port, self._tcp.errorString()), flush=True)
            return
        self._write_discovery_file()
        QApplication.instance().aboutToQuit.connect(self._remove_discovery_file)
        print("koma_mcp: bridge listening on 127.0.0.1:%d" % self._tcp.serverPort(), flush=True)

    def _write_discovery_file(self):
        info = {
            "pid": os.getpid(),
            "host": "127.0.0.1",
            "port": self._tcp.serverPort(),
            "token": self._token,
            "version": Krita.instance().version(),
        }
        os.makedirs(os.path.dirname(self._discovery_path), exist_ok=True)
        descriptor = os.open(self._discovery_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(descriptor, "w") as handle:
            json.dump(info, handle)

    def _remove_discovery_file(self):
        self._tcp.close()
        try:
            with open(self._discovery_path) as handle:
                if json.load(handle).get("pid") != os.getpid():
                    return
            os.remove(self._discovery_path)
        except (OSError, ValueError):
            pass

    # -- connections ---------------------------------------------------

    def _on_new_connection(self):
        while self._tcp.hasPendingConnections():
            socket = self._tcp.nextPendingConnection()
            self._connections.append(_Connection(self, socket))

    def drop_connection(self, connection):
        if connection in self._connections:
            self._connections.remove(connection)
        self._queue = [item for item in self._queue if item[0] is not connection]
        connection.deleteLater()

    # -- request processing --------------------------------------------

    def enqueue(self, connection, line):
        self._queue.append((connection, line))
        if not self._busy:
            self._process_queue()

    def _process_queue(self):
        while self._queue and not self._busy:
            connection, line = self._queue.pop(0)
            self._busy = True
            try:
                self._handle(connection, line)
            finally:
                self._busy = False

    def _handle(self, connection, line):
        request_id = None
        try:
            request = json.loads(line.decode("utf-8"))
            request_id = request.get("id")
            method = request.get("method", "")
            params = request.get("params") or {}
            if not isinstance(params, dict):
                raise CommandError("params must be an object")
        except (ValueError, UnicodeDecodeError, AttributeError) as error:
            connection.send({"id": request_id, "error": {"message": "malformed request: %s" % error}})
            return

        if not connection.authenticated:
            if method != "auth":
                connection.send({"id": request_id, "error": {"message": "authenticate first"}})
                return
            if not secrets.compare_digest(str(params.get("token", "")), self._token):
                connection.send({"id": request_id, "error": {"message": "invalid token"}})
                connection.socket.disconnectFromHost()
                return
            connection.authenticated = True
            connection.send({"id": request_id, "result": {
                "pid": os.getpid(), "version": Krita.instance().version()}})
            return

        try:
            result = self._session.dispatch(method, params)
        except Exception as error:  # Reported to the client, never swallowed.
            connection.send({"id": request_id, "error": self._describe_error(error)})
            return

        if isinstance(result, Deferred):
            self._run_deferred(connection, request_id, result)
        else:
            connection.send({"id": request_id, "result": result})

    def _run_deferred(self, connection, request_id, deferred):
        """Run the deferred's steps on successive timer ticks, then reply."""
        steps = list(deferred.steps)

        def fail(error):
            connection.send({"id": request_id, "error": self._describe_error(error)})

        def finish():
            try:
                result = deferred.finish()
            except Exception as error:
                fail(error)
                return
            connection.send({"id": request_id, "result": result})

        failed = [False]

        def run_step(index):
            if failed[0]:
                return
            if index == len(steps):
                QTimer.singleShot(max(0, int(deferred.settle_ms)), finish)
                return
            _delay, step = steps[index]
            # Schedule what comes next before running the step: a step that
            # opens a modal dialog does not return until the dialog closes,
            # and the remaining steps and the reply must still be delivered
            # from the dialog's event loop.
            next_delay = steps[index + 1][0] if index + 1 < len(steps) else 0
            QTimer.singleShot(max(0, int(next_delay)), lambda: run_step(index + 1))
            try:
                step()
            except Exception as error:
                failed[0] = True
                deferred.abort()
                fail(error)

        first_delay = steps[0][0] if steps else 0
        QTimer.singleShot(max(0, int(first_delay)), lambda: run_step(0))

    @staticmethod
    def _describe_error(error):
        if isinstance(error, CommandError):
            return {"message": str(error)}
        return {
            "message": "%s: %s" % (type(error).__name__, error),
            "traceback": "".join(traceback.format_exception(type(error), error, error.__traceback__)),
        }
