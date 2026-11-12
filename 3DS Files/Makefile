#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

# GNU Make's $(notdir ...)/$(dir ...)/etc. are whitespace-list based, so this
# Makefile (like all devkitPro templates) cannot be invoked from a path
# containing a space -- $(notdir $(CURDIR)) silently returns garbage and the
# top-level/recursive-build detection below breaks. If this project's path
# has a space in it (at the least "3DS Files" does), build via
# tools/build_cia.ps1 (subst's a temporary space-free drive letter around the build) or
# `subst Z: "<path to 3DS Files>"` by hand, then run make from Z:\.
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# ROMFS is the directory which contains the RomFS, relative to the Makefile
# APP_TITLE/APP_DESCRIPTION/APP_AUTHOR are stored in the SMDH file
#---------------------------------------------------------------------------------
TARGET		:=	NFSU2-3DS
BUILD		:=	build
SOURCES		:=	source source/vp6
DATA		:=	data
INCLUDES	:=	include
# No ROMFS -- the CIA/.3dsx is engine-only and ships with zero game data.
# Real game assets live in an external directory on the SD card, pointed to
# by nfsu2_config.txt at runtime (see source/main.c).
ROMFS		:=

# The HOME Menu's name for the title. The installer shows the short title, but
# the HOME Menu's bubble shows the LONG description over the publisher, so with
# it empty the bubble read just "Octoblimp" on hardware (the user, 2026-11-08,
# #428). The title alone in it still showed only "Octoblimp" (#441), so it
# follows ZombieOutbreakSim-3DS, whose bubble shows on the same console: a
# description of its own, the title in it.
APP_TITLE		:= Need for Speed: Underground 2
APP_DESCRIPTION	:= Need for Speed: Underground 2 PC Game Port to Nintendo 3DS
APP_AUTHOR		:= Octoblimp
# makerom -ver packs MAJOR.MINOR.MICRO as (MAJOR << 10) | (MINOR << 4) | MICRO.
APP_VERSION_MAJOR	:= 1
APP_VERSION_MINOR	:= 0
# 0.9.1 (#441): a new version makes the HOME Menu take the new SMDH text.
# 0.9.2: the 11-09 tester build (#326, #449, #448).
# 0.10.0: the 11-11 tester build (the user: "increment the minor version number by one").
# 0.11.0: the 11-11 evening tester build (the user: "increment the version by one").
# 1.0.0: the 11-11 night tester build (the user: "I do want the version number changed
#   to 1.0.0 as this may be the final build").
# MINOR has 6 bits (0..63), MICRO 4 (0..15).
APP_VERSION_MICRO	:= 0

# The HOME Menu banner (a spinning 3D logo, CGFX), its stereo sound and the
# icon, made from the PC data by tools/make_banner.py (build_cia.ps1 runs it
# first) and committed in meta/nfsu2. Without them, the old flat placeholders.
ifneq ($(wildcard meta/nfsu2/banner.cgfx),)
BANNER_ART	:= -ci meta/nfsu2/banner.cgfx -a meta/nfsu2/banner.wav
ICON		:= meta/nfsu2/icon.png
else
BANNER_ART	:= -i meta/banner.png -a meta/silence.wav
endif
BANNER_ICON	:= $(if $(ICON),$(ICON),meta/icon.png)

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O3 -mword-relocations \
			-ffunction-sections \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -D__3DS__

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS	:=	-g $(ARCH)
# NOTE: do not raise the .3dsx stack with -Wl,--defsym,__stacksize__=... here.
# 3dsxtool reads that literal out of the pool as a relocation source and dies
# with "Relocation to invalid address!". The .3dsx keeps libctru's 32KB default;
# meta/app.rsf's StackSize covers the CIA. What actually keeps both safe is that
# the frontend loader chain no longer needs ~35KB of stack -- see texpack.c's
# s_active[] comment and loadFngScreen()'s scratch below.
# The wraps route the world's linear allocations into its own arena
# (source/linarena.h, TASKS.md #270); every other caller still reaches
# libctru's allocator through them unchanged. The fread wrap keeps the loading
# screen moving through a load (source/loadpump.h, TASKS.md #303); disarmed it
# is the real fread. The C3D_FrameBegin / C3D_FrameEnd wraps pace the
# world's frames on the present, not on a fresh vblank (source/framepace.h,
# TASKS.md #349). The fopen / stat wraps lay the data mods in GameDir/mods
# over the stock files (source/modfs.h, TASKS.md #320).
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) \
		-Wl,--wrap=linearAlloc,--wrap=linearMemAlign,--wrap=linearFree,--wrap=fread \
		-Wl,--wrap=C3D_FrameBegin,--wrap=C3D_FrameEnd \
		-Wl,--wrap=fopen,--wrap=stat

LIBS	:= -lcitro2d -lcitro3d -lctru -lz -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(CTRULIB) $(PORTLIBS)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))
PICAFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
# #352: the in-world programs share one shader blob, worldsh (source/shaders.h
# lists them; keep the order). citro3d uploads a program's code when the blob
# it comes from changes, so separate blobs paid a code upload on most program
# switches in a frame.
export SHADER_GROUP := scene scenehl scenerf carsh carenv car envmap particle post carge
PICAFILES	:=	$(filter-out $(addsuffix .v.pica,$(SHADER_GROUP)),$(PICAFILES))

#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_SOURCES 	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES)) $(PICAFILES:.v.pica=.shbin.o) worldsh.shbin.o

export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)

export HFILES	:=	$(addsuffix .h,$(subst .,_,$(BINFILES))) $(PICAFILES:.v.pica=_shbin.h) worldsh_shbin.h

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS	:=	$(if $(NO_SMDH),,$(OUTPUT).smdh)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.png)
	ifneq (,$(findstring $(TARGET).png,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).png
	else
		ifneq (,$(findstring icon.png,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.png
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
	export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

ifneq ($(ROMFS),)
	export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: all clean cia

#---------------------------------------------------------------------------------
all: $(BUILD) $(DEPSDIR)
	@$(MAKE) --no-print-directory -C "$(BUILD)" -f "$(CURDIR)/Makefile"

$(BUILD):
	@mkdir -p "$@"

ifneq ($(DEPSDIR),$(BUILD))
$(DEPSDIR):
	@mkdir -p "$@"
endif

#---------------------------------------------------------------------------------
# CIA packaging. makerom/bannertool aren't in devkitPro's package repos, so
# prebuilt binaries live in tools/3ds-bin (see HANDOFF.md). Requires
# meta/banner.png, meta/icon.png, meta/silence.wav (bannertool) plus
# meta/app.rsf (makerom).
#---------------------------------------------------------------------------------
MAKEROM		:= $(TOPDIR)/../tools/3ds-bin/makerom.exe
BANNERTOOL	:= $(TOPDIR)/../tools/3ds-bin/bannertool.exe

cia: all
	@"$(BANNERTOOL)" makebanner $(BANNER_ART) -o meta/banner.bnr
	@"$(BANNERTOOL)" makesmdh -s "$(APP_TITLE)" -l "$(if $(APP_DESCRIPTION),$(APP_DESCRIPTION), )" -p "$(APP_AUTHOR)" -i $(BANNER_ICON) -o meta/icon.icn
	@"$(MAKEROM)" -f cia -o "$(TARGET).cia" -DAPP_ENCRYPTED=false -target t \
		-elf "$(OUTPUT).elf" -icon meta/icon.icn -banner meta/banner.bnr \
		-ver $$(( ($(APP_VERSION_MAJOR) << 10) | ($(APP_VERSION_MINOR) << 4) | $(APP_VERSION_MICRO) )) \
		-rsf meta/app.rsf
	@echo built ... $(TARGET).cia

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr "$(BUILD)" "$(TARGET).3dsx" "$(OUTPUT).smdh" "$(TARGET).elf" "$(TARGET).cia" meta/banner.bnr meta/icon.icn

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)

$(OFILES_SOURCES) : $(HFILES)

$(OUTPUT).elf	:	$(OFILES)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

#---------------------------------------------------------------------------------
# citro2d .bcfnt glyph atlas (see data/conduit.bcfnt, generated from the
# user-supplied Conduit ITC Medium Italic.otf via devkitPro's mkbcfnt).
#---------------------------------------------------------------------------------
%.bcfnt.o	%_bcfnt.h :	%.bcfnt
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

#---------------------------------------------------------------------------------
# PICA200 shader compilation (picasso + bin2s), same pattern as devkitPro's
# own citro3d examples (graphics/gpu/*/Makefile) -- .v.pica compiles straight
# to .shbin.o + _shbin.h in one step, skipping an intermediate .shbin Make
# target (which conflicts with 3ds_rules' own %.shbin pattern rule and
# produces a bogus "circular dependency" from make).
#---------------------------------------------------------------------------------
define shader-as
	$(eval CURBIN := $*.shbin)
	echo "$(CURBIN).o: $< $1" > $(DEPSDIR)/$*.shbin.d
	echo "extern const u8" `(echo $(CURBIN) | tr . _)`"_end[];" > `(echo $(CURBIN) | tr . _)`.h
	echo "extern const u8" `(echo $(CURBIN) | tr . _)`"[];" >> `(echo $(CURBIN) | tr . _)`.h
	echo "extern const u32" `(echo $(CURBIN) | tr . _)`_size";" >> `(echo $(CURBIN) | tr . _)`.h
	picasso -o $(CURBIN) $1
	bin2s $(CURBIN) | $(AS) -o $*.shbin.o
endef

%.shbin.o %_shbin.h : %.v.pica
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(call shader-as,$<)

# The shared blob (#352): every SHADER_GROUP source, in that order, one DVLE each.
worldsh.shbin.o worldsh_shbin.h : $(addsuffix .v.pica,$(SHADER_GROUP))
#---------------------------------------------------------------------------------
	@echo worldsh.shbin
	@echo "extern const u8 worldsh_shbin_end[];" > worldsh_shbin.h
	@echo "extern const u8 worldsh_shbin[];" >> worldsh_shbin.h
	@echo "extern const u32 worldsh_shbin_size;" >> worldsh_shbin.h
	@picasso -o worldsh.shbin $^
	@bin2s worldsh.shbin | $(AS) -o worldsh.shbin.o

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
