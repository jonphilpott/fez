#!/usr/bin/env python3
"""Draws the FEZ app icon (amber pixel letters on black, like the manual's title) and writes mac/FEZ.icns.
Plain Python, no libraries: the PNG is written by hand with zlib. Needs macOS's iconutil and sips for the last step.
Run from the repo root:   python3 mac/make_icon.py   (or with --ios out.png for the full-square iPad version)"""
import math, os, struct, subprocess, tempfile, zlib

SIZE = 1024
AMBER = (255, 210, 26)
CYAN = (0, 220, 230)           # the scope's colour in the app
BLACK = (0, 0, 0)

# Each letter is a 5 wide, 7 tall grid of blocks. '#' is lit.
LETTERS = {
  "F": ["#####", "#....", "#....", "####.", "#....", "#....", "#...."],
  "E": ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
  "Z": ["#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"],
}

def draw(margin=100, radius=185):
    # 1) lay the three letters out on one grid: 5 wide each, one empty column between them
    cols = 3 * 5 + 2
    rows = 7
    grid = [[False] * cols for _ in range(rows)]
    for n, ch in enumerate("FEZ"):
        for y, row in enumerate(LETTERS[ch]):
            for x, c in enumerate(row):
                if c == "#": grid[y][n * 6 + x] = True
    # 2) one block is 46 pixels, so the text is 782 wide and sits in the middle of the 1024 square
    block = 46
    left = (SIZE - cols * block) // 2
    top = 215                                     # a little above the middle: the wave goes underneath
    # a sine wave in small blocks under the letters (a "sound computer"): one block per column, joined so the line has no gaps
    cell = 23
    wave_left = left
    wave_cols = (cols * block) // cell
    mid, amp = 745 // cell, 3                     # centre row, and how many rows high each half of the wave is
    wave = set()
    prev = None
    for i in range(wave_cols):
        y = mid + round(amp * math.sin(2 * math.pi * i / (wave_cols / 2)))
        lo_y, hi_y = (y, y) if prev is None else (min(prev, y), max(prev, y))
        for yy in range(lo_y, hi_y + 1): wave.add((i, yy))
        prev = y
    # 3) macOS icons are a rounded square, with a margin round it (the system draws no mask of its own).
    #    For iOS the picture is a plain full square (margin 0, radius 0): iOS rounds the corners itself
    lo, hi = margin, SIZE - margin
    def inside(x, y):
        if not (lo <= x < hi and lo <= y < hi): return False
        cx = min(max(x, lo + radius), hi - 1 - radius)      # nearest point of the inner rectangle
        cy = min(max(y, lo + radius), hi - 1 - radius)
        return (x - cx) ** 2 + (y - cy) ** 2 <= radius ** 2
    rows_out = []
    for y in range(SIZE):
        line = bytearray([0])                               # PNG filter byte: none
        for x in range(SIZE):
            if not inside(x, y):
                line += bytes((0, 0, 0, 0))                 # outside the square: transparent
                continue
            wx, wy = (x - wave_left) // cell, y // cell
            if (wx, wy) in wave and (x - wave_left) % cell and y % cell:
                line += bytes(CYAN + (255,)); continue
            gx, gy = (x - left) // block, (y - top) // block
            lit = 0 <= gx < cols and 0 <= gy < rows and grid[gy][gx]
            # a one-pixel gap between blocks gives the chunky pixel look
            if lit and ((x - left) % block == 0 or (y - top) % block == 0): lit = False
            line += bytes(AMBER + (255,) if lit else BLACK + (255,))
        rows_out.append(bytes(line))
    return b"".join(rows_out)

def png(path, raw):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))

import sys
if len(sys.argv) == 3 and sys.argv[1] == "--ios":        # python3 mac/make_icon.py --ios <out.png>: the 1024 square for an iPad app's asset catalogue
    png(sys.argv[2], draw(0, 0))
    print("wrote", sys.argv[2])
    sys.exit(0)

here = os.path.dirname(os.path.abspath(__file__))
with tempfile.TemporaryDirectory() as tmp:
    big = os.path.join(tmp, "big.png")
    png(big, draw())
    iconset = os.path.join(tmp, "FEZ.iconset")
    os.mkdir(iconset)
    # the sizes macOS asks for: each of 16, 32, 128, 256 and 512, at 1x and 2x
    for s in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            px = s * scale
            name = "icon_%dx%d%s.png" % (s, s, "@2x" if scale == 2 else "")
            subprocess.run(["sips", "-z", str(px), str(px), big, "--out", os.path.join(iconset, name)],
                           check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["iconutil", "-c", "icns", iconset, "-o", os.path.join(here, "FEZ.icns")], check=True)
    # Windows icon: a .ico file is a small header, one directory entry per size, then the PNG pictures themselves (allowed since Vista)
    ico_sizes = (16, 32, 48, 64, 128, 256)
    pngs = []
    for px in ico_sizes:
        out = os.path.join(tmp, "ico%d.png" % px)
        subprocess.run(["sips", "-z", str(px), str(px), big, "--out", out], check=True, stdout=subprocess.DEVNULL)
        pngs.append(open(out, "rb").read())
    header = struct.pack("<HHH", 0, 1, len(pngs))                      # reserved, type 1 = icon, count
    offset = 6 + 16 * len(pngs)
    entries = b""
    for px, data in zip(ico_sizes, pngs):
        entries += struct.pack("<BBBBHHII", px % 256, px % 256, 0, 0, 1, 32, len(data), offset)   # 256 is stored as 0
        offset += len(data)
    open(os.path.join(here, "FEZ.ico"), "wb").write(header + entries + b"".join(pngs))
    # keep a PNG of the icon for the README and the GitHub page
    subprocess.run(["sips", "-z", "256", "256", big, "--out", os.path.join(here, "icon.png")],
                   check=True, stdout=subprocess.DEVNULL)
print("wrote mac/FEZ.icns, mac/FEZ.ico and mac/icon.png")
