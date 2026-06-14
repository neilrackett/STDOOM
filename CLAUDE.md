# DOOM Accelerator — project context for Claude

## What this is

STDOOM is a port of Linux Doom to the Atari ST. This repo adds a **SidecarTridge
Multi-device accelerator** path: when a SidecarTridge is present, the RP2040
handles the CPU-intensive chunky-to-planar (C2P) conversion, freeing the ST's
68000 for game logic.

Build targets:

- **`linuxdoom-1.10/`** — the ST-side `.TOS` binary (`make stdoom`, via `stcmd`)
- **`sidecart/rp/`** — the RP2040 firmware (`.uf2`, built with `bash sidecart/rp/build.sh pico_w debug`)
- **`sidecart/tests/`** — standalone SidecarTridge test apps:
  - `make -C sidecart tests` to build all test apps at once
  - `make -C sidecart pingtest` for detection + transport smoke tests
  - `make -C sidecart c2ptest` for the full 320x200 C2P round trip and screen copy
  - `make -C sidecart recttest` for the dirty-rect (arbitrary `x,y,w,h`) pipeline
  Use these first when validating new SidecarTridge features before wiring them into STDOOM

@sidecart/README.md

## Milestone plan

| #   | What                                                                                                                                 | Status                      |
| --- | ------------------------------------------------------------------------------------------------------------------------------------ | --------------------------- |
| 1   | **Detection** — ST PINGs the SidecarTridge; firmware echoes token + version string; ST reports detected/not-detected at startup      | **Complete**                |
| 2   | **C2P offload** — ST uploads chunky framebuffer; RP2040 converts to planar and writes to shared ROM-in-RAM slot; ST blits from there | **Complete**                |
| 3   | **Full C2P replacement** — route *all* rendering (splash, menus, intermission, automap, status bar, gameplay) through the accelerator when present, with dirty-rect support (status-bar cells, zoomed view); refactor into a separate `sidecart_c2p.c` | **Complete**                |
| 4   | **Palette + dither modes** — `SET_MAP` replaced by `SET_PALETTE` (raw 768-byte RGB); RP2040 owns all colour reduction and returns chosen 16 ST colours; selectable modes: nearest-colour, greyscale, 2×2/4×4 Bayer; median-cut + k-means palette generation; palette selection quality needs future refinement | **Complete**                |
| 5   | **Async data processing** — non-blocking C2P dispatch; ST returns to game logic immediately after firing C2P and synchronises only at vsync before the planar copy | **Abandoned** (upload-bound, ~1.2fps; saved on `turbo-async`) |
| 6   | **Upload-optimisation investigation + new C2P modes** — standalone `UPTEST.TOS` upload benchmark, HW-confirmed: ~203ms/frame upload floor (≈4.9fps ceiling), RLE ~10× slower (68000 encode ~1.8s/frame), dirty-row a net loss ⇒ upload irreducible for gameplay; two new greyscale-dither modes (GREY 2×2/4×4 Bayer) + mode-switch key moved to keypad `*`; `-noturbo`→`-nosidecart` | **Complete** (HW-confirmed)  |

@MILESTONES.md

## Milestone 1 — complete

The detection protocol (`sidecart_md_detect` in `linuxdoom-1.10/sidecart_md.c`) is complete:

1. Checks ready magic at `$FAF00A` (expect `$53`)
2. Checks the seed at `$FAF004` to guard against a dead cartridge
3. Sends `CMD_STDOOM_PING` (op `$0010`, d3=`'STDM'`) via `stdoom_send_sync_command`
4. Waits for the RP2040 to echo the random token back to `$FAF000`

On success the firmware writes `DOOM Accelerator/1.0` into the shared result
buffer and `sidecart_md_result()` returns it to the ST side.

Use `make -C sidecart pingtest` and `sidecart/tests/PINGTEST.TOS` to validate detection and
transport in isolation, and `make -C sidecart c2ptest` / `sidecart/tests/C2PTEST.TOS` for
the full visible round trip, before wiring new SidecarTridge features into
`linuxdoom-1.10/`.

## Milestone 2 — complete

C2P offload works end-to-end. STDOOM proper renders gameplay frames through the
accelerator on a Mega STE at **16 MHz + cache**, with correct geometry.

- `sidecart/tests/C2PTEST.TOS` runs the full synchronous round trip: detect,
  `INIT`, `SET_MAP`, `BLIT_ROWS`, `C2P`, then planar copy to the ST screen. It
  prints per-phase timings and a planar checksum, and is the preferred benchmark
  and regression tool.
- STDOOM's `c2p_screen_md` (in `atari_c2p.c`) uploads `screens[0]` in 6-row
  chunks, triggers `C2P`, and copies planar slot 0 to the screen, with a clean
  fall-through to software `c2p_screen_lorez` on any failure or non-gameplay
  frame.
- STE blitter copy is used for the planar-to-screen copy on STE/Mega STE, with a
  CPU longword fallback on plain ST.

### Hard-won fixes that made it work (do not regress)

- **Mega STE cache register (`$FFFF8E21`)** uses canonical MSTE_CC values
  `$FF`/`$FE`/`$F4` (cache = bit0). Naive `$01`/`$03` writes corrupt the
  must-be-set upper bits and destabilise the machine. Kept in sync between
  `atari_checkcpu.c` and `sidecart_md.c`'s cache guard.
- **STE blitter registers start at `$FFFF8A20`** (the first 16 words at
  `$8A00` are halftone RAM). The old `$8A02..$8A1E` offsets wrote into halftone
  RAM, so the blitter never ran and the screen stayed black. Fixed in
  `atari_c2p.c` and `c2ptest.c`.
- **`Super()` must be mode-aware.** STDOOM runs the whole game loop in
  supervisor mode (`I_Init` enters super and stays), so the cache guard uses
  SUP_INQUIRE and only toggles mode when entered from user mode. The naive
  `Super(0L)/Super(ssp)` pattern crashes (2 bombs) when already supervisor.
- **The RP2040 must NOT byte-swap the planar output.** `stdoom_pack_to_planar`
  `memcpy`s the plane words to ROM-in-RAM; the ST reads uint16 values directly.
  Byte-swapping exchanges the two 8-pixel halves of every 16-pixel group and
  scrambles columns.
- **C2PTEST's synthetic pattern hides pixel-order bugs** (adjacent pixels map to
  the same ST colour). Validate C2P pixel ORDER against real Doom output or a
  per-pixel-varying pattern, not the smooth gradient.

### Cache guard note

Mega STE cache is temporarily disabled around SidecarTridge command traffic in
`sidecart_md.c`, then restored afterwards, because cache can hide the ROM3 bus
reads the protocol relies on.

### Key files

| File                                        | Purpose                                           |
| ------------------------------------------- | ------------------------------------------------- |
| `linuxdoom-1.10/sidecart_md.c/.h`           | ST-side client: detect/PING + INIT/SET_MAP/BLIT_ROWS/C2P(rect), mode-aware cache guard, per-command IPL-7 interrupt mask |
| `linuxdoom-1.10/sidecart_stubs.S`           | Low-level SidecarTridge bus protocol (asm)        |
| `linuxdoom-1.10/sidecart_c2p.c/.h`          | **(M3)** all accelerated C2P: reduction map, `c2p_screen_md()`/`c2p_statusbar_md()` drawfuncs, blitter/CPU planar→screen rect copy, palette hook; `sidecart_c2p_install()` overrides the software pointers |
| `linuxdoom-1.10/atari_c2p.c`                | Pure software C2P + the function pointers `sidecart_c2p.c` installs over (no sidecart code since M3) |
| `linuxdoom-1.10/atari_checkcpu.c`           | Mega STE turbo/cache register (canonical `$FF`/`$FE`/`$F4`) |
| `sidecart/rp/src/stdoom_worker.c`           | RP2040 worker: PING/INIT/SET_MAP/BLIT_ROWS/C2P dispatch, `pack_to_planar_rect`, token echo |
| `sidecart/rp/src/include/stdoom_commands.h` | Shared memory map, command IDs, offsets           |
| `sidecart/tests/pingtest.c`                 | Standalone detection + transport smoke test       |
| `sidecart/tests/c2ptest.c`                  | Standalone full C2P round-trip test (`make -C sidecart c2ptest`) |
| `sidecart/tests/recttest.c`                 | Standalone dirty-rect (`x,y,w,h`) pipeline test (`make -C sidecart recttest`) |

### Diagnostic output (startup, before game loads)

```
MD detected: DOOM Accelerator/1.0   (accelerator present)
MD not detected; SW C2P             (no accelerator → software path)
MD C2P init failed; SW C2P          (detected but INIT/SET_MAP failed)
```

The verbose `MD ready=/stage=/irq=` dump used while bringing up M1/M2 was removed
in M3. To restore it for low-level transport debugging, use
`sidecart_md_detect_verbose()` (stage codes: 0=ok, 1=ready mismatch, 2=dead seed,
3=PING timeout) and the `STDOOM_DBG_*` counters declared in `sidecart_md.h`.

## Milestone 3 — complete

Full C2P replacement. With the accelerator present, **every** screen (splash,
menus, intermission/finale, automap, status bar, gameplay) renders through the
accelerator, with dirty-rect support for the shrunk gameplay view and the status
bar. Confirmed on hardware (Mega STE, 16 MHz + cache).

- **Architecture split.** `atari_c2p.c` is pure software again and knows nothing
  about the sidecart. All accelerated code lives in `sidecart_c2p.c`;
  `sidecart_c2p_install()` (called after `init_c2p_table()` in `I_InitGraphics`)
  overrides the software `c2p_screen_drawfunc`/`c2p_statusbar_drawfunc`/
  `set_doom_palette` pointers. `sidecart_c2p_init()` (in `D_DoomMain`, user mode)
  runs detection + `INIT`/`SET_MAP`. This mirrors how a clean SDL XBIOS driver
  swaps implementations.
- **Rect pipeline.** `CMD_STDOOM_C2P` carries an optional convert rect
  `(x,y,w,h)`; the firmware `stdoom_pack_to_planar_rect` converts only that
  sub-rect. `c2p_statusbar_md` updates only the dirty status-bar cells; the
  shrunk gameplay view uploads/converts/copies only the active view rectangle.

### Hard-won fixes that made it work (do not regress)

- **Blitter from ROM4 is unreliable for sub-rects.** A multi-row blit
  (`X_COUNT=80, Y_COUNT=h`) tears on the last rows; a blit with `INC_Y!=0` (true
  sub-rect) produces stripe corruption. `sidecart_md_copy_planar_to_screen` uses
  a linear blit (`X_COUNT=total, Y_COUNT=1`) only for full-width rects, and a CPU
  row-by-row copy for true sub-rects.
- **Inverted/empty dirty box.** The software `c2p_statusbar` wrapper forces
  `y_begin` up to `SCREENHEIGHT-32`; a top-screen HUD message then yields
  `y_begin > y_end`. `c2p_statusbar_md` must reject `y_end <= y_begin ||
  x_end <= x_begin` first, or the unsigned width/height underflows and the copy
  runs off screen RAM (2-bomb address-error crash on "start new game").
- **Shrunk-view border.** Doom draws the GRNROCK border into `screens[0]` only on
  the few frames after a view-size change or menu/automap dismissal (its
  `borderdrawcount=3`). `c2p_screen_md` forces a full-frame push for 3 ticks
  whenever the border may have been redrawn, else the partial path leaves stale
  menu pixels around the view.
- **Interrupt masking.** The bus protocol's ROM3 reads must be atomic; an MFP
  keyboard/timer interrupt mid-command corrupts it and bounces the frame to the
  software renderer (a dithered flash on menu keypresses). Each per-frame command
  raises the 68000 IPL to 7 via `sidecart_md_set_intr_mask(1)` (supervisor only;
  user-mode init runs unmasked since MOVE-to-SR is privileged).

Validate the dirty-rect pipeline in isolation with
`make -C sidecart recttest` / `sidecart/tests/RECTTEST.TOS` before touching STDOOM.

## Build commands

```bash
# All commands run from repo root

# Standalone test apps
make -C sidecart tests      # builds all test apps (ping, c2p, rect)
make -C sidecart pingtest   # builds sidecart/tests/PINGTEST.TOS
make -C sidecart c2ptest    # builds sidecart/tests/C2PTEST.TOS
make -C sidecart recttest   # builds sidecart/tests/RECTTEST.TOS

# ST side
make stdoom           # builds linuxdoom-1.10/build/dist/STDOOM.TOS

# RP2040 microfirmware for SidecarTridge Multi-device
make sidecart debug   # builds both test apps plus sidecart/dist/[UUID]-[VERSION].uf2
```

## Shared memory map (ROM4 base `$FA0000`)

| Offset  | ST address | Size    | Purpose                                         |
| ------- | ---------- | ------- | ----------------------------------------------- |
| `$0000` | `$FA0000`  | 32000 B | Planar slot 0 (Milestone 2)                     |
| `$F000` | `$FAF000`  | 4 B     | Random token (RP2040 writes to unblock ST)      |
| `$F004` | `$FAF004`  | 4 B     | Random token seed                               |
| `$F008` | `$FAF008`  | 2 B     | Status (`$00`=idle, `$01`=busy, `$02`=done)     |
| `$F00A` | `$FAF00A`  | 2 B     | Ready word (both bytes = `$53` when worker up)  |
| `$F100` | `$FAF100`  | 2048 B  | Result buffer (PING writes version string here) |
