# SPDX-FileCopyrightText: 2026 Koma Paint contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for the Koma Paint MCP server.

The protocol tests start the server as a subprocess and talk MCP to it over
stdio, without Koma Paint. The end-to-end test (KOMA_MCP_INTEGRATION=1)
launches an installed Koma Paint headless and drives it through the server.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

SERVER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "komapaint_mcp.py")


class McpClient:
    def __init__(self):
        self.process = subprocess.Popen([sys.executable, SERVER], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, text=True)
        self._next_id = 0

    def request(self, method, params=None):
        self._next_id += 1
        self._write({"jsonrpc": "2.0", "id": self._next_id, "method": method, "params": params or {}})
        reply = json.loads(self.process.stdout.readline())
        assert reply["id"] == self._next_id, reply
        return reply

    def notify(self, method):
        self._write({"jsonrpc": "2.0", "method": method})

    def _write(self, message):
        self.process.stdin.write(json.dumps(message) + "\n")
        self.process.stdin.flush()

    def initialize(self):
        reply = self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                            "clientInfo": {"name": "test", "version": "1"}})
        self.notify("notifications/initialized")
        return reply["result"]

    def call(self, name, arguments=None):
        result = self.request("tools/call", {"name": name, "arguments": arguments or {}})["result"]
        texts = [item["text"] for item in result["content"] if item["type"] == "text"]
        images = [item for item in result["content"] if item["type"] == "image"]
        if result.get("isError"):
            raise AssertionError("%s failed: %s" % (name, texts[0] if texts else result))
        return json.loads(texts[0]), images

    def close(self):
        self.process.stdin.close()
        self.process.wait(30)
        self.process.stdout.close()


class ProtocolTest(unittest.TestCase):
    def setUp(self):
        self.client = McpClient()

    def tearDown(self):
        self.client.close()

    def test_initialize_negotiates_version(self):
        result = self.client.initialize()
        self.assertEqual(result["protocolVersion"], "2025-06-18")
        self.assertEqual(result["serverInfo"]["name"], "komapaint")
        self.assertIn("tools", result["capabilities"])

    def test_unknown_protocol_version_falls_back_to_latest(self):
        reply = self.client.request("initialize", {"protocolVersion": "1999-01-01", "capabilities": {},
                                                   "clientInfo": {"name": "test", "version": "1"}})
        self.assertEqual(reply["result"]["protocolVersion"], "2025-06-18")

    def test_tools_have_valid_schemas(self):
        self.client.initialize()
        tools = self.client.request("tools/list")["result"]["tools"]
        names = [tool["name"] for tool in tools]
        self.assertEqual(len(names), len(set(names)))
        for expected in ("app_launch", "app_attach", "canvas_drag", "ui_screenshot", "python_exec"):
            self.assertIn(expected, names)
        for tool in tools:
            schema = tool["inputSchema"]
            self.assertEqual(schema["type"], "object", tool["name"])
            self.assertTrue(tool["description"], tool["name"])
            for required in schema.get("required", []):
                self.assertIn(required, schema["properties"], tool["name"])

    def test_bridge_tool_without_connection_is_a_tool_error(self):
        self.client.initialize()
        result = self.client.request("tools/call", {"name": "app_state", "arguments": {}})["result"]
        self.assertTrue(result["isError"])
        self.assertIn("app_launch", result["content"][0]["text"])

    def test_unknown_tool_is_a_tool_error(self):
        self.client.initialize()
        result = self.client.request("tools/call", {"name": "no_such_tool", "arguments": {}})["result"]
        self.assertTrue(result["isError"])

    def test_unknown_method_is_a_protocol_error(self):
        self.client.initialize()
        reply = self.client.request("resources/list")
        self.assertEqual(reply["error"]["code"], -32601)

    def _fake_prefix(self, script):
        """A prefix whose bin/komapaint is a shell script, standing in for Koma Paint."""
        prefix = tempfile.mkdtemp(prefix="komapaint-mcp-test-")
        self.addCleanup(lambda: subprocess.run(["rm", "-rf", prefix]))
        os.makedirs(os.path.join(prefix, "bin"))
        executable = os.path.join(prefix, "bin", "komapaint")
        with open(executable, "w") as stream:
            stream.write("#!/bin/sh\n" + script)
        os.chmod(executable, 0o755)
        return prefix

    def _launch_error(self, arguments):
        self.client.initialize()
        result = self.client.request("tools/call", {"name": "app_launch", "arguments": arguments})["result"]
        self.assertTrue(result["isError"])
        return result["content"][0]["text"]

    def test_isolated_launch_gets_private_tmpdir(self):
        # Exits straight away, as Krita does when it finds another instance.
        prefix = self._fake_prefix('echo "TMPDIR=$TMPDIR"\nexit 3\n')
        text = self._launch_error({"prefix": prefix, "headless": True, "timeout": 10})
        tmp = [line[len("TMPDIR="):] for line in text.splitlines() if line.startswith("TMPDIR=")]
        self.assertEqual(len(tmp), 1, text)
        self.assertRegex(tmp[0], r"komapaint-mcp-profile-[^/]+/tmp$")
        self.assertFalse(os.path.exists(tmp[0]), "profile not removed after a failed launch")

    def test_exit_zero_reports_running_instance(self):
        prefix = self._fake_prefix("exit 0\n")
        text = self._launch_error({"prefix": prefix, "isolated": False, "headless": True, "timeout": 10})
        self.assertIn("Another Koma Paint is probably already running", text)
        self.assertIn("app_attach", text)

    def test_status_before_launch(self):
        self.client.initialize()
        status, _ = self.client.call("app_status")
        self.assertFalse(status["running"])
        self.assertFalse(status["connected"])


@unittest.skipUnless(os.environ.get("KOMA_MCP_INTEGRATION") == "1",
                     "set KOMA_MCP_INTEGRATION=1 to launch Koma Paint")
class EndToEndTest(unittest.TestCase):
    """Drives a headless Koma Paint in a throwaway profile."""

    @classmethod
    def setUpClass(cls):
        cls.client = McpClient()
        cls.client.initialize()
        cls.client.call("app_launch", {"headless": True})

    @classmethod
    def tearDownClass(cls):
        try:
            result, _ = cls.client.call("app_quit")
            assert result["exit_code"] == 0, result
        finally:
            cls.client.close()

    def test_painting_undo_dialogs_and_screenshots(self):
        call = self.client.call
        call("ui_resize_window", {"width": 1600, "height": 1000})
        call("doc_new", {"width": 1000, "height": 1000, "name": "E2E"})
        call("layer_add", {"name": "Ink"})

        # Engine stroke.
        result, _ = call("paint_stroke", {"points": [[100, 100, 0.2], [400, 150, 1.0]]})
        self.assertGreater(result["bounds_after"][2], 0)

        # Canvas input through the brush tool, then undo with the shortcut.
        call("tool_activate", {"id": "KritaShape/KisToolBrush"})
        before, _ = call("pixel_stats", {"node": "Ink", "rect": [0, 250, 1000, 750]})
        self.assertEqual(before["non_transparent"], 0)
        call("canvas_drag", {"points": [[300, 400], [500, 600], [700, 800]]})
        stroke, _ = call("pixel_stats", {"node": "Ink", "rect": [0, 250, 1000, 750]})
        self.assertGreater(stroke["non_transparent"], 0)
        call("ui_key", {"keys": "Ctrl+Z", "widget": "canvas"})
        undone, _ = call("pixel_stats", {"node": "Ink", "rect": [0, 250, 1000, 750]})
        self.assertEqual(undone["non_transparent"], 0)

        # Pen input with pressure.
        call("canvas_drag", {"device": "pen", "points": [[100, 900, 0.2], [500, 920, 1.0], [900, 900, 0.3]]})
        pen, _ = call("pixel_stats", {"node": "Ink", "rect": [0, 850, 1000, 150]})
        self.assertGreater(pen["non_transparent"], 0)

        # Space + drag pans the view.
        start, _ = call("canvas_view")
        call("canvas_drag", {"points": [[600, 500], [500, 450], [400, 400]], "hold_keys": ["Space"]})
        panned, _ = call("canvas_view")
        self.assertNotEqual(start["center"], panned["center"])

        # Ctrl + Alt + drag changes the brush size.
        size_before, _ = call("brush_get")
        call("canvas_drag", {"points": [[500, 500], [560, 500], [620, 500], [680, 500]],
                             "modifiers": ["ctrl", "alt"], "hold_keys": ["Ctrl", "Alt"]})
        size_after, _ = call("brush_get")
        self.assertNotEqual(size_before["size"], size_after["size"])

        # A modal dialog does not block the bridge.
        call("action_trigger", {"name": "file_new"})
        windows, _ = call("ui_windows")
        self.assertIsNotNone(windows["active_modal"])
        cancel, _ = call("ui_find", {"class": "QPushButton", "text": "Cancel"})
        self.assertEqual(len(cancel["matches"]), 1)
        call("ui_click", {"widget": cancel["matches"][0]["id"]})
        windows, _ = call("ui_windows")
        self.assertIsNone(windows["active_modal"])

        # Images come back as MCP image content.
        _, images = call("ui_screenshot", {"max_size": 400})
        self.assertEqual(images[0]["mimeType"], "image/png")
        _, images = call("render_document", {"max_size": 200})
        self.assertEqual(len(images), 1)

        logs, _ = call("app_logs", {"grep": "koma_mcp"})
        self.assertTrue(any("bridge listening" in line["text"] for line in logs["lines"]))

    def test_exports_leave_out_draft_layers(self):
        call = self.client.call
        top, bottom = [0, 0, 400, 200], [0, 200, 400, 200]
        info, _ = call("doc_new", {"width": 400, "height": 400, "name": "Drafts"})
        document = info["name"]
        # Hide the white Paper layer so that exported pixels are only the strokes.
        paper = [layer for layer in info["layers"] if layer["name"] == "Paper"]
        for layer in paper:
            call("layer_set", {"node": layer["id"], "properties": {"visible": False}})

        call("layer_add", {"name": "Ink"})
        call("paint_stroke", {"points": [[50, 100, 1.0], [350, 100, 1.0]]})
        call("layer_add", {"name": "Sketch"})
        call("paint_stroke", {"points": [[50, 300, 1.0], [350, 300, 1.0]]})

        # The Layers panel's quick toggle marks the active layer as a draft.
        toggle, _ = call("ui_find", {"class": "QToolButton", "text": "Set as draft layer"})
        self.assertEqual(len(toggle["matches"]), 1)
        call("ui_click", {"widget": toggle["matches"][0]["id"]})
        toggle, _ = call("ui_find", {"class": "QToolButton", "text": "Set as draft layer"})
        self.assertTrue(toggle["matches"][0]["checked"])
        on_canvas, _ = call("pixel_stats", {"rect": bottom})
        self.assertGreater(on_canvas["non_transparent"], 0)

        directory = tempfile.mkdtemp(prefix="koma-mcp-draft-")
        try:
            def draft_and_ink(path):
                """Opaque pixels of the Ink (top) and Sketch (bottom) strokes in a saved file."""
                if path.endswith(".png"):
                    # Read the file itself: a document saved as this path is still open.
                    result, _ = call("python_exec", {"code": (
                        "from PyQt6.QtGui import QImage\n"
                        "image = QImage(%r)\n"
                        "[sum(image.pixelColor(x, y).alpha() > 0 for x in range(400) for y in rows)\n"
                        " for rows in (range(0, 200), range(200, 400))]") % path})
                    ink, draft = json.loads(result["value"])
                else:
                    call("doc_open", {"path": path})
                    ink = call("pixel_stats", {"document": path, "rect": top})[0]["non_transparent"]
                    draft = call("pixel_stats", {"document": path, "rect": bottom})[0]["non_transparent"]
                    call("doc_close", {"document": path, "discard": True})
                self.assertGreater(ink, 0, path)
                return draft

            # Scripted exports (Document.exportImage) leave the draft out...
            png = os.path.join(directory, "export.png")
            call("doc_export", {"document": document, "path": png})
            after, _ = call("pixel_stats", {"document": document, "rect": bottom})
            # ...without touching the open document...
            self.assertEqual(after["non_transparent"], on_canvas["non_transparent"])
            self.assertEqual(draft_and_ink(png), 0)

            # ...and a .kra keeps it.
            kra = os.path.join(directory, "export.kra")
            call("doc_export", {"document": document, "path": kra})
            self.assertGreater(draft_and_ink(kra), 0)

            # Saving as PNG goes through the background saving path, which leaves it out too.
            saved = os.path.join(directory, "saved.png")
            call("doc_save", {"document": document, "path": saved})
            self.assertEqual(draft_and_ink(saved), 0)
        finally:
            shutil.rmtree(directory, ignore_errors=True)
        call("doc_close", {"document": document, "discard": True})


if __name__ == "__main__":
    unittest.main()
