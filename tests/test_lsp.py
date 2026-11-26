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
STD_DIR = Path.home() / "projects" / "lunso-boot" / "std"


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
    def test_lsp_lifecycle_and_cross_module_features(self) -> None:
        env = os.environ.copy()
        if LUNSO_BIN.exists():
            env["LUNSO"] = str(LUNSO_BIN)
        if STD_DIR.exists():
            env["LUNSO_STD_DIR"] = str(STD_DIR)

        with tempfile.TemporaryDirectory() as tmp:
            workdir = Path(tmp)
            (workdir / "support").mkdir()
            (workdir / "support" / "math.lunso").write_text(
                "pub const OFFSET: i32 = 5\n"
                "pub fun add_two(v: i32): i32 {\n"
                "    return v + 2\n"
                "}\n"
                "priv fun hidden(): i32 {\n"
                "    return 99\n"
                "}\n",
                encoding="utf-8",
            )
            doc_path = workdir / "demo.lunso"
            uri = doc_path.as_uri()
            source = (
                "import std.io\n"
                "import support.math\n"
                "import \n"
                "import std.\n"
                "import support.\n"
                "struct Pair {\n"
                "    a: i32\n"
                "}\n"
                "fun helper(x: i32): i32 {\n"
                "    return math.add_two(x)\n"
                "}\n"
                "fun main(): i32 {\n"
                "    io.\n"
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
            assert proc.stdin is not None and proc.stdout is not None and proc.stderr is not None

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
            _diag = read_msg(proc.stdout)

            # 1. `import ` completion (root)
            send({
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/completion",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 2, "character": 7}},
            })
            comp_root = {item["label"] for item in read_msg(proc.stdout)["result"]}
            self.assertIn("std", comp_root)
            self.assertIn("support", comp_root)

            # 2. `import std.` completion
            send({
                "jsonrpc": "2.0",
                "id": 3,
                "method": "textDocument/completion",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 3, "character": 11}},
            })
            comp_std = {item["label"] for item in read_msg(proc.stdout)["result"]}
            self.assertIn("io", comp_std)
            self.assertIn("mem", comp_std)
            self.assertIn("file", comp_std)

            # 3. `import support.` completion
            send({
                "jsonrpc": "2.0",
                "id": 4,
                "method": "textDocument/completion",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 4, "character": 15}},
            })
            comp_sup = {item["label"] for item in read_msg(proc.stdout)["result"]}
            self.assertIn("math", comp_sup)

            # 4. `io.` member completion
            send({
                "jsonrpc": "2.0",
                "id": 5,
                "method": "textDocument/completion",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 12, "character": 7}},
            })
            comp_io = {item["label"] for item in read_msg(proc.stdout)["result"]}
            self.assertIn("println", comp_io)
            self.assertIn("eprintln", comp_io)

            # 5. Hover on imported `math.add_two`
            send({
                "jsonrpc": "2.0",
                "id": 6,
                "method": "textDocument/hover",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 9, "character": 18}},
            })
            hover_imp = read_msg(proc.stdout)["result"]
            self.assertIn("pub fun add_two(v: i32): i32", hover_imp["contents"]["value"])

            # 6. Go to definition on imported `math.add_two`
            send({
                "jsonrpc": "2.0",
                "id": 7,
                "method": "textDocument/definition",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 9, "character": 18}},
            })
            def_imp = read_msg(proc.stdout)["result"]
            self.assertEqual(def_imp["uri"], (workdir / "support" / "math.lunso").as_uri())
            self.assertEqual(def_imp["range"]["start"]["line"], 1)

            # 7. Diagnostics on valid single-file change
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

            send({"jsonrpc": "2.0", "id": 8, "method": "shutdown", "params": None})
            self.assertIsNone(read_msg(proc.stdout)["result"])
            send({"jsonrpc": "2.0", "method": "exit", "params": None})
            proc.stdin.close()
            proc.stdout.close()
            proc.stderr.close()
            self.assertEqual(proc.wait(timeout=5), 0)


if __name__ == "__main__":
    unittest.main()
