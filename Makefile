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

HANDHELD := $(wildcard handheld/*.c)

# The desktop app (keyboard driven, tiled window) and the small-screen app (Brick-style buttons, 256x192 picture).
DESK := $(wildcard desk/*.c)

fez: $(CORE) $(DESK) core/*.h desk/*.h
	$(CC) $(CFLAGS) -Icore -Idesk $(SDL_CFLAGS) $(CORE) $(DESK) -o $@ $(SDL_LIBS) -lm

fez_brick: $(CORE) $(HANDHELD) sdl/brick_main.c core/*.h handheld/*.h
	$(CC) $(CFLAGS) -Icore -Ihandheld $(SDL_CFLAGS) $(CORE) $(HANDHELD) sdl/brick_main.c -o $@ $(SDL_LIBS) -lm

TESTS := $(wildcard test/*.c)

render: $(CORE) tools/render.c core/*.h
	$(CC) $(CFLAGS) -Icore $(CORE) tools/render.c -o $@ -lm

test_core: $(CORE) $(TESTS) core/*.h test/test.h
	$(CC) $(CFLAGS) -Icore -Itest $(CORE) $(TESTS) -o $@ -lm

test: test_core
	./test_core

clean:
	rm -rf fez fez_brick deskshot uishot test_core render *.dSYM


.PHONY: all test clean

# Draws every screen to a picture file without a window (see tools/uishot.c).
uishot: $(CORE) $(HANDHELD) tools/uishot.c core/*.h handheld/*.h
	$(CC) $(CFLAGS) -Icore -Ihandheld $(CORE) $(HANDHELD) tools/uishot.c -o $@ -lm

# Draws the desktop layouts to picture files without a window (see tools/deskshot.c).
deskshot: $(CORE) $(DESK) tools/deskshot.c core/*.h desk/*.h
	$(CC) $(CFLAGS) -Icore -Idesk $(CORE) $(filter-out desk/main.c,$(DESK)) tools/deskshot.c -o $@ -lm
