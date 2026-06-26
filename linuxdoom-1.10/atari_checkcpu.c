#include <mint/osbind.h>
#include <mint/cookie.h>

#include "atari_sfp004.h"

#pragma GCC diagnostic ignored "-Wunused-value"

#ifndef C__MCH
#define C__MCH 0x5f4d4348L /* '_MCH' */
#endif

#define MCH_MEGA_STE   0x00010010L

/*
 * Mega STE CPU control register ($FFFF8E21), canonical MSTE_CC values:
 *   $FF = 16 MHz + cache enabled
 *   $FE = 16 MHz, cache disabled
 *   $F4 =  8 MHz, cache disabled (TOS default after cold boot)
 *
 * The upper bits are "must be set" hardware/config flags. Writing the naive
 * bit0=speed / bit1=cache values ($01 / $03) clears those flags, leaves the
 * register in an undefined state, and does NOT actually switch the CPU. This
 * was the source of the Mega STE instability. Keep these values in sync with
 * sidecart_md.c and atarist-sdl's SDL_megaste.c. See:
 *   https://github.com/phjanderson/MSTE_CC
 */
#define MEGASTE_CTRL_BYTE_ADDR 0xffff8e21UL
#define MEGASTE_16MHZ_CACHE    0xFFu
#define MEGASTE_16MHZ_NOCACHE  0xFEu
#define MEGASTE_CTRL_CACHE_BIT 0x01u  /* bit0 of the canonical byte = cache on */

static int megaste_ctrl_saved_valid = 0;
static unsigned char megaste_ctrl_saved = 0;

int atari_is_megaste_16mhz_cache(void)
{
    static int cached = -1;
    long mch_cookie = 0;

    if (cached >= 0)
        return cached;

    cached = 0;
    if (C_FOUND != Getcookie(C__MCH, &mch_cookie))
        return cached;
    if (mch_cookie != MCH_MEGA_STE)
        return cached;

    {
        volatile unsigned char *ctrlb = (volatile unsigned char *)MEGASTE_CTRL_BYTE_ADDR;
        if (*ctrlb & MEGASTE_CTRL_CACHE_BIT)
            cached = 1;
    }

    return cached;
}

void atari_enable_megaste_turbo(void)
{
    long mch_cookie = 0;
    if (C_FOUND != Getcookie(C__MCH, &mch_cookie))
        return;
    if (mch_cookie != MCH_MEGA_STE)
        return;

    {
        volatile unsigned char *ctrlb = (volatile unsigned char *)MEGASTE_CTRL_BYTE_ADDR;

        if (!megaste_ctrl_saved_valid) {
            megaste_ctrl_saved = *ctrlb;
            megaste_ctrl_saved_valid = 1;
        }

        /*
         * Write the canonical 16MHz+cache value. Do NOT write the high byte at
         * $8E20 or the naive $03 value: both corrupt the must-be-set bits and
         * destabilise the machine (see the note by the #defines above).
         */
        *ctrlb = MEGASTE_16MHZ_CACHE;

        Cconws("Mega STe: ");
        if (*ctrlb & MEGASTE_CTRL_CACHE_BIT) {
            Cconws("16MHz + cache on\r\n");
            (void)atari_is_megaste_16mhz_cache();
        } else {
            Cconws("could not lock 16MHz+cache\r\n");
        }
    }
}

void atari_restore_megaste_turbo(void)
{
    long mch_cookie = 0;
    volatile unsigned char *ctrlb = (volatile unsigned char *)MEGASTE_CTRL_BYTE_ADDR;

    if (!megaste_ctrl_saved_valid)
        return;
    if (C_FOUND != Getcookie(C__MCH, &mch_cookie))
        return;
    if (mch_cookie != MCH_MEGA_STE)
        return;

    *ctrlb = megaste_ctrl_saved;
}

// This function is mainly for suppressing mintlib's CPU and FPU detection, but we can also print some 
void _checkcpu() {
    long cookie = 0;
    char m020_detected = 0;
    if (C_FOUND == Getcookie(C__CPU, &cookie)) {
        Cconws("CPU: ");
        if (cookie == 0) Cconws("68000");
        else if (cookie == 10) Cconws("68010");
        else if (cookie == 20) Cconws("68020");
        else if (cookie == 30) Cconws("68030");
        else if (cookie == 40) Cconws("68040");
        else if (cookie == 60) Cconws("68060");
        else Cconws("unknown");
        m020_detected = cookie >= 20;
        Cconws("\r\n");
    } else {
        Cconws("CPU type not detected\r\n");
    }
    char fpu_detected = 0;
    if (C_FOUND == Getcookie(C__FPU, &cookie)) {
        Cconws("FPU:");
        if (1 & (cookie >> 16)) {
            Cconws(" SFP-004");
        }
        if (3 & (cookie >> 17)) {
            unsigned short fpu = 3 & (cookie >> 17);
            if (fpu == 1) Cconws(" 6888?");
            else if (fpu == 2) Cconws(" 68881");
            else if (fpu == 3) Cconws(" 68882");
            fpu_detected = 1;
        }
        if (1 & (cookie >> 19)) {
            Cconws(" integrated");
            fpu_detected = 1;
        }
        Cconws("\r\n");
    } else {
        Cconws("FPU type not detected\r\n");
    }

    /* Latch the optional memory-mapped 68882 dispatch path (soft-float fallback
     * when absent — see atari_sfp004.c). */
    sfp004_init();
    if (sfp004_available())
        Cconws("SFP-004 FPU dispatch enabled\r\n");

    char sufficient = 1;
    #ifdef __HAVE_68881__
    sufficient &= fpu_detected;
    #endif
    
    #if defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__) || defined(__mc68060__)
    sufficient &= m020_detected;
    #endif

    if (sufficient) {
        Cconws("CPU check passed.\r\n");
        return;
    }

    Cconws("\r\nDetected hardware does not\r\n");
    Cconws("meet the requirements for\r\n");
    Cconws("running this executable.\r\n\r\n");
    #if defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__) || defined(__mc68060__)
    Cconws("This executable requires a 68020+ CPU.\r\n");
    #endif
    #ifdef __HAVE_68881__
    Cconws("This executable requires an FPU.\r\n");
    #endif

    Cconws("\r\n");
    Cconws("Press [i] to ignore.\r\n");
    Cconws("Press any other key to abort.\r\n");

    long key = Cconin() & 0x7f;
    Cconws("\r\n");
    if (key == 'i' || key == 'I') {
        return;
    }

    Pterm(-1);
}
