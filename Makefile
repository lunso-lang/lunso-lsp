CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
PYTHON ?= python3

.PHONY: all clean test

all: build/lunso-lsp

build/lunso-lsp: src/main.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

test: build/lunso-lsp
	cd tree-sitter-lunso && tree-sitter test
	$(PYTHON) tests/test_lsp.py

clean:
	rm -f build/lunso-lsp
