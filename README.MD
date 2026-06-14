# STDOOM MD

[STDOOM](https://github.com/indyjo/STDOOM) + [DOOM Accelerator](sidecart/README.md) = STDOOM MD

![title screen](titlepic.png)
![screenshot](screen1.png)

## Introduction

This is an experiment by [Neil Rackett](https://x.com/neilrackett) to see if it's possible to harness the raw power of a SidecarTridge Multi-device to make [STDOOM](https://github.com/indyjo/STDOOM) playable on a stock Atari ST.

## System requirements

- Atari ST with at least 4MB of RAM.
- [SidecarTridge Multi-device](https://sidecartridge.com) with [DOOM Accelerator](sidecart/README.md).

Without DOOM Accelerator, it's just regular STDOOM and will most likely be unplayably slow on an 8MHz ST.

## How to play

- Install the [DOOM Accelerator](sidecart/README.md) microfirmware on your SidecarTridge Multi-device.
- Copy the `STDOOM.TOS` executable into a folder on your ST's (real or emulated) hard drive.
- Put a WAD file like `DOOM1.WAD` next to it.
- Start `STDOOM.TOS`.
- Use keyboard or mouse for control, just as in the original game.
- If you have a system with at least a Motorola 68020 CPU but no FPU, you can use the `STDOOM20.TOS` executable.
- If you have a system with at least a Motorola 68020 with FPU, you can use the `STDOOM2F.TOS` executable.

TOS files and microfirmware are available from the [releases page](https://github.com/neilrackett/atarist-stdoom/releases).

## Features

- Runs DOOM (and DOOM II) on Atari ST computers.
- Supports low resolution (320x200, 16 colors).
- Zoomed mode for higher performance (activate by reducing window size using = key or menu).
- Audio sample playback using STe DMA sound.
- Music playback using the YM-2149 soundchip (work in progress).
- Controllable with keyboard and mouse.

### Not implemented

- Network gameplay

## How to compile

You need a C compiler and GNU Make. I recommend compiling with [`atarist-toolkit-docker`](https://github.com/sidecartridge/atarist-toolkit-docker).

To compile STDOOM, run `make stdoom`.

To compile the STDOOM Accellerator firmware, run `make sidecart`; see the [`sidecart/README.md`](sidecart/README.md) file for details.

STDOOM has the following compile-time dependencies:

- mintlib https://github.com/freemint/mintlib
- fdlibm https://github.com/freemint/fdlibm

### A word on "fastcall"

STDOOM is able to use the 'fastcall' calling convention, available in [Thorsten Otto's Crossmint tools](https://www.tho-otto.de/crossmint.php).

If you want to use `-mfastcall`, you must ensure that all libraries, including libc and libm, also support fastcall.

## How to benchmark

- Rename `STDOOM.TOS` to `STDOOM.TTP`
- On start, enter parameters: `-timedemo demo3 -nosound`
- The demo plays. When it's finished, a message says:
  Error: timed 2134 gametics in x realtics
- x is the time elapsed in 1/35th of a second. Calculate FPS: 35\*2134/x

## Command-line options

STDOOM MD supports the same command-line options of the original Linuxdoom, plus:

- `-nomusic` disables YM-2149 soundchip music.
- `-nosfx` disables STE DMA sound effects.
- `-nosound` disables sound altogether.
- `-nomste16` skips the Mega STE 16MHz speed switch at startup (uses existing speed setting).
