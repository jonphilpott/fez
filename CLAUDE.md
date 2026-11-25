# FEZ: C64-style register synth (C, SDL2)

FEZ is the project's name (F is for Forth; the backronym for E and Z is still undecided). 


**Requirement: the code must be easily maintainable by humans, not just by agents.** Plain, boring, well-commented code; one definition of each thing; no clever tricks. Comments teach a junior developer new to DSP and C++ (the why and how, numbered steps in long functions). Minimal libraries: plain C99, SDL2 only for the apps.

## What this is

A SID-style register synthesiser with a Forth-like token VM, a Buchla/Music Easel-style voice (vactrol lowpass gate, Moog-style ladder for the mod voice), four pulsers, a modulation matrix and virtual devices (cellular automaton, Lorenz attractor, 8-step sequencer, quantiser, Turing machine, keyboard, buffer device with grains), and a serial effects chain (tape delay, spring, compressor). The core is portable C with no malloc or OS calls. It began as a JavaScript prototype (no longer in this repo); comments that say "the JS" mean that reference.

Targets: the desktop app (macOS, Linux; works now), a Raspberry Pi build of the same (HDMI screen 1024x600, USB keyboard, custom MIDI controller with 32 fixed-CC pots), the TrimUI Brick handheld (256x192 logical screen at 4x), and later an STM32H743 (Daisy-class).

## Layout

    core/       portable C core: fez.h (the one public header), vm.c, regs.c, synth.c, devices.c, buf.c, audio.c (voice, effects),
                filters.c, scales.c; tables.c (parameters, names, help, example patches: edited by hand) and reg_names.h (the register map);
                regview.c (how registers are listed, formatted, edited), palette.c and patchfile.c (a whole patch as text and back; no file calls) are shared by both UIs
    desk/       the DESKTOP app (`fez`): keyboard-driven, window split into tiles with tabs (editor, patches, pulsers, seq, matrix,
                devices (pages: cellular, lorenz, quantiser, turing, delay, keys), buffer (pages: tape, voices), ctrl (every register in one list under
                group headings; Tab switches units and raw numbers; the master volume LEVEL is its first row), scope), 3 layouts; the editor keeps its own
                text per block and applies on Ctrl+Enter; gfx.c draws into a pixel array, main.c is the only SDL file;
                userpatch.c is the only patch disk code, wavrec.c the only recording disk code
    handheld/   the small-screen UI (256x192, Brick-style buttons); no SDL inside. STALE: it lacks the keyboard device, mute, recording and the merged CTRL list
    sdl/brick_main.c   SDL2 shell for the handheld UI (`fez_brick`)
    test/       test_core.c (tables) and test_patchfile.c (every example patch survives a write and read back). The old golden-vector tests against the JS were dropped.
    tools/      render.c offline renderer (WAV output, level check, 100 percent feedback stress test), deskshot.c and uishot.c (draw screens to PPM without a window)

## Build and test (from the repo root)

    make test            # builds and runs the tests; must end "all tests ok"
    make && ./fez        # desktop app (needs SDL2: brew install sdl2, or apt install libsdl2-dev)
    make fez_brick && ./fez_brick
    make render && ./render all     # peak and RMS of every example patch (NaN, silence and runaway feedback show here)
    make deskshot / make uishot     # screens to .ppm files

Compile with `-ffp-contract=off` (the Makefile does): fused multiply-add changes the Lorenz results and filters between machines. The sound itself can only be judged by ear: `render` catches silence, clipping, NaN and runaway feedback, nothing more.

## Using the desktop app

Files: one visible folder, `~/FEZ/`, with `patches/` (text files `<name>.fez`) and `recordings/` (WAV takes). On first run with a new `~/FEZ/patches/`, patches from the old hidden folder (`~/Library/Application Support/fez/patches/` on macOS, `~/.local/share/fez/patches/` on Linux) are copied in, and the old files are left alone. The app starts from a blank patch. Patches tab: Left/Right switch MY PATCHES (the default) and EXAMPLES (built in), typing filters, Enter loads, Ctrl+S saves (always asks for a name), Ctrl+D or Delete deletes. No autosave, by decision. The buffer audio is not saved. Loading a patch clears the buffer, the delay, the spring and the compressor.

Keys: plain `[` and `]` move to the previous or next page inside a tab (editor blocks, Patches lists, Devices pages); Ctrl or Alt with them change the tab, so the editor cannot type brackets. Ctrl (or Alt) + arrows move between tiles, Ctrl+Tab next layout, Ctrl+= / Ctrl+- text size, Ctrl+G gate, Ctrl+P pulsers, Ctrl+F full screen, Ctrl+Q quit, Ctrl+S save, Ctrl+T start/stop a recording (the final output, after mute, as a 24-bit stereo WAV at the synth rate, named `<date>_<time>_<patch>.wav`; Ctrl+R is the editor's run-block, so it was not free), Shift+Esc global mute (a 20 ms fade of the output; the synth keeps running; plain Esc still cancels entries). No Home/End/Page keys: it must be fast on a laptop. Target screen 1024x600 (the Pi LCD). The CTRL list's group headings come from the `GROUPS` table in `desk/tab_values.c` (each starts at a register constant); a new device group needs one line there.

Colours: one table, `core/palette.c`, shared by both UIs; a high-contrast scheme on pure black. Each ink has one job: amber (gold) is selection and the write head, cyan register names and waveforms, green notes and on-states, red errors, and magenta (`COL_MAG`) is movement: modulation (the "now" values and ticks), playheads, grain voices, pulser activity, held keys and the CTRL group headings.

Recording: the audio thread never touches the disk. It drops each output sample into a ring buffer (`rec_ring` in `main.c`) and the main loop empties it into the WAV file.

## Things that are true and easy to get wrong

- **The register map is grouped, and code never writes a bare address.** Each device owns one unbroken run of addresses (voice 100-110 with the complex oscillator's CXWAVE and CXBLEND and GATE, CV 111-114, mixer 115-119 ending in LEVEL, pots, mod voice, delay and spring 130-139, slew, cellular 400s, Lorenz, sequencer, buffer, grains, pulsers 510-526, quantiser, Turing, keyboard 550-553). Addresses are written once, in `core/reg_names.h`; everything else uses names (`REG_DLYTIME`), including the tables and the example patches in `tables.c`, so regrouping the map is one edit there. To add a register, give it the next free address in its group, or open a new group, then add it to `NAME_TABLE` (and `PARAM_TABLE`, `HELP_TABLE`) in `core/tables.c`. Saved patches and the example patches store names, so moving a register does not break them.
- **The tables are the single source of truth.** Names, parameter defaults and ranges, one-line help for the small screens (49 characters max; a test enforces it) and the example patches are all in `tables.c`.
- **Quantiser and Turing machine** (registers QON to QROOT and TMPUL to TMOUT, matrix source 20 `TM`): the quantiser snaps the *effective* pitch (`eff[PITCH]`, after the matrix), never the PITCH register. `TMBITS` is the one register that lives outside the register file (`tm.bits`); `patchfile.c` special-cases it. The Turing machine uses one random number per step, always.
- **The effects chain is in series**: voice, mod voice and the buffer's return (MIXBF) go into the tape delay, its output into the spring, that into the compressor. The tape delay (DLYTIME 0 to 12 s, linear, 0.37 ms a step, a 2.3 MB ring; the buffer device runs up to 30 s, 5.8 MB; both are fixed arrays in `audio.h` and `buf.h`) has DLYFB (0 to 100 percent, no cap), DLYMIX (a true dry/wet crossfade: full means loop only), DLYIN (how much sound is written into the loop, squared curve, default full; 0 closes the loop), DLYLP (feedback lowpass, 300 Hz to 12 kHz), DLYWOB (wow and flutter), DLYNZ (hiss written into the loop) and DLYFRZ (freeze at or above 16384; blends the loop to an exact copy). Tested at maximum settings: bounded, and a frozen loop does not fade. DLYTIME 0 is a comb filter, not an echo.
- **Keyboard device** (registers KBNOTE, KBGATE read only, KBOCT, KBPUL; matrix source 21 `KB`, ROUTE takes KBNOTE as its source): `fez_note_on/off/all_notes_off` in `synth.c` is the one place notes enter, and where MIDI will land. A key down writes PITCH and KBNOTE, fires the envelope and fires the KBPUL pulsers; newest key wins, and letting go of it returns to the one before with no new trigger. **Sustain**: `gate_hold` (set while any key is down) keeps the control envelope at the top and the audio lowpass gate's LED on; the last key up lets it fall over REL. Blocks, pulsers and the sequencer still fire the old trigger envelope. On the desktop the KEYS page plays the tracker layout (Z S X D C V G B H N J M, then Q 2 W 3 E R 5 T 6 Y 7 U, by scancode so any keyboard layout works); only that page plays, but a key-up is always honoured, and losing window focus lets go of everything.
- **Pitch is 8.8 semitones**: MIDI note x 256 (C4 = 15360, octave = 3072). `ST` converts semitones, `HZ` converts tenths of a hertz (4400 HZ = A4). Registers hold 0 to 32767; literals are 15 bit. Block size is 128 slots.
- **Pulsers hold no block.** Each block has a 4-bit pulser mask (BP0 to BP15); SQPUL and GRPUL are masks for the sequencer clock and the next grain; RUN runs a block next tick. A block runs once per tick however many pulsers fire.
- **Pulser rate** is pulses per minute, 10 minimum (a file load clamps lower rates to 10, so patches use probabilistic blocks for slow events).
- **Low-passed white noise is very quiet.** The mod voice noise feed (`MDNZ`) is lifted by sqrt(8000 / cutoff Hz), limited to 1..6, to keep sweeps at a steady loudness.
- **Envelope times**: ATK is 1 ms to 500 ms and REL is 10 ms to 4 s on log scales, so register values such as 12000 are short. Patches with "slow" notes need values above 20000.
- **LEVEL** (the master volume) is owned by a knob (`level_pot`) so blocks cannot write it; the CTRL tab edits it directly.
- **`fez_round`** rounds halves up like JavaScript's Math.round (C's round() rounds halves away from zero, which differs for negative halves).
- **Verification so far has been headless** (scripted desk tests under UBSan, pictures of the screens, the app run on SDL's dummy drivers), not in a real window or by ear. The SDL key and audio code in `desk/main.c` (mute fade, scancode piano, focus loss, recording) has had no real-hardware test yet.

## Current state (25 Nov 2026)

13 example patches (including Ambient Loops, an Eno-style tape-loop piece, and Deep Loop, Frippertronics at ambient speed: 7.3 s delay, low DLYIN, 30 s buffer, a breathing input level). The desktop app is considered feels-good; next is testing the build on Linux.

## Open ideas, not started

- Bring the handheld UI up to date with the desktop (keyboard device, mute, CTRL list).
- `MDRES`: resonance for the mod voice filter (for narrower noise sweeps).
- MIDI input (parked), a Raspberry Pi build, the STM32 build.
