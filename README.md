# lunso-lsp

Language Server Protocol (LSP) implementation, Tree-sitter grammar, and Zed extension for the **Lunso** programming language.

## Build & Test

```sh
make
make test
```

Set `LUNSO=/path/to/lunso` if the `lunso` compiler binary is not in `PATH` or `~/projects/lunso-boot/build/lunso`.

## Zed Extension (`zed/`)

The `zed/` directory contains the Zed extension:
- `zed/extension.toml` — extension manifest (`lunso` grammar + `lunso-lsp` language server)
- `zed/src/lib.rs` — launches `lunso-lsp` from `PATH`
- `zed/languages/lunso/` — `config.toml`, `highlights.scm`, `brackets.scm`, `indents.scm`, `outline.scm`

To test locally in Zed:
1. Build and install `lunso-lsp` (and `lunso`) somewhere in `PATH` (e.g. `~/.local/bin`).
2. In Zed, run **zed: install dev extension** and select the `zed/` directory.
