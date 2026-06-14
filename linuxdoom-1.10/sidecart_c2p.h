#ifndef SIDECART_C2P_H
#define SIDECART_C2P_H

/*
 * SidecarTridge-accelerated C2P (DOOM Accelerator).
 *
 * When a SidecarTridge Multi-device running the DOOM Accelerator firmware is
 * present, this layer overrides the software C2P function pointers in
 * atari_c2p.c with versions that offload chunky-to-planar conversion to the
 * RP2040. When no accelerator is detected, the software path in atari_c2p.c is
 * used unchanged.
 *
 * Lifecycle:
 *   sidecart_c2p_init()     — detect + INIT + SET_MAP. Call once early (in
 *                             D_DoomMain, user mode, before I_Init). Sets
 *                             c2p_md_active.
 *   sidecart_c2p_install()  — override the software drawfunc/palette pointers
 *                             with the accelerated versions. Call after
 *                             init_c2p_table() in I_InitGraphics. No-op unless
 *                             c2p_md_active and the screen is low-res.
 */

/* 1 = SidecarTridge DOOM Accelerator firmware detected and primed. */
extern int c2p_md_active;

/* Default render mode at first launch (one place to change the default).
 * 0=NEAREST, 1=2x2 BAYER, 2=4x4 BAYER, 3=GREYSCALE (STDOOM_MODE_* in
 * sidecart_md.h). Persisted to doomrc.cfg via the m_misc.c defaults[] table. */
#define STDOOM_DEFAULT_RENDER_MODE 0

/* Current render mode. Loaded from doomrc.cfg by M_LoadDefaults (before
 * sidecart_c2p_init), saved by M_SaveDefaults on quit. */
extern int render_mode;

/* Default palette source at first launch (STDOOM_PALGEN_* in sidecart_md.h):
 * 1 = generated (median-cut + k-means), 0 = fixed hand-tuned subset. */
#define STDOOM_DEFAULT_PALETTE_GEN 1

/* Current palette source for nearest/Bayer modes. Persisted to doomrc.cfg via
 * the m_misc.c defaults[] table. Toggled live with the '0' key for A/B. */
extern int palette_gen;

void sidecart_c2p_init(void);
void sidecart_c2p_install(void);

/* Advance the render mode by delta (mod STDOOM_MODE_COUNT), apply it on the
 * accelerator, refresh the 16 ST colours, and announce it via the HUD. No-op
 * when the accelerator is inactive. Bound to the keypad '*' key in i_video.c. */
void sidecart_c2p_cycle_render_mode(int delta);

/* Toggle the palette source (generated <-> fixed subset) live, apply it on the
 * accelerator, refresh the 16 ST colours, and announce via the HUD. No-op when
 * the accelerator is inactive. Bound to the '0' key in i_video.c. */
void sidecart_c2p_toggle_palette_gen(void);

#endif /* SIDECART_C2P_H */
