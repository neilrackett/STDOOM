/**
 * File: stdoom_worker.c
 * Description: DOOM Accelerator worker (Core 0).
 *
 * Milestone 2: synchronous, single-slot C2P. The worker publishes a ready
 * magic at boot, seeds the random token, answers CMD_STDOOM_PING by writing a
 * version string into the result buffer and echoing the random token, accepts
 * a 256-byte chunky-to-ST4 map, assembles chunky rows into a single 320x200
 * staging buffer, and converts that buffer into planar slot 0 on CMD_C2P.
 */

#include "stdoom_commands.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "debug.h"
#include "memfunc.h"
#include "pico/stdlib.h"
#include "stdoom_protocol.h"

/* ── Linker symbol for ROM-in-RAM base (defined in memmap_rp.ld) ─────────── */
extern unsigned int __rom_in_ram_start__;

/* Version string returned by PING. */
#define STDOOM_VERSION_STRING "DOOM Accelerator/1.0"

/* Cached addresses (set in stdoom_worker_init, read-only thereafter). */
static uint32_t s_rom_base;
static uint32_t s_token_addr;
static uint32_t s_token_seed_addr;
static volatile char *s_result_mem;
static volatile uint16_t *s_status_mem;
static volatile uint16_t *s_ready_mem;
static volatile uint16_t *s_planar_mem;

/* Milestone 2 render state. */
static uint16_t s_frame_width = STDOOM_FRAME_WIDTH;
static uint16_t s_frame_height = STDOOM_FRAME_HEIGHT;
static uint16_t s_chunky_pitch = STDOOM_FRAME_WIDTH;
static uint8_t s_chunky_frame[STDOOM_CHUNKY_SIZE];

/* Milestone 4 palette / render-mode state.
 * s_mode_lut[cell][doom_index] -> 0..15 ST colour, where cell is the 4x4 Bayer
 * cell ((y&3)<<2)|(x&3). For nearest/greyscale all 16 cells are identical; for
 * the Bayer modes they differ. s_st_colors holds the 16 chosen ST hardware
 * palette words returned to the ST (in $FF8240 format). */
static uint8_t s_mode_lut[16][256];
static uint8_t s_palette_rgb[768];     /* last uploaded DOOM palette (256xRGB) */
static uint8_t s_ref_rgb[16][3];       /* DISPLAYED RGB of the 16 chosen colours
                                        * (after ST hardware quantisation), used
                                        * for nearest/dither matching */
static uint16_t s_st_colors[16];       /* 16 chosen colours as ST palette words */
static int s_render_mode = STDOOM_MODE_NEAREST;
static int s_palette_gen = STDOOM_PALGEN_GENERATED;  /* subset vs median-cut */
static volatile uint16_t *s_stcolors_mem;

/* Working storage for median-cut palette generation (touched only on a palette
 * rebuild, which is rare). s_palgen_axis is the channel qsort compares by. */
static uint8_t s_palgen_idx[256];
static int s_palgen_axis;

/* The 16 DOOM palette indices used as the fixed ST colour set (matches the host
 * subset_lorez[16] in atari_c2p.c, so nearest mode reproduces the M3 look). */
static const uint8_t s_subset[16] = {
    0, 90, 101, 241, 202, 252, 38, 219, 144, 136, 158, 120, 72, 58, 249, 4};

/* Drain spurious commands the PIO/parser may latch during cold start. */
static volatile bool s_dispatch_armed = false;
static TransmissionProtocol s_loop_proto;

/* Sparse UART diagnostics so we can prove which ST-side path is active without
 * flooding the console every frame. */
static uint32_t s_diag_ping_count;
static uint32_t s_diag_init_count;
static uint32_t s_diag_set_map_count;
static uint32_t s_diag_blit_rows_count;
static uint32_t s_diag_c2p_count;

#define STDOOM_BUS_BYTE_WORD(value) ((uint16_t)((uint16_t)(value) << 8))

/* Ready flag: write the magic to BOTH bytes of the bus word so the ST sees the
 * same byte regardless of which half of the 16-bit word is at the even
 * address. */
#define STDOOM_READY_WORD \
  ((uint16_t)((STDOOM_READY_MAGIC << 8) | STDOOM_READY_MAGIC))

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Copy a byte buffer while swapping adjacent bytes.
 * This avoids any halfword-alignment requirement on the RP2040. */
static void stdoom_copy_swap_bytes(uint8_t *dst, const uint8_t *src,
                                   uint32_t byte_count) {
  uint32_t i = 0;

  for (; i + 1u < byte_count; i += 2u) {
    dst[i] = src[i + 1u];
    dst[i + 1u] = src[i];
  }
  if (i < byte_count) {
    dst[i] = src[i];
  }
}

/* Rolling non-zero seed source. rand() must NOT be used: on the RP2040 there
 * is no RTC, so time()==0 -> srand(0) -> newlib rand() returns 0 on its first
 * call, producing a zero token/seed that breaks the ST handshake (the ST waits
 * for seed_addr != token, which can never be true when both are 0). A simple
 * LCG seeded non-zero guarantees a changing, always-non-zero value. */
static uint32_t s_seed_state = 0xABCD1234u;

static uint32_t stdoom_next_seed(void) {
  s_seed_state = s_seed_state * 1103515245u + 12345u;
  return s_seed_state | 1u;  /* force non-zero */
}

/* Write the random token back to shared memory to unblock the ST and seed the
 * next token value. */
static void stdoom_send_response(uint32_t random_token) {
  TPROTO_SET_RANDOM_TOKEN(s_token_addr, random_token);
  TPROTO_SET_RANDOM_TOKEN(s_token_seed_addr, stdoom_next_seed());
}

/* Copy a NUL-terminated C string into the result buffer with the m68k
 * big-endian byte order the ST expects (the parser/ST read pairs of bytes from
 * each 16-bit bus word high-byte-first). */
static void stdoom_write_result(const char *str) {
  char tmp[STDOOM_RESULT_MAX_SIZE];
  size_t len = strlen(str);
  if (len >= sizeof(tmp)) {
    len = sizeof(tmp) - 1;
  }
  memset(tmp, 0, sizeof(tmp));
  memcpy(tmp, str, len);
  stdoom_copy_swap_bytes((uint8_t *)s_result_mem, (const uint8_t *)tmp,
                         (uint32_t)((len + 2u) & ~(size_t)1u));
}

/* ── Palette / colour reduction (M4) ─────────────────────────────────────── */

/* Match the host convert_channel()/stcolor() (atari_c2p.c): top 3 bits + the
 * STe low bit, packed 0x0RGB. The ST writes the returned word straight to the
 * $FF8240 palette registers, so the firmware must produce the identical format. */
static uint16_t stdoom_convert_channel(uint8_t v) {
  uint16_t r = (uint16_t)((v & 0xE0u) >> 5);  /* bits 7,6,5 -> 2,1,0 */
  r |= (uint16_t)((v & 0x10u) >> 1);          /* STe low bit */
  return r;
}

static uint16_t stdoom_st_color_word(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t entry = stdoom_convert_channel(r);
  entry = (uint16_t)(entry << 4) | stdoom_convert_channel(g);
  entry = (uint16_t)(entry << 4) | stdoom_convert_channel(b);
  return entry;
}

/* The intensity (0..255) the ST actually displays for an 8-bit channel value,
 * after quantisation to the hardware 4-bit nibble.
 *
 * Despite the STE's unusual nibble encoding (the "extra" STE bit is stored at
 * nibble bit 3, the MSB, while colour bits 3:1 occupy bits 2:0), the actual
 * 4-bit colour value is simply the top nibble of the 8-bit input.  Using
 * stdoom_convert_channel(v) * 17 is WRONG: convert_channel(16) = 8, giving
 * a displayed intensity of 136 instead of 17.  The correct formula is just
 * (v >> 4) * 17 — top nibble times the STE step size. */
static uint8_t stdoom_displayed_channel(uint8_t v) {
  return (uint8_t)(((uint16_t)(v >> 4)) * 17u);
}

/* Perceptual colour distance (Riemersma "redmean" approximation): a green-
 * weighted, red-mean-adjusted squared distance that tracks human colour
 * perception far better than plain RGB Euclidean — fewer visibly-wrong matches
 * in skies/browns. Integer-only; max term well within int32. */
static long stdoom_color_dist(int r1, int g1, int b1, int r2, int g2, int b2) {
  long rmean = ((long)r1 + (long)r2) / 2;
  long dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
  return (((512 + rmean) * dr * dr) >> 8) + 4 * dg * dg +
         (((767 - rmean) * db * db) >> 8);
}

/* Index of the nearest of the 16 chosen colours (s_ref_rgb) to (r,g,b). */
static uint8_t stdoom_nearest_ref(uint8_t r, uint8_t g, uint8_t b) {
  long best = 0x7FFFFFFFL;
  uint8_t best_k = 0;
  for (uint8_t k = 0; k < 16u; k++) {
    long d = stdoom_color_dist(r, g, b, s_ref_rgb[k][0], s_ref_rgb[k][1],
                               s_ref_rgb[k][2]);
    if (d < best) {
      best = d;
      best_k = k;
    }
  }
  return best_k;
}

/* Find the nearest and second-nearest of the 16 chosen colours (s_ref_rgb) to
 * (r,g,b). Used by the ordered-dither modes. */
static void stdoom_two_nearest_ref(uint8_t r, uint8_t g, uint8_t b,
                                   uint8_t *out_a, uint8_t *out_b) {
  long best = 0x7FFFFFFFL, best2 = 0x7FFFFFFFL;
  uint8_t a = 0, b2 = 0;
  for (uint8_t k = 0; k < 16u; k++) {
    long d = stdoom_color_dist(r, g, b, s_ref_rgb[k][0], s_ref_rgb[k][1],
                               s_ref_rgb[k][2]);
    if (d < best) {
      best2 = best;
      b2 = a;
      best = d;
      a = k;
    } else if (d < best2) {
      best2 = d;
      b2 = k;
    }
  }
  *out_a = a;
  *out_b = b2;
}

/* ── Median-cut + k-means palette generation (M4 Stage 3) ────────────────── */

/* qsort comparator: order palette indices by the channel in s_palgen_axis. */
static int stdoom_palgen_cmp(const void *pa, const void *pb) {
  uint8_t a = *(const uint8_t *)pa;
  uint8_t b = *(const uint8_t *)pb;
  int va = s_palette_rgb[(uint32_t)a * 3u + (uint32_t)s_palgen_axis];
  int vb = s_palette_rgb[(uint32_t)b * 3u + (uint32_t)s_palgen_axis];
  return va - vb;
}

/* Median-cut: partition the 256 palette colours into 16 boxes, splitting the
 * box with the largest luma-weighted axis range at its median. Each box's mean
 * is the representative colour. Produces 16 RGB triples in out_rgb. */
static void stdoom_median_cut(uint8_t out_rgb[16][3]) {
  /* Axis weights bias splits toward the channels the eye resolves best. */
  static const int axis_w[3] = {2, 4, 3};
  uint16_t box_start[16];
  uint16_t box_count[16];
  uint8_t nboxes = 1;
  int b, ch;

  for (b = 0; b < 256; b++) {
    s_palgen_idx[b] = (uint8_t)b;
  }
  box_start[0] = 0;
  box_count[0] = 256;

  while (nboxes < 16u) {
    int best = -1, best_axis = 0;
    long best_score = -1;

    for (b = 0; b < nboxes; b++) {
      int mn[3] = {255, 255, 255};
      int mx[3] = {0, 0, 0};
      long arange;
      int axis;
      uint16_t k;

      if (box_count[b] < 2u) {
        continue;
      }
      for (k = 0; k < box_count[b]; k++) {
        const uint8_t *c =
            &s_palette_rgb[(uint32_t)s_palgen_idx[box_start[b] + k] * 3u];
        for (ch = 0; ch < 3; ch++) {
          if (c[ch] < mn[ch]) mn[ch] = c[ch];
          if (c[ch] > mx[ch]) mx[ch] = c[ch];
        }
      }
      arange = -1;
      axis = 0;
      for (ch = 0; ch < 3; ch++) {
        long r = (long)(mx[ch] - mn[ch]) * axis_w[ch];
        if (r > arange) {
          arange = r;
          axis = ch;
        }
      }
      if (arange > best_score) {
        best_score = arange;
        best = b;
        best_axis = axis;
      }
    }

    if (best < 0) {
      break; /* no box is splittable (all single-colour) */
    }

    /* Sort the chosen box by its widest axis and split at the median. */
    s_palgen_axis = best_axis;
    qsort(&s_palgen_idx[box_start[best]], box_count[best],
          sizeof(s_palgen_idx[0]), stdoom_palgen_cmp);
    {
      uint16_t half = (uint16_t)(box_count[best] / 2u);
      box_start[nboxes] = (uint16_t)(box_start[best] + half);
      box_count[nboxes] = (uint16_t)(box_count[best] - half);
      box_count[best] = half;
      nboxes++;
    }
  }

  for (b = 0; b < 16; b++) {
    if (b < nboxes && box_count[b] > 0u) {
      uint32_t sr = 0, sg = 0, sb = 0;
      uint16_t k;
      for (k = 0; k < box_count[b]; k++) {
        const uint8_t *c =
            &s_palette_rgb[(uint32_t)s_palgen_idx[box_start[b] + k] * 3u];
        sr += c[0];
        sg += c[1];
        sb += c[2];
      }
      out_rgb[b][0] = (uint8_t)(sr / box_count[b]);
      out_rgb[b][1] = (uint8_t)(sg / box_count[b]);
      out_rgb[b][2] = (uint8_t)(sb / box_count[b]);
    } else {
      out_rgb[b][0] = out_rgb[b][1] = out_rgb[b][2] = 0;
    }
  }
}

/* k-means (Lloyd) refinement of the 16 centroids over the 256 palette colours,
 * using the perceptual distance for assignment. Tightens the clusters so the
 * nearest/second-nearest pair is as close as possible — the key to smooth,
 * non-dotty dither. Empty clusters keep their previous centroid. */
#define STDOOM_KMEANS_ITERS 8
static void stdoom_kmeans(uint8_t cen[16][3]) {
  int it, k, i;
  for (it = 0; it < STDOOM_KMEANS_ITERS; it++) {
    uint32_t sum[16][3];
    uint32_t count[16];
    for (k = 0; k < 16; k++) {
      sum[k][0] = sum[k][1] = sum[k][2] = 0;
      count[k] = 0;
    }
    for (i = 0; i < 256; i++) {
      const uint8_t *c = &s_palette_rgb[i * 3];
      long best = 0x7FFFFFFFL;
      int bk = 0;
      for (k = 0; k < 16; k++) {
        long d = stdoom_color_dist(c[0], c[1], c[2], cen[k][0], cen[k][1],
                                   cen[k][2]);
        if (d < best) {
          best = d;
          bk = k;
        }
      }
      sum[bk][0] += c[0];
      sum[bk][1] += c[1];
      sum[bk][2] += c[2];
      count[bk]++;
    }
    for (k = 0; k < 16; k++) {
      if (count[k]) {
        cen[k][0] = (uint8_t)(sum[k][0] / count[k]);
        cen[k][1] = (uint8_t)(sum[k][1] / count[k]);
        cen[k][2] = (uint8_t)(sum[k][2] / count[k]);
      }
    }
  }
}

/* Rec.601 luma (0..255) of an 8-bit RGB triple. */
static uint8_t stdoom_luma(uint8_t r, uint8_t g, uint8_t b) {
  return (uint8_t)(((uint32_t)r * 77u + (uint32_t)g * 150u +
                    (uint32_t)b * 29u) >> 8);
}

/* 4x4 ordered-dither thresholds, indexed by Bayer cell ((y&3)<<2)|(x&3).
 * Values 0..15 (matches the host bayer[4][4] in atari_c2p.c). */
static const uint8_t s_bayer4[16] = {
    0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

/* 2x2 ordered-dither thresholds for the 4 sub-cells (y&1,x&1), scaled to the
 * same 0..15 space as the 4x4 matrix (values 2,10,14,6). */
static const uint8_t s_bayer2[4] = {2, 10, 14, 6};

/* Threshold (0..15) for a 4x4 Bayer cell in the requested mode. */
static uint8_t stdoom_dither_threshold(int mode, uint8_t cell) {
  if (mode == STDOOM_MODE_BAYER2) {
    uint8_t row2 = (uint8_t)((cell >> 2) & 1u);
    uint8_t col2 = (uint8_t)(cell & 1u);
    return s_bayer2[(row2 << 1) | col2];
  }
  return s_bayer4[cell & 15u];
}

/* Publish the 16 chosen ST colour words to ROM-in-RAM (bus order, no swap: the
 * ST reads each uint16 directly, like the planar words). */
static void stdoom_write_stcolors(void) {
  if (!s_stcolors_mem) {
    return;
  }
  for (uint16_t k = 0; k < STDOOM_STCOLORS_COUNT; k++) {
    s_stcolors_mem[k] = s_st_colors[k];
  }
}

/* Snap 16 source RGB colours to the ST hardware palette: store the ST word in
 * s_st_colors and the DISPLAYED RGB (post-quantisation) in s_ref_rgb so the LUT
 * matches against what the screen actually shows. */
static void stdoom_set_refs_from_rgb(const uint8_t src[16][3]) {
  for (uint8_t k = 0; k < 16u; k++) {
    s_st_colors[k] = stdoom_st_color_word(src[k][0], src[k][1], src[k][2]);
    s_ref_rgb[k][0] = stdoom_displayed_channel(src[k][0]);
    s_ref_rgb[k][1] = stdoom_displayed_channel(src[k][1]);
    s_ref_rgb[k][2] = stdoom_displayed_channel(src[k][2]);
  }
}

/* Fill the 16 reference colours for nearest/Bayer modes, from either the fixed
 * hand-tuned subset or a generated (median-cut + k-means) palette. */
static void stdoom_build_refs(void) {
  uint8_t pal[16][3];
  uint8_t k;

  if (s_palette_gen == STDOOM_PALGEN_GENERATED) {
    stdoom_median_cut(pal);
    stdoom_kmeans(pal);
  } else {
    for (k = 0; k < 16u; k++) {
      const uint8_t *c = &s_palette_rgb[(uint32_t)s_subset[k] * 3u];
      pal[k][0] = c[0];
      pal[k][1] = c[1];
      pal[k][2] = c[2];
    }
  }
  stdoom_set_refs_from_rgb(pal);
}

/* Rebuild the 16 chosen ST colours and the per-cell reduction LUT for the
 * current render mode, from the last uploaded palette (s_palette_rgb).
 *
 * NEAREST/BAYER2/BAYER4 use the fixed s_subset[] palette (matching the M3
 * look); GREY / GREY_BAYER2 / GREY_BAYER4 generate their own 16-step grey ramp.
 * Bayer modes do a classic 2-colour ordered dither between each DOOM colour's
 * nearest and second-nearest ST colours, thresholded per Bayer cell; the
 * greyscale Bayer modes dither between the two adjacent grey levels instead, to
 * give a larger effective grey palette and reduce banding. (Median-cut palette
 * generation is Stage 3.) */
static void stdoom_build_palette_and_lut(void) {
  if (s_render_mode == STDOOM_MODE_GREY ||
      s_render_mode == STDOOM_MODE_GREY_BAYER2 ||
      s_render_mode == STDOOM_MODE_GREY_BAYER4) {
    /* Greyscale Bayer variants dither between adjacent grey levels; pick the
     * matching 2x2/4x4 threshold matrix (plain GREY does no dither). */
    int grey_dither = (s_render_mode != STDOOM_MODE_GREY);
    int bayer_mode = (s_render_mode == STDOOM_MODE_GREY_BAYER2)
                         ? STDOOM_MODE_BAYER2
                         : STDOOM_MODE_BAYER4;

    /* 16-step grey ramp: level k -> grey value k*17 (0..255). */
    for (uint8_t k = 0; k < 16u; k++) {
      uint8_t v = (uint8_t)(k * 17u);
      s_ref_rgb[k][0] = v;
      s_ref_rgb[k][1] = v;
      s_ref_rgb[k][2] = v;
      s_st_colors[k] = stdoom_st_color_word(v, v, v);
    }
    for (uint32_t i = 0; i < 256u; i++) {
      const uint8_t *c = &s_palette_rgb[i * 3u];
      uint8_t y = stdoom_luma(c[0], c[1], c[2]);
      if (!grey_dither) {
        uint8_t level = (uint8_t)(((uint32_t)y * 15u + 127u) / 255u);
        for (uint8_t cell = 0; cell < 16u; cell++) {
          s_mode_lut[cell][i] = level;
        }
      } else {
        /* Grey levels are evenly spaced 17 apart, so the luma maps to a lower
         * level lo and a 0..16 fraction towards lo+1; dither on that fraction. */
        uint8_t lo = (uint8_t)(y / 17u);                 /* 0..15 */
        uint8_t hi = (uint8_t)((lo < 15u) ? lo + 1u : 15u);
        int t16 = (int)y - (int)((uint16_t)lo * 17u);    /* 0..16 */
        for (uint8_t cell = 0; cell < 16u; cell++) {
          uint8_t thr = stdoom_dither_threshold(bayer_mode, cell);
          s_mode_lut[cell][i] = (uint8_t)((t16 > (int)thr) ? hi : lo);
        }
      }
    }
    return;
  }

  stdoom_build_refs();

  if (s_render_mode == STDOOM_MODE_BAYER2 ||
      s_render_mode == STDOOM_MODE_BAYER4) {
    for (uint32_t i = 0; i < 256u; i++) {
      const uint8_t *c = &s_palette_rgb[i * 3u];
      uint8_t a, b;
      int dr, dg, db, denom, dot, t16;
      stdoom_two_nearest_ref(c[0], c[1], c[2], &a, &b);

      /* Project the DOOM colour onto the A->B line: t in 0..16. */
      dr = (int)s_ref_rgb[b][0] - (int)s_ref_rgb[a][0];
      dg = (int)s_ref_rgb[b][1] - (int)s_ref_rgb[a][1];
      db = (int)s_ref_rgb[b][2] - (int)s_ref_rgb[a][2];
      denom = dr * dr + dg * dg + db * db;
      dot = ((int)c[0] - (int)s_ref_rgb[a][0]) * dr +
            ((int)c[1] - (int)s_ref_rgb[a][1]) * dg +
            ((int)c[2] - (int)s_ref_rgb[a][2]) * db;
      if (denom <= 0 || dot <= 0) {
        t16 = 0;
      } else if (dot >= denom) {
        t16 = 16;
      } else {
        t16 = (dot * 16) / denom;
      }

      for (uint8_t cell = 0; cell < 16u; cell++) {
        uint8_t thr = stdoom_dither_threshold(s_render_mode, cell);
        s_mode_lut[cell][i] = (uint8_t)((t16 > (int)thr) ? b : a);
      }
    }
    return;
  }

  /* NEAREST: identical for every Bayer cell. */
  for (uint32_t i = 0; i < 256u; i++) {
    const uint8_t *c = &s_palette_rgb[i * 3u];
    uint8_t m = stdoom_nearest_ref(c[0], c[1], c[2]);
    for (uint8_t cell = 0; cell < 16u; cell++) {
      s_mode_lut[cell][i] = m;
    }
  }
}

/* Reset the reduction LUT to an identity map (used at INIT and before any
 * palette/map upload, and by the SET_MAP path's defaults). */
static void stdoom_reset_map(void) {
  for (uint32_t i = 0; i < 256u; i++) {
    for (uint8_t cell = 0; cell < 16u; cell++) {
      s_mode_lut[cell][i] = (uint8_t)(i & 0x0Fu);
    }
  }
}

static void stdoom_reset_buffers(void) {
  memset(s_chunky_frame, 0, sizeof(s_chunky_frame));
  /* ROM-in-RAM planar slot is zeroed by emul_publish_rom() at boot. */
}

/* C2P for an arbitrary word-aligned sub-rectangle.
 *
 * rx must be a multiple of 16.  The converted words are written into the
 * correct offsets of s_planar_words (the s_planar_words array is kept intact
 * outside the rect so partial updates in later milestones compose correctly).
 * Only the rows [ry..ry+rh) are then copied to s_planar_mem, leaving the rest
 * of ROM-in-RAM slot 0 undisturbed.
 *
 * For the full-frame case (rx=0, ry=0, rw=s_frame_width, rh=s_frame_height)
 * the result is byte-identical to the previous single-call memcpy path.
 *
 * NO byte-swap on the copy to s_planar_mem — the ST reads uint16 values
 * directly and the plane words are already in the correct form.
 */
static void stdoom_pack_to_planar_rect(uint16_t rx, uint16_t ry,
                                       uint16_t rw, uint16_t rh) {
  uint16_t words_per_row = (uint16_t)(s_frame_width / 4u);
  uint16_t blk_start = (uint16_t)(rx / 16u);
  uint16_t blk_count = (uint16_t)(rw / 16u);
  /* Write directly to ROM-in-RAM; no intermediate staging buffer needed.
   * The ST is blocked on the token and cannot read the planar slot until we
   * echo it after this function returns. Cast away volatile for the inner
   * loop — ROM-in-RAM is ordinary SRAM on the RP2040. */
  uint16_t *planar_base = (uint16_t *)s_planar_mem;

  for (uint16_t y = ry; y < ry + rh; y++) {
    const uint8_t *row = &s_chunky_frame[(uint32_t)y * s_chunky_pitch];
    uint16_t *dst_row = planar_base + (uint32_t)y * words_per_row;
    uint8_t y_cell = (uint8_t)((y & 3u) << 2);  /* Bayer cell row component */

    for (uint16_t blk = blk_start; blk < blk_start + blk_count; blk++) {
      uint16_t plane0 = 0;
      uint16_t plane1 = 0;
      uint16_t plane2 = 0;
      uint16_t plane3 = 0;
      const uint8_t *src = row + (blk * 16u);
      uint16_t x0 = (uint16_t)(blk * 16u);

      for (uint16_t px = 0; px < 16u; px++) {
        /* Per-pixel Bayer cell = (y&3)<<2 | (x&3); for nearest/grey all 16 LUT
         * cells are identical, so this is a no-op there. */
        uint8_t cell = (uint8_t)(y_cell | ((x0 + px) & 3u));
        uint8_t mapped = s_mode_lut[cell][src[px]] & 0x0Fu;
        uint16_t bit = (uint16_t)(1u << (15u - px));
        if (mapped & 0x1u) plane0 |= bit;
        if (mapped & 0x2u) plane1 |= bit;
        if (mapped & 0x4u) plane2 |= bit;
        if (mapped & 0x8u) plane3 |= bit;
      }

      dst_row[(blk * 4u) + 0u] = plane0;
      dst_row[(blk * 4u) + 1u] = plane1;
      dst_row[(blk * 4u) + 2u] = plane2;
      dst_row[(blk * 4u) + 3u] = plane3;
    }
  }
}

/* Upscaling C2P (Milestone 7): read source rect (sx,sy,sw,sh) from the staged
 * chunky frame and write a pixel-replicated scale*-magnified planar rect to the
 * TOP-LEFT of slot 0 (dst origin 0,0, size sw*scale x sh*scale).  Mirrors the
 * software zoom path (c2p_screen_lorez): a small centred view fills the 320x168
 * play area, so the GRNROCK border the host renders is never read.
 *
 * scale is a power of two (2 or 4); use a shift so the RP2040 (no hardware
 * divide) maps output->source coords cheaply.  The Bayer LUT cell is taken from
 * the OUTPUT coords (oy&3)<<2 | (ox&3), matching the software c2p_2x/4x tables.
 *
 * dst x is always 0 (16-aligned) so plane-word packing is unaffected; the host
 * then blits the full-width 320 x (sh*scale) rect with the linear blitter. */
static void stdoom_pack_to_planar_scaled(uint16_t sx, uint16_t sy,
                                         uint16_t sw, uint16_t sh,
                                         uint8_t scale) {
  uint16_t words_per_row = (uint16_t)(s_frame_width / 4u);
  uint8_t shift = (scale >= 4u) ? 2u : 1u;       /* scale 2->1, 4->2 */
  uint16_t dst_w = (uint16_t)(sw << shift);
  uint16_t dst_h = (uint16_t)(sh << shift);
  uint16_t blk_count = (uint16_t)(dst_w / 16u);
  uint16_t *planar_base = (uint16_t *)s_planar_mem;

  if (dst_w > s_frame_width)   dst_w = s_frame_width;
  if (dst_h > s_frame_height)  dst_h = s_frame_height;

  for (uint16_t oy = 0; oy < dst_h; oy++) {
    const uint8_t *src_row =
        &s_chunky_frame[(uint32_t)(sy + (oy >> shift)) * s_chunky_pitch];
    uint16_t *dst_row = planar_base + (uint32_t)oy * words_per_row;
    uint8_t y_cell = (uint8_t)((oy & 3u) << 2);

    for (uint16_t blk = 0; blk < blk_count; blk++) {
      uint16_t plane0 = 0;
      uint16_t plane1 = 0;
      uint16_t plane2 = 0;
      uint16_t plane3 = 0;
      uint16_t x0 = (uint16_t)(blk * 16u);

      for (uint16_t px = 0; px < 16u; px++) {
        uint16_t ox = (uint16_t)(x0 + px);
        uint8_t cell = (uint8_t)(y_cell | (ox & 3u));
        uint8_t mapped = s_mode_lut[cell][src_row[sx + (ox >> shift)]] & 0x0Fu;
        uint16_t bit = (uint16_t)(1u << (15u - px));
        if (mapped & 0x1u) plane0 |= bit;
        if (mapped & 0x2u) plane1 |= bit;
        if (mapped & 0x4u) plane2 |= bit;
        if (mapped & 0x8u) plane3 |= bit;
      }

      dst_row[(blk * 4u) + 0u] = plane0;
      dst_row[(blk * 4u) + 1u] = plane1;
      dst_row[(blk * 4u) + 2u] = plane2;
      dst_row[(blk * 4u) + 3u] = plane3;
    }
  }
}

static bool stdoom_diag_should_log(uint32_t count) {
  return count < 4u || (count % 120u) == 0u;
}

static void stdoom_init_frame_state(uint16_t width, uint16_t height) {
  if (width == 0) {
    width = STDOOM_FRAME_WIDTH;
  }
  if (height == 0) {
    height = STDOOM_FRAME_HEIGHT;
  }
  if (width > STDOOM_FRAME_WIDTH) {
    width = STDOOM_FRAME_WIDTH;
  }
  if (height > STDOOM_FRAME_HEIGHT) {
    height = STDOOM_FRAME_HEIGHT;
  }

  s_frame_width = width;
  s_frame_height = height;
  s_chunky_pitch = width;
  s_render_mode = STDOOM_MODE_NEAREST;
  s_palette_gen = STDOOM_PALGEN_GENERATED;
  stdoom_reset_map();
  stdoom_reset_buffers();
}

static void stdoom_store_chunky_rows(uint16_t y, uint16_t rows,
                                     uint16_t width, uint16_t pitch,
                                     const uint8_t *buf, uint32_t buf_bytes) {
  uint16_t copy_width = width;

  (void)pitch;
  if (y >= s_frame_height || rows == 0 || copy_width == 0 || !buf) {
    return;
  }
  if (copy_width > s_chunky_pitch) {
    copy_width = s_chunky_pitch;
  }
  if ((uint32_t)copy_width * rows > buf_bytes) {
    rows = (uint16_t)(buf_bytes / copy_width);
  }
  if (rows == 0) {
    return;
  }
  if ((uint32_t)y + rows > s_frame_height) {
    rows = (uint16_t)(s_frame_height - y);
  }

  for (uint16_t row = 0; row < rows; row++) {
    uint8_t *dst = &s_chunky_frame[((uint32_t)(y + row) * s_chunky_pitch)];
    const uint8_t *src = &buf[(uint32_t)row * width];
    stdoom_copy_swap_bytes(dst, src, copy_width);
  }
}

/* ── Command dispatch ────────────────────────────────────────────────────── */

static void stdoom_dispatch_command(const TransmissionProtocol *proto) {
  if (proto->payload_size < 4u) {
    DPRINTF("stdoom_dispatch: short payload for 0x%04X\n", proto->command_id);
    return;
  }

  uint32_t random_token = TPROTO_GET_RANDOM_TOKEN(proto->payload);
  const uint16_t *payload16 = (const uint16_t *)proto->payload;
  const uint16_t *params16 = payload16 + 2;

  switch (proto->command_id) {
    case CMD_STDOOM_PING: {
      s_diag_ping_count++;
      if (stdoom_diag_should_log(s_diag_ping_count)) {
        DPRINTF("stdoom_dispatch: PING #%lu token=%08lX\n",
                (unsigned long)s_diag_ping_count, (unsigned long)random_token);
      }
      stdoom_write_result(STDOOM_VERSION_STRING);
      *s_status_mem = STDOOM_BUS_BYTE_WORD(STDOOM_STATUS_DONE);
      stdoom_send_response(random_token);
      break;
    }
    case CMD_STDOOM_INIT: {
      uint32_t d3;
      uint16_t width;
      uint16_t height;

      if (proto->payload_size < 8u) {
        DPRINTF("stdoom_dispatch: short INIT payload\n");
        return;
      }

      d3 = ((uint32_t)params16[1] << 16) | params16[0];
      width = (uint16_t)(d3 >> 16);
      height = (uint16_t)(d3 & 0xFFFFu);

      s_diag_init_count++;
      if (stdoom_diag_should_log(s_diag_init_count)) {
        DPRINTF("stdoom_dispatch: INIT #%lu width=%u height=%u\n",
                (unsigned long)s_diag_init_count, (unsigned)width,
                (unsigned)height);
      }

      *s_status_mem = STDOOM_BUS_BYTE_WORD(STDOOM_STATUS_BUSY);
      stdoom_init_frame_state(width, height);
      *s_status_mem = STDOOM_BUS_BYTE_WORD(STDOOM_STATUS_DONE);
      stdoom_send_response(random_token);
      break;
    }
    case CMD_STDOOM_SET_MAP: {
      uint32_t map_bytes;
      const uint8_t *map_buf;

      if (proto->payload_size < 16u) {
        DPRINTF("stdoom_dispatch: short SET_MAP payload\n");
        return;
      }

      map_bytes = (uint32_t)proto->payload_size - 16u;
      if (map_bytes > 256u) {
        map_bytes = 256u;
      }
      map_buf = (const uint8_t *)&params16[6];

      s_diag_set_map_count++;
      if (stdoom_diag_should_log(s_diag_set_map_count)) {
        DPRINTF("stdoom_dispatch: SET_MAP #%lu bytes=%lu\n",
                (unsigned long)s_diag_set_map_count, (unsigned long)map_bytes);
      }

      /* SET_MAP installs an explicit 256-entry nearest map (no dither): write it
       * into LUT cell 0, fill any short upload with the identity default, then
       * replicate to all 16 Bayer cells so the pack loop is mode-agnostic. Used
       * by the standalone test apps (c2ptest/recttest); the game path uses
       * SET_PALETTE/SET_MODE instead. */
      *s_status_mem = STDOOM_BUS_BYTE_WORD(STDOOM_STATUS_BUSY);
      if ((map_bytes & 1u) == 0u) {
        stdoom_copy_swap_bytes(s_mode_lut[0], map_buf, map_bytes);
      } else {
        uint32_t even_bytes = map_bytes & ~1u;
        stdoom_copy_swap_bytes(s_mode_lut[0], map_buf, even_bytes);
        s_mode_lut[0][even_bytes] = map_buf[even_bytes];
      }
      if (map_bytes < 256u) {
        for (uint32_t i = map_bytes; i < 256u; i++) {
          s_mode_lut[0][i] = (uint8_t)(i & 0x0Fu);
        }
      }
      for (uint8_t cell = 1; cell < 16u; cell++) {
        memcpy(s_mode_lut[cell], s_mode_lut[0], 256u);
      }
      *s_status_mem = STDOOM_BUS_BYTE_WORD(STDOOM_STATUS_DONE);
      stdoom_send_response(random_token);
      break;
    }
    case CMD_STDOOM_BLIT_ROWS: {
      uint32_t d3;
      uint32_t d4;
      uint32_t d5;
      uint16_t y;
      uint16_t width;
      uint16_t rows;
      uint16_t pitch;
      uint32_t buf_bytes;
      const uint8_t *buf;

      if (proto->payload_size < 16u) {
        DPRINTF("stdoom_dispatch: short BLIT_ROWS payload\n");
        return;
      }

      d3 = ((uint32_t)params16[1] << 16) | params16[0];
      d4 = ((uint32_t)params16[3] << 16) | params16[2];
      d5 = ((uint32_t)params16[5] << 16) | params16[4];
      y = (uint16_t)(d3 & 0xFFFFu);
      width = (uint16_t)(d4 >> 16);
      rows = (uint16_t)(d4 & 0xFFFFu);
      pitch = (uint16_t)(d5 >> 16);
      buf = (const uint8_t *)&params16[6];
      buf_bytes = (uint32_t)proto->payload_size - 16u;

      s_diag_blit_rows_count++;
      if (stdoom_diag_should_log(s_diag_blit_rows_count)) {
        DPRINTF("stdoom_dispatch: BLIT_ROWS #%lu y=%u rows=%u width=%u pitch=%u\n",
                (unsigned long)s_diag_blit_rows_count, (unsigned)y,
                (unsigned)rows, (unsigned)width, (unsigned)pitch);
      }

      *s_status_mem = STDOOM_BUS_BYTE_WORD(STDOOM_STATUS_BUSY);
      stdoom_store_chunky_rows(y, rows, width, pitch, buf, buf_bytes);
      *s_status_mem = STDOOM_BUS_BYTE_WORD(STDOOM_STATUS_DONE);
      stdoom_send_response(random_token);
      break;
    }
    case CMD_STDOOM_C2P: {
      /* Rect parameters are optional (added in Milestone 3).
       * d3 = (rx<<16)|ry, d4 = (rw<<16)|rh.  payload16 indices: d3.low at
       * [0], d3.high at [1], d4.low at [2], d4.high at [3] (RP2040 reads the
       * big-endian m68k words in memory order).  When absent, use full frame.
       * payload_size includes the 4-byte token, so >= 12 means d3+d4 present. */
      uint16_t rx = 0;
      uint16_t ry = 0;
      uint16_t rw = s_frame_width;
      uint16_t rh = s_frame_height;
      /* Magnify factor (Milestone 7) carried in the spare low nibble of rx,
       * which is otherwise forced to a multiple of 16.  0/1 = no scaling
       * (1:1 rect, the M3 path); 2/4 = upscale the source rect to the screen
       * top-left.  Keeps sidecart_stubs.S unchanged (no extra register). */
      uint8_t scale = 0;

      if (proto->payload_size >= 12u) {
        uint32_t d3 = ((uint32_t)params16[1] << 16) | params16[0];
        uint32_t d4 = ((uint32_t)params16[3] << 16) | params16[2];
        uint16_t raw_rx = (uint16_t)(d3 >> 16);
        scale = (uint8_t)(raw_rx & 15u);
        rx = (uint16_t)(raw_rx & ~15u);
        ry = (uint16_t)(d3 & 0xFFFFu);
        rw = (uint16_t)(d4 >> 16);
        rh = (uint16_t)(d4 & 0xFFFFu);
        /* Clamp to frame bounds. */
        if (rx >= s_frame_width)  rx = 0;
        if (ry >= s_frame_height) ry = 0;
        if (rw == 0 || rx + rw > s_frame_width)  rw = s_frame_width - rx;
        if (rh == 0 || ry + rh > s_frame_height) rh = s_frame_height - ry;
      }

      s_diag_c2p_count++;
      if (stdoom_diag_should_log(s_diag_c2p_count)) {
        DPRINTF("stdoom_dispatch: C2P #%lu rx=%u ry=%u rw=%u rh=%u scale=%u\n",
                (unsigned long)s_diag_c2p_count,
                (unsigned)rx, (unsigned)ry, (unsigned)rw, (unsigned)rh,
                (unsigned)scale);
      }

      *s_status_mem = STDOOM_BUS_BYTE_WORD(STDOOM_STATUS_BUSY);
      if (scale > 1u) {
        stdoom_pack_to_planar_scaled(rx, ry, rw, rh, scale);
      } else {
        stdoom_pack_to_planar_rect(rx, ry, rw, rh);
      }
      *s_status_mem = STDOOM_BUS_BYTE_WORD(STDOOM_STATUS_DONE);
      stdoom_send_response(random_token);
      break;
    }
    case CMD_STDOOM_SET_PALETTE: {
      /* Upload the active 768-byte DOOM palette (256xRGB). Same payload layout
       * as SET_MAP: [4B token][12B reserved][data]. Then rebuild the 16 ST
       * colours + reduction LUT for the current mode and publish the colours. */
      uint32_t pal_bytes;
      const uint8_t *pal_buf;

      if (proto->payload_size < 16u) {
        DPRINTF("stdoom_dispatch: short SET_PALETTE payload\n");
        return;
      }
      pal_bytes = (uint32_t)proto->payload_size - 16u;
      if (pal_bytes > 768u) {
        pal_bytes = 768u;
      }
      pal_buf = (const uint8_t *)&params16[6];

      DPRINTF("stdoom_dispatch: SET_PALETTE bytes=%lu mode=%d\n",
              (unsigned long)pal_bytes, s_render_mode);

      *s_status_mem = STDOOM_BUS_BYTE_WORD(STDOOM_STATUS_BUSY);
      if ((pal_bytes & 1u) == 0u) {
        stdoom_copy_swap_bytes(s_palette_rgb, pal_buf, pal_bytes);
      } else {
        uint32_t even_bytes = pal_bytes & ~1u;
        stdoom_copy_swap_bytes(s_palette_rgb, pal_buf, even_bytes);
        s_palette_rgb[even_bytes] = pal_buf[even_bytes];
      }
      stdoom_build_palette_and_lut();
      stdoom_write_stcolors();
      *s_status_mem = STDOOM_BUS_BYTE_WORD(STDOOM_STATUS_DONE);
      stdoom_send_response(random_token);
      break;
    }
    case CMD_STDOOM_SET_MODE: {
      /* d3 = render mode (0..STDOOM_MODE_COUNT-1). Rebuild from the stored
       * palette and republish the chosen 16 ST colours. */
      uint32_t d3;
      int mode;

      if (proto->payload_size < 8u) {
        DPRINTF("stdoom_dispatch: short SET_MODE payload\n");
        return;
      }
      d3 = ((uint32_t)params16[1] << 16) | params16[0];
      mode = (int)(d3 & 0xFFFFu);
      if (mode < 0 || mode >= STDOOM_MODE_COUNT) {
        mode = STDOOM_MODE_NEAREST;
      }

      DPRINTF("stdoom_dispatch: SET_MODE %d\n", mode);

      *s_status_mem = STDOOM_BUS_BYTE_WORD(STDOOM_STATUS_BUSY);
      s_render_mode = mode;
      stdoom_build_palette_and_lut();
      stdoom_write_stcolors();
      *s_status_mem = STDOOM_BUS_BYTE_WORD(STDOOM_STATUS_DONE);
      stdoom_send_response(random_token);
      break;
    }
    case CMD_STDOOM_SET_PALGEN: {
      /* d3 = palette source (STDOOM_PALGEN_SUBSET / STDOOM_PALGEN_GENERATED).
       * Rebuild from the stored palette and republish the 16 ST colours. */
      uint32_t d3;
      int gen;

      if (proto->payload_size < 8u) {
        DPRINTF("stdoom_dispatch: short SET_PALGEN payload\n");
        return;
      }
      d3 = ((uint32_t)params16[1] << 16) | params16[0];
      gen = (int)(d3 & 0xFFFFu);
      if (gen != STDOOM_PALGEN_SUBSET && gen != STDOOM_PALGEN_GENERATED) {
        gen = STDOOM_PALGEN_GENERATED;
      }

      DPRINTF("stdoom_dispatch: SET_PALGEN %d\n", gen);

      *s_status_mem = STDOOM_BUS_BYTE_WORD(STDOOM_STATUS_BUSY);
      s_palette_gen = gen;
      stdoom_build_palette_and_lut();
      stdoom_write_stcolors();
      *s_status_mem = STDOOM_BUS_BYTE_WORD(STDOOM_STATUS_DONE);
      stdoom_send_response(random_token);
      break;
    }
    default:
      DPRINTF("stdoom_dispatch: ignoring command 0x%04X\n", proto->command_id);
      break;
  }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void stdoom_worker_init(void) {
  s_rom_base = (uint32_t)&__rom_in_ram_start__;
  s_token_addr = s_rom_base + STDOOM_RANDOM_TOKEN_OFFSET;
  s_token_seed_addr = s_rom_base + STDOOM_RANDOM_TOKEN_SEED_OFFSET;
  s_result_mem = (volatile char *)(s_rom_base + STDOOM_RESULT_OFFSET);
  s_status_mem = (volatile uint16_t *)(s_rom_base + STDOOM_STATUS_OFFSET);
  s_ready_mem = (volatile uint16_t *)(s_rom_base + STDOOM_READY_OFFSET);
  s_planar_mem = (volatile uint16_t *)(s_rom_base + STDOOM_PLANAR0_OFFSET);
  s_stcolors_mem = (volatile uint16_t *)(s_rom_base + STDOOM_STCOLORS_OFFSET);

  *s_status_mem = STDOOM_BUS_BYTE_WORD(STDOOM_STATUS_IDLE);
  *s_ready_mem = 0;
  stdoom_init_frame_state(STDOOM_FRAME_WIDTH, STDOOM_FRAME_HEIGHT);

  /* Seed the random token with a guaranteed non-zero value (see
   * stdoom_next_seed: rand() would yield 0 here without an RTC). */
  TPROTO_SET_RANDOM_TOKEN(s_token_seed_addr, stdoom_next_seed());

  /* Drain anything the protocol parser has already latched before arming. */
  TransmissionProtocol drain;
  for (int i = 0; i < 50; i++) {
    while (stdoom_consume_protocol(&drain)) {
      DPRINTF("stdoom_worker_init: draining startup cmd 0x%04X payload=%u\n",
              drain.command_id, (unsigned)drain.payload_size);
    }
    sleep_ms(1);
  }

  /* Publish the ready magic now that the worker can answer commands. */
  *s_ready_mem = STDOOM_READY_WORD;
  s_dispatch_armed = true;

  DPRINTF("DOOM Accelerator ready. PING=0x%02X\n", CMD_STDOOM_PING);
}

/* TEMPORARY: publish the diagnostic counters into spare shared-memory slots so
 * the ST can display them. Offsets are above the result-buffer-free region and
 * unused by the protocol. ST reads at ROM4_ADDR + offset. */
#define STDOOM_DBG_ROM3_IRQ_OFFSET 0xF010
#define STDOOM_DBG_CMD_OFFSET      0xF014
#define STDOOM_DBG_CHK_ERR_OFFSET  0xF018
#define STDOOM_DBG_LAST_CMD_OFFSET 0xF01C

void __not_in_flash_func(stdoom_worker_loop)(void) {
  if (stdoom_consume_protocol(&s_loop_proto)) {
    if (!s_dispatch_armed) {
      DPRINTF("stdoom_worker_loop: dropping pre-arm cmd 0x%04X\n",
              s_loop_proto.command_id);
    } else {
      stdoom_dispatch_command(&s_loop_proto);
    }
  }

  /* Publish diagnostic counters every iteration. */
  *(volatile uint32_t *)(s_rom_base + STDOOM_DBG_ROM3_IRQ_OFFSET) =
      stdoom_dbg_rom3_irq;
  *(volatile uint32_t *)(s_rom_base + STDOOM_DBG_CMD_OFFSET) = stdoom_dbg_cmd;
  *(volatile uint32_t *)(s_rom_base + STDOOM_DBG_CHK_ERR_OFFSET) =
      stdoom_dbg_chk_err;
  *(volatile uint32_t *)(s_rom_base + STDOOM_DBG_LAST_CMD_OFFSET) =
      (uint32_t)stdoom_dbg_last_cmd;
}
