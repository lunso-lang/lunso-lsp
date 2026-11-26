LUNSO ?= ../lunsoc/build/lunsoc
LUNSOC_SRC ?= ../lunsoc/src
LUNSO_STD ?= ../lunsoc/std
LUNSO_STD_OBJ ?= ../lunsoc/build/std-linux-x86_64.o

.PHONY: all test clean

all: build/lunso-lsp

build/lunso-lsp: src/main.lunso src/transport.lunso src/analysis.lunso
	mkdir -p build
	LUNSO_STD_OBJ=$(CURDIR)/$(LUNSO_STD_OBJ) $(LUNSO) --build -I $(CURDIR)/src -I $(LUNSOC_SRC) --std $(LUNSO_STD) src/main.lunso -o build/lunso-lsp

test: all
	python3 tests/run.py

clean:
	rm -rf build
