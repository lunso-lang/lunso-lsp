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
LUNSO_BIN = ROOT.parent / "lunsoc" / "build" / "lunsoc"
STD_DIR = ROOT.parent / "lunsoc" / "std"


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
                "import std.collections { Vec }\n"
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
                "    var tokens: Vec[Inner]\n"
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
            self.assertTrue({"data", "len", "cap", "arena", "push", "push_value", "as_slice", "as_const_slice"}.issubset(set(comp_vec_map.keys())))
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
            self.assertIn("defer", comp_main)
            self.assertIn("return", comp_main)

            # Prefix filtering on completion: typing "a" must not return "false"
            send({
                "jsonrpc": "2.0",
                "id": 78,
                "method": "textDocument/completion",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 36, "character": 5}},
            })
            # At line 36 ("    return helper(local_val)"), char 5 is "r" -> only 'r*' items
            comp_r = {item["label"] for item in read_msg(proc.stdout)["result"]}
            self.assertIn("return", comp_r)
            self.assertNotIn("false", comp_r)
            self.assertNotIn("defer", comp_r)

            # signatureHelp on math.add_two(x) at line 20 char 24
            send({
                "jsonrpc": "2.0",
                "id": 76,
                "method": "textDocument/signatureHelp",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 20, "character": 24}},
            })
            sig_resp = read_msg(proc.stdout)["result"]
            self.assertIn("pub fun add_two(v: i32): i32", sig_resp["signatures"][0]["label"])
            self.assertEqual(sig_resp["activeParameter"], 0)

            # signatureHelp on struct literal `Box[T] { value: item }` at line 17 char 22
            send({
                "jsonrpc": "2.0",
                "id": 77,
                "method": "textDocument/signatureHelp",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 17, "character": 22}},
            })
            ssig_resp = read_msg(proc.stdout)["result"]
            self.assertIn("struct Box { value: T }", ssig_resp["signatures"][0]["label"])
            self.assertEqual(ssig_resp["signatures"][0]["parameters"][0]["label"], "value: T")

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

            # 8b. Go to definition on `import support.math` and selective `{ add_two }`
            send({
                "jsonrpc": "2.0",
                "id": 80,
                "method": "textDocument/definition",
                "params": {"textDocument": {"uri": sub_uri}, "position": {"line": 0, "character": 16}},
            })
            def_mod = read_msg(proc.stdout)["result"]
            self.assertEqual(def_mod["uri"], (workdir / "support" / "math.lunso").as_uri())
            self.assertEqual(def_mod["range"]["start"]["line"], 0)

            send({
                "jsonrpc": "2.0",
                "id": 81,
                "method": "textDocument/definition",
                "params": {"textDocument": {"uri": sub_uri}, "position": {"line": 0, "character": 23}},
            })
            def_sel = read_msg(proc.stdout)["result"]
            self.assertEqual(def_sel["uri"], (workdir / "support" / "math.lunso").as_uri())
            self.assertEqual(def_sel["range"]["start"]["line"], 1)

            # 8c. Hover & Go to definition on struct fields (including generic `tokens.data`)
            # Re-open `uri` with original source first since step 7 ran `didChange` on `uri`
            send({
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": uri,
                        "languageId": "lunso",
                        "version": 3,
                        "text": source,
                    }
                },
            })
            _diag_reopen = read_msg(proc.stdout)
            send({
                "jsonrpc": "2.0",
                "id": 83,
                "method": "textDocument/hover",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 31, "character": 12}},
            })
            hover_field = read_msg(proc.stdout)["result"]
            self.assertIn("data: *Inner", hover_field["contents"]["value"])

            send({
                "jsonrpc": "2.0",
                "id": 84,
                "method": "textDocument/definition",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 31, "character": 12}},
            })
            def_field = read_msg(proc.stdout)["result"]
            self.assertTrue(def_field["uri"].endswith("/std/collections.lunso"))
            self.assertEqual(def_field["range"]["start"]["line"], 29)

            # 8d. documentSymbol
            send({
                "jsonrpc": "2.0",
                "id": 85,
                "method": "textDocument/documentSymbol",
                "params": {"textDocument": {"uri": uri}},
            })
            doc_syms = {s["name"] for s in read_msg(proc.stdout)["result"]}
            self.assertTrue({"Inner", "Box", "Pair", "wrap", "helper", "main"}.issubset(doc_syms))

            send({"jsonrpc": "2.0", "id": 8, "method": "shutdown", "params": None})
            self.assertIsNone(read_msg(proc.stdout)["result"])
            send({"jsonrpc": "2.0", "method": "exit", "params": None})
            proc.stdin.close()
            proc.stdout.close()
            proc.stderr.close()
            self.assertEqual(proc.wait(timeout=5), 0)

    def test_traits_and_tagged_unions(self) -> None:
        env = os.environ.copy()
        if LUNSO_BIN.exists():
            env["LUNSO"] = str(LUNSO_BIN)
        if STD_DIR.exists():
            env["LUNSO_STD_DIR"] = str(STD_DIR)

        with tempfile.TemporaryDirectory() as tmp:
            workdir = Path(tmp)
            doc_path = workdir / "features_06.lunso"
            uri = doc_path.as_uri()
            source = (
                "import std.io\n"
                "import std.io.\n"
                "import std.collections { HashMap }\n"
                "import std.mem\n"
                "pub trait Printable {\n"
                "    fun print_me(v: i32)\n"
                "}\n"
                "enum Value : Eq, Hash {\n"
                "    None,\n"
                "    Int(i64),\n"
                "}\n"
                "fun eval_v(v: Value): i64 {\n"
                "    match v {\n"
                "        Value.None => return 0\n"
                "        Value.Int(num) => {\n"
                "            return num\n"
                "        }\n"
                "    }\n"
                "}\n"
                "fun main(): i32 {\n"
                "    var a: mem.Arena\n"
                "    a.init(4096)\n"
                "    var m: HashMap[[]u8, Value]\n"
                "    m.init(&a, 8)\n"
                "    var v = Value.Int(42)\n"
                "    var x = Value.\n"
                "    return eval_v(v) as i32\n"
                "}\n"
            )
            doc_path.write_text(source, encoding="utf-8")

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

            send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"rootUri": workdir.as_uri(), "capabilities": {}}})
            _init = read_msg(proc.stdout)
            send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {"uri": uri, "languageId": "lunso", "version": 1, "text": source}}})
            _diag = read_msg(proc.stdout)

            # documentSymbol includes trait Printable and enum Value
            send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/documentSymbol", "params": {"textDocument": {"uri": uri}}})
            syms = {s["name"] for s in read_msg(proc.stdout)["result"]}
            self.assertTrue({"Printable", "Value", "eval_v", "main"}.issubset(syms))

            # Hover on Value.Int (line 24: `    var v = Value.Int(42)`)
            send({"jsonrpc": "2.0", "id": 3, "method": "textDocument/hover", "params": {"textDocument": {"uri": uri}, "position": {"line": 24, "character": 18}}})
            hv = read_msg(proc.stdout)["result"]
            self.assertIn("Int(i64)", hv["contents"]["value"])

            # Definition on Value.Int jumps to line 9
            send({"jsonrpc": "2.0", "id": 4, "method": "textDocument/definition", "params": {"textDocument": {"uri": uri}, "position": {"line": 24, "character": 18}}})
            df = read_msg(proc.stdout)["result"]
            self.assertEqual(df["range"]["start"]["line"], 9)

            # Completion after `Value.` (line 25: `    var x = Value.`)
            send({"jsonrpc": "2.0", "id": 5, "method": "textDocument/completion", "params": {"textDocument": {"uri": uri}, "position": {"line": 25, "character": 18}}})
            comp = {item["label"] for item in read_msg(proc.stdout)["result"]}
            self.assertEqual(comp, {"None", "Int"})

            # Definition on `num` inside match arm (line 15: `            return num`) jumps to line 14
            send({"jsonrpc": "2.0", "id": 6, "method": "textDocument/definition", "params": {"textDocument": {"uri": uri}, "position": {"line": 15, "character": 20}}})
            df_num = read_msg(proc.stdout)["result"]
            self.assertEqual(df_num["range"]["start"]["line"], 14)

            # Definition on struct method call `m.init(&a, 8)` (line 23: `    m.init(&a, 8)`) jumps to std/collections.lunso
            send({"jsonrpc": "2.0", "id": 8, "method": "textDocument/definition", "params": {"textDocument": {"uri": uri}, "position": {"line": 23, "character": 7}}})
            df_method = read_msg(proc.stdout)["result"]
            self.assertTrue(df_method["uri"].endswith("/std/collections.lunso"))

            # Completion after `import std.io.` suggests exported symbols like println and *
            send({"jsonrpc": "2.0", "id": 9, "method": "textDocument/completion", "params": {"textDocument": {"uri": uri}, "position": {"line": 1, "character": 14}}})
            comp_imp = {item["label"] for item in read_msg(proc.stdout)["result"]}
            self.assertIn("println", comp_imp)
            self.assertIn("*", comp_imp)

            send({"jsonrpc": "2.0", "id": 7, "method": "shutdown", "params": None})
            self.assertIsNone(read_msg(proc.stdout)["result"])
            send({"jsonrpc": "2.0", "method": "exit", "params": None})
            proc.stdin.close()
            proc.stdout.close()
            proc.stderr.close()
            self.assertEqual(proc.wait(timeout=5), 0)


if __name__ == "__main__":
    unittest.main()
