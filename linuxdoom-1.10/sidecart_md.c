/**
 * File: sidecart_md.c
 * Description: DOOM Accelerator ST-side client library implementation.
 *
 * Wraps the SidecarTridge low-level command protocol (send_sync, implemented
 * in sidecart_stubs.S) for use from the Doom C code. Register marshalling is
 * done in the assembly wrappers to keep this C ABI-safe on m68k.
 */

#include "sidecart_md.h"

#include <mint/cookie.h>
#include <mint/osbind.h>

/* C ABI entry points provided by sidecart_stubs.S.
 *   send_sync_command(cmd, payload_size, d3, d4)
 *     payload_size is in bytes, EXCLUDING the 4-byte token (added internally).
 *     Returns 0 on success (token handshake completed), -1 on timeout. */
extern int stdoom_send_sync_command(int cmd, int payload_size, long d3, long d4);
extern int stdoom_send_sync_write_command(int cmd, const char *buf, int byte_count,
                                          int chunk_idx, int total_chunks,
                                          int chunk_size);

#define STDOOM_PING_RETRY_COUNT 3

#ifndef C__MCH
#define C__MCH 0x5f4d4348L /* '_MCH' */
#endif

#ifndef C_FOUND
#define C_FOUND 0
#endif

#define MCH_MEGA_STE_COOKIE     0x00010010L
#define MEGASTE_CTRL_ADDR       ((volatile unsigned char *)0xFFFF8E21UL)
/*
 * Mega STE CPU control register, canonical MSTE_CC values: $FF = 16MHz+cache,
 * $FE = 16MHz no cache, $F4 = 8MHz no cache. The upper bits are must-be-set
 * config flags; cache enable is bit0. Keep in sync with atari_checkcpu.c and
 * atarist-sdl's SDL_megaste.c. Toggle ONLY bit0 so the must-be-set bits are
 * preserved no matter what value the register currently holds.
 */
#define MEGASTE_16MHZ_CACHE     0xFFu
#define MEGASTE_16MHZ_NOCACHE   0xFEu
#define MEGASTE_CTRL_CACHE_BIT  0x01u

typedef struct sidecart_md_cache_guard_s {
  int active;
  unsigned char saved_ctrl;
} sidecart_md_cache_guard_t;

static int sidecart_md_is_megaste(void) {
  static int cached = -1;
  long mch_cookie = 0;

  if (cached >= 0) {
    return cached;
  }

  cached = 0;
  if (Getcookie(C__MCH, &mch_cookie) == C_FOUND &&
      mch_cookie == MCH_MEGA_STE_COOKIE) {
    cached = 1;
  }

  return cached;
}

static int sidecart_md_cache_enabled(unsigned char ctrl) {
  return (ctrl & MEGASTE_CTRL_CACHE_BIT) != 0;
}

static unsigned char sidecart_md_cache_disabled_ctrl(unsigned char ctrl) {
  /* Clear only the cache-enable bit (bit0), preserving the must-be-set upper
   * config bits, so we never write an invalid value to the register. */
  return (unsigned char)(ctrl & ~MEGASTE_CTRL_CACHE_BIT);
}

/* Super(0L)/Super(ssp) is only correct when called FROM user mode. STDOOM runs
 * the whole game loop in supervisor mode (I_Init enters super and stays), so the
 * naive pattern would call Super(0L) while already supervisor and then drop to
 * user mode with a bad USP -> bus error. Enter super only if we are in user
 * mode, and skip the switch-back in that case. SUP_INQUIRE (Super(1L)) returns
 * non-zero when already supervisor, 0 when in user mode. */
#define SIDECART_ALREADY_SUPER (-1L)

static long sidecart_md_super_enter(void) {
  if ((long)Super((void *)1L) != 0L) {
    return SIDECART_ALREADY_SUPER;
  }
  return (long)Super(0L);
}

static void sidecart_md_super_exit(long token) {
  if (token != SIDECART_ALREADY_SUPER) {
    Super((void *)token);
  }
}

static void sidecart_md_cache_guard_begin(sidecart_md_cache_guard_t *guard) {
  long super_token;

  guard->active = 0;
  guard->saved_ctrl = 0;

  if (!sidecart_md_is_megaste()) {
    return;
  }

  super_token = sidecart_md_super_enter();
  guard->saved_ctrl = *MEGASTE_CTRL_ADDR;

  /* Mega STE cache can hide the ROM3 read side effects that carry the
   * SidecarTridge protocol. Keep 16MHz speed but force cache off while the
   * command is being signalled, then restore the caller's original setting. */
  if (sidecart_md_cache_enabled(guard->saved_ctrl)) {
    *MEGASTE_CTRL_ADDR = sidecart_md_cache_disabled_ctrl(guard->saved_ctrl);
    guard->active = 1;
  }

  sidecart_md_super_exit(super_token);
}

static void sidecart_md_cache_guard_end(sidecart_md_cache_guard_t *guard) {
  long super_token;

  if (!guard->active) {
    return;
  }

  super_token = sidecart_md_super_enter();
  *MEGASTE_CTRL_ADDR = guard->saved_ctrl;
  sidecart_md_super_exit(super_token);
  guard->active = 0;
}

/* ── Interrupt mask around the cartridge-bus protocol ─────────────────────── */
/* The SidecarTridge command transport drives the RP2040 via timing-critical
 * ROM3 bus reads (see sidecart_stubs.S). If an MFP/timer interrupt fires in the
 * middle of a command — e.g. the keyboard interrupt on a menu cursor keypress,
 * or a menu-sound timer — it stalls the read sequence, the RP2040's parser sees
 * a corrupted command, drops it, and the ST's token wait times out. The frame
 * then falls back to the software renderer, which is visible as a brief
 * dithered flash during menu navigation. Raising the 68000 IPL to 7 around each
 * command keeps the read sequence atomic.
 *
 * MOVE-to-SR is privileged, so we only mask once rendering has started (always
 * supervisor mode after I_Init). The one-time detect/INIT/SET_MAP commands run
 * in user mode during D_DoomMain with masking disabled, so no privileged SR
 * write is ever executed in user mode. Enabled via sidecart_md_set_intr_mask(1)
 * from sidecart_c2p_install(). */
static int s_mask_intr = 0;

void sidecart_md_set_intr_mask(int enable) { s_mask_intr = enable; }

static unsigned short sidecart_md_intr_begin(void) {
  unsigned short saved = 0;
  if (s_mask_intr) {
    __asm__ volatile("move.w %%sr,%0\n\t"
                     "ori.w  #0x0700,%%sr"
                     : "=d"(saved) : : "cc");
  }
  return saved;
}

static void sidecart_md_intr_end(unsigned short saved) {
  if (s_mask_intr) {
    __asm__ volatile("move.w %0,%%sr" : : "d"(saved) : "cc");
  }
}

static int sidecart_md_guarded_send_sync_command(int cmd, int payload_size,
                                                 long d3, long d4) {
  sidecart_md_cache_guard_t guard;
  unsigned short isr;
  int rc;

  sidecart_md_cache_guard_begin(&guard);
  isr = sidecart_md_intr_begin();
  rc = stdoom_send_sync_command(cmd, payload_size, d3, d4);
  sidecart_md_intr_end(isr);
  sidecart_md_cache_guard_end(&guard);
  return rc;
}

static int sidecart_md_guarded_send_sync_write_command(
    int cmd, const char *buf, int byte_count, int chunk_idx, int total_chunks,
    int chunk_size) {
  sidecart_md_cache_guard_t guard;
  unsigned short isr;
  int rc;

  sidecart_md_cache_guard_begin(&guard);
  isr = sidecart_md_intr_begin();
  rc = stdoom_send_sync_write_command(
      cmd, buf, byte_count, chunk_idx, total_chunks, chunk_size);
  sidecart_md_intr_end(isr);
  sidecart_md_cache_guard_end(&guard);
  return rc;
}

/* ── Read the result string from STDOOM_RESULT_ADDR ─────────────────────── */
/* The RP2040 stores the result with a 16-bit byte swap (big-endian on the
 * m68k bus). Un-swap while copying. */
void sidecart_md_result(char *buf, int size) {
  volatile unsigned short *src = (volatile unsigned short *)STDOOM_RESULT_ADDR;
  unsigned char *dst = (unsigned char *)buf;
  int max = size - 1;
  int i = 0;

  while (i < max) {
    unsigned short w = *src++;
    unsigned char hi = (unsigned char)(w >> 8);
    unsigned char lo = (unsigned char)(w & 0xFF);
    if (hi == 0) break;
    dst[i++] = hi;
    if (lo == 0) break;
    if (i < max) dst[i++] = lo;
  }
  dst[i] = '\0';
}

/* ── Inter-command settle delay ───────────────────────────────────────────── */
/* Ported from md-js (mdjs.c). A freshly-booted SidecarTridge can lose the
 * first protocol command to a cartridge-bus settle race in the RP's parser.
 * Keep this out of the frame upload path: on a stock 68000 this loop is very
 * expensive when multiplied by 30+ BLIT_ROWS commands. */
static void sidecart_md_settle(void) {
  volatile long i;
  for (i = 0; i < 200000L; i++) {
  }
}

static void sidecart_md_settle_once(void) {
  static int settled = 0;

  if (!settled) {
    sidecart_md_settle();
    settled = 1;
  }
}

static int sidecart_md_send_sync_command_once(int cmd, int payload_size,
                                              long d3, long d4) {
  sidecart_md_settle_once();
  return sidecart_md_guarded_send_sync_command(cmd, payload_size, d3, d4);
}

static int sidecart_md_ping_retry(void) {
  int attempt;
  int rc = -1;

  for (attempt = 0; attempt < STDOOM_PING_RETRY_COUNT; attempt++) {
    rc = sidecart_md_send_sync_command_once(CMD_STDOOM_PING, 0, 0L, 0L);
    if (rc == 0) {
      return 0;
    }
    if (attempt + 1 < STDOOM_PING_RETRY_COUNT) {
      sidecart_md_settle();
    }
  }

  return rc;
}

/* ── Detection ──────────────────────────────────────────────────────────── */
/* Aligned with md-js's proven mdjs_ping(): a single PING with payload_size 0
 * (the 4-byte random token is added by the stub internally). Detection is
 * authoritative on the token handshake, exactly as md-js/examples/mdjscode
 * checks for firmware. The ready byte and seed are read for the on-screen
 * diagnostic only.
 *
 * Diagnostic stage codes (via *stage):
 *   0 = success
 *   1 = ready magic mismatch (recorded, no longer aborts)
 *   2 = seed looks dead (recorded, no longer aborts)
 *   3 = PING handshake timed out                                            */
int sidecart_md_detect_verbose(int *stage, unsigned char *ready,
                               unsigned long *seed_out, int *ping_rc) {
  unsigned char ready_val;
  unsigned long seed;
  int rc;

  ready_val = *STDOOM_READY_ADDR;
  if (ready) *ready = ready_val;

  seed = *STDOOM_SEED_ADDR;
  if (seed_out) *seed_out = seed;

  if (stage) *stage = 0;
  if (ready_val != STDOOM_READY_MAGIC) {
    if (stage) *stage = 1;
  }
  if (seed == 0UL || seed == 0xFFFFFFFFUL) {
    if (stage) *stage = 2;
  }

  rc = sidecart_md_ping_retry();
  if (ping_rc) *ping_rc = rc;
  if (rc != 0) {
    if (stage) *stage = 3;
    return 0;
  }

  if (stage) *stage = 0;
  return 1;
}

int sidecart_md_detect(void) {
  return sidecart_md_detect_verbose(0, 0, 0, 0);
}

int sidecart_md_init(unsigned short width, unsigned short height) {
  return sidecart_md_send_sync_command_once(
      CMD_STDOOM_INIT, 4, ((long)width << 16) | (long)height, 0L);
}

int sidecart_md_set_map(const unsigned char *doom8_to_st4) {
  if (!doom8_to_st4) {
    return -1;
  }
  sidecart_md_settle_once();
  return sidecart_md_guarded_send_sync_write_command(
      CMD_STDOOM_SET_MAP, (const char *)doom8_to_st4, 256, 0, 0, 256);
}

int sidecart_md_blit_rows(unsigned short y, unsigned short rows,
                          unsigned short width, unsigned short pitch,
                          const unsigned char *chunky) {
  int byte_count;

  if (!chunky || rows == 0 || width == 0) {
    return -1;
  }

  byte_count = (int)rows * (int)width;
  sidecart_md_settle_once();
  return sidecart_md_guarded_send_sync_write_command(
      CMD_STDOOM_BLIT_ROWS, (const char *)chunky, byte_count, (int)y,
      ((int)width << 16) | (int)rows, ((int)pitch << 16));
}

int sidecart_md_c2p_rect(unsigned short x, unsigned short y,
                         unsigned short w, unsigned short h) {
  return sidecart_md_send_sync_command_once(
      CMD_STDOOM_C2P, 8,
      ((long)x << 16) | (long)y,
      ((long)w << 16) | (long)h);
}

int sidecart_md_c2p(void) {
  return sidecart_md_c2p_rect(0, 0, STDOOM_FRAME_WIDTH, STDOOM_FRAME_HEIGHT);
}

int sidecart_md_c2p_scaled(unsigned short x, unsigned short y,
                           unsigned short w, unsigned short h,
                           unsigned short scale) {
  /* Magnify factor rides in the spare low nibble of x (word-aligned), so the
   * d3/d4 layout and payload size are unchanged (see CMD_STDOOM_C2P doc). */
  return sidecart_md_send_sync_command_once(
      CMD_STDOOM_C2P, 8,
      ((long)(((unsigned)x & ~15u) | ((unsigned)scale & 15u)) << 16) | (long)y,
      ((long)w << 16) | (long)h);
}

int sidecart_md_set_palette(const unsigned char *rgb768) {
  if (!rgb768) {
    return -1;
  }
  sidecart_md_settle_once();
  return sidecart_md_guarded_send_sync_write_command(
      CMD_STDOOM_SET_PALETTE, (const char *)rgb768, 768, 0, 0, 768);
}

int sidecart_md_set_mode(int mode) {
  return sidecart_md_send_sync_command_once(CMD_STDOOM_SET_MODE, 4,
                                            (long)mode, 0L);
}

int sidecart_md_set_palgen(int gen) {
  return sidecart_md_send_sync_command_once(CMD_STDOOM_SET_PALGEN, 4,
                                            (long)gen, 0L);
}

/* The firmware stores the 16 ST colour words in bus order (no swap), so the ST
 * reads them straight from ROM-in-RAM and feeds them to the palette registers. */
void sidecart_md_get_st_colors(unsigned short *out16) {
  volatile unsigned short *src = STDOOM_STCOLORS_ADDR;
  int i;
  if (!out16) {
    return;
  }
  for (i = 0; i < 16; i++) {
    out16[i] = src[i];
  }
}
