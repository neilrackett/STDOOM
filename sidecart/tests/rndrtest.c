/*
 * rndrtest.c - SidecarTridge DOOM Accelerator render-mode (M4) test.
 *
 * Uploads a 768-byte (256xRGB) test palette and a 320x200 chunky image showing
 * a 16x16 grid of all 256 palette indices, then renders it through the RP2040.
 * Pressing any key cycles the render mode (NEAREST -> 2x2 BAYER -> 4x4 BAYER ->
 * GREYSCALE -> GREY 2x2 BAYER -> GREY 4x4 BAYER), which re-derives the 16 ST
 * colours on the firmware; the test reads them back, loads the hardware palette
 * registers, re-runs C2P and re-displays. ESC quits.
 *
 * This exercises the full M4 path (SET_PALETTE + readback + SET_MODE + all
 * render modes) on a known 256-colour image, in isolation, before relying on it
 * in STDOOM.
 *
 * Build (from repo root):
 *   make -C sidecart rndrtest
 */
#include <mint/osbind.h>
#include <stdio.h>
#include <string.h>

#include "sidecart_md.h"

#define CELL_W 20u  /* 16 columns x 20 px = 320 */
#define CELL_H 12u  /* rows are scaled to fill 200 (see fill_grid_frame)        */

static const char *s_mode_names[STDOOM_MODE_COUNT] = {
    "NEAREST", "2x2 BAYER", "4x4 BAYER", "HALFTONE"};

static const char *s_palgen_names[2] = {"FIXED", "GENERATED"};

/* Palette source (STDOOM_PALGEN_*); toggled with 'g'. Matches the firmware
 * default (generated). */
static int s_palgen = STDOOM_PALGEN_GENERATED;

/* VT52 console escapes (the TOS console driver interprets these via printf):
 *   HOME   — move cursor to top-left
 *   CLREOL — clear from cursor to end of line
 * Used to keep the per-mode status text anchored at the top-left over the grid
 * image, overwriting in place rather than scrolling the image up. */
#define VT52_HOME "\033H"
#define VT52_CLREOL "\033K"

static unsigned char  s_palette[768];
static unsigned char  s_chunky[STDOOM_CHUNKY_SIZE];
static unsigned char  s_saved_screen[STDOOM_PLANAR_SIZE];
static unsigned short s_saved_palette[16];

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

/* A synthetic but varied 256-colour palette: a smooth 2D R/G gradient over the
 * 16x16 grid with a diagonal blue component, so ordered dithering between the
 * chosen ST colours is clearly visible and greyscale shows a full luma range. */
static void fill_test_palette(unsigned char *pal)
{
    int i;
    for (i = 0; i < 256; i++) {
        int col = i & 15;
        int row = i >> 4;
        pal[i * 3 + 0] = (unsigned char)(col * 17);             /* R: columns */
        pal[i * 3 + 1] = (unsigned char)(row * 17);             /* G: rows    */
        pal[i * 3 + 2] = (unsigned char)(((col + row) * 255) / 30); /* B: diag */
    }
}

/* 320x200 grid: each pixel maps to one of the 256 indices (cy*16+cx). */
static void fill_grid_frame(unsigned char *frame)
{
    unsigned short x, y;
    for (y = 0; y < STDOOM_FRAME_HEIGHT; y++) {
        unsigned short cy = (unsigned short)(((unsigned long)y * 16u) / STDOOM_FRAME_HEIGHT);
        for (x = 0; x < STDOOM_FRAME_WIDTH; x++) {
            unsigned short cx = (unsigned short)(x / CELL_W);
            if (cx > 15u) cx = 15u;
            frame[(unsigned long)y * STDOOM_FRAME_WIDTH + x] =
                (unsigned char)(cy * 16u + cx);
        }
    }
}

static int blit_full_frame(const unsigned char *chunky)
{
    unsigned short y;
    for (y = 0; y < STDOOM_FRAME_HEIGHT; y += 6) {
        unsigned short rows = STDOOM_FRAME_HEIGHT - y;
        if (rows > 6) rows = 6;
        if (sidecart_md_blit_rows(y, rows, STDOOM_FRAME_WIDTH, STDOOM_FRAME_WIDTH,
                                  chunky + (unsigned long)y * STDOOM_FRAME_WIDTH) != 0)
            return -1;
    }
    return 0;
}

/* Plain CPU longword copy of planar slot 0 to the ST screen (speed irrelevant
 * for a test; avoids the blitter register dance). */
static void copy_planar_to_screen(unsigned char *screen)
{
    const unsigned long *src = (const unsigned long *)STDOOM_PLANAR0_ADDR;
    unsigned long *dst = (unsigned long *)screen;
    unsigned long longs = STDOOM_PLANAR_SIZE / 4u;
    while (longs--)
        *dst++ = *src++;
}

static void print_st_colors(const unsigned short *c)
{
    int i;
    printf("ST colours:" VT52_CLREOL "\n ");
    for (i = 0; i < 16; i++) {
        if (i == 8) printf(VT52_CLREOL "\n ");
        printf(" %03X", (unsigned)(c[i] & 0x0FFF));
    }
    printf(VT52_CLREOL "\n");
}

static void restore_display(unsigned char *screen)
{
    memcpy(screen, s_saved_screen, STDOOM_PLANAR_SIZE);
    install_palette(s_saved_palette);
}

/* Render the grid in the given mode: SET_MODE, read back + install the 16 ST
 * colours, C2P, copy to screen. Returns 0 on success. */
static int render_mode(int mode, unsigned char *screen)
{
    unsigned short stcolors[16];

    if (sidecart_md_set_mode(mode) != 0) {
        printf("SET_MODE %d failed\n", mode);
        return -1;
    }
    sidecart_md_get_st_colors(stcolors);
    if (sidecart_md_c2p() != 0) {
        printf("C2P failed\n");
        return -1;
    }
    install_palette(stcolors);
    copy_planar_to_screen(screen);

    /* Status text anchored at top-left (VT52 home), each line cleared to EOL so
     * a shorter mode name leaves no residue. The block is only a few lines, well
     * above the bottom of the screen, so it overwrites the grid in place and
     * never scrolls the image when cycling modes. */
    printf(VT52_HOME "RENDER: %s   PALETTE: %s" VT52_CLREOL "\n",
           s_mode_names[mode], s_palgen_names[s_palgen]);
    print_st_colors(stcolors);
    printf("Key=mode  g=palette  ESC=quit" VT52_CLREOL "\n");
    return 0;
}

int main(void)
{
    unsigned char *screen;
    long key;

    if (Getrez() != 0) {
        printf("RNDRTEST requires low resolution.\n");
        return 1;
    }

    screen = (unsigned char *)Physbase();
    memcpy(s_saved_screen, screen, STDOOM_PLANAR_SIZE);
    save_palette(s_saved_palette);

    fill_test_palette(s_palette);
    fill_grid_frame(s_chunky);

    printf("DOOM Accelerator render-mode (M4) test\n");
    printf("256-colour 16x16 grid.\n");
    printf("Key cycles mode; 'g' toggles palette; ESC quits.\n\n");

    for (;;) {
        int stage = 0, ping_rc = -2, detected;
        unsigned char ready = 0;
        unsigned long seed = 0;
        int init_rc, pal_rc, blit_rc, mode;

        key = Cconin();
        if ((key & 0xFF) == 27) break;

        detected = sidecart_md_detect_verbose(&stage, &ready, &seed, &ping_rc);
        printf("stage=%d ping=%d det=%d\n", stage, ping_rc, detected);
        if (!detected) {
            printf("Not detected. Press key to retry; ESC quits.\n\n");
            continue;
        }

        init_rc = sidecart_md_init(STDOOM_FRAME_WIDTH, STDOOM_FRAME_HEIGHT);
        pal_rc = (init_rc == 0) ? sidecart_md_set_palette(s_palette) : -1;
        blit_rc = (pal_rc == 0) ? blit_full_frame(s_chunky) : -1;
        printf("INIT=%d SET_PALETTE=%d BLIT=%d\n", init_rc, pal_rc, blit_rc);
        if (init_rc != 0 || pal_rc != 0 || blit_rc != 0) {
            printf("Aborted. Press key to retry; ESC quits.\n\n");
            continue;
        }

        /* Cycle modes on each keypress; 'g' toggles palette source (re-renders
         * the same mode for an A/B comparison); ESC exits. */
        mode = STDOOM_MODE_NEAREST;
        for (;;) {
            unsigned char ch;
            if (render_mode(mode, screen) != 0)
                break;
            key = Cconin();
            ch = (unsigned char)(key & 0xFF);
            if (ch == 27) {
                restore_display(screen);
                return 0;
            }
            if (ch == 'g' || ch == 'G') {
                s_palgen = (s_palgen == STDOOM_PALGEN_GENERATED)
                               ? STDOOM_PALGEN_SUBSET
                               : STDOOM_PALGEN_GENERATED;
                sidecart_md_set_palgen(s_palgen);
                continue; /* re-render the same mode with the new palette */
            }
            mode = (mode + 1) % STDOOM_MODE_COUNT;
        }

        printf("Press key to restart; ESC quits.\n\n");
    }

    restore_display(screen);
    return 0;
}
