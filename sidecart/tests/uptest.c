/*
 * uptest.c - SidecarTridge DOOM Accelerator ST->cart UPLOAD benchmark.
 *
 * The accelerator is upload-bound: the 68000 must push the 320x200 8bpp chunky
 * frame (64000 bytes) over the cartridge bus every frame, and the bus transport
 * (sidecart_stubs.S) moves ~2 bytes per ROM3 read (~1us/byte on an 8MHz 68000).
 * This standalone test measures, on three real Doom screenshots, how much the
 * upload can be reduced WITHOUT touching the firmware, protocol, or STDOOM:
 *
 *   E1  baseline full-frame upload + chunk-size sweep (per-command overhead)
 *   E2  PackBits RLE: encode cost + compressed size + compressed upload time
 *   E3  dirty-row delta between consecutive frames (compare cost + upload time)
 *
 * Bus transfer time is purely a function of byte count (the RP2040 just watches
 * addresses; the ST drives the whole tst.b loop), so a compressed / partial byte
 * stream sent through CMD_STDOOM_BLIT_ROWS is timed accurately even though the
 * firmware stores it as raw chunky. RP-side RLE/dirty decode is the trivial
 * follow-up IF these numbers justify it - deliberately not done here.
 *
 * Build (from repo root):
 *   make -C sidecart uptest
 */
#include <mint/osbind.h>
#include <stdio.h>
#include <string.h>

#include "sidecart_md.h"
#include "doom_frames.h"

/* Each timed upload is repeated REPEATS times so the coarse 200Hz (5ms) system
 * clock yields a stable total. */
#define REPEATS 8

/* Chunk sizes for the E1 sweep, expressed in rows (x320 bytes/row). 6 rows =
 * 1920 bytes = the near-max payload STDOOM actually uses. */
static const unsigned short s_sweep_rows[] = {6, 3, 2, 1};
#define SWEEP_N (sizeof(s_sweep_rows) / sizeof(s_sweep_rows[0]))

/* VT52 console escapes (TOS console driver). */
#define VT52_HOME "\033H"
#define VT52_CLREOL "\033K"

/* PackBits worst case expands by ~1/128; pad generously. */
static unsigned char s_rle[DOOM_FRAME_BYTES + DOOM_FRAME_BYTES / 64 + 64];
static unsigned char s_dirty[DOOM_FRAME_BYTES];
static unsigned char s_saved_screen[STDOOM_PLANAR_SIZE];
static unsigned short s_saved_palette[16];

/* ─────────────────────────── display helpers ──────────────────────────── */

static void install_palette(const unsigned short *pal)
{
    long old_sp = Super(0L);
    volatile unsigned short *reg = (volatile unsigned short *)0xFF8240UL;
    short i;
    for (i = 0; i < 16; i++)
        *reg++ = *pal++;
    if (old_sp) Super((void *)old_sp);
}

static void save_palette(unsigned short *pal)
{
    long old_sp = Super(0L);
    volatile unsigned short *reg = (volatile unsigned short *)0xFF8240UL;
    short i;
    for (i = 0; i < 16; i++)
        *pal++ = *reg++;
    if (old_sp) Super((void *)old_sp);
}

static void copy_planar_to_screen(unsigned char *screen)
{
    const unsigned long *src = (const unsigned long *)STDOOM_PLANAR0_ADDR;
    unsigned long *dst = (unsigned long *)screen;
    unsigned long longs = STDOOM_PLANAR_SIZE / 4u;
    while (longs--)
        *dst++ = *src++;
}

/* ──────────────────────────── timing helpers ──────────────────────────── */

static unsigned long timer_ticks(void)
{
    long old_stack = Super(0L);
    volatile unsigned long *hz_200 = (volatile unsigned long *)0x4BAL;
    unsigned long ticks = *hz_200;
    if (old_stack != 0) Super((void *)old_stack);
    return ticks;
}

/* KB/s for `nbytes` sent `reps` times over `ticks` 200Hz ticks. */
static unsigned long kb_per_s(unsigned long nbytes, int reps, unsigned long ticks)
{
    unsigned long total;
    if (ticks == 0) return 0;
    total = nbytes * (unsigned long)reps;        /* <= 64000*8 = 512000 */
    return (total * 200UL / ticks) / 1024UL;     /* total*200 <= 1.024e8, safe */
}

/* ──────────────────────────── upload helpers ──────────────────────────── */

/*
 * Generic timed byte-stream uploader. Sends `nbytes` from `buf` in chunks of at
 * most `chunk_len` bytes via CMD_STDOOM_BLIT_ROWS. The chunk is sent as a single
 * (rows=1, width=chunk_len) command: byte_count = rows*width, so the bus moves
 * exactly chunk_len bytes regardless of where the firmware stores them. y=0 for
 * every chunk - the stored location is irrelevant; we only measure transfer
 * time, which depends solely on the byte count and the command count. Returns 0
 * on success, -1 on a transport timeout.
 */
static int upload_blob(const unsigned char *buf, unsigned long nbytes,
                       unsigned long chunk_len)
{
    unsigned long off = 0;
    while (off < nbytes) {
        unsigned long remaining = nbytes - off;
        unsigned short this_len =
            (unsigned short)((remaining < chunk_len) ? remaining : chunk_len);
        if (sidecart_md_blit_rows(0, 1, this_len, this_len, buf + off) != 0)
            return -1;
        off += this_len;
    }
    return 0;
}

/* Number of BLIT_ROWS commands upload_blob() issues for nbytes at chunk_len. */
static unsigned long command_count(unsigned long nbytes, unsigned long chunk_len)
{
    return (nbytes + chunk_len - 1) / chunk_len;
}

/* Faithful full-frame upload (real y/rows/width=320) used only for the visual
 * sanity render so the firmware stores the frame correctly. */
static int blit_full_frame(const unsigned char *chunky)
{
    unsigned short y;
    for (y = 0; y < STDOOM_FRAME_HEIGHT; y += 6) {
        unsigned short rows = STDOOM_FRAME_HEIGHT - y;
        if (rows > 6) rows = 6;
        if (sidecart_md_blit_rows(y, rows, STDOOM_FRAME_WIDTH, STDOOM_FRAME_WIDTH,
                                  chunky + (unsigned long)y * STDOOM_FRAME_WIDTH)
            != 0)
            return -1;
    }
    return 0;
}

/* ─────────────────────────────── RLE / diff ───────────────────────────── */

/*
 * PackBits-style RLE. Control byte:
 *   0x00..0x7F : (n+1) literal bytes follow      (1..128 literals)
 *   0x81..0xFF : repeat the next byte (257-n)x    (2..128 repeats)
 * Never expands by more than ~1/128. Returns encoded length.
 */
static unsigned long rle_encode(const unsigned char *src, unsigned long n,
                                unsigned char *dst)
{
    unsigned long i = 0, o = 0;
    while (i < n) {
        unsigned long run = 1;
        while (run < 128 && (i + run) < n && src[i + run] == src[i])
            run++;
        if (run >= 2) {
            dst[o++] = (unsigned char)(257 - run);
            dst[o++] = src[i];
            i += run;
        } else {
            unsigned long start = i;
            unsigned long lit = 0;
            while (lit < 128 && i < n) {
                if ((i + 1) < n && src[i + 1] == src[i])
                    break;                 /* a run starts at i+1; flush literals */
                i++;
                lit++;
            }
            dst[o++] = (unsigned char)(lit - 1);
            memcpy(dst + o, src + start, lit);
            o += lit;
        }
    }
    return o;
}

/*
 * Pack rows that differ between prev and cur into dst (contiguous changed rows).
 * Returns the changed-row count; *out_bytes = changed rows * 320.
 */
static int pack_dirty_rows(const unsigned char *prev, const unsigned char *cur,
                           unsigned char *dst, unsigned long *out_bytes)
{
    int changed = 0;
    unsigned short y;
    unsigned long o = 0;
    for (y = 0; y < STDOOM_FRAME_HEIGHT; y++) {
        unsigned long row = (unsigned long)y * STDOOM_FRAME_WIDTH;
        if (memcmp(prev + row, cur + row, STDOOM_FRAME_WIDTH) != 0) {
            memcpy(dst + o, cur + row, STDOOM_FRAME_WIDTH);
            o += STDOOM_FRAME_WIDTH;
            changed++;
        }
    }
    *out_bytes = o;
    return changed;
}

/* ─────────────────────────────── experiments ──────────────────────────── */

/* Time `reps` uploads of nbytes at chunk_len; return total 200Hz ticks, or
 * 0xFFFFFFFF on transport timeout. */
static unsigned long time_uploads(const unsigned char *buf, unsigned long nbytes,
                                  unsigned long chunk_len, int reps)
{
    unsigned long t0, t1;
    int r;
    t0 = timer_ticks();
    for (r = 0; r < reps; r++) {
        if (upload_blob(buf, nbytes, chunk_len) != 0)
            return 0xFFFFFFFFUL;
    }
    t1 = timer_ticks();
    return t1 - t0;
}

/* E1: baseline 64000-byte upload at 6/3/2/1-row chunks. */
static void exp_baseline(const unsigned char *frame)
{
    unsigned long ticks6 = 0, ticks1 = 0;
    unsigned int k;

    printf("E1 baseline upload (64000 B):\n");
    printf(" chunk  ms   KB/s  cmds\n");
    for (k = 0; k < SWEEP_N; k++) {
        unsigned long clen = (unsigned long)s_sweep_rows[k] * STDOOM_FRAME_WIDTH;
        unsigned long ticks = time_uploads(frame, DOOM_FRAME_BYTES, clen, REPEATS);
        unsigned long ms, kbs, cmds;
        if (ticks == 0xFFFFFFFFUL) {
            printf(" %2dr   TIMEOUT\n", (int)s_sweep_rows[k]);
            continue;
        }
        ms = ticks * 5UL / REPEATS;
        kbs = kb_per_s(DOOM_FRAME_BYTES, REPEATS, ticks);
        cmds = command_count(DOOM_FRAME_BYTES, clen);
        printf(" %2dr  %4lu  %4lu  %4lu\n", (int)s_sweep_rows[k], ms, kbs, cmds);
        if (s_sweep_rows[k] == 6) ticks6 = ticks;
        if (s_sweep_rows[k] == 1) ticks1 = ticks;
    }
    /* Per-command overhead estimate from the 6-row vs 1-row delta (same bytes,
     * 166 more commands per upload at 1 row). */
    if (ticks6 && ticks1 && ticks1 > ticks6) {
        unsigned long dcmd = command_count(DOOM_FRAME_BYTES, STDOOM_FRAME_WIDTH)
                           - command_count(DOOM_FRAME_BYTES, 6UL * STDOOM_FRAME_WIDTH);
        unsigned long us = (ticks1 - ticks6) * 5000UL / ((unsigned long)REPEATS * dcmd);
        printf(" ~per-cmd overhead: %lu us\n", us);
    }
}

/* E2: PackBits RLE - encode cost, ratio, compressed upload. */
static void exp_rle(const unsigned char *frame)
{
    unsigned long t0, t1, enc_ticks, up_ticks;
    unsigned long rlen, ms_enc, ms_up, kbs;
    int r;

    t0 = timer_ticks();
    for (r = 0; r < REPEATS; r++)
        rlen = rle_encode(frame, DOOM_FRAME_BYTES, s_rle);
    t1 = timer_ticks();
    enc_ticks = t1 - t0;
    ms_enc = enc_ticks * 5UL / REPEATS;

    up_ticks = time_uploads(s_rle, rlen, 6UL * STDOOM_FRAME_WIDTH, REPEATS);
    printf("E2 RLE: %lu B (%lu%%)\n", rlen, rlen * 100UL / DOOM_FRAME_BYTES);
    if (up_ticks == 0xFFFFFFFFUL) {
        printf(" upload TIMEOUT\n");
        return;
    }
    ms_up = up_ticks * 5UL / REPEATS;
    kbs = kb_per_s(rlen, REPEATS, up_ticks);
    printf(" encode %lu ms, upload %lu ms (%lu KB/s)\n", ms_enc, ms_up, kbs);
    printf(" net enc+up: %lu ms\n", ms_enc + ms_up);
}

/* E3: dirty-row delta from `prev` to `cur`. */
static void exp_dirty(int from, int to,
                      const unsigned char *prev, const unsigned char *cur)
{
    unsigned long t0, t1, cmp_ticks, up_ticks;
    unsigned long bytes = 0, ms_cmp, ms_up, kbs;
    int changed, r;

    t0 = timer_ticks();
    for (r = 0; r < REPEATS; r++)
        changed = pack_dirty_rows(prev, cur, s_dirty, &bytes);
    t1 = timer_ticks();
    cmp_ticks = t1 - t0;
    ms_cmp = cmp_ticks * 5UL / REPEATS;

    printf("E3 dirty %d->%d: %d rows, %lu B (%lu%%)\n",
           from, to, changed, bytes, bytes * 100UL / DOOM_FRAME_BYTES);
    if (bytes == 0) {
        printf(" (identical frames)\n");
        return;
    }
    up_ticks = time_uploads(s_dirty, bytes, 6UL * STDOOM_FRAME_WIDTH, REPEATS);
    if (up_ticks == 0xFFFFFFFFUL) {
        printf(" upload TIMEOUT\n");
        return;
    }
    ms_up = up_ticks * 5UL / REPEATS;
    kbs = kb_per_s(bytes, REPEATS, up_ticks);
    printf(" compare %lu ms, upload %lu ms (%lu KB/s)\n", ms_cmp, ms_up, kbs);
    printf(" net cmp+up: %lu ms\n", ms_cmp + ms_up);
}

/* ───────────────────────────────── main ───────────────────────────────── */

/* Detect + INIT + SET_PALETTE(frame 0) + warm up the one-time settle spin.
 * Returns 0 on success. */
static int accel_setup(void)
{
    int stage = 0, ping_rc = -2, detected;
    unsigned char ready = 0;
    unsigned long seed = 0;

    detected = sidecart_md_detect_verbose(&stage, &ready, &seed, &ping_rc);
    printf("detect: stage=%d ping=%d det=%d\n", stage, ping_rc, detected);
    if (!detected) {
        printf("DOOM Accelerator not detected.\n");
        return -1;
    }
    if (sidecart_md_init(STDOOM_FRAME_WIDTH, STDOOM_FRAME_HEIGHT) != 0) {
        printf("INIT failed\n");
        return -1;
    }
    if (sidecart_md_set_palette(doom_palettes[0]) != 0) {
        printf("SET_PALETTE failed\n");
        return -1;
    }
    /* Warm-up upload (untimed) so the one-time settle spin in the first
     * blit_rows doesn't inflate E1. */
    upload_blob(doom_frames[0], DOOM_FRAME_BYTES, 6UL * STDOOM_FRAME_WIDTH);
    return 0;
}

/* Visual sanity: render frame 0 through the accelerator (NEAREST) so the user
 * can confirm the embedded data and pipeline are correct. */
static void visual_sanity(unsigned char *screen)
{
    unsigned short stcolors[16];
    if (blit_full_frame(doom_frames[0]) != 0) return;
    if (sidecart_md_set_mode(STDOOM_MODE_NEAREST) != 0) return;
    sidecart_md_get_st_colors(stcolors);
    if (sidecart_md_c2p() != 0) return;
    install_palette(stcolors);
    copy_planar_to_screen(screen);
    printf(VT52_HOME "frame 0 (NEAREST). Key=run benchmark" VT52_CLREOL "\n");
}

int main(void)
{
    unsigned char *screen;
    int f;

    if (Getrez() != 0) {
        printf("UPTEST requires low resolution.\n");
        return 1;
    }
    screen = (unsigned char *)Physbase();
    memcpy(s_saved_screen, screen, STDOOM_PLANAR_SIZE);
    save_palette(s_saved_palette);

    printf("DOOM Accelerator UPLOAD benchmark\n");
    printf("%d real frames, %d reps.\n\n", DOOM_FRAME_COUNT, REPEATS);

    if (accel_setup() != 0) {
        printf("Press a key to quit.\n");
        Cconin();
        return 1;
    }

    visual_sanity(screen);
    Cconin();
    /* Leave the displayed frame up; print results over it, top-left. */
    printf(VT52_HOME);

    for (f = 0; f < DOOM_FRAME_COUNT; f++) {
        printf("=== FRAME %d ===" VT52_CLREOL "\n", f);
        exp_baseline(doom_frames[f]);
        exp_rle(doom_frames[f]);
        printf("Key=next\n");
        if ((Cconin() & 0xFF) == 27) goto done;
    }

    printf("=== DIRTY-ROW DELTAS ===\n");
    for (f = 1; f < DOOM_FRAME_COUNT; f++)
        exp_dirty(f - 1, f, doom_frames[f - 1], doom_frames[f]);
    printf("Done. Key=quit\n");
    Cconin();

done:
    memcpy(screen, s_saved_screen, STDOOM_PLANAR_SIZE);
    install_palette(s_saved_palette);
    return 0;
}
