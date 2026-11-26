#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "lunso-lsp"
STD_DIR = (ROOT.parent / "lunsoc" / "std").resolve()


def send_msg(proc: subprocess.Popen, payload: dict) -> None:
    raw = json.dumps(payload).encode("utf-8")
    header = f"Content-Length: {len(raw)}\r\n\r\n".encode("ascii")
    assert proc.stdin is not None
    proc.stdin.write(header + raw)
    proc.stdin.flush()


def read_msg(proc: subprocess.Popen) -> dict:
    assert proc.stdout is not None
    content_len = None
    while True:
        line = proc.stdout.readline()
        if not line:
            raise RuntimeError("unexpected EOF reading LSP headers")
        text = line.decode("ascii").strip()
        if not text:
            break
        if text.lower().startswith("content-length:"):
            content_len = int(text.split(":", 1)[1].strip())
    assert content_len is not None, "missing Content-Length"
    body = proc.stdout.read(content_len)
    if len(body) != content_len:
        raise RuntimeError(f"truncated LSP body: {len(body)} vs {content_len}")
    return json.loads(body.decode("utf-8"))


def main() -> int:
    if not BIN.is_file():
        print(f"missing binary: {BIN}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)
        helper_path = tmp / "helper.lunso"
        main_path = tmp / "main.lunso"
        helper_uri = helper_path.as_uri()
        main_uri = main_path.as_uri()

        proc = subprocess.Popen(
            [str(BIN), "--std", str(STD_DIR)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        try:
            # 1. initialize
            send_msg(
                proc,
                {
                    "jsonrpc": "2.0",
                    "id": 1,
                    "method": "initialize",
                    "params": {"rootUri": tmp.as_uri(), "capabilities": {}},
                },
            )
            init_resp = read_msg(proc)
            assert init_resp["id"] == 1
            caps = init_resp["result"]["capabilities"]
            assert caps["hoverProvider"] is True
            assert caps["definitionProvider"] is True
            assert init_resp["result"]["serverInfo"]["name"] == "lunso-lsp"

            send_msg(proc, {"jsonrpc": "2.0", "method": "initialized", "params": {}})

            # 2. didOpen unsaved overlay helper.lunso + main.lunso
            helper_src = "pub const FACTOR: i32 = 2\npub fun add(a: i32, b: i32): i32 {\n    return a + b\n}\n"
            main_src = "import helper\n\nfun main(): i32 {\n    return helper.add(1, helper.FACTOR)\n}\n"

            send_msg(
                proc,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {
                        "textDocument": {
                            "uri": helper_uri,
                            "languageId": "lunso",
                            "version": 1,
                            "text": helper_src,
                        }
                    },
                },
            )
            d1 = read_msg(proc)
            assert d1["method"] == "textDocument/publishDiagnostics"
            assert d1["params"]["diagnostics"] == []

            send_msg(
                proc,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {
                        "textDocument": {
                            "uri": main_uri,
                            "languageId": "lunso",
                            "version": 1,
                            "text": main_src,
                        }
                    },
                },
            )
            d2 = read_msg(proc)
            assert d2["method"] == "textDocument/publishDiagnostics"
            assert d2["params"]["diagnostics"] == [], f"unexpected diags: {d2}"

            # 3. hover on helper.add (line 3, col 18)
            send_msg(
                proc,
                {
                    "jsonrpc": "2.0",
                    "id": 2,
                    "method": "textDocument/hover",
                    "params": {
                        "textDocument": {"uri": main_uri},
                        "position": {"line": 3, "character": 19},
                    },
                },
            )
            hover_resp = read_msg(proc)
            assert hover_resp["id"] == 2
            assert "fun add(a: i32, b: i32): i32" in hover_resp["result"]["contents"]["value"], hover_resp

            # 4. definition on helper.add -> helper.lunso line 1, char 8
            send_msg(
                proc,
                {
                    "jsonrpc": "2.0",
                    "id": 3,
                    "method": "textDocument/definition",
                    "params": {
                        "textDocument": {"uri": main_uri},
                        "position": {"line": 3, "character": 19},
                    },
                },
            )
            def_resp = read_msg(proc)
            assert def_resp["id"] == 3
            assert def_resp["result"]["uri"] == helper_uri, def_resp
            assert def_resp["result"]["range"]["start"] == {"line": 1, "character": 8}, def_resp

            # 5. completion after helper. (line 3, col 18 -> right after '.')
            comp_src = "import helper\n\nfun main(): i32 {\n    return helper.\n}\n"
            send_msg(
                proc,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": main_uri, "version": 2},
                        "contentChanges": [{"text": comp_src}],
                    },
                },
            )
            _ = read_msg(proc)  # diagnostics for incomplete syntax
            send_msg(
                proc,
                {
                    "jsonrpc": "2.0",
                    "id": 4,
                    "method": "textDocument/completion",
                    "params": {
                        "textDocument": {"uri": main_uri},
                        "position": {"line": 3, "character": 18},
                    },
                },
            )
            comp_resp = read_msg(proc)
            assert comp_resp["id"] == 4
            labels = {item["label"] for item in comp_resp["result"]}
            assert {"add", "FACTOR"}.issubset(labels), comp_resp

            # 6. didChange with type error -> non-empty diagnostics, then fix -> empty
            bad_src = "fun main(): i32 {\n    return \"oops\"\n}\n"
            send_msg(
                proc,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": main_uri, "version": 3},
                        "contentChanges": [{"text": bad_src}],
                    },
                },
            )
            d_bad = read_msg(proc)
            assert len(d_bad["params"]["diagnostics"]) == 1, d_bad
            assert d_bad["params"]["diagnostics"][0]["range"]["start"]["line"] == 1, d_bad

            send_msg(
                proc,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": main_uri, "version": 4},
                        "contentChanges": [{"text": main_src}],
                    },
                },
            )
            d_fixed = read_msg(proc)
            assert d_fixed["params"]["diagnostics"] == [], d_fixed

            # 7. didClose, shutdown, exit
            send_msg(proc, {"jsonrpc": "2.0", "method": "textDocument/didClose", "params": {"textDocument": {"uri": main_uri}}})
            _ = read_msg(proc)
            send_msg(proc, {"jsonrpc": "2.0", "id": 5, "method": "shutdown", "params": None})
            sh_resp = read_msg(proc)
            assert sh_resp["id"] == 5 and sh_resp["result"] is None
            send_msg(proc, {"jsonrpc": "2.0", "method": "exit", "params": None})

            rc = proc.wait(timeout=5)
            stderr_out = proc.stderr.read().decode("utf-8", errors="replace") if proc.stderr else ""
            assert rc == 0, f"expected exit 0, got {rc}; stderr={stderr_out}"
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()

    print("OK (initialize, overlays, diagnostics, hover, definition, completion, shutdown/exit)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
