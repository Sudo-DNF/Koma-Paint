# SPDX-FileCopyrightText: 2026 Koma Paint contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Krita extension that owns the MCP bridge server for the session."""

from krita import Extension

from .server import BridgeServer


class KomaMcpExtension(Extension):
    """Starts the bridge as soon as Krita has set up its extensions.

    The plugin is opt-in (Python plugins are disabled by default, and test
    harnesses enable it with KRITA_ENABLE_PYTHON_PLUGINS), so being loaded is
    the signal to listen.
    """

    def __init__(self, parent):
        super().__init__(parent)
        self._server = None

    def setup(self):
        self._server = BridgeServer(self)
        self._server.start()

    def createActions(self, window):
        pass
