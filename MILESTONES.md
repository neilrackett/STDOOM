# DOOM Accelerator Microfirmware (in atarist-stdoom/sidecart) — progressive, simplest-first

## Context

STDOOM (Atari ST Doom port) on its `turbo` branch is CPU-bound on a stock 8 MHz 68000. A large fixed per-frame cost is **C2P (chunky-to-planar) conversion** —
turning the 320×200 8bpp chunky framebuffer into the ST's interleaved 4-plane
low-res format. We want to offload that (and, later, _anything in the render
pipeline_) to a **SidecarTridge Multi-device** RP2040 cartridge when present,
keeping the existing software path as a fallback.

This follows the proven workspace pattern: **md-sdl** firmware + **atarist-sdl**'s
`SDL_xbios_md.c` offload C2P to the sidecart; **md-js** is the cleanest plain-TOS
example of the detection/command transport (same `m68k-atari-mint-gcc` toolchain
STDOOM uses).

### Guiding principle (user): simplest thing that works, first

Get _something running_ as early as possible, even if imperfect/slower, then
evolve. The decisions below deliberately take the leanest path and supersede the
earlier async/dynamic-dither choices:

- **Synchronous + a single planar slot.** No mailbox, no double-buffering. ST
  uploads chunky → triggers C2P → waits for the token → copies the one planar
  slot to screen. **Keeps md-js's token map at `$FAF000`/`$FAF004` untouched, so
  `sidecart_stubs.S` is copied verbatim with zero edits.**
- **Crude C2P first: 256→16 nearest colour, no dither.** DOOM's framebuffer
  bytes are full 0–255 palette indices, and the ST low-res shows only 16 colours,
  so a reduction is unavoidable. To keep the firmware trivial (no RGB-distance
  math on the RP), the **host builds a 256-byte `doom8_to_st4[256]` map at init**
  (the dominant entry of each `mix_weights_lorez[i]` row gives the nearest ST
  colour 0–15) and uploads it once. The firmware then just looks up
  `st4 = doom8_to_st4[chunky_byte]` and scatters its 4 bits into the ST 4-plane
  interleaved layout — no dither, no per-line phase. Output will look flat/banded
  and differ from the software renderer (which dithers); that is the accepted
  trade for the smallest first version. Evolve toward dither/fidelity later.
- **PING milestone first, C2P second.** Each milestone is independently runnable.
- **Scope: 1× low-res only.** Zoom/menu/automap/statusbar stay on the software
  fallback.
- **Single repo.** Everything new lives inside the existing `atarist-stdoom`
  repo — no separate `md-stdoom` project. The RP2040 firmware goes in a new
  `atarist-stdoom/sidecart/` subdirectory (copied & adapted from md-js); the
  host client files go in `linuxdoom-1.10/` with a `sidecart_` prefix.

Output will **not** be pixel-identical to STDOOM's software C2P yet — acceptable.

---

## Milestone 1 — implemented: PING detection + host fallback

Milestone 1 is complete. The code currently in the repo implements a synchronous
PING handshake plus a clean host-side fallback when the accelerator is absent.

### 1A. Firmware implementation in `sidecart/`

- `sidecart/rp/src/stdoom_worker.c` publishes the ready magic at boot, seeds the
  token, answers `CMD_STDOOM_PING`, writes `DOOM Accelerator/1.0` into the
  shared result buffer, and echoes the token back to the ST.
- `sidecart/rp/src/stdoom_protocol.c` and
  `sidecart/rp/src/include/stdoom_protocol.h` provide the cartridge-bus protocol
  bridge.
- `sidecart/rp/src/include/stdoom_commands.h` defines the command IDs and shared
  memory offsets.
- `sidecart/target/atarist/src/main.s` is the cartridge loader stub.

### 1B. Shared memory map (ROM4 base `$FA0000`)

```
$FA0000  planar slot 0      32000 B   (reserved for Milestone 2)
$FAF000  random token        4 B       (echoed by the firmware)
$FAF004  random token seed   4 B
$FAF008  status word         2 B
$FAF00A  ready word          2 B       (worker-ready magic)
$FAF100  result buffer    <=2048 B     (PING version string)
```

### 1C. Host-side detection in `linuxdoom-1.10/`

- `sidecart_md_detect_verbose()` checks ready magic, reads the seed, then issues
  `CMD_STDOOM_PING`. It returns `1` on success and sets `stage` to `0/1/2/3` for
  success, ready mismatch, dead seed, or timeout.
- `sidecart_md_result()` copies the version string from `$FAF100`.
- `c2p_md_init()` in `atari_c2p.c` runs detection at startup and currently prints:
  - `MD ready=$XX(want $53) seed=$XXXXXXXX`
  - `MD stage=N ping=N`
  - `MD irq=%lX cmd=%lX ce=%lX lc=%lX`
  - `MD detected: DOOM Accelerator/1.0`
  - `MD not detected; SW C2P`

### 1D. Standalone test app

- `sidecart/tests/pingtest.c` builds to `sidecart/tests/PINGTEST.TOS` via `make -C sidecart pingtest`.
- Use it to validate SidecarTridge protocol changes before wiring them into
  STDOOM.

### Milestone 1 exit

- With the sidecart firmware flashed, STDOOM reports
  `MD detected: DOOM Accelerator/1.0`.
- Without the firmware, STDOOM falls back to `MD not detected; SW C2P`.
- `make -C sidecart pingtest` produces `sidecart/tests/PINGTEST.TOS`, which is the preferred
  place to exercise new SidecarTridge features before touching STDOOM proper.

---

## Milestone 2 — C2P offload (synchronous, single slot, simplest C2P)

### Current status: complete

C2P offload works end-to-end. STDOOM proper renders gameplay frames through the
accelerator on a Mega STE at **16 MHz + cache**, with correct geometry.

- The RP2040 firmware accepts `INIT`, `SET_MAP`, `BLIT_ROWS`, and `C2P`.
- The ST host uploads the full 320x200 chunky frame in 6-row chunks, triggers
  C2P, then copies planar slot 0 back to the ST display.
- `sidecart/tests/C2PTEST.TOS` is the canonical standalone validation app. It
  prints timings for detect/init/map/upload/C2P/copy plus a planar checksum.
- STE blitter copy is used for the planar-to-screen copy on STE/Mega STE, CPU
  longword fallback on plain ST.
- Mega STE cache is temporarily disabled around SidecarTridge protocol traffic
  and restored afterwards, because the protocol depends on uncached ROM3 reads.

#### Hard-won fixes (do not regress)

- **Mega STE cache register `$FFFF8E21`** uses canonical MSTE_CC values
  `$FF`/`$FE`/`$F4` (cache = bit0), not the naive `$01`/`$03` that corrupts the
  must-be-set bits and destabilises the machine.
- **STE blitter registers start at `$FFFF8A20`** ($8A00 = 16 words of halftone
  RAM). The old `$8A02..$8A1E` offsets wrote into halftone RAM, so the blit
  never ran (black screen).
- **`Super()` is mode-aware** in `sidecart_md.c`: STDOOM is always in supervisor
  mode after `I_Init`, and the naive `Super(0L)/Super(ssp)` pattern crashes
  (2 bombs) when already supervisor.
- **No planar byte-swap on the RP2040**: `stdoom_pack_to_planar` `memcpy`s plane
  words to ROM-in-RAM (the ST reads uint16 values directly). Byte-swapping
  scrambled the 8-pixel halves of every 16-pixel group.
- **C2PTEST's synthetic pattern hides pixel-order bugs** — validate C2P pixel
  order against real Doom output, not the smooth gradient.

### 2A. Firmware: add commands + a minimal Core 0 C2P

- New commands: `CMD_STDOOM_INIT 0x11` (d3=(w<<16|h)), `CMD_STDOOM_SET_MAP 0x12`
  (upload the 256-byte `doom8_to_st4` reduction map, once at init),
  `CMD_STDOOM_BLIT_ROWS 0x13` (chunky rows: d3=y, d4=(w<<16|rows),
  d5=(pitch<<16), buf=pixels — assemble the 64000 B chunky buffer in RP RAM),
  `CMD_STDOOM_C2P 0x14` (run C2P into slot 0, then echo token).
- **Synchronous, Core 0 is fine** for the first version (no FIFO/mailbox). On
  `C2P`, run the crude loop: for each pixel `st4 = doom8_to_st4[chunky_byte]`
  (0–15), scatter its 4 plane bits into the ST interleaved 16-pixel word group
  (px0 → bit15), then `memcpy` the plane words to slot 0. **Do NOT byte-swap**
  the plane words (the original `COPY_AND_CHANGE_ENDIANESS_BLOCK16` was wrong):
  the ST reads ROM-in-RAM uint16 values directly, so the values the RP computes
  are already what the shifter needs. No dither, no per-line phase.

### 2B. Host: reduction map + `c2p_screen_md` + planar copy

- Build `doom8_to_st4[256]` in `c2p_md_init` from `mix_weights_lorez` (index of
  the largest weight in each row = nearest ST colour). Send it once via
  `CMD_STDOOM_SET_MAP`. Rebuild/resend if the DOOM palette changes
  (`I_SetPalette`).
- `sidecart_md_init(320,200)` once (after detect, in `c2p_md_init`).
- New **`c2p_screen_md(out,in)`** in `atari_c2p.c`:
  - If the frame is zoom/menu/automap (derive exactly as `c2p_screen_lorez`,
    `atari_c2p.c:1865`), **fall through to `c2p_screen_lorez` (software)**.
  - Else: upload `in` (= `screens[0]`) in ~6-row chunks (≈1920 B, under the ~2096
    payload cap) via `send_sync_write`; send `CMD_STDOOM_C2P` and wait for the
    token (synchronous); then copy planar slot 0 → `out` (= `st_screen =
Physbase()`).
  - **`sidecart_md_copy_planar_to_screen(PLANAR0, out)`** — port md-sdl's
    `md_copy_planar_to_screen` (`SDL_xbios_md.c`): STE blitter fast path when
    `_MCH` is STE/MegaSTE, else plain-ST CPU longword loop. This copy is what
    avoids the Shifter/68000 ROM4 bus contention (the Shifter then reads from
    screen RAM, not the cartridge).
- In `c2p_md_init` (Milestone 2): when `g_md_active`, set
  `c2p_screen_drawfunc = c2p_screen_md`. Leave `c2p_statusbar` software.

### Implemented so far

- `sidecart/tests/C2PTEST.TOS` now covers the full visible round trip and is the
  preferred place to verify new changes before touching STDOOM proper.
- `sidecart/tests/PINGTEST.TOS` remains the preferred transport/detection smoke
  test.
- The current `C2PTEST.TOS` image and checksum output are treated as the
  expected Milestone 2 test oracle.

### Milestone 2 exit criteria — met

- ✅ In a 1x low-res level, STDOOM proper produces displayed frames through the
  sidecart path with correct geometry and without crashing.
- ✅ The software fallback (`c2p_screen_lorez`) is only used when intentionally
  falling back (non-gameplay frames or a failed sidecart call), not because the
  accelerator path failed silently.
- ✅ Mega STE cache/turbo interaction is stable at 16 MHz + cache during play.

---

## Milestone 3 — full C2P replacement (all screens via the accelerator)

### Current status: complete

Complete and confirmed on hardware (Mega STE, 16 MHz + cache). All screens
(splash, menus, intermission/finale, automap, status bar, gameplay) render
through the accelerator, with dirty-rect support for the shrunk gameplay view
and the status bar. `atari_c2p.c` is pure software again; all sidecart code
lives in `sidecart_c2p.c` and installs over the software function pointers via
`sidecart_c2p_install()`.

Hard-won fixes during M3 (do not regress):

- **Blitter from ROM4 is unreliable for sub-rects.** A multi-row blit
  (`X_COUNT=80, Y_COUNT=h`) tears on the last rows (the beam outruns the blit),
  and a blit with `INC_Y!=0` (true sub-rect) produces stripe corruption. The
  planar→screen copy uses a linear blit (`X_COUNT=total, Y_COUNT=1`) only for
  full-width rects, and a CPU row-by-row copy for true sub-rects.
- **Inverted/empty dirty box.** The software `c2p_statusbar` wrapper forces
  `y_begin` up to `SCREENHEIGHT-32`; when the dirty box is entirely a top-screen
  HUD message that yields `y_begin > y_end`. `c2p_statusbar_md` must reject
  `y_end <= y_begin || x_end <= x_begin` first — otherwise the unsigned
  width/height underflows and the copy runs off screen RAM (2-bomb crash).
- **Shrunk-view border.** Doom draws the GRNROCK view border into `screens[0]`
  only on the few frames after a view-size change or menu/automap dismissal
  (its `borderdrawcount=3`). The partial-view path forces a full-frame push for
  3 ticks whenever the border may have been redrawn so it reaches the screen.
- **Interrupt masking.** The bus protocol's ROM3 reads must be atomic — an MFP
  keyboard/timer interrupt mid-command corrupts it and bounces the frame to the
  software renderer (visible as a dithered flash on menu keypresses). Each
  per-frame command raises the 68000 IPL to 7 (`sidecart_md_set_intr_mask(1)`,
  supervisor only).

### Goal

When the accelerator is present, route **all** graphics/C2P through it, not just
1× low-res gameplay: the splash screen, menus, intermission/finale, automap, and
status bar. Effectively, the sidecart C2P _replaces_ the software C2P entirely
for the duration of the session (with software remaining the fallback only when
no accelerator is detected). Note the gameplay status bar is already accelerated
in M2 (the full 320×200 `screens[0]`, including the bottom 32 rows, goes through
`c2p_screen_md`); M3 is about the non-gameplay screens and a clean architecture.

### Dirty-rect rendering (required)

M3 must support **dirty-rectangle** C2P, not just full-frame:

- **Status bar:** only the cells that actually change (ammo/health digits, face,
  arms, keys) are C2P'd and copied, mirroring the software `c2p_statusbar`
  dirty-box path `(y_begin, y_end, x_begin, x_end)`.
- **Zoomed/small view:** when the playfield is scaled to a small window, only the
  active view rectangle is uploaded/converted/copied, not the full 320×200.

This means the sidecart path needs **arbitrary `(x, y, w, h)` region** support
end-to-end:

- ST uploads only the dirty rows/columns for that region.
- The RP2040 C2P converts just that region into the correct planar slot offset
  (16-pixel x-alignment for plane words; clamp/extend x to word boundaries as the
  software path does — see `c2p_statusbar`, `atari_c2p.c`).
- The planar→screen copy blits only that region to the matching screen offset
  (the blitter's `X_COUNT`/`Y_COUNT`/`DST_*` line stepping handles a sub-rect;
  the M2 copy used a single linear `X_COUNT=16000, Y_COUNT=1` full-frame blit, so
  a rect copy needs proper per-line `Y_COUNT` + destination/source line
  increments).
- Keep full-frame as the path for screens that change wholesale (splash, menu,
  scene changes).

### Planned architecture: `atari_c2p.c` (software) + `sidecart_c2p.c` (accelerated)

- **Revert `atari_c2p.c` to its original, software-only form** (the pre-sidecart
  version in git history): software drawfuncs and the
  `c2p_screen_drawfunc`/`c2p_statusbar_drawfunc`/`set_doom_palette`/… function
  pointers, with no `c2p_md_*` code interleaved.
- **New `sidecart_c2p.c`** holds everything sidecart-specific currently living in
  `atari_c2p.c` (reduction map build, `c2p_screen_md`, the blitter planar→screen
  copy, the md palette hook). When the accelerator is detected, a single
  `sidecart_c2p_install()` overrides the software function pointers with the
  accelerated versions.
- Expose the function pointers and the software drawfuncs/palette helpers via
  `atari_c2p.h` so `sidecart_c2p.c` can install over them and fall back to them.
- This mirrors how a clean SDL XBIOS driver would swap implementations, which is
  the longer-term motivation (see "Why" below).

### Open questions for M3

- **Region command shape.** Define the dirty-rect protocol: either extend
  `BLIT_ROWS`/`C2P` with `(x, w)` (they already carry `y`, `rows`, `width`,
  `pitch`), or add explicit region commands. Keep x word-aligned (×16) for plane
  packing.
- **Per-screen routing.** Decide which screens use full-frame vs dirty-rect
  (gameplay status bar + zoomed view = dirty-rect; splash/menu/scene change =
  full-frame). The software path's existing dirty-box logic (`c2p_statusbar`,
  zoom view dimensions) is the reference for the rects.
- **Cadence.** Splash/menu frames are infrequent and dirty-rects are small, so
  the synchronous single-slot model is almost certainly still fine (no
  double-buffering needed yet).

### M3 exit criteria

- With the accelerator present, every STDOOM screen from the splash onwards is
  rendered through the sidecart C2P with correct geometry.
- Dirty-rect updates work: the status bar updates only changed cells, and a
  small/zoomed view uploads/converts/copies only the active view rectangle.
- With no accelerator, behaviour is identical to the reverted software
  `atari_c2p.c` (clean fallback, no regressions).
- `atari_c2p.c` contains no sidecart-specific code; all of it lives in
  `sidecart_c2p.c`.

---

## Milestone 4 — richer palette + dither modes

### Current status: complete

Confirmed on hardware (Mega STE, 16 MHz + cache). All four render modes work
end-to-end; palette shifts (damage/bonus/radiation) update correctly at runtime;
settings persist to `doomrc.cfg`.

### What was built

- **`CMD_STDOOM_SET_PALETTE`** replaces `CMD_STDOOM_SET_MAP`. The host uploads
  the raw 768-byte DOOM palette (`256 × RGB`) on every `I_SetPalette` call; the
  RP2040 derives the 16 ST output colours and publishes them to `STDOOM_STCOLORS`
  (`$FAF020`); the host reads them back and writes the hardware palette registers.
- **Four render modes** selectable at runtime via UNDO key (cycles) and persisted
  to `doomrc.cfg`:
  - **Nearest-colour** — direct map to closest of 16 ST colours.
  - **2×2 Bayer** — ordered dither between each pixel's two nearest ST colours.
  - **4×4 Bayer** — same with finer 4×4 matrix.
  - **Greyscale** — Rec.601 luma → 16-step grey ramp.
- **Median-cut + k-means palette generation** (default): the RP2040 derives the
  16 output colours from the uploaded palette using median-cut partitioning
  followed by 8 Lloyd iterations with redmean perceptual distance. A fixed
  hand-tuned subset is available as a fallback, toggled live with the `0` key.
- **`-nosidecart`** CLI flag forces the pure software path.
- All accelerated rendering (UNDO, `0`, palette toggle) announces itself via the
  standard HUD message path; messages are visible even in a shrunk view window.

### Hard-won fixes (do not regress)

- **`stdoom_displayed_channel` must use `(v >> 4) * 17`, not
  `stdoom_convert_channel(v) * 17`.** The STE stores the "extra" colour LSB at
  nibble bit 3 (the MSB position); multiplying the raw nibble by 17 gives a
  completely scrambled intensity mapping (e.g. v=16 → nibble 8 → intensity 136
  instead of 17), producing an all-purple cast on the generated palette.
- **RP2040 SRAM budget.** The `s_planar_words[16000]` staging buffer (32 KB) must
  NOT be reintroduced; `stdoom_pack_to_planar_rect` packs directly into
  `s_planar_mem` (ROM-in-RAM). With M4 statics (`s_mode_lut` 4 KB etc.) the
  budget is tight; re-adding it causes an OOM panic in `settings_init`.

### Known limitations / future refinement

- **Palette selection quality.** The median-cut + k-means generated palette
  produces correct colours but the quality of the 16-colour reduction — and
  therefore the dither output — will benefit from further tuning. Candidates:
  - Perceptual weighting in the median-cut axis selection (currently luma-weighted
    range {R×2, G×4, B×3}).
  - More k-means iterations or a better seed (currently 8 Lloyd passes from the
    median-cut means).
  - Improved distance metric or explicit palette-shift-aware regeneration so
    damage/radiation palettes dither as cleanly as the normal palette.
  This is deferred to a future pass; the current output is playable and visually
  acceptable on hardware.

### Pipeline architecture (for reference)

```
On I_SetPalette:
  CMD_STDOOM_SET_PALETTE  →  768 bytes (256 × RGB) of active DOOM palette
                          ←  32 bytes (16 × RGB) chosen ST colours at $FAF020
  Host writes hardware palette registers from the returned 16 colours.

Each frame (unchanged from M3):
  BLIT_ROWS               →  raw 8bpp chunky pixel data (64000 bytes)
  CMD_STDOOM_C2P          →  RP2040 converts in selected mode → planar slot 0
```

---

## Milestone 5 — async data processing

### Current status: implemented, hardware-tested, ABANDONED (2026-06-13)

M5 was built and tested on hardware (saved on the `turbo-async` branch, **not**
merged). The non-blocking dispatch plumbing worked flawlessly — the ST fired C2P
and resumed game logic every frame — but the accelerator turned out to be
**upload-bound, not conversion-bound**: pushing the 64000-byte chunky frame over
the cartridge bus (~1µs/byte ≈ 64ms/frame) dominates, and async only hides the
RP2040's *conversion* time, never the *upload*. Result: ~1.2 fps, slower than the
~4 fps software baseline. Async cannot help while the upload is the bottleneck, so
M5 was abandoned as a direction. This finding motivated M6's upload-optimisation
investigation (which confirmed the upload itself cannot be meaningfully reduced for
gameplay) and the project's renaming from "STDOOM Turbo" to **STDOOM MD**.

### Goal

Decouple game-logic time from RP2040 C2P time by making C2P dispatch
**non-blocking**: the ST fires the C2P command and immediately returns to game
logic (AI, physics, input) without waiting for the token. It synchronises only
at the point it actually needs the planar output — e.g. at vsync before the
planar copy. This hides RP2040 conversion latency behind 68000 work with no
extra memory cost and no protocol changes to `sidecart_stubs.S`.

### Motivation

In M3's synchronous model the ST stalls for the full token round trip after
every C2P command. If game logic takes longer than the RP2040's conversion time
(likely on a stock 8 MHz 68000), that stall is pure waste. Non-blocking dispatch
eliminates it with minimal complexity.

### Key design decisions

- **Single planar slot** — no second buffer needed. The ST simply does not
  write new chunky data or read the planar slot until it has confirmed the token,
  so there is no race on the existing shared memory.
- **Non-blocking send**: issue the C2P command without spinning on the token;
  store the expected token locally and check it (with timeout) only when the
  planar copy is about to happen.
- **`sidecart_stubs.S` unchanged** — the token handshake is identical; only the
  ST-side polling point moves later in the frame.
- **Synchronous path remains** as a compile-time or runtime fallback.

### If upload time proves significant (post-M5)

If profiling after M5 shows that chunky upload itself is a bottleneck, a second
chunky staging buffer (not a second planar slot) could allow overlapping the next
upload with the current conversion — but this is deferred until measurement
justifies the added complexity.

### M5 exit criteria

- The ST fires C2P and returns immediately to game logic.
- Synchronisation (token wait) occurs only just before the planar copy at vsync.
- No tearing or corruption at 16 MHz + cache on Mega STE.
- Synchronous path (M3) still selectable as fallback.

---

## Milestone 6 — upload-optimisation investigation + new C2P modes

### Current status: complete (research conclusion + shipped render modes)

After M5 proved the accelerator is **upload-bound**, M6 had two parts on the
`turbo-upload-optimisation` branch (forked from M4): a time-boxed investigation of
whether the ST→cart upload can be made meaningfully cheaper, and two new greyscale
render modes shipped as a quality improvement. The headline outcome is the
research conclusion that drove renaming the project from "STDOOM Turbo" to
**STDOOM MD**: making DOOM meaningfully fast on a stock ST via this accelerator is
not achievable, because the per-frame chunky upload is an irreducible floor for
gameplay. The work remains valuable as a model for a future SDL XBIOS driver.

### 6A. Upload-optimisation experiment (option C) — concluded

Goal: make the ST→cart upload as efficient as possible, investigated **entirely as
a standalone test `.TOS`** (no STDOOM/firmware changes). Bus transfer time is a
pure function of byte count, so a compressed/partial byte stream sent via the
existing `CMD_STDOOM_BLIT_ROWS` is timed faithfully with no firmware change.

- **`sidecart/tests/png2chunky.py`** — host PNG→C generator (pure Python 3 stdlib:
  zlib + full PNG unfilter), converts three real Doom screenshots (`doom_0/1/2.png`,
  320×200 8-bit indexed) into `sidecart/tests/doom_frames.h` (generated; gitignored,
  rebuilt on every `make uptest`).
- **`sidecart/tests/uptest.c` → `UPTEST.TOS`** — benchmark: E1 baseline upload +
  chunk sweep {6,3,2,1} rows (per-command overhead, ns/byte, theoretical floor);
  E2 PackBits RLE (encode time + ratio + compressed upload); E3 dirty-row delta
  between consecutive frames. Built via `make -C sidecart uptest`.

**Conclusion: compression/skip is a dead end for gameplay — hardware-confirmed**
(Mega STE, 16 MHz + cache, low res, 3 real frames; steady across all three):

- **E1 baseline upload floor:** 64000 B in 6-row chunks = **~203 ms** (307 KB/s,
  34 commands) — STDOOM's chunk size. Sweep: 3r 223 ms, 2r 307 ms, 1r 416 ms
  (150 KB/s, 200 commands); ~per-command overhead **~1.28 ms**. That is a hard
  **~4.9 fps upload ceiling** before any rendering or C2P. Chunks bigger than 6
  rows only buy ~20% (overhead × command count) and would need a protocol-cap
  raise — 6 rows is already near the floor.
- **E2 PackBits RLE — net loss, decisively.** Ratios 95% / 88% / 84% match the host
  projection (gameplay scenes are high-entropy/textured), but the 68000 **encode
  costs ~1.7–1.8 seconds per frame** against only a ~10–30 ms upload saving, so net
  encode+upload is **~1900–2000 ms vs the 203 ms baseline — roughly 10× slower.**
  On-68000 compression is hopeless here.
- **E3 dirty-row delta — net loss.** ~100% of rows change between consecutive frames
  (200/200 then 198/200, camera/player motion), and the per-row compare adds
  66–83 ms, so net is 270–283 ms — worse than the 203 ms baseline.

The only large content-independent lever is fewer bytes/pixel (pre-reduce to 4bpp
on the ST), but that was **explicitly rejected** by the user: it moves colour
reduction back onto the 68000 and removes the ability to apply arbitrary
post-processing (e.g. Bayer dither) on the RP2040, defeating the experiment.

### 6B. New greyscale-dither render modes + key rebind — shipped

- **Two new modes after GREYSCALE:** `STDOOM_MODE_GREY_BAYER2` (4) and
  `STDOOM_MODE_GREY_BAYER4` (5); `STDOOM_MODE_COUNT` is now 6. They ordered-dither
  between the two adjacent grey levels (the 16-step ramp is evenly spaced 17 apart),
  giving a larger *effective* grey palette to reduce banding. Implemented by
  widening the GREY branch of `stdoom_build_palette_and_lut` in `stdoom_worker.c`;
  constants mirrored in `stdoom_commands.h` and `sidecart_md.h`; HUD names in
  `sidecart_c2p.c` and `rndrtest.c`.
- **Mode-switch key moved UNDO → numeric keypad `*`** (scancode 0x66, previously
  unhandled) in `i_video.c`; UNDO reverts to plain F12.
- **`-noturbo` CLI flag renamed `-nosidecart`.**

### M6 exit criteria — met

- Upload-efficiency question answered with hardware-measured numbers (baseline
  ~203 ms/frame ≈ 4.9 fps ceiling; RLE ~10× slower via the 68000 encode cost;
  dirty-row a net loss): the upload floor cannot be meaningfully reduced for
  gameplay → experiment concluded.
- Six render modes selectable at runtime via keypad `*`, persisted to `doomrc.cfg`.
- Standalone `UPTEST.TOS` builds and runs; STDOOM builds clean with the new modes.

---

## Why this approach (longer-term motivation)

If the STDOOM accelerator path works well, it becomes the **model for a new Atari
ST SDL XBIOS driver**. The current `atarist-sdl` `SDL_xbios_md.c` driver is
experimental, unstable, and not really usable; a clean, proven C2P-replacement +
palette/dither design here is intended to be ported into a solid SDL driver. This
is also why M3 favours a clean `atari_c2p.c` (software) / `sidecart_c2p.c`
(accelerated) split — it maps directly onto an SDL driver swapping
implementations.

---

## Verification

- **Build firmware:** `cd /Users/neil/Projects/__OS/atarist-stdoom/sidecart &&
./build.sh pico release <new-uuid>` → UF2 + JSON in `dist/`; `target/atarist`
  builds `BOOT.BIN` via its Makefile.
- **Build host:** `cd /Users/neil/Projects/__OS/atarist-stdoom/linuxdoom-1.10 &&
make` → `atari/build/STDOOM.TOS` (+ `STDOOM20/2F.TOS`); confirm `sidecart_md.o`
  and `sidecart_stubs.o` link.
- **PING (M1):** run `make -C sidecart pingtest` and exercise `sidecart/tests/PINGTEST.TOS` to
  validate detection and version reporting, then flash the UF2 and run STDOOM to
  confirm `MD detected: DOOM Accelerator/1.0`. Flash a different firmware to
  confirm software fallback (`MD not detected; SW C2P`).
- **C2P (M2):** in a 1× low-res level, confirm displayed frames have correct
  geometry/no garbage. Validate planar byte order by C2P-ing a known horizontal
  colour ramp and comparing slot bytes the ST reads against the expected
  interleaved layout. Toggle `g_md_active` off to A/B geometry against the
  software renderer.

### Status note

Milestones 1–4 and 6 are complete and confirmed on hardware (Mega STE, 16 MHz +
cache). Milestone 5 (async dispatch) was implemented and hardware-tested but
abandoned: the accelerator is upload-bound, so async could not help (see M5/M6
above). M6 concluded the upload floor is irreducible for gameplay, which is why
the project was renamed from "STDOOM Turbo" to STDOOM MD.

## Critical files

- **Destination (single repo):** `atarist-stdoom/sidecart/` (firmware) +
  `atarist-stdoom/linuxdoom-1.10/` (host client).
- `/Users/neil/Projects/__OS/md-js/` — source to copy _from_;
  `rp/src/{mdjs_protocol.c, js_worker.c, emul.c}`,
  `target/atarist/src/{sidecart_stubs.S, mdjs.c, mdjs.h}`.
- `/Users/neil/Projects/__OS/md-sprites-demo/target/atarist/src/main.s` — the
  validated reference for STE blitter copy-to-screen (correct `$FFFF8A20`
  register offsets). Prefer this for copy/blitter questions.
- `/Users/neil/Projects/__OS/md-sdl/` — EXPERIMENTAL, not a source of truth.
  `rp/src/emul.c` (`sdl_c2p`) and `src/video/xbios/SDL_xbios_md.c` are useful for
  _structure/ideas_ (and the documented Mega STE `$FF`/`$FE`/`$F4` register
  values in `SDL_megaste.c`), but verify anything before relying on it.
- `/Users/neil/Projects/__OS/atarist-stdoom/linuxdoom-1.10/{i_video.c,
atari_c2p.c, atari_c2p.h, sidecart_c2p.c, sidecart_c2p.h, sidecart_md.c,
sidecart_md.h, Makefile}` — host integration points. Since M3, `atari_c2p.c` is
pure software and all sidecart-accelerated C2P lives in `sidecart_c2p.c`.

## Deferred — future offload candidates (thought exercise)

Nothing below is planned or scoped. These are possibilities worth remembering if
the RP2040 offload approach proves fruitful beyond M5.

### STDOOM-specific

- **Column and span rendering** (`R_DrawColumn`, `R_DrawSpan`) — the hottest
  loops in the software renderer, drawing texture-mapped columns and floor/ceiling
  spans. Offloading these would require streaming draw commands (wall segment
  descriptors, span endpoints, texture data) to the RP2040 and reading back a
  rendered strip — significant protocol work but potentially the biggest framerate
  gain.
- **Sound mixing** — the ST has no hardware PCM mixer; all channel mixing is done
  by the 68000 at interrupt time. The RP2040 could mix channels into a DMA-ready
  buffer and push it to ST RAM, removing the per-sample interrupt load entirely.
- **Screen wipe / melt transitions** — the level-end melt effect runs a per-column
  scroll that is cheap but measurable; easy to offload as a post-process on the
  planar output already in ROM-in-RAM.
- **Palette FX** — damage flash, radiation suit green tint, and bonus gold pulse
  are currently done by rewriting the ST hardware palette registers each tick.
  The RP2040 could maintain a "base" palette and apply tint transforms on request,
  removing the per-frame colour-math from the 68000.
- **Automap line drawing** — the overhead map draws hundreds of lines per frame
  into the chunky buffer; a simple line-drawing command would let the RP2040 fill
  a chunky or planar layer without touching the 68000.

### Broader SDL driver / general ST use

- **Arbitrary C2P** — C2P for modes beyond 320×200×4bpp: ST medium res (640×200
  2bpp), high res (640×400 1bpp), or STE extended palette (4096 colour). The
  RP2040 is fast enough to handle any ST video mode given the right reduction map.
- **Framebuffer scaling** — scaling a small chunky buffer (e.g. 160×100 for a
  double-pixel zoom mode) up to 320×200 before C2P, or letterboxing a 4:3 source
  into the ST's fixed resolution. Useful for any SDL app that renders at less than
  native resolution.
- **Sprite / surface compositing** — blitting a source surface onto a destination
  with transparency (colour-key or 1-bit mask) in chunky space before C2P.
  Removes the most common SDL_BlitSurface hot path from the 68000.
- **Font / glyph rendering** — stamping a glyph atlas into a chunky surface at a
  given position; useful for any text-heavy SDL app (UI, debug overlays).
- **2D primitive rendering** — filled rectangles, horizontal/vertical lines,
  circles. Primitive enough to be a general "chunky GPU" command set.
- **Audio mixing / DSP** — same argument as STDOOM but as a general SDL_mixer
  back-end: the RP2040 mixes N software channels and writes the result to ST DMA
  sound RAM, with optional simple effects (volume, panning, low-pass filter).
- **Asset decompression** — LZ4 or similar decompression of compressed WAD lumps
  or sprite sheets into ST RAM, hiding load latency behind the 68000's other work.
  Particularly useful on floppy-based systems where load time dominates.
- **YUV/RGB conversion** — for video playback (Cinepak, FLIC) the RP2040 could
  convert a compressed colour frame to chunky ST palettised output in one pass,
  enabling video that the 68000 alone could not decode and display in real time.

### Upload optimisations

- **Row-level skip** — maintain a 200-bit dirty mask tracking which rows the
  renderer touched in a given frame and skip unchanged rows in `BLIT_ROWS`. Large
  uniform areas (sky, ceiling, floor) are often identical frame-to-frame. Cheap
  to maintain; combines naturally with the existing dirty-rect logic.
- **Larger chunk size** — `BLIT_ROWS` currently sends fixed 6-row chunks. If
  per-chunk protocol overhead is significant, fewer larger chunks per frame may
  be faster. Worth profiling before assuming the current size is optimal.
- **Simple RLE (PackBits-style)** — a compare-and-count loop over adjacent bytes
  is much simpler than full LZ compression and may be feasible on the 68000 for
  typical Doom scenes, which tend to have long runs (uniform sky, flat floors,
  repeated wall columns). The RP2040 decompression is essentially free. Worth
  benchmarking to see if the 68000 encoding cost is outweighed by the reduced
  upload time.

### Notes

- The RP2040 has ~264 KB SRAM and two cores. Many of the above could run on Core
  1 while Core 0 handles the protocol, but SRAM limits how much intermediate
  buffer space is available concurrently.
- A general "chunky GPU" command set (compositing, scaling, primitives) would make
  the SidecarTridge a broadly useful coprocessor for any TOS or SDL application,
  not just STDOOM — which is the longer-term SDL driver motivation.

---

## Other deferred items

- Resolving cartridge load timing to present "DOOM Accelerator ready" during ST
  boot. **Investigated 2026-06-15:** root cause is a **cold-boot race** — TOS scans
  `$FA0000` for the cartridge magic very early in reset, before the bit-27 CA_INIT
  point, and the RP2040 often hasn't established ROM emulation yet
  (`emul_publish_rom()` runs only after RP firmware boot + clock/voltage/SELECT
  setup, and in debug builds + UART/DPRINTF init), so the banner routine never runs.
  The accelerator still works because STDOOM.TOS launches seconds later, by which
  time the RP is up. The CA_INIT flag (`$08000000`, bit 27) and `target_firmware.h`
  were verified correct/in-sync; the banner string was corrected from "STDOOM
  Accelerator ready" to "DOOM Accelerator ready" (`target/atarist/src/main.s`,
  regenerated `target_firmware.h`). Even so the banner only flashes before the
  desktop clears it. **Recommended:** rely on STDOOM.TOS's own `MD detected: DOOM
  Accelerator/1.0` startup line rather than the boot banner (runs late enough, stays
  on screen). Reflash needed for the corrected string to reach hardware.

(Dynamic palette + Bayer dither is **Milestone 4**; full-pipeline / all-screen
offload is **Milestone 3**; non-blocking C2P dispatch is **Milestone 5**.)
