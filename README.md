# Vib-Tatsuji

A **Taiko no Tatsujin**-style rhythm game engine for the **PlayStation 2**,
written in C on top of ps2sdk. It loads songs from a **USB stick** in
open-taiko's `.tja` format, and plays with the real drum controller (TaTaCon)
or with a pad.

It runs on actual hardware, not just in an emulator.

## What's there

- **The song clock comes from the audio**, not from counting frames. Notes live
  on a millisecond timeline and hits are judged in time, so a dropped frame
  never drifts the chart out of sync with the music.
- **`.tja` reader**: regular and big notes, drumrolls, balloons, `#BPMCHANGE`,
  `#MEASURE`, `#SCROLL`, `#GOGOSTART`, `#DELAY`, and branching charts (one
  branch is read).
- **USB stick scanning**: one folder per song, the way open-taiko lays them out.
- **All four note types** playable, with the real game's judgement windows —
  which differ per difficulty.
- **Shin'uchi scoring**: the whole chart is worth one million points.
- **Soul gauge** (魂ゲージ) with the clear threshold set by difficulty and level.
- **Japanese titles**, rendered from the console's own BIOS fonts.
- Menus you can drive **with nothing but the drum**, a real pause with a
  3-second countdown, latency calibration, volume settings and per-song
  high scores — all saved on the stick itself.

## What's not there yet

- **Big notes** count with a single hit; the real game asks for both patches of
  the same colour at once.
- **Visual style.** Right now there's just enough on screen to play.
- No song preview in the menu, even though `.tja` files carry `DEMOSTART` for
  exactly that.

## Building

You need Docker; the container brings the ps2dev toolchain.

```sh
docker compose run --rm dev sh -c 'make'      # motor.elf
docker compose run --rm dev sh -c 'make iso'  # motor.iso, for OPL
```

## Running it

On a console, with **OPL** from a hard drive: drop `motor.iso` into your DVD/CD
folder.

In an emulator:

```sh
pcsx2-qt -batch -elf $PWD/motor.elf
```

The **absolute** path matters: PCSX2 doesn't resolve relative ones, boots with
no ELF, and then dies inside the recompiler with a message that says nothing
about any of that.

## Adding songs

One folder per song in the root of the stick, each with its `.tja` and its
`.ogg` inside — exactly how open-taiko leaves them:

```
mass0:/
  My Song/
    my song.tja
    my song.ogg
```

One extra folder level (open-taiko's genre folders) and loose `.tja` files in
the root both work too. Audio must be **Ogg Vorbis**; `.tja` files can be UTF-8
or Shift-JIS.

**There are no songs in this repo**: the ones used for testing are copyrighted
music.

The first time it boots, the game offers to calibrate your audio latency with
the metronome track. Whatever comes out is written to `TATSUJI.CFG` **on the
stick itself**, next to the songs, and high scores go into a `PUNTOS.CFG` inside
each song's own folder — so your data travels with the stick.

## Controls

Copied from the real drum:

| | left | right |
|---|---|---|
| **red** (don, the centre skin) | D-pad LEFT and DOWN | CIRCLE and CROSS |
| **blue** (ka, the rims) | L1 | R1 |

Two buttons per colour isn't decoration: it's what lets you alternate hands, and
without that the fast charts are physically impossible — Oni has 100 ms gaps and
Edit has 50.

Menus work the same way: **blue** moves, **red** picks. START opens the options
in the song selector, and the pause menu during a song.

## The files

| | |
|---|---|
| `motor.c` | the whole engine: catalogue, menus, gameplay and results |
| `tja.c` / `tja.h` | the `.tja` chart reader |
| `sjis.c` / `sjis.h` | UTF-8 to Shift-JIS, for the Japanese titles |
| `sjis_tabla.c` | generated table — `gen_sjis.py` makes it, don't hand-edit |
| `prueba_tja.c` / `prueba_sjis.c` | checks that compile and run on a PC |
| `libogg_fix/` | libogg's `framing.c` rebuilt at `-O1` (see DISENO.md) |

## Why it's built this way

**[DISENO.md](DISENO.md)** *(in Spanish)* is the engineering notebook: what was
measured, what was tried, and what broke before any of this worked. Why the
clock comes from the audio instead of the frame counter, why no thread here can
ever busy-wait, why one libogg source file had to be recompiled at a different
optimisation level, where the judgement windows and the scoring maths come from,
and a good few more traps that cost hours each.

If you're going to touch the code, start there.

## Thanks to

- [ps2dev/ps2sdk](https://github.com/ps2dev/ps2sdk), which is what makes any of
  this possible.
- [OpenTaiko](https://github.com/0auBSQ/OpenTaiko) — the judgement windows, the
  score distribution and the soul gauge maths all come from reading its source.
