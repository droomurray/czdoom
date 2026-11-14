# czdoom - Doom for the M5Stack CardputerZero
#
#   make            build ./build/czdoom
#   make deb        build dist/czdoom_<version>_arm64.deb
#   make NOSOUND=1  build without SDL2 (no sound, no extra dependencies)
#
# Cross-compiling: make CROSS_COMPILE=aarch64-linux-gnu- PKG_CONFIG=aarch64-linux-gnu-pkg-config

VERSION            ?= 0.1.0
DOOMGENERIC_REPO   ?= https://github.com/ozkl/doomgeneric.git
DOOMGENERIC_COMMIT ?= e257a09527062bfb986b6e5e0780839b6cbd5b71

CROSS_COMPILE ?=
CC            := $(CROSS_COMPILE)gcc
PKG_CONFIG    ?= pkg-config

VENDOR := vendor/doomgeneric
DG     := $(VENDOR)/doomgeneric
BUILD  := build
OUT    := $(BUILD)/czdoom

CFLAGS  ?= -O2 -g0
CFLAGS  += -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -I$(DG) -w
LDLIBS  += -lm

ENGINE := dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop \
	d_main d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus \
	i_endoom i_joystick i_scale i_sound i_system i_timer memio m_argv m_bbox \
	m_cheat m_config m_controls m_fixed m_menu m_misc m_random p_ceilng p_doors \
	p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr \
	p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data \
	r_draw r_main r_plane r_segs r_sky r_things sha1 sounds statdump st_lib \
	st_stuff s_sound tables v_video wi_stuff w_checksum w_file w_main w_wad \
	z_zone w_file_stdc i_input i_video doomgeneric

ifndef NOSOUND
ENGINE  += mus2mid i_sdlmusic i_sdlsound
CFLAGS  += -DFEATURE_SOUND $(shell $(PKG_CONFIG) --cflags sdl2 SDL2_mixer)
LDLIBS  += $(shell $(PKG_CONFIG) --libs sdl2 SDL2_mixer)
endif

OBJS := $(addprefix $(BUILD)/engine/,$(addsuffix .o,$(ENGINE))) $(BUILD)/doomgeneric_cardputerzero.o

.PHONY: all deb clean distclean
all: $(OUT)

# The engine is fetched at a pinned commit rather than copied into this repo.
$(VENDOR)/.fetched:
	rm -rf $(VENDOR)
	git init -q $(VENDOR)
	git -C $(VENDOR) fetch -q --depth 1 $(DOOMGENERIC_REPO) $(DOOMGENERIC_COMMIT)
	git -C $(VENDOR) checkout -q FETCH_HEAD
	touch $@

$(BUILD)/engine/%.o: $(VENDOR)/.fetched
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $(DG)/$*.c -o $@

$(BUILD)/doomgeneric_cardputerzero.o: src/doomgeneric_cardputerzero.c $(VENDOR)/.fetched
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -W -Wall -c $< -o $@

$(OUT): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $@ $(LDLIBS)

deb: $(OUT)
	VERSION=$(VERSION) STRIP=$(CROSS_COMPILE)strip NOSOUND=$(NOSOUND) packaging/mkdeb.sh $(OUT) dist

clean:
	rm -rf $(BUILD) dist

distclean: clean
	rm -rf vendor
