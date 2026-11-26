# lunso-lsp

Language Server Protocol (LSP) server for the Lunso programming language, written in Lunso (`>= 0.3.1`).

## Features

- `textDocument/publishDiagnostics` (with in-memory overlays for unsaved editor buffers)
- `textDocument/hover`
- `textDocument/definition`
- `textDocument/completion`

## Build & Test

```sh
make
make test
```
