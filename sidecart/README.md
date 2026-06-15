# DOOM Accelerator

Microfirmware for the [SidecarTridge Multi-device](https://sidecartridge.com)
by [Neil Rackett](https://x.com/neilrackett)

## Introduction

At the heart of your SidecarTridge Multi-device is an RP2040 processor with the same raw power as a 386DX or 486SX CPU.

The DOOM Accelerator microfirmware is designed to harness that power into a coprocessor that can make DOOM truly playable on a stock Atari ST.

The ST host side lives in [`../linuxdoom-1.10`](../linuxdoom-1.10) (`sidecart_md.c/.h`, `sidecart_stubs.S`);
this directory is the RP2040 firmware.

## Installation

1. Download the latest files from the [releases page](https://github.com/neilrackett/atarist-stdoom/releases).
2. Copy the `.uf2` and `.json` files to the `/apps` folder of your SidecarT's microSD card.
3. On the Booster screen, press ESC for the app list and select the DOOM Accelerator app.
4. To return to Booster, turn on your ST while holding the SELECT button on your SidecarT.

You'll know if the microfirmware is working because you'll see "DOOM Accelerator ready" on your ST's screen as it boots.

## Status

- **Milestone 1 (done):** PING detection. The firmware publishes a ready magic,
  answers `CMD_STDOOM_PING` with a version string, and echoes the protocol
  token. STDOOM reports "accelerator detected" at startup.
- **Milestone 2 (done):** C2P (chunky-to-planar) offload. STDOOM uploads the
  chunky framebuffer, the RP2040 converts it to planar slot 0, and the ST blits
  it to screen. Gameplay frames render through the accelerator end-to-end on a
  Mega STE at 16 MHz + cache, with correct geometry. `sidecart/tests/C2PTEST.TOS`
  remains the preferred standalone hardware validation target.
- **Milestone 3 (done):** full C2P replacement. _All_ rendering (splash, menus,
  intermission, automap, status bar, gameplay) goes through the accelerator when
  present, with dirty-rect support (only changed status-bar cells / the active
  zoomed-view rectangle are updated). The sidecart path now lives in a separate
  `sidecart_c2p.c` that overrides the pure-software `atari_c2p.c`. Confirmed on a
  Mega STE at 16 MHz + cache. `sidecart/tests/RECTTEST.TOS` validates the
  dirty-rect pipeline standalone.
- **Milestone 4 (done):** dynamic 16-colour palette and selectable dither modes.
  `CMD_STDOOM_SET_PALETTE` replaces `CMD_STDOOM_SET_MAP`; the RP2040 now owns all
  colour reduction (median-cut + k-means palette generation, or a fixed hand-tuned
  subset) and returns the chosen 16 ST colours for the host to apply directly to
  the hardware palette registers. Four render modes switchable at runtime:
  nearest-colour, 2×2 Bayer, 4×4 Bayer, greyscale. Confirmed on Mega STE at
  16 MHz + cache. Palette selection quality will be further refined in a future
  pass. `sidecart/tests/RNDRTEST.TOS` validates the palette pipeline standalone.
- **Milestone 5 (abandoned):** async data processing — non-blocking C2P dispatch.
  Built and hardware-tested (saved on the `turbo-async` branch, not merged): the
  plumbing worked, but the accelerator is **upload-bound, not conversion-bound**,
  so hiding the RP2040's conversion behind game logic gave ~1.2 fps (slower than
  the ~4 fps software baseline). Abandoned as a direction.
- **Milestone 6 (done, hardware-confirmed):** upload-optimisation investigation +
  new C2P modes. A standalone `UPTEST.TOS` benchmark (`make -C sidecart uptest`)
  measured the upload path on real Doom frames (Mega STE, 16 MHz + cache) and
  confirmed the upload floor is **irreducible for gameplay**: the baseline upload is
  **~203 ms/frame** (307 KB/s, 6-row chunks) — a ~4.9 fps ceiling before any render
  or C2P; PackBits RLE saves only 5–16% of bytes but its 68000 encode costs
  **~1.8 s/frame** (~10× slower net); and dirty-row deltas change ~100% of rows
  frame-to-frame (a net loss). This is why the project was renamed from "STDOOM
  Turbo" to STDOOM MD. Two new greyscale-dither render modes (GREY 2×2 / 4×4 Bayer)
  were shipped for finer grey gradation; the mode-switch key moved from UNDO to the
  numeric keypad `*`, and `-noturbo` was renamed `-nosidecart`.
- **Milestone 7 (done, hardware-confirmed):** viewport scaling + palette/render
  expansion. When the gameplay view is shrunk, the RP2040 nearest-neighbour
  upscales the small (fast) 68000 render into the size-9 framed layout — every
  size fills the viewport with an even GRNROCK border and the HUD message in the
  top border, **at no extra upload cost**, so smaller views run faster (the one
  lever that makes the accelerator beat software, since M6 showed the full-frame
  upload is the floor). The render key (keypad `*`) selects the dither style
  (nearest / 2×2 / 4×4 Bayer / halftone) and the palette key (keypad `/`) selects
  the 16-colour set (hand-tuned, generated, EGA, C64, ZX Spectrum, PICO-8,
  greyscale); both persist. The "DOOM Accelerator ready" boot banner now shows
  with a release build.

Longer term, this accelerator design is intended as the model for a new (clean)
Atari ST SDL XBIOS driver.

## Building

The firmware builds against the Raspberry Pi Pico SDK + Pico Extras, registered
as git submodules of the `atarist-stdoom` repo at `sidecart/pico-sdk` and
`sidecart/pico-extras`.

From the repo root:

```sh
git submodule update --init --recursive sidecart/pico-sdk sidecart/pico-extras
```

(`rp/build.sh` also runs this automatically and pins the SDK tags.)

You also need:

- the ARM cross-toolchain, CMake, and picotool on your PATH.
- [`atarist-toolkit-docker`](https://github.com/sidecartridge/atarist-toolkit-docker) to compile the ST host side.

## Build

In this folder, run:

```sh
make build        # release build
make build debug  # debug build and test apps
```

Flash the resulting `.uf2` to the SidecarT or copy both the `.uf2` (with the version
number removed) and `.json` to the `apps` folder on your SidecarT's SD card.

## Layout

```
rp/src/                RP2040 firmware
  stdoom_commands.h    command IDs + shared ROM-in-RAM memory map
  stdoom_protocol.c    cartridge-bus protocol DMA bridge
  stdoom_worker.c      command dispatch (PING/INIT/SET_MAP/BLIT_ROWS/C2P) + token handshake
  emul.c               bootstrap (ROM emulation + worker loop)
target/atarist/src/    m68k cartridge stub (main.s) → target_firmware.h
desc/app.json          SidecarT app manifest
```
