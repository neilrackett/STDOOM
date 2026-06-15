/**
 * File: stdoom_commands.h
 * Description: DOOM Accelerator — RP2040 command IDs, shared memory map
 *              and the public worker API.
 *
 * DOOM Accelerator offloads CPU-intensive STDOOM render work (C2P first) to
 * the SidecarTridge RP2040 when one is present. The ST detects the firmware
 * with a PING and, in Milestone 2, uploads the chunky framebuffer for the
 * RP2040 to convert to the ST's planar format.
 *
 * Threading model (Milestone 2): everything runs on Core 0. PING just echoes
 * the random token and fills a version string. C2P is synchronous and
 * single-slot.
 *
 * ROM-in-RAM layout (ST sees ROM4 base $FA0000):
 *   0x0000..0x7D00  planar slot 0     (32000 B) — written by C2P (Milestone 2)
 *   0xF000..0xF003  random token       (4 B)    — written to unblock the ST
 *   0xF004..0xF007  random token seed  (4 B)
 *   0xF008..0xF009  status word        (2 B)
 *   0xF00A..0xF00B  ready word         (2 B)    — worker-ready magic
 *   0xF100..0xF8FF  result buffer      (≤2048 B)— PING version string
 *
 * The token/seed/ready offsets match md-js so the ST-side sidecart_stubs.S can
 * be reused verbatim (token at $FAF000, seed at $FAF004).
 */

#ifndef STDOOM_COMMANDS_H
#define STDOOM_COMMANDS_H

#include <stdbool.h>
#include <stdint.h>

#include "tprotocol.h"

/* ── Command IDs ────────────────────────────────────────────────────────── */
/* Start at 0x10 to stay clear of the terminal/booster range. */
#define CMD_STDOOM_PING 0x10 /* Detect firmware; d3='STDM'; version at $FAF100 */
#define CMD_STDOOM_INIT 0x11
#define CMD_STDOOM_SET_MAP 0x12
#define CMD_STDOOM_BLIT_ROWS 0x13
#define CMD_STDOOM_C2P 0x14
/* C2P: d3=(rx<<16)|ry, d4=(rw<<16)|rh (optional rect, M3).  rx is word-aligned
 * (multiple of 16), so its spare low nibble carries an M7 magnify flag: 0/1 =
 * 1:1 convert of the rect; >=2 = nearest-neighbour upscale the (rx,ry,rw,rh)
 * source rect into the size-9 framed view rect (STDOOM_FRAME_VIEW_*), leaving
 * the surrounding GRNROCK border already in the slot untouched. */
#define CMD_STDOOM_SET_PALETTE 0x15 /* Upload 768B (256xRGB) DOOM palette (M4) */
#define CMD_STDOOM_SET_MODE 0x16    /* d3=render mode 0..5 (M4) */
#define CMD_STDOOM_SET_PALGEN 0x17  /* d3=palette source 0..STDOOM_PALGEN_COUNT-1 */
/* 0x18..0x1F reserved for future render offload (column/span, palette FX). */

/* ── Render modes (dither style; M4) ────────────────────────────────────── */
#define STDOOM_MODE_NEAREST 0
#define STDOOM_MODE_BAYER2 1
#define STDOOM_MODE_BAYER4 2
#define STDOOM_MODE_HALFTONE 3 /* 4x4 clustered-dot (halftone) ordered dither */
#define STDOOM_MODE_COUNT 4

/* ── Palette source / 16-colour set (M4 Stage 3) ────────────────────────── */
/* The 16 ST colours: a hand-tuned DOOM subset, a median-cut + k-means palette
 * generated from the uploaded palette, a fixed famous palette, or a 16-step
 * greyscale ramp.  The render mode (nearest/Bayer) then maps each DOOM colour
 * to the closest of these 16 — greyscale needs no explicit luma, since the
 * perceptual redmean reduction already weights green much like luma does. */
#define STDOOM_PALGEN_SUBSET 0      /* fixed hand-tuned subset of DOOM colours */
#define STDOOM_PALGEN_GENERATED 1   /* median-cut + k-means from uploaded palette */
/* Fixed external 16-colour palettes (just for fun): the 16 ST colours are the
 * famous palette, and each DOOM colour is mapped to its nearest entry. */
#define STDOOM_PALGEN_EGA 2
#define STDOOM_PALGEN_C64 3
#define STDOOM_PALGEN_ZX 4
#define STDOOM_PALGEN_PICO8 5
#define STDOOM_PALGEN_GREY 6        /* 16-step greyscale ramp (g = k*17) */
#define STDOOM_PALGEN_COUNT 7

/* PING magic — the ST sends this in d3 and expects a successful token echo. */
#define STDOOM_PING_MAGIC 0x5354444D /* 'STDM' */

/* ── Shared ROM-in-RAM offsets ──────────────────────────────────────────── */
#define STDOOM_RANDOM_TOKEN_OFFSET 0xF000
#define STDOOM_RANDOM_TOKEN_SEED_OFFSET (STDOOM_RANDOM_TOKEN_OFFSET + 4)
#define STDOOM_STATUS_OFFSET 0xF008
#define STDOOM_READY_OFFSET 0xF00A
/* 0xF010..0xF01F: diagnostic counters (see stdoom_worker.c / sidecart_md.h). */
/* Chosen 16 ST hardware-palette words (M4), written by SET_PALETTE/SET_MODE.
 * 16 x uint16, stored in bus order (no byte-swap) so the ST reads them directly
 * and feeds them straight to the $FF8240 palette registers. */
#define STDOOM_STCOLORS_OFFSET 0xF020
#define STDOOM_STCOLORS_COUNT 16
#define STDOOM_STCOLORS_SIZE (STDOOM_STCOLORS_COUNT * 2)
#define STDOOM_RESULT_OFFSET 0xF100
#define STDOOM_RESULT_MAX_SIZE 2048

/* STDOOM low-res frame geometry and planar output slot 0 (Milestone 2). */
#define STDOOM_FRAME_WIDTH 320
#define STDOOM_FRAME_HEIGHT 200
#define STDOOM_CHUNKY_SIZE (STDOOM_FRAME_WIDTH * STDOOM_FRAME_HEIGHT)
#define STDOOM_PLANAR0_OFFSET 0x0000
#define STDOOM_PLANAR_SIZE 32000
#define STDOOM_PLANAR_WORDS (STDOOM_PLANAR_SIZE / 2)
#define STDOOM_PLANAR_WORDS_PER_ROW (STDOOM_FRAME_WIDTH / 4)

/* Framed-upscale target (Milestone 7): Doom's size-9 view layout — the first
 * view size that shows the GRNROCK border (scaledviewwidth=288, viewheight=148,
 * centred at 16,10).  Shrunk views smaller than this are nearest-neighbour
 * upscaled INTO this rect so every size shares the same even border (which Doom
 * already draws around the view) with the HUD message in the top border.
 * X/W are multiples of 16 (plane-word aligned).  MUST match sidecart_md.h. */
#define STDOOM_FRAME_VIEW_X 16
#define STDOOM_FRAME_VIEW_Y 10
#define STDOOM_FRAME_VIEW_W 288
#define STDOOM_FRAME_VIEW_H 148

/* Ready magic written to the ready word once the worker is up. Both bytes of
 * the bus word carry the magic so the ST sees it regardless of which half of
 * the 16-bit word is exposed at the even address. */
#define STDOOM_READY_MAGIC 0x53 /* 'S' */

/* Status values (mirrored on the ST side). */
#define STDOOM_STATUS_IDLE 0x00
#define STDOOM_STATUS_BUSY 0x01
#define STDOOM_STATUS_DONE 0x02

/* ── Public API (called from Core 0 / emul.c) ───────────────────────────── */

/**
 * @brief Initialise worker state and publish the ready magic.
 * Call once from emul_start() before entering the main loop.
 */
void stdoom_worker_init(void);

/**
 * @brief Process any pending command from the ST. Call from the main loop.
 */
void __not_in_flash_func(stdoom_worker_loop)(void);

#endif /* STDOOM_COMMANDS_H */
