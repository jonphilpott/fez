# FEZ build. Plain make and a C compiler; SDL2 for the apps.
#   make test      build and run the tests
#   make           build the SDL app (./fez)
CC      ?= cc
# -D_DEFAULT_SOURCE: strict C99 hides POSIX calls (opendir, mkdir) on Linux's glibc; macOS shows them anyway.
# -ffp-contract=off: never fuse a*b+c into one instruction. A fused multiply-add rounds differently, and compilers on
# Apple Silicon and ARM fuse by default; this keeps the Lorenz attractor and the filters giving the same numbers on every machine.
CFLAGS  ?= -std=c99 -D_DEFAULT_SOURCE -O2 -g -Wall -Wextra -Wpedantic -ffp-contract=off
CORE    := $(wildcard core/*.c)
SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
SDL_LIBS   := $(shell sdl2-config --libs 2>/dev/null)

all: fez

# The app: a tiled desktop window, or with --brick the handheld version (256x192, Brick-style buttons) of the same screens.
DESK := $(wildcard desk/*.c)

fez: $(CORE) $(DESK) core/*.h desk/*.h
	$(CC) $(CFLAGS) -Icore -Idesk $(SDL_CFLAGS) $(CORE) $(DESK) -o $@ $(SDL_LIBS) -lm

TESTS := $(wildcard test/*.c)

render: $(CORE) tools/render.c core/*.h
	$(CC) $(CFLAGS) -Icore $(CORE) tools/render.c -o $@ -lm

# the tests also build the UI code (everything in desk/ except the SDL shell), to test the handheld controls
test_core: $(CORE) $(TESTS) core/*.h desk/*.h test/test.h
	$(CC) $(CFLAGS) -Icore -Idesk -Itest $(CORE) $(filter-out desk/main.c,$(DESK)) $(TESTS) -o $@ -lm

test: test_core
	./test_core

clean:
	rm -rf fez deskshot padshot test_core render tools/manual_gen dist *.dSYM

# Cross-build for the TrimUI Brick: run inside the toolchain container (brick/build.sh does that). The container's environment
# supplies CC (an aarch64 compiler) and pkg-config pointing at the Brick's own SDL2. Output goes to dist/, so it never overwrites
# the ./fez you run on this machine.
brick: dist/fez dist/probe

dist/fez: $(CORE) $(DESK) core/*.h desk/*.h
	mkdir -p dist
	$(CC) $(CFLAGS) -Icore -Idesk $(shell pkg-config --cflags sdl2) $(CORE) $(DESK) -o $@ $(shell pkg-config --libs sdl2) -lm

dist/probe: brick/probe.c
	mkdir -p dist
	$(CC) -std=c99 -O2 -Wall $(shell pkg-config --cflags sdl2) brick/probe.c -o $@ $(shell pkg-config --libs sdl2) -lm

# The manual: one HTML page built from docs/ and the tables in core/ (see tools/manual.c). Output: docs/manual.html.
manual: docs/manual.html

docs/manual.html: $(CORE) tools/manual.c core/*.h docs/template.html docs/style.css $(wildcard docs/chapters/*.html) $(wildcard docs/shots/*.png)
	$(CC) $(CFLAGS) -Icore $(CORE) tools/manual.c -o tools/manual_gen -lm
	./tools/manual_gen docs/manual.html

# A zip of the Brick pak for a GitHub release: run brick/build.sh first (it needs Docker), then   make brick-zip VERSION=0.1.0
# The zip holds one folder, FEZ.pak, which the user copies to Tools/tg5040/ on the card.
VERSION ?= dev
brick-zip:
	@test -d dist/FEZ.pak || { echo "dist/FEZ.pak is missing: run brick/build.sh first"; exit 1; }
	rm -f dist/FEZ-brick-v$(VERSION).zip
	cd dist && zip -qr FEZ-brick-v$(VERSION).zip FEZ.pak
	@echo "made dist/FEZ-brick-v$(VERSION).zip"

.PHONY: all test clean brick manual brick-zip

# Draws every handheld screen to a picture file without a window (see tools/padshot.c).
padshot: $(CORE) $(DESK) tools/padshot.c core/*.h desk/*.h
	$(CC) $(CFLAGS) -Icore -Idesk $(CORE) $(filter-out desk/main.c,$(DESK)) tools/padshot.c -o $@ -lm

# Draws the desktop layouts to picture files without a window (see tools/deskshot.c).
deskshot: $(CORE) $(DESK) tools/deskshot.c core/*.h desk/*.h
	$(CC) $(CFLAGS) -Icore -Idesk $(CORE) $(filter-out desk/main.c,$(DESK)) tools/deskshot.c -o $@ -lm
