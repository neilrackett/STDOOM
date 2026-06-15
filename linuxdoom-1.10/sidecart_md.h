/**
 * File: sidecart_md.h
 * Description: DOOM Accelerator ST-side client library — public header.
 *
 * Provides a small C API for STDOOM to detect and talk to the DOOM Accelerator
 * running on a SidecarTridge Multi-device. The low-level
 * SidecarTridge command protocol is implemented in sidecart_stubs.S; this
 * header/implementation pair wraps it for use from the Doom C code.
 *
 * Milestone 1: detection only (sidecart_md_detect via PING).
 * Milestone 2 adds the C2P offload entry points.
 */

#ifndef SIDECART_MD_H
#define SIDECART_MD_H

/* ── Command IDs (must match stdoom_commands.h on the RP2040 side) ───────── */
#define CMD_STDOOM_PING 0x0010
#define CMD_STDOOM_INIT 0x0011
#define CMD_STDOOM_SET_MAP 0x0012
#define CMD_STDOOM_BLIT_ROWS 0x0013
#define CMD_STDOOM_C2P 0x0014
#define CMD_STDOOM_SET_PALETTE 0x0015
#define CMD_STDOOM_SET_MODE 0x0016
#define CMD_STDOOM_SET_PALGEN 0x0017

/* Render modes (must match stdoom_commands.h). */
#define STDOOM_MODE_NEAREST 0
#define STDOOM_MODE_BAYER2 1
#define STDOOM_MODE_BAYER4 2
#define STDOOM_MODE_GREY 3
#define STDOOM_MODE_GREY_BAYER2 4
#define STDOOM_MODE_GREY_BAYER4 5
#define STDOOM_MODE_COUNT 6

/* Palette source for nearest/Bayer modes (must match stdoom_commands.h). */
#define STDOOM_PALGEN_SUBSET 0
#define STDOOM_PALGEN_GENERATED 1

/* STDOOM low-res frame geometry. */
#define STDOOM_FRAME_WIDTH 320
#define STDOOM_FRAME_HEIGHT 200
#define STDOOM_CHUNKY_SIZE (STDOOM_FRAME_WIDTH * STDOOM_FRAME_HEIGHT)
#define STDOOM_PLANAR_SIZE 32000

/* PING magic — sent in d3. The firmware echoes the random token for any PING,
 * so this is informational/handshake padding ('STDM'). */
#define STDOOM_PING_MAGIC 0x5354444DL /* 'STDM' */

/* ── Shared memory addresses (ROM4 base $FA0000) ────────────────────────── */
#define STDOOM_TOKEN_ADDR ((volatile unsigned long *)0xFAF000L)
#define STDOOM_SEED_ADDR ((volatile unsigned long *)0xFAF004L)
#define STDOOM_STATUS_ADDR ((volatile unsigned char *)0xFAF008L)
#define STDOOM_READY_ADDR ((volatile unsigned char *)0xFAF00AL)
/* 16 chosen ST hardware-palette words returned by SET_PALETTE/SET_MODE (M4),
 * stored in bus order so they can be read directly and written to $FF8240. */
#define STDOOM_STCOLORS_ADDR ((volatile unsigned short *)0xFAF020L)
#define STDOOM_RESULT_ADDR ((volatile char *)0xFAF100L)
#define STDOOM_PLANAR0_ADDR 0xFA0000L

/* TEMPORARY diagnostic counters published by the RP2040 worker. Values are
 * 16-bit word-swapped (ROM-in-RAM bus order), so treat as zero/non-zero. */
#define STDOOM_DBG_ROM3_IRQ_ADDR ((volatile unsigned long *)0xFAF010L)
#define STDOOM_DBG_CMD_ADDR      ((volatile unsigned long *)0xFAF014L)
#define STDOOM_DBG_CHK_ERR_ADDR  ((volatile unsigned long *)0xFAF018L)
#define STDOOM_DBG_LAST_CMD_ADDR ((volatile unsigned long *)0xFAF01CL)

/* Ready magic written by the firmware once the worker is up (STDOOM_READY_MAGIC
 * = 'S' in stdoom_commands.h). The firmware writes it to both bytes of the bus
 * word, so a byte read at the even address $FAF00A returns it directly. */
#define STDOOM_READY_MAGIC 0x53

/* Status values (mirror STDOOM_STATUS_* in stdoom_commands.h). */
#define STDOOM_STATUS_IDLE 0x00
#define STDOOM_STATUS_BUSY 0x01
#define STDOOM_STATUS_DONE 0x02

#define STDOOM_RESULT_SIZE 2048

/* ── Public API ─────────────────────────────────────────────────────────── */

/**
 * @brief Detect whether the DOOM Accelerator firmware is present.
 *
 * Checks the ready magic, guards against a stale/absent cartridge via the
 * random seed, then issues a PING and waits for the token handshake. On
 * success the firmware version string is available at STDOOM_RESULT_ADDR.
 *
 * @return 1 if the accelerator is present and responding, 0 otherwise.
 */
int sidecart_md_detect(void);

/**
 * @brief Diagnostic detection. Reports which stage failed and the raw values
 *        read, for troubleshooting on hardware. Any out-pointer may be NULL.
 * @param stage   0=ok, 1=ready mismatch, 2=dead seed, 3=PING timeout.
 * @param ready   Raw byte read from STDOOM_READY_ADDR.
 * @param seed_out Raw 32-bit seed read from STDOOM_SEED_ADDR.
 * @param ping_rc  send_sync return code (0 ok, -1 timeout).
 * @return 1 if detected, 0 otherwise.
 */
int sidecart_md_detect_verbose(int *stage, unsigned char *ready,
                               unsigned long *seed_out, int *ping_rc);

/**
 * @brief Initialise the STDOOM C2P firmware for the active frame size.
 * @param width  Frame width in pixels.
 * @param height Frame height in pixels.
 * @return 0 on success, -1 on timeout.
 */
int sidecart_md_init(unsigned short width, unsigned short height);

/**
 * @brief Upload the 256-byte chunky-to-ST4 reduction map.
 * @param doom8_to_st4  256-byte lookup table mapping 8bpp Doom indices to 0..15.
 * @return 0 on success, -1 on timeout.
 */
int sidecart_md_set_map(const unsigned char *doom8_to_st4);

/**
 * @brief Upload a chunk of chunky rows into the RP2040 staging buffer.
 * @param y       Start row in the full 320x200 chunky framebuffer.
 * @param rows    Number of rows in this chunk.
 * @param width   Chunk width in bytes.
 * @param pitch   Source pitch in bytes.
 * @param chunky  Pointer to contiguous chunky rows.
 * @return 0 on success, -1 on timeout.
 */
int sidecart_md_blit_rows(unsigned short y, unsigned short rows,
                          unsigned short width, unsigned short pitch,
                          const unsigned char *chunky);

/**
 * @brief Run the RP2040 chunky-to-planar conversion for a word-aligned
 *        sub-rectangle of the staged chunky frame.
 *
 * @param x  Left edge of the rectangle in pixels (must be a multiple of 16).
 * @param y  Top edge in pixels.
 * @param w  Width in pixels (must be a multiple of 16).
 * @param h  Height in pixels.
 * @return 0 on success, -1 on timeout.
 */
int sidecart_md_c2p_rect(unsigned short x, unsigned short y,
                         unsigned short w, unsigned short h);

/**
 * @brief Run the RP2040 chunky-to-planar conversion for the full staged frame.
 *        Equivalent to sidecart_md_c2p_rect(0, 0, STDOOM_FRAME_WIDTH, STDOOM_FRAME_HEIGHT).
 * @return 0 on success, -1 on timeout.
 */
int sidecart_md_c2p(void);

/**
 * @brief Upscaling C2P (Milestone 7): convert source rect (x,y,w,h) of the
 *        staged chunky frame, pixel-replicated by @p scale, into the TOP-LEFT
 *        of planar slot 0 (dst origin 0,0, size w*scale x h*scale).  Used for
 *        the 2x/4x zoom view so a small centred render fills the play area.
 *
 * @param x     Source left edge in pixels (low nibble is reused for scale, so
 *              the word-aligned value must be a multiple of 16).
 * @param y     Source top edge in pixels.
 * @param w     Source width in pixels (multiple of 16).
 * @param h     Source height in pixels.
 * @param scale Magnify factor (2 or 4; 1 is equivalent to sidecart_md_c2p_rect).
 * @return 0 on success, -1 on timeout.
 */
int sidecart_md_c2p_scaled(unsigned short x, unsigned short y,
                           unsigned short w, unsigned short h,
                           unsigned short scale);

/**
 * @brief Upload the active 768-byte DOOM palette (256 x RGB) to the accelerator
 *        (M4). The firmware reduces it to 16 ST colours for the current mode and
 *        publishes them; read them back with sidecart_md_get_st_colors().
 * @param rgb768 Pointer to 768 bytes (256 colours x 3 bytes R,G,B).
 * @return 0 on success, -1 on timeout.
 */
int sidecart_md_set_palette(const unsigned char *rgb768);

/**
 * @brief Select the RP2040 render mode (M4): STDOOM_MODE_NEAREST/BAYER2/BAYER4/
 *        GREY. The firmware rebuilds its colour LUT and the 16 ST colours from
 *        the last uploaded palette.
 * @param mode One of STDOOM_MODE_*.
 * @return 0 on success, -1 on timeout.
 */
int sidecart_md_set_mode(int mode);

/**
 * @brief Select the palette source for nearest/Bayer modes (M4 Stage 3):
 *        STDOOM_PALGEN_SUBSET (fixed hand-tuned DOOM colours) or
 *        STDOOM_PALGEN_GENERATED (median-cut + k-means from the uploaded
 *        palette). The firmware rebuilds its LUT and the 16 ST colours.
 * @param gen One of STDOOM_PALGEN_*.
 * @return 0 on success, -1 on timeout.
 */
int sidecart_md_set_palgen(int gen);

/**
 * @brief Read the 16 chosen ST hardware-palette words published by the last
 *        SET_PALETTE/SET_MODE/SET_PALGEN. Stored in bus order, so copy directly.
 * @param out16 Destination array of 16 unsigned shorts (ready for $FF8240).
 */
void sidecart_md_get_st_colors(unsigned short *out16);

/**
 * @brief Copy the firmware version string (set by the last PING) into a C
 *        buffer, un-swapping the big-endian word storage.
 * @param buf  Destination buffer.
 * @param size Size of buf in bytes.
 */
void sidecart_md_result(char *buf, int size);

/**
 * @brief Enable/disable masking 68000 interrupts (IPL 7) around each
 *        cartridge-bus command.
 *
 * Keeps the timing-critical ROM3 read sequence atomic so MFP/timer interrupts
 * (keyboard keypress, menu-sound timer) cannot corrupt a command and force a
 * software-render fallback. MOVE-to-SR is privileged, so only enable this once
 * the game is running in supervisor mode (after I_Init); leave it disabled for
 * the user-mode detect/INIT/SET_MAP commands issued during D_DoomMain.
 *
 * @param enable Non-zero to mask interrupts around commands; zero to disable.
 */
void sidecart_md_set_intr_mask(int enable);

#endif /* SIDECART_MD_H */
