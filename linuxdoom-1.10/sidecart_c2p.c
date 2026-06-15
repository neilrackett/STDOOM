/*
 * sidecart_c2p.c — SidecarTridge-accelerated C2P for STDOOM (Milestones 3-4).
 *
 * Holds everything sidecart-specific: the RP2040 C2P offload screen/statusbar
 * drawfuncs, the planar->screen blitter copy, the palette hook, and the runtime
 * render-mode control.  When the accelerator is detected, sidecart_c2p_install()
 * overrides the software function pointers (declared in atari_c2p.h) with the
 * accelerated versions.
 *
 * Since M4 the RP2040 owns colour reduction: the host uploads the raw 768-byte
 * DOOM palette (SET_PALETTE) and the firmware returns the 16 chosen ST colours.
 *
 * atari_c2p.c is the pure software fallback and knows nothing about the
 * sidecart; this mirrors how a clean SDL XBIOS driver swaps implementations.
 */

#include <mint/osbind.h>
#include <stdio.h>

#include "doomdef.h"
#include "doomstat.h"
#include "m_argv.h"
#include "atari_c2p.h"
#include "sidecart_c2p.h"
#include "sidecart_md.h"

/* inhelpscreens is declared locally in d_main.c, not in a shared header. */
extern boolean inhelpscreens;

/* True while a HUD message is on screen (hu_stuff.c); used to repaint the top
 * message line in the shrunk-view partial path, which otherwise skips it. */
extern boolean HU_MessageActive(void);

/* Rows of the top HUD message line to repaint in the partial path. The HU
 * message font is a single line (~8px); 16 rows covers it with margin. */
#define MD_MSG_STRIP_ROWS 16

/* DOOM Accelerator active flag (1 = SidecarTridge firmware detected). */
int c2p_md_active = 0;

/* Current render mode (persisted to doomrc.cfg via m_misc.c defaults[]). */
int render_mode = STDOOM_DEFAULT_RENDER_MODE;

/* Current palette source (persisted to doomrc.cfg via m_misc.c defaults[]). */
int palette_gen = STDOOM_DEFAULT_PALETTE_GEN;

/* HUD labels for each render mode, indexed by STDOOM_MODE_*. */
static char *const s_render_mode_names[STDOOM_MODE_COUNT] = {
    "RENDER: NEAREST", "RENDER: 2x2 BAYER", "RENDER: 4x4 BAYER",
    "RENDER: GREYSCALE", "RENDER: GREY 2x2 BAYER", "RENDER: GREY 4x4 BAYER"};

/* HUD labels for each palette source, indexed by STDOOM_PALGEN_*. */
static char *const s_palette_gen_names[2] = {
    "PALETTE: FIXED", "PALETTE: GENERATED"};

/*
 * STE blitter registers (fast copy path from ROM4 slot 0 to ST screen RAM).
 *
 * The first 16 words at BLT_BASE ($FFFF8A00) are the halftone RAM; the actual
 * registers start at +$20. Offsets below match the Atari STE hardware reference
 * and md-sprites-demo/main.s.  Earlier +$02..+$1E offsets wrote into halftone
 * RAM so the blitter control was never set and the blit silently did nothing.
 */
#define BLT_BASE 0xFFFF8A00UL
#define BLT_SRC_INC_X (*(volatile unsigned short *)(BLT_BASE + 0x20))
#define BLT_SRC_INC_Y (*(volatile unsigned short *)(BLT_BASE + 0x22))
#define BLT_SRC_ADDR (*(volatile unsigned long *)(BLT_BASE + 0x24))
#define BLT_END_MASK1 (*(volatile unsigned short *)(BLT_BASE + 0x28))
#define BLT_END_MASK2 (*(volatile unsigned short *)(BLT_BASE + 0x2A))
#define BLT_END_MASK3 (*(volatile unsigned short *)(BLT_BASE + 0x2C))
#define BLT_DST_INC_X (*(volatile unsigned short *)(BLT_BASE + 0x2E))
#define BLT_DST_INC_Y (*(volatile unsigned short *)(BLT_BASE + 0x30))
#define BLT_DST_ADDR (*(volatile unsigned long *)(BLT_BASE + 0x32))
#define BLT_X_COUNT (*(volatile unsigned short *)(BLT_BASE + 0x36))
#define BLT_Y_COUNT (*(volatile unsigned short *)(BLT_BASE + 0x38))
#define BLT_HOP (*(volatile unsigned char *)(BLT_BASE + 0x3A))
#define BLT_OP (*(volatile unsigned char *)(BLT_BASE + 0x3B))
#define BLT_CTRL (*(volatile unsigned char *)(BLT_BASE + 0x3C))

#define BLT_CTRL_BUSY 0x80u
#define BLT_CTRL_HOG 0x40u
#define BLT_HOP_SOURCE 0x02u
#define BLT_OP_COPY 0x03u

/* Each planar row: 320px / 16px·word × 4 planes × 2 B/word = 160 bytes. */
#define PLANAR_ROW_BYTES 160u
#define PLANAR_WORDS_PER_ROW 80u

static void (*s_sw_set_doom_palette)(const unsigned char *colors);
static int s_blitter_present = -1;

/*
 * Set to 1 after c2p_screen_md completes a full-frame offload (which covers
 * the status bar rows).  The next c2p_statusbar_md call consumes and clears
 * this flag, skipping redundant work.
 */
static int c2p_md_frame_offloaded = 0;

static int blitter_present(void)
{
    if (s_blitter_present < 0)
    {
        long mode = Blitmode(-1);
        s_blitter_present = (mode & 1) != 0;
    }
    return s_blitter_present;
}

/* Run the software palette path (sets the 16 ST hardware registers directly). */
static void sidecart_sw_palette(const unsigned char *colors)
{
    if (s_sw_set_doom_palette)
        s_sw_set_doom_palette(colors);
    else
        set_st_doom_palette(colors);
}

/* Disable the accelerator and restore the software function pointers. */
static void sidecart_c2p_fallback(void)
{
    c2p_md_active = 0;
    c2p_screen_drawfunc = c2p_screen_lorez;
    c2p_statusbar_drawfunc = c2p_statusbar_lorez;
    set_doom_palette = s_sw_set_doom_palette ? s_sw_set_doom_palette
                                             : set_st_doom_palette;
}

/* Palette hook (M4): the RP2040 owns colour reduction.  Upload the raw 768-byte
 * DOOM palette, read back the 16 ST colours the firmware chose for the current
 * mode, and load the hardware registers from them.  On any failure, drop to the
 * software path. */
static void set_st_doom_palette_md(const unsigned char *colors)
{
    unsigned short stcolors[16];

    if (!c2p_md_active)
    {
        sidecart_sw_palette(colors);
        return;
    }

    if (sidecart_md_set_palette(colors) != 0)
    {
        printf("MD SET_PALETTE failed; SW C2P\n");
        sidecart_c2p_fallback();
        sidecart_sw_palette(colors);
        return;
    }

    sidecart_md_get_st_colors(stcolors);
    install_palette(stcolors);
}

/*
 * Copy a word-aligned planar rectangle from ROM4 slot 0 to ST screen RAM.
 * x must be a multiple of 16.
 *
 * Full-width rects (y_skip_bytes==0): linear blitter X_COUNT=total,Y_COUNT=1
 *   — avoids the display tearing that a per-row blit (X_COUNT=80,Y_COUNT=h)
 *   causes when reading from ROM4 cartridge space.
 * True sub-rects (y_skip_bytes>0): CPU row copy; the blitter with INC_Y!=0
 *   reads from ROM4 unreliably (produces stripe corruption on STE).
 */
static void sidecart_md_copy_planar_to_screen(unsigned long src_addr,
                                              unsigned char *dst,
                                              unsigned short x,
                                              unsigned short y,
                                              unsigned short w,
                                              unsigned short h)
{
    unsigned short words_per_subrow = (unsigned short)((w / 16u) * 4u);
    unsigned short y_skip_bytes = (unsigned short)((PLANAR_WORDS_PER_ROW - words_per_subrow) * 2u);
    unsigned long byte_offset = (unsigned long)y * PLANAR_ROW_BYTES + (unsigned long)(x / 16u) * 8u;

    if (blitter_present() && y_skip_bytes == 0)
    {
        /* Linear blit: full-width rect, source is contiguous. Use X_COUNT =
         * total words, Y_COUNT = 1 to avoid the display-scan tearing that a
         * per-row blit (X_COUNT=80, Y_COUNT=h) causes when reading from ROM4. */
        unsigned short total_words = (unsigned short)((unsigned long)words_per_subrow * h);
        short old_sr;
        __asm__ volatile(
            "move.w %%sr,%0\n\t"
            "ori.w  #0x0700,%%sr"
            : "=d"(old_sr) : : "cc");

        BLT_SRC_INC_X = 2;
        BLT_SRC_INC_Y = 0;
        BLT_SRC_ADDR = src_addr + byte_offset;
        BLT_END_MASK1 = 0xFFFFu;
        BLT_END_MASK2 = 0xFFFFu;
        BLT_END_MASK3 = 0xFFFFu;
        BLT_DST_INC_X = 2;
        BLT_DST_INC_Y = 0;
        BLT_DST_ADDR = (unsigned long)dst + byte_offset;
        BLT_X_COUNT = total_words;
        BLT_Y_COUNT = 1;
        BLT_HOP = BLT_HOP_SOURCE;
        BLT_OP = BLT_OP_COPY;
        BLT_CTRL = (unsigned char)(BLT_CTRL_HOG | BLT_CTRL_BUSY);
        while (BLT_CTRL & BLT_CTRL_BUSY)
        {
        }

        __asm__ volatile(
            "move.w %0,%%sr" : : "d"(old_sr) : "cc");
        return;
    }

    /* CPU fallback: row-by-row longword copy.  Used for true sub-rects
     * (y_skip_bytes>0) where blitter INC_Y!=0 corrupts ROM4 reads, and
     * on plain ST where there is no blitter. */
    {
        unsigned short subrow_longs = words_per_subrow / 2u;
        unsigned short skip_longs = y_skip_bytes / 4u;
        const unsigned long *src = (const unsigned long *)(src_addr + byte_offset);
        unsigned long *d = (unsigned long *)((unsigned long)dst + byte_offset);
        for (unsigned short row = 0; row < h; row++)
        {
            for (unsigned short i = 0; i < subrow_longs; i++)
                *d++ = *src++;
            src += skip_longs;
            d += skip_longs;
        }
    }
}

/* ── Upload helper ──────────────────────────────────────────────────────────
 * Upload chunky rows [y_start .. y_start+h) at full STDOOM_FRAME_WIDTH.
 * Returns 0 on success, -1 on any BLIT_ROWS failure.
 */
static int upload_rows(short y_start, short h, const unsigned char *in)
{
    for (short y = y_start; y < y_start + h; y += 6)
    {
        short rows = (y_start + h) - y;
        if (rows > 6)
            rows = 6;
        if (sidecart_md_blit_rows((unsigned short)y, (unsigned short)rows,
                                  STDOOM_FRAME_WIDTH, STDOOM_FRAME_WIDTH,
                                  in + (y * SCREENWIDTH)) != 0)
            return -1;
    }
    return 0;
}

/*
 * Accelerated full-screen drawfunc.
 *
 * When the player has shrunk the view (viewheight < SCREENHEIGHT) during
 * gameplay, only the active view rectangle is uploaded/C2P'd/blitted, and the
 * status-bar rows are left to c2p_statusbar_md.
 *
 * Full-screen gameplay, menus, splash, intermission and automap go through the
 * full 320x200 path.
 */
static void c2p_screen_md(unsigned char *out, const unsigned char *in)
{
    /* Tracks whether the previous frame rendered via the shrunk-view partial
     * path, so c2p_screen_md can force a border-redraw burst when re-entering
     * gameplay from a menu/automap/full-screen frame (see below). */
    static int s_prev_in_partial_branch = 0;

    c2p_md_frame_offloaded = 0;

    if (!c2p_md_active)
    {
        c2p_screen_lorez(out, in);
        return;
    }

    {
        boolean is_gameplay = (gamestate == GS_LEVEL) && !menuactive && !inhelpscreens && !automapactive;

        /* Only take the partial-view path when the view dimensions are valid.
         * Before R_ExecuteSetViewSize has run once (e.g. the first frame after
         * starting a new game when gametic is still 0, so R_RenderPlayerView is
         * skipped) these globals are 0; a 0-sized rect would leave the screen
         * un-updated. Fall through to the full-frame path in that case. */
        if (is_gameplay && viewheight > 0 && scaledviewwidth > 0 &&
            viewheight < SCREENHEIGHT)
        {
            /* Partial-view path: upload only the active gameplay rectangle. */
            unsigned short vx = (unsigned short)viewwindowx;
            unsigned short vy = (unsigned short)viewwindowy;
            unsigned short vw = (unsigned short)scaledviewwidth;
            unsigned short vh = (unsigned short)viewheight;
            /* M7 magnify factor: exactly 160px -> 2x, 80px -> 4x (the two view
             * sizes the software path zooms cleanly); any other width stays 1:1
             * (today's in-place partial path, with the GRNROCK border). */
            unsigned short scale = (vw == SCREENWIDTH / 2) ? 2
                                 : (vw == SCREENWIDTH / 4) ? 4 : 1;

            /* Border-frame detection.  When the view is shrunk
             * (scaledviewwidth < 320) Doom draws the GRNROCK border around the
             * view into screens[0], but only for a few frames after the view
             * size changes or a menu/automap is dismissed (its borderdrawcount=3
             * logic, d_main.c).  The partial path only pushes the 3D view rect,
             * so on those frames the freshly drawn border would never reach the
             * screen and stale menu pixels remain.  Force a full frame for 3
             * ticks whenever the border may have been redrawn — the view rect
             * changed, or we are re-entering gameplay after the screen was last
             * drawn by something else (menu, automap, splash, full-screen view).
             * Once it has settled, the border is static and the partial path is
             * correct. */
            static unsigned short s_pvx, s_pvy, s_pvw, s_pvh;
            static int s_border_full_frames;

            /* Magnified zoom (M7): upload only the small centred view (the same
             * rows the 1:1 partial path already uploads — no extra upload
             * cost), have the RP2040 upscale it to fill the 320 x (vh*scale)
             * play area, and copy it full-width via the linear-blit fast path.
             * No border burst is needed: the whole play area is overwritten.
             * The status bar is left to c2p_statusbar_md (c2p_md_frame_offloaded
             * stays 0). */
            if (scale > 1)
            {
                s_prev_in_partial_branch = 1;
                if (upload_rows((short)vy, (short)vh, in) != 0)
                {
                    c2p_screen_lorez(out, in);
                    return;
                }
                if (sidecart_md_c2p_scaled(vx, vy, vw, vh, scale) != 0)
                {
                    c2p_screen_lorez(out, in);
                    return;
                }
                sidecart_md_copy_planar_to_screen(
                    (unsigned long)STDOOM_PLANAR0_ADDR, out, 0, 0,
                    STDOOM_FRAME_WIDTH, (unsigned short)(vh * scale));
                return;
            }

            if (!s_prev_in_partial_branch ||
                vx != s_pvx || vy != s_pvy || vw != s_pvw || vh != s_pvh)
                s_border_full_frames = 3;
            s_pvx = vx; s_pvy = vy; s_pvw = vw; s_pvh = vh;
            s_prev_in_partial_branch = 1;

            if (s_border_full_frames > 0)
            {
                /* Fall through to the full-frame path below (covers the
                 * border and the status bar). */
                s_border_full_frames--;
            }
            else
            {
                /* The top HUD message line (y=0) sits above the 3D viewport and
                 * is otherwise never repainted in the partial path, so an active
                 * message (pickups, "RENDER:"/"PALETTE:") would never reach the
                 * screen. Push that thin top strip while a message is showing,
                 * plus a couple of frames after so the erase reaches the screen
                 * too. Skipped when the viewport already starts at the top. */
                static int s_msg_strip_frames;
                if (HU_MessageActive())
                    s_msg_strip_frames = 2;
                if (s_msg_strip_frames > 0)
                {
                    s_msg_strip_frames--;
                    if (vy > 0)
                    {
                        if (upload_rows(0, MD_MSG_STRIP_ROWS, in) != 0 ||
                            sidecart_md_c2p_rect(0, 0, STDOOM_FRAME_WIDTH,
                                                 MD_MSG_STRIP_ROWS) != 0)
                        {
                            c2p_screen_lorez(out, in);
                            return;
                        }
                        sidecart_md_copy_planar_to_screen(
                            (unsigned long)STDOOM_PLANAR0_ADDR, out, 0, 0,
                            STDOOM_FRAME_WIDTH, MD_MSG_STRIP_ROWS);
                    }
                }

                if (upload_rows((short)vy, (short)vh, in) != 0)
                {
                    c2p_screen_lorez(out, in);
                    return;
                }
                if (sidecart_md_c2p_rect(vx, vy, vw, vh) != 0)
                {
                    c2p_screen_lorez(out, in);
                    return;
                }
                sidecart_md_copy_planar_to_screen(
                    (unsigned long)STDOOM_PLANAR0_ADDR, out, vx, vy, vw, vh);
                /* Status bar is NOT covered — c2p_statusbar_md must handle it. */
                return;
            }
        }
        else
        {
            s_prev_in_partial_branch = 0;
        }
    }

    /* Full-frame path: menus, splash, full-screen gameplay, or a full-screen
     * (un-shrunk) gameplay view. */
    if (upload_rows(0, SCREENHEIGHT, in) != 0)
    {
        c2p_screen_lorez(out, in);
        return;
    }
    if (sidecart_md_c2p() != 0)
    {
        c2p_screen_lorez(out, in);
        return;
    }
    sidecart_md_copy_planar_to_screen((unsigned long)STDOOM_PLANAR0_ADDR, out,
                                      0, 0, STDOOM_FRAME_WIDTH, STDOOM_FRAME_HEIGHT);
    c2p_md_frame_offloaded = 1;
}

/*
 * Accelerated status-bar drawfunc.
 *
 * When the preceding c2p_screen_md offloaded a full frame (which already
 * covers the status-bar rows), consume the flag and return early.
 *
 * Otherwise (a shrunk/partial view is active, so c2p_screen_md didn't cover
 * the status-bar rows): upload the dirty rows at full width, C2P the dirty
 * rect, and blit just that rect to screen.
 */
static void c2p_statusbar_md(unsigned char *out, const unsigned char *in,
                             short y_begin, short y_end,
                             short x_begin, short x_end)
{
    if (c2p_md_frame_offloaded)
    {
        c2p_md_frame_offloaded = 0;
        return;
    }

    /* Guard against an inverted/empty dirty box.  The software c2p_statusbar
     * wrapper only rejects y_end<=0, then forces y_begin up to SCREENHEIGHT-32;
     * when the dirty box is entirely a HUD message at the TOP of the screen the
     * result is y_begin > y_end (and similarly x_begin can exceed x_end).  The
     * software loops no-op on that, but here (unsigned short)(y_end - y_begin)
     * underflows to ~65535 and the copy loop runs off the end of screen RAM
     * (address error / 2 bombs). */
    if (y_end <= y_begin || x_end <= x_begin)
        return;

    /* Upload the dirty status-bar rows at full width, then C2P + blit the
     * dirty rect (the blitter takes care of restricting to [x_begin,x_end)). */
    if (upload_rows(y_begin, y_end - y_begin, in) != 0)
    {
        c2p_statusbar_lorez(out, in, y_begin, y_end, x_begin, x_end);
        return;
    }
    if (sidecart_md_c2p_rect((unsigned short)x_begin, (unsigned short)y_begin,
                             (unsigned short)(x_end - x_begin),
                             (unsigned short)(y_end - y_begin)) != 0)
    {
        c2p_statusbar_lorez(out, in, y_begin, y_end, x_begin, x_end);
        return;
    }
    sidecart_md_copy_planar_to_screen(
        (unsigned long)STDOOM_PLANAR0_ADDR, out,
        (unsigned short)x_begin, (unsigned short)y_begin,
        (unsigned short)(x_end - x_begin), (unsigned short)(y_end - y_begin));
}

/* ── Public API ─────────────────────────────────────────────────────────── */

/* Detect the SidecarTridge DOOM Accelerator firmware and prime the C2P path.
 * Call once early in D_DoomMain (user mode, before I_Init). */
void sidecart_c2p_init(void)
{
    /* -nosidecart forces the pure software path: skip detection entirely so
     * c2p_md_active stays 0 and everything behaves exactly as if no cartridge
     * were present (sidecart_c2p_install no-ops, keypad '*' swallowed, UNDO->spy
     * / HELP->gamma kept). */
    if (M_CheckParm("-nosidecart"))
    {
        c2p_md_active = 0;
        printf("MD disabled (-nosidecart); SW C2P\n");
        return;
    }

    c2p_md_active = sidecart_md_detect();

    if (c2p_md_active)
    {
        char version[64];
        int init_rc, mode_rc;
        sidecart_md_result(version, sizeof(version));
        init_rc = sidecart_md_init(STDOOM_FRAME_WIDTH, STDOOM_FRAME_HEIGHT);
        /* render_mode/palette_gen were loaded from doomrc.cfg by M_LoadDefaults;
         * clamp them in case the config holds stale/out-of-range values. The
         * first I_SetPalette uploads the palette and populates the 16 ST
         * colours (rebuilt for this mode + palette source). */
        if (render_mode < 0 || render_mode >= STDOOM_MODE_COUNT)
            render_mode = STDOOM_DEFAULT_RENDER_MODE;
        if (palette_gen != STDOOM_PALGEN_SUBSET &&
            palette_gen != STDOOM_PALGEN_GENERATED)
            palette_gen = STDOOM_DEFAULT_PALETTE_GEN;
        mode_rc = (init_rc == 0) ? sidecart_md_set_mode(render_mode) : -1;
        /* Set the palette source before the first SET_PALETTE so the initial
         * 16 colours are generated/selected as configured. */
        if (mode_rc == 0)
            mode_rc = sidecart_md_set_palgen(palette_gen);
        if (init_rc != 0 || mode_rc != 0)
        {
            c2p_md_active = 0;
            printf("MD detected: %s\n", version);
            printf("MD C2P init failed; SW C2P\n");
        }
        else
        {
            printf("MD detected: %s\n", version);
        }
    }
    else
    {
        printf("MD not detected; SW C2P\n");
    }
}

/* Override the software function pointers with the accelerated versions.
 * Call after init_c2p_table() in I_InitGraphics.  No-op if no accelerator or
 * not in low-res mode. */
void sidecart_c2p_install(void)
{
    if (!c2p_md_active || Getrez() != 0)
        return;

    /* Capture the software palette function so the md hook and fallback can
     * defer to it. */
    s_sw_set_doom_palette = set_doom_palette;

    c2p_screen_drawfunc = c2p_screen_md;
    c2p_statusbar_drawfunc = c2p_statusbar_md;
    set_doom_palette = set_st_doom_palette_md;

    /* We are now in supervisor mode (I_Init ran), and all subsequent C2P
     * commands are per-frame. Mask interrupts around the bus protocol so a
     * keypress/sound interrupt can't corrupt a command and bounce a frame to
     * the software renderer. */
    sidecart_md_set_intr_mask(1);
}

/* Advance the render mode (wraps), apply it on the firmware, refresh the 16 ST
 * hardware colours from the firmware's new choice, and announce via the HUD.
 * The new render_mode persists to doomrc.cfg on quit (M_SaveDefaults), so the
 * last-used mode is restored next launch. No-op when the accelerator is
 * inactive (keypad '*' is then simply swallowed in i_video.c). */
void sidecart_c2p_cycle_render_mode(int delta)
{
    unsigned short stcolors[16];
    int mode;

    if (!c2p_md_active)
        return;

    mode = render_mode + delta;
    /* Wrap into 0..STDOOM_MODE_COUNT-1 without relying on C's negative %. */
    mode %= STDOOM_MODE_COUNT;
    if (mode < 0)
        mode += STDOOM_MODE_COUNT;

    if (sidecart_md_set_mode(mode) != 0)
        return; /* leave the current mode/colours untouched on failure */

    render_mode = mode;
    sidecart_md_get_st_colors(stcolors);
    install_palette(stcolors);

    /* Announce via DOOM's standard HUD message path (same as item pickups). */
    players[consoleplayer].message = s_render_mode_names[mode];
}

/* Toggle the palette source (generated <-> fixed subset), apply it on the
 * firmware, refresh the 16 ST hardware colours from the firmware's new choice,
 * and announce via the HUD. The new palette_gen persists to doomrc.cfg on quit.
 * No-op when the accelerator is inactive. */
void sidecart_c2p_toggle_palette_gen(void)
{
    unsigned short stcolors[16];
    int gen;

    if (!c2p_md_active)
        return;

    gen = (palette_gen == STDOOM_PALGEN_GENERATED) ? STDOOM_PALGEN_SUBSET
                                                   : STDOOM_PALGEN_GENERATED;

    if (sidecart_md_set_palgen(gen) != 0)
        return; /* leave the current source/colours untouched on failure */

    palette_gen = gen;
    sidecart_md_get_st_colors(stcolors);
    install_palette(stcolors);

    players[consoleplayer].message = s_palette_gen_names[gen];
}
