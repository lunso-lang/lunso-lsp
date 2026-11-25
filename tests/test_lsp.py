#!/usr/bin/env python3
"""Integration tests for lunso-lsp over JSON-RPC stdio."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
LSP_BIN = ROOT / "build" / "lunso-lsp"
LUNSO_BIN = Path.home() / "projects" / "lunso-boot" / "build" / "lunso"


def encode_msg(payload: dict) -> bytes:
    raw = json.dumps(payload).encode("utf-8")
    return f"Content-Length: {len(raw)}\r\n\r\n".encode("ascii") + raw


def read_msg(stream) -> dict:
    content_length = 0
    while True:
        line = stream.readline()
        if not line:
            raise EOFError("unexpected EOF from lunso-lsp")
        if line in (b"\r\n", b"\n"):
            break
        if line.lower().startswith(b"content-length:"):
            content_length = int(line.split(b":", 1)[1].strip())
    body = stream.read(content_length)
    return json.loads(body.decode("utf-8"))


class TestLunsoLsp(unittest.TestCase):
    def test_lsp_lifecycle_diagnostics_symbols_hover_def_completion(self) -> None:
        env = os.environ.copy()
        if LUNSO_BIN.exists():
            env["LUNSO"] = str(LUNSO_BIN)

        with tempfile.TemporaryDirectory() as tmp:
            doc_path = Path(tmp) / "demo.lunso"
            uri = doc_path.as_uri()
            source = (
                "struct Pair {\n"
                "    a: i32\n"
                "}\n"
                "fun helper(x: i32): i32 {\n"
                "    return x + 1\n"
                "}\n"
                "fun main(): i32 {\n"
                "    return helper(10)\n"
                "}\n"
            )

            proc = subprocess.Popen(
                [str(LSP_BIN)],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=env,
            )
            assert proc.stdin is not None and proc.stdout is not None

            def send(msg: dict) -> None:
                proc.stdin.write(encode_msg(msg))
                proc.stdin.flush()

            send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
            init_resp = read_msg(proc.stdout)
            self.assertEqual(init_resp["id"], 1)
            self.assertTrue(init_resp["result"]["capabilities"]["hoverProvider"])

            send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            send({
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": uri,
                        "languageId": "lunso",
                        "version": 1,
                        "text": source,
                    }
                },
            })
            diag_ok = read_msg(proc.stdout)
            self.assertEqual(diag_ok["method"], "textDocument/publishDiagnostics")
            self.assertEqual(diag_ok["params"]["diagnostics"], [])

            send({
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/documentSymbol",
                "params": {"textDocument": {"uri": uri}},
            })
            syms = read_msg(proc.stdout)["result"]
            self.assertEqual([s["name"] for s in syms], ["Pair", "helper", "main"])

            send({
                "jsonrpc": "2.0",
                "id": 3,
                "method": "textDocument/hover",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 7, "character": 12}},
            })
            hover = read_msg(proc.stdout)["result"]
            self.assertIn("fun helper(x: i32): i32", hover["contents"]["value"])

            send({
                "jsonrpc": "2.0",
                "id": 4,
                "method": "textDocument/definition",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 7, "character": 12}},
            })
            defn = read_msg(proc.stdout)["result"]
            self.assertEqual(defn["range"]["start"]["line"], 3)

            send({
                "jsonrpc": "2.0",
                "id": 5,
                "method": "textDocument/completion",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 7, "character": 11}},
            })
            comp = read_msg(proc.stdout)["result"]
            labels = {item["label"] for item in comp}
            self.assertIn("match", labels)
            self.assertIn("helper", labels)

            # Introduce an error and verify diagnostics catch it.
            send({
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 2},
                    "contentChanges": [{"text": "fun main(): i32 {\n    var x: i32 = \"bad\"\n    return x\n}\n"}],
                },
            })
            diag_err = read_msg(proc.stdout)
            self.assertEqual(diag_err["method"], "textDocument/publishDiagnostics")
            self.assertGreaterEqual(len(diag_err["params"]["diagnostics"]), 1)

            send({"jsonrpc": "2.0", "id": 6, "method": "shutdown", "params": None})
            shut = read_msg(proc.stdout)
            self.assertIsNone(shut["result"])
            send({"jsonrpc": "2.0", "method": "exit", "params": None})
            proc.stdin.close()
            proc.stdout.close()
            proc.stderr.close()
            self.assertEqual(proc.wait(timeout=5), 0)


if __name__ == "__main__":
    unittest.main()
