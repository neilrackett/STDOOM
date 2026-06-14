; STDOOM Accelerator cartridge loader stub
; Based on the SidecarTridge firmware loader (C) 2023-2025 by Diego Parrilla
; License: GPL v3
;
; This cartridge image is emulated by the RP2040 in ROM4. It only needs to:
;   1. Provide a valid ROM cartridge header so TOS maps the cartridge.
;   2. On boot, poll the ready byte the RP2040 writes once the STDOOM Accelerator
;      worker is up, and print a one-line status.
; The real detection/offload happens inside STDOOM.TOS via the sidecart_md
; client library — there is no bundled GEM demo app here.

; ROM cartridge header format: https://www.atari-forum.com/viewtopic.php?t=14086

ROM4_ADDR			equ $FA0000
SCREEN_SIZE			equ (-4096)	; Work area just before screen memory

; Shared command/handshake addresses (must match stdoom_commands.h).
RANDOM_TOKEN_ADDR:        equ (ROM4_ADDR + $F000)        ; $FAF000
RANDOM_TOKEN_SEED_ADDR:   equ (RANDOM_TOKEN_ADDR + 4)    ; $FAF004
STDOOM_READY_ADDR         equ (ROM4_ADDR + $F00A)        ; Worker-ready byte
STDOOM_READY_MAGIC        equ $53                        ; 'S' — STDOOM_READY_MAGIC
STDOOM_BOOT_READY_TIMEOUT equ 250                        ; VBL polls before giving up

	include inc/tos.s

; XBIOS Get Screen Base — returns the screen memory address in D0.
get_screen_base		macro
					move.w #2,-(sp)
					trap #14
					addq.l #2,sp
					endm

	section

;Rom cartridge

	org ROM4_ADDR

	dc.l $abcdef42 					; magic number
first:
	dc.l 0							; CA_NEXT — no further programs
	dc.l $08000000 + pre_auto		; CA_INIT — after GEMDOS init, before boot
	dc.l 0							; CA_RUN — none
	dc.w GEMDOS_TIME 				; time
	dc.w GEMDOS_DATE 				; date
	dc.l end_pre_auto - pre_auto	; size
	dc.b "MDSTDOOM",0
	even

pre_auto:
; Relocate the boot code from ROM into RAM and run it there to avoid unstable
; behaviour executing directly from the emulated cartridge ROM.
	get_screen_base
	move.l d0, a2

	lea SCREEN_SIZE(a2), a2		; Work area just after the screen memory
	move.l a2, a3				; Save relocation destination in A3
	move.l #end_rom_code - start_rom_code, d6
	lea start_rom_code, a1
	addq.l #3, d6
	lsr.l #2, d6
	subq #1, d6
.copy_rom_code:
	move.l (a1)+, (a2)+
	dbf d6, .copy_rom_code
	jmp (a3)

start_rom_code:
; Detect the STDOOM Accelerator worker by polling the ready byte the RP2040 writes once
; the worker is up. D7 = 1 if available, 0 otherwise.
	move.w #STDOOM_BOOT_READY_TIMEOUT, d6
	clr.l d7
.detect_poll:
	cmpi.b #STDOOM_READY_MAGIC, (STDOOM_READY_ADDR).l
	beq.s .found
	move.w #37, -(sp)			; XBIOS Vsync — wait for next vertical blank
	trap #14
	addq.l #2, sp
	dbf d6, .detect_poll
	bra.s .detect_done
.found:
	move.l #1, d7				; Worker detected
.detect_done:

; Print a one-line status, then return to GEM.
	tst.l d7
	beq.s .no_worker
	pea msg_ready
	move.w #9, -(sp)			; GEMDOS Cconws
	trap #1
	addq.l #6, sp
	bra.s .done
.no_worker:
	pea msg_not_detected
	move.w #9, -(sp)			; GEMDOS Cconws
	trap #1
	addq.l #6, sp
.done:
	rts

msg_ready:
	dc.b "DOOM Accelerator ready",$d,$a,0
	even

msg_not_detected:
	dc.b "DOOM Accelerator not detected",$d,$a,0
	even

	even
; Non-zero, even-aligned end marker. firmware.py trims trailing zero bytes when
; generating target_firmware.h and requires the result to be an even number of
; bytes; this guards against an odd-length trim when the messages above change.
	dc.w $FFFF

end_rom_code:
end_pre_auto:
	even
	dc.l 0
