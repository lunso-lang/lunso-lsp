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
                "import std.vec\n"
                "import support.math\n"
                "import \n"
                "import std.\n"
                "import support.\n"
                "struct Inner {\n"
                "    val: i32\n"
                "}\n"
                "struct Box[T] {\n"
                "    value: T\n"
                "}\n"
                "struct Pair {\n"
                "    a: i32\n"
                "    nested: Inner\n"
                "}\n"
                "fun wrap[T](item: T): Box[T] {\n"
                "    return Box[T] { value: item }\n"
                "}\n"
                "fun helper(x: i32): i32 {\n"
                "    return math.add_two(x)\n"
                "}\n"
                "fun main(): i32 {\n"
                "    io.\n"
                "    var local_val: i32 = 42\n"
                "    var items: []Pair\n"
                "    items[0].\n"
                "    items[0].nested.\n"
                "    unknown_var.\n"
                "    var tokens: vec.Vec[Inner]\n"
                "    tokens.\n"
                "    tokens.data[0].\n"
                "    wrap.[Inner](tokens.data[0]).\n"
                "    for var idx: i32 = 0; idx < 3; idx += 1 {\n"
                "        var inner = idx + local_val\n"
                "    }\n"
                "    return helper(local_val)\n"
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
                "params": {"textDocument": {"uri": uri}, "position": {"line": 3, "character": 7}},
            })
            comp_root = {item["label"] for item in read_msg(proc.stdout)["result"]}
            self.assertIn("std", comp_root)
            self.assertIn("support", comp_root)

            # 2. `import std.` completion
            send({
                "jsonrpc": "2.0",
                "id": 3,
                "method": "textDocument/completion",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 4, "character": 11}},
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
                "params": {"textDocument": {"uri": uri}, "position": {"line": 5, "character": 15}},
            })
            comp_sup = {item["label"] for item in read_msg(proc.stdout)["result"]}
            self.assertIn("math", comp_sup)

            # 4. `io.` member completion
            send({
                "jsonrpc": "2.0",
                "id": 5,
                "method": "textDocument/completion",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 23, "character": 7}},
            })
            comp_io = {item["label"] for item in read_msg(proc.stdout)["result"]}
            self.assertIn("println", comp_io)
            self.assertIn("eprintln", comp_io)

            # 4b. `items[0].` and `items[0].nested.` struct field completion (no keywords!)
            send({
                "jsonrpc": "2.0",
                "id": 51,
                "method": "textDocument/completion",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 26, "character": 13}},
            })
            comp_item = {item["label"] for item in read_msg(proc.stdout)["result"]}
            self.assertEqual(comp_item, {"a", "nested"})

            send({
                "jsonrpc": "2.0",
                "id": 52,
                "method": "textDocument/completion",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 27, "character": 20}},
            })
            comp_nested = {item["label"] for item in read_msg(proc.stdout)["result"]}
            self.assertEqual(comp_nested, {"val"})

            send({
                "jsonrpc": "2.0",
                "id": 53,
                "method": "textDocument/completion",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 28, "character": 16}},
            })
            comp_unknown = read_msg(proc.stdout)["result"]
            self.assertEqual(comp_unknown, [])

            # 4c. Generics: `tokens.`, `tokens.data[0].`, `wrap.[Inner](...).`
            send({
                "jsonrpc": "2.0",
                "id": 54,
                "method": "textDocument/completion",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 30, "character": 11}},
            })
            comp_vec_items = read_msg(proc.stdout)["result"]
            comp_vec_map = {item["label"]: item.get("detail") for item in comp_vec_items}
            self.assertEqual(set(comp_vec_map.keys()), {"data", "len", "cap", "arena"})
            self.assertEqual(comp_vec_map["data"], "data: *Inner")

            send({
                "jsonrpc": "2.0",
                "id": 55,
                "method": "textDocument/completion",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 31, "character": 19}},
            })
            comp_vec_elem = {item["label"] for item in read_msg(proc.stdout)["result"]}
            self.assertEqual(comp_vec_elem, {"val"})

            send({
                "jsonrpc": "2.0",
                "id": 56,
                "method": "textDocument/completion",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 32, "character": 34}},
            })
            comp_wrap = read_msg(proc.stdout)["result"]
            comp_wrap_map = {item["label"]: item.get("detail") for item in comp_wrap}
            self.assertEqual(comp_wrap_map, {"value": "value: Inner"})

            # 5. Hover on imported `math.add_two`
            send({
                "jsonrpc": "2.0",
                "id": 6,
                "method": "textDocument/hover",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 20, "character": 18}},
            })
            hover_imp = read_msg(proc.stdout)["result"]
            self.assertIn("pub fun add_two(v: i32): i32", hover_imp["contents"]["value"])

            # 6. Go to definition on imported `math.add_two`
            send({
                "jsonrpc": "2.0",
                "id": 7,
                "method": "textDocument/definition",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 20, "character": 18}},
            })
            def_imp = read_msg(proc.stdout)["result"]
            self.assertEqual(def_imp["uri"], (workdir / "support" / "math.lunso").as_uri())
            self.assertEqual(def_imp["range"]["start"]["line"], 1)

            # 6b. Hover & Go to definition on parameter `x` and generic param `item`
            send({
                "jsonrpc": "2.0",
                "id": 70,
                "method": "textDocument/hover",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 20, "character": 24}},
            })
            hover_param = read_msg(proc.stdout)["result"]
            self.assertIn("x: i32", hover_param["contents"]["value"])

            send({
                "jsonrpc": "2.0",
                "id": 701,
                "method": "textDocument/hover",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 17, "character": 28}},
            })
            hover_gparam = read_msg(proc.stdout)["result"]
            self.assertIn("item: T", hover_gparam["contents"]["value"])

            send({
                "jsonrpc": "2.0",
                "id": 71,
                "method": "textDocument/definition",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 20, "character": 24}},
            })
            def_param = read_msg(proc.stdout)["result"]
            self.assertEqual(def_param["uri"], uri)
            self.assertEqual(def_param["range"]["start"]["line"], 19)
            self.assertEqual(def_param["range"]["start"]["character"], 11)

            # 6c. Hover & Go to definition on local `local_val` and loop var `idx`
            send({
                "jsonrpc": "2.0",
                "id": 72,
                "method": "textDocument/hover",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 36, "character": 20}},
            })
            hover_local = read_msg(proc.stdout)["result"]
            self.assertIn("var local_val: i32 = 42", hover_local["contents"]["value"])

            send({
                "jsonrpc": "2.0",
                "id": 73,
                "method": "textDocument/definition",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 36, "character": 20}},
            })
            def_local = read_msg(proc.stdout)["result"]
            self.assertEqual(def_local["uri"], uri)
            self.assertEqual(def_local["range"]["start"]["line"], 24)
            self.assertEqual(def_local["range"]["start"]["character"], 8)

            send({
                "jsonrpc": "2.0",
                "id": 74,
                "method": "textDocument/hover",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 34, "character": 21}},
            })
            hover_loop = read_msg(proc.stdout)["result"]
            self.assertIn("var idx: i32 = 0", hover_loop["contents"]["value"])

            # Completion inside `main` includes `local_val`
            send({
                "jsonrpc": "2.0",
                "id": 75,
                "method": "textDocument/completion",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 36, "character": 11}},
            })
            comp_main = {item["label"] for item in read_msg(proc.stdout)["result"]}
            self.assertIn("local_val", comp_main)
            self.assertNotIn("idx", comp_main)

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

            # 8. Submodule in nested directory importing sibling via project root (`main.lunso`)
            (workdir / "main.lunso").write_text(
                "import support.sub\n"
                "fun main(): i32 { return support.sub.use_math(3) }\n",
                encoding="utf-8",
            )
            sub_path = workdir / "support" / "sub.lunso"
            sub_text = (
                "import support.math { add_two }\n"
                "pub fun use_math(x: i32): i32 {\n"
                "    return add_two(x)\n"
                "}\n"
            )
            sub_path.write_text(sub_text, encoding="utf-8")
            sub_uri = sub_path.as_uri()
            send({
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": sub_uri,
                        "languageId": "lunso",
                        "version": 1,
                        "text": sub_text,
                    }
                },
            })
            sub_diag = read_msg(proc.stdout)
            self.assertEqual(sub_diag["params"]["diagnostics"], [])

            send({
                "jsonrpc": "2.0",
                "id": 79,
                "method": "textDocument/definition",
                "params": {"textDocument": {"uri": sub_uri}, "position": {"line": 2, "character": 12}},
            })
            sub_def = read_msg(proc.stdout)["result"]
            self.assertEqual(sub_def["uri"], (workdir / "support" / "math.lunso").as_uri())

            send({"jsonrpc": "2.0", "id": 8, "method": "shutdown", "params": None})
            self.assertIsNone(read_msg(proc.stdout)["result"])
            send({"jsonrpc": "2.0", "method": "exit", "params": None})
            proc.stdin.close()
            proc.stdout.close()
            proc.stderr.close()
            self.assertEqual(proc.wait(timeout=5), 0)


if __name__ == "__main__":
    unittest.main()
