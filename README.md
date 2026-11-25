# FEZ

FEZ is a synthesiser you program. One voice makes the sound; a few lines of a small Forth-like language decide what the voice does and when. It is built for slow, layered, ambient music: tape loops, long delays, drones that drift.

You write a line such as `C4 PITCH !`, press a key, and the note changes. Everything the sound depends on is a named number, called a register, and everything can be reached from code, from a list of knobs, or from a modulation matrix.

## What is in it

- **A language of 46 words** with a stack, sixteen blocks of code, sixteen scratch registers and saturating 16-bit arithmetic (numbers stick at the ends instead of wrapping, which is what you want for sound).
- **A voice** in the Buchla and Music Easel tradition: a complex oscillator with FM from a second oscillator, a wavefolder and a vactrol-style lowpass gate. A separate, never-gated mod voice (oscillator or noise through a Moog-style ladder filter) handles drones and sweeps.
- **Space**: a tape delay (up to 12 seconds, with wow, hiss, a feedback lowpass and freeze), a stereo spring, and a fixed compressor.
- **A buffer device** that records and replays up to 30 seconds, with grains.
- **Four pulsers** that fire blocks on their own clocks, and a **modulation matrix** of 16 routes.
- **Devices that make notes and movement**: a cellular automaton, a Lorenz attractor, an 8-step sequencer, a scale quantiser, a Turing machine and a keyboard.
- **Sixteen modulation pots** (`MP0` to `MP15`), ready for knobs and, later, MIDI. On the handheld the two analogue sticks are `MP0` to `MP3`.
- **Thirteen example patches**, and patches saved as plain text files.

The core is portable C with no allocation and no operating system calls. Two programs sit on top of it: a desktop app for macOS and Linux, and a handheld version for the TrimUI Brick.

## Build and run

You need a C compiler and SDL2.

    make test           # the tests; must end "all tests ok"
    make && ./fez       # the desktop app
    make manual         # builds docs/manual.html

Patches and recordings live in `~/FEZ/` (`patches/` and `recordings/`).

## The TrimUI Brick

    brick/build.sh      # needs Docker (Colima is fine); leaves dist/FEZ.pak

Copy the `FEZ.pak` folder to the card as `Tools/tg5040/FEZ.pak`, put the card in the Brick, and start FEZ from the Tools menu. It works with NextUI and MinUI. The handheld chapter of the manual has the button layout.

## Documentation

Open `docs/manual.html` in a browser. It is one page: a first-sound walkthrough, the language, the voice, devices, playing, the handheld, the example patches, and a full word and register reference (generated from the code, so it cannot drift).

`CLAUDE.md` describes the layout of the source and the rules of the code, for anyone (or any agent) who works on it.

## Layout

    core/       the portable core: VM, registers, synth, devices, buffer, effects
    desk/       the desktop app (SDL2) and the shared screen code
    brick/      build script and launch scripts for the Brick
    test/       tests, including golden vectors
    tools/      manual generator, screenshot and rendering tools
    docs/       manual chapters, style, screenshots and the built manual.html
