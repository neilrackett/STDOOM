################################################################
#
# $Id:$
#
# $Log:$
#
CC = m68k-atari-mint-gcc

FASTCALL=0

# Add -mfastcall only when FASTCALL=1, and link the matching libm.a variant so
# the calling convention agrees between Doom and libm.
ifeq ($(FASTCALL),1)
FASTCALLFLAG=-mfastcall
LIBVARIANT=.fastcall
else
FASTCALLFLAG=
LIBVARIANT=
endif

# SFP-004 FPU library lives in the atarist-sfp004 submodule. -I makes
# #include "atari_sfp004.h" resolve; the vpath below lets the %.o rules find
# atari_sfp004.c there (OBJS still lists a flat $(OBJDIR)/atari_sfp004.o).
CFLAGS=-O3 -Wall -DNORMALUNIX -DLINUX -D__BIG_ENDIAN__ -DUSEASM $(FASTCALLFLAG) -std=c99 -fasm -Iatarist-sfp004
CFLAGS20=$(CFLAGS) -m68020-60 -mtune=68020 -msoft-float
CFLAGS2F=$(CFLAGS) -m68020-60 -mtune=68020 -m68881

vpath %.c atarist-sfp004
LDFLAGS= -Wl,-Map=$@.map
LDLIBS=./deps/68000.soft-float$(LIBVARIANT)/libm.a
LDLIBS20=./deps/68020-60.soft-float$(LIBVARIANT)/libm.a
LDLIBS2F=./deps/68020-60.68881$(LIBVARIANT)/libm.a

# subdirectory for objects
O=build
OBJDIR=$(O)/obj
BUILDDIR=$(O)/dist

EXT=.TOS
TARGET=STDOOM$(EXT)
TARGET20=STDOOM20$(EXT)
TARGET2F=STDOOM2F$(EXT)

# not too sophisticated dependency
OBJS=				\
		$(OBJDIR)/atari_c2p.o		\
		$(OBJDIR)/atari_checkcpu.o	\
		$(OBJDIR)/atari_sfp004.o	\
		$(OBJDIR)/atari_ym.o		\
		$(OBJDIR)/sidecart_c2p.o	\
		$(OBJDIR)/sidecart_md.o		\
		$(OBJDIR)/sidecart_stubs.o	\
		$(OBJDIR)/doomdef.o		\
		$(OBJDIR)/doomstat.o		\
		$(OBJDIR)/dstrings.o		\
		$(OBJDIR)/i_system.o		\
		$(OBJDIR)/i_sound.o		\
		$(OBJDIR)/i_video.o		\
		$(OBJDIR)/i_net.o			\
		$(OBJDIR)/tables.o			\
		$(OBJDIR)/f_finale.o		\
		$(OBJDIR)/f_wipe.o 		\
		$(OBJDIR)/d_main.o			\
		$(OBJDIR)/d_net.o			\
		$(OBJDIR)/d_items.o		\
		$(OBJDIR)/g_game.o			\
		$(OBJDIR)/m_menu.o			\
		$(OBJDIR)/m_misc.o			\
		$(OBJDIR)/m_argv.o  		\
		$(OBJDIR)/m_bbox.o			\
		$(OBJDIR)/m_fixed.o		\
		$(OBJDIR)/m_swap.o			\
		$(OBJDIR)/m_cheat.o		\
		$(OBJDIR)/m_random.o		\
		$(OBJDIR)/am_map.o			\
		$(OBJDIR)/p_ceilng.o		\
		$(OBJDIR)/p_doors.o		\
		$(OBJDIR)/p_enemy.o		\
		$(OBJDIR)/p_floor.o		\
		$(OBJDIR)/p_inter.o		\
		$(OBJDIR)/p_lights.o		\
		$(OBJDIR)/p_map.o			\
		$(OBJDIR)/p_maputl.o		\
		$(OBJDIR)/p_plats.o		\
		$(OBJDIR)/p_pspr.o			\
		$(OBJDIR)/p_setup.o		\
		$(OBJDIR)/p_sight.o		\
		$(OBJDIR)/p_spec.o			\
		$(OBJDIR)/p_switch.o		\
		$(OBJDIR)/p_mobj.o			\
		$(OBJDIR)/p_telept.o		\
		$(OBJDIR)/p_tick.o			\
		$(OBJDIR)/p_saveg.o		\
		$(OBJDIR)/p_user.o			\
		$(OBJDIR)/r_bsp.o			\
		$(OBJDIR)/r_data.o			\
		$(OBJDIR)/r_draw.o			\
		$(OBJDIR)/r_main.o			\
		$(OBJDIR)/r_plane.o		\
		$(OBJDIR)/r_segs.o			\
		$(OBJDIR)/r_sky.o			\
		$(OBJDIR)/r_things.o		\
		$(OBJDIR)/w_wad.o			\
		$(OBJDIR)/wi_stuff.o		\
		$(OBJDIR)/v_video.o		\
		$(OBJDIR)/st_lib.o			\
		$(OBJDIR)/st_stuff.o		\
		$(OBJDIR)/hu_stuff.o		\
		$(OBJDIR)/hu_lib.o			\
		$(OBJDIR)/s_sound.o		\
		$(OBJDIR)/z_zone.o			\
		$(OBJDIR)/info.o				\
		$(OBJDIR)/sounds.o		\
		$(OBJDIR)/i_main.o
OBJS20=$(OBJS:%.o=%.20.o)
OBJS2F=$(OBJS:%.o=%.2f.o)

all:	 $(BUILDDIR)/$(TARGET) # $(BUILDDIR)/$(TARGET20) $(BUILDDIR)/$(TARGET2F)

clean:
	rm -rf $(OBJDIR)
	rm -f $(BUILDDIR)/$(TARGET) $(BUILDDIR)/$(TARGET20) $(BUILDDIR)/$(TARGET2F) \
		$(BUILDDIR)/$(TARGET).map $(BUILDDIR)/$(TARGET20).map $(BUILDDIR)/$(TARGET2F).map

$(BUILDDIR)/$(TARGET):	$(OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDLIBS)

$(BUILDDIR)/$(TARGET20):	$(OBJS20) | $(BUILDDIR)
	$(CC) $(CFLAGS20) $(OBJS20) -o $@ $(LDLIBS20)

$(BUILDDIR)/$(TARGET2F):	$(OBJS2F) | $(BUILDDIR)
	$(CC) $(CFLAGS2F) $(OBJS2F) -o $@ $(LDLIBS2F)

$(OBJDIR)/%.2f.o:	%.c | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS2F) -c $< -o $@

$(OBJDIR)/%.20.o:	%.c | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS20) -c $< -o $@

$(OBJDIR)/%.o:	%.c | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# Assembly stubs (.S, preprocessed) for the SidecarTridge protocol. Pure 68000,
# CPU-agnostic, but per-variant rules keep the OBJS20/OBJS2F derivation working.
$(OBJDIR)/%.2f.o:	%.S | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS2F) -c $< -o $@

$(OBJDIR)/%.20.o:	%.S | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS20) -c $< -o $@

$(OBJDIR)/%.o:	%.S | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# Rule for compiling to assembly on demand
$(OBJDIR)/%.2f.s: %.c | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS2F) -c $< -S -fverbose-asm -o $@

$(OBJDIR)/%.20.s: %.c | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS20) -c $< -S -fverbose-asm -o $@
	
$(OBJDIR)/%.s: %.c | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -S -fverbose-asm -o $@

$(OBJDIR) $(BUILDDIR):
	mkdir -p $@

#############################################################
#
#############################################################
