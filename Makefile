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
	rm -rf fez fez_mac demo deskshot padshot test_core render tools/manual_gen dist *.dSYM *.ppm

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

# ---- macOS app: make mac-app, mac-zip or mac-dmg  (VERSION=0.1.0 names the release; see mac/Info.plist and mac/make_icon.py)
# SDL2 is linked into the program, so it runs on a Mac without Homebrew. It is signed ad hoc ("-"), which is enough to run
# on the Mac that made it; on another Mac the user right-clicks the app and chooses Open once (README says so).
# For a double-click install a Developer ID signature and notarisation are needed (US$99 a year); that is not set up.
APP := dist/FEZ.app

# SDL2 linked in as a library file (libSDL2.a, which Homebrew provides), so the program needs no SDL2 installed. macOS itself
# can never be linked fully statically (libSystem and the system frameworks are always shared), but those are on every Mac.
fez_mac: $(CORE) $(DESK) core/*.h desk/*.h
	$(CC) $(CFLAGS) -Icore -Idesk $(SDL_CFLAGS) $(CORE) $(DESK) -o $@ $(shell sdl2-config --static-libs) -lm

mac/FEZ.icns: mac/make_icon.py
	python3 mac/make_icon.py

mac-app: fez_mac mac/FEZ.icns mac/Info.plist
	rm -rf $(APP)
	mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources
	cp fez_mac $(APP)/Contents/MacOS/fez
	cp mac/FEZ.icns $(APP)/Contents/Resources/FEZ.icns
	sed 's/@VERSION@/$(VERSION)/g' mac/Info.plist > $(APP)/Contents/Info.plist
	codesign --force --deep --sign - $(APP)
	@echo "made $(APP)"

# ditto, not zip: plain zip can break the signature
mac-zip: mac-app
	rm -f dist/FEZ-mac-v$(VERSION).zip
	ditto -c -k --keepParent $(APP) dist/FEZ-mac-v$(VERSION).zip
	@echo "made dist/FEZ-mac-v$(VERSION).zip"

mac-dmg: mac-app
	rm -rf dist/dmg dist/FEZ-mac-v$(VERSION).dmg
	mkdir dist/dmg
	cp -R $(APP) dist/dmg/
	ln -s /Applications dist/dmg/Applications
	hdiutil create -quiet -volname "FEZ $(VERSION)" -srcfolder dist/dmg -ov dist/FEZ-mac-v$(VERSION).dmg
	rm -rf dist/dmg
	@echo "made dist/FEZ-mac-v$(VERSION).dmg"

# ---- Windows: cross-built on the Mac with MinGW. One fez.exe with SDL2 linked in (Windows' own libraries are always there).
#   brew install mingw-w64      once
#   make win-sdl                once: downloads SDL2's MinGW package into ~/sdk (not into the repo)
#   make win-zip VERSION=0.1.0  dist/FEZ-win-v0.1.0.zip
WIN_SDL_VERSION := 2.32.10
WIN_SDL ?= $(HOME)/sdk/SDL2-$(WIN_SDL_VERSION)/x86_64-w64-mingw32
WIN_CC  ?= x86_64-w64-mingw32-gcc

win-sdl:
	mkdir -p $(HOME)/sdk
	curl -sL -o $(HOME)/sdk/sdl-mingw.tgz https://github.com/libsdl-org/SDL/releases/download/release-$(WIN_SDL_VERSION)/SDL2-devel-$(WIN_SDL_VERSION)-mingw.tar.gz
	tar xzf $(HOME)/sdk/sdl-mingw.tgz -C $(HOME)/sdk
	rm $(HOME)/sdk/sdl-mingw.tgz

# -mwindows: no console window. SDL2main supplies WinMain. The libraries are what `sdl2-config --static-libs` lists for MinGW.
dist/FEZ.exe: $(CORE) $(DESK) core/*.h desk/*.h mac/fez.rc mac/FEZ.ico
	@test -d $(WIN_SDL) || { echo "SDL2 for MinGW not found in $(WIN_SDL): run make win-sdl"; exit 1; }
	mkdir -p dist
	x86_64-w64-mingw32-windres -I mac mac/fez.rc -O coff -o dist/fez_icon.o
	$(WIN_CC) $(CFLAGS) -Icore -Idesk -I$(WIN_SDL)/include/SDL2 $(CORE) $(DESK) dist/fez_icon.o -o $@ \
	  -s -L$(WIN_SDL)/lib -lmingw32 -lSDL2main -lSDL2 -mwindows -static -lm -ldinput8 -ldxguid -luser32 -lgdi32 -lwinmm -limm32 \
	  -lole32 -loleaut32 -lshell32 -lsetupapi -lversion -luuid -lcfgmgr32
	rm -f dist/fez_icon.o

win-zip: dist/FEZ.exe
	rm -f dist/FEZ-win-v$(VERSION).zip
	cd dist && zip -q FEZ-win-v$(VERSION).zip FEZ.exe
	@echo "made dist/FEZ-win-v$(VERSION).zip"

.PHONY: all test clean brick manual brick-zip mac-app mac-zip mac-dmg win-sdl win-zip

# Draws every handheld screen to a picture file without a window (see tools/padshot.c).
padshot: $(CORE) $(DESK) tools/padshot.c core/*.h desk/*.h
	$(CC) $(CFLAGS) -Icore -Idesk $(CORE) $(filter-out desk/main.c,$(DESK)) tools/padshot.c -o $@ -lm

# The 30 second portrait demo video: ./demo OUT_DIR   (needs ffmpeg; see tools/demo.c)
demo: $(CORE) $(DESK) tools/demo.c core/*.h desk/*.h
	$(CC) $(CFLAGS) -Icore -Idesk $(CORE) $(filter-out desk/main.c,$(DESK)) tools/demo.c -o $@ -lm

# Draws the desktop layouts to picture files without a window (see tools/deskshot.c).
deskshot: $(CORE) $(DESK) tools/deskshot.c core/*.h desk/*.h
	$(CC) $(CFLAGS) -Icore -Idesk $(CORE) $(filter-out desk/main.c,$(DESK)) tools/deskshot.c -o $@ -lm
