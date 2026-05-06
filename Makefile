.SUFFIXES:

ifeq ($(strip $(PSL1GHT)),)
$(error "PSL1GHT must be set in the environment.")
endif

include $(PSL1GHT)/ppu_rules

TARGET		:=	$(notdir $(CURDIR))
BUILD		:=	build
SOURCES		:=	source source/http source/ps3http source/savedata source/transport source/state source/ui source/ui/buttons source/ui/logos source/fonts source/fonts_ttf
INCLUDES	:=	include source source/savedata source/transport source/state source/http source/ps3http source/ui source/fonts_ttf

# Anchor PS3UI_DIR to this Makefile's location, not CURDIR — the inner
# `make -C build/` re-parses this file with CURDIR=build/, which would
# resolve $(CURDIR)/../ps3ui to the wrong directory.
PS3UI_DIR	:=	$(abspath $(dir $(firstword $(MAKEFILE_LIST)))/../ps3ui)

EMBEDDED_ASSETS := $(CURDIR)/source/http/embedded_assets.h
WEB_ASSETS      := $(CURDIR)/source/web/index.html $(CURDIR)/source/web/app.js

TITLE		:=	SaveSync
APPID		:=	NP0SVSYNC
CONTENTID	:=	UP0001-$(APPID)_00-0000000000000000

PS3_IP		?=	192.168.2.22
HTTP_PORT	?=	8080

ICON0		:=	$(CURDIR)/pkgfiles/ICON0.PNG
SFOXML		:=	$(CURDIR)/sfo.xml
PKGFILES	:=	$(CURDIR)/pkgfiles

CFLAGS		=	-O2 -Wall -mcpu=cell $(MACHDEP) $(INCLUDE) -DSAVESYNC_HTTP_PORT=$(HTTP_PORT)
CXXFLAGS	=	$(CFLAGS) -fno-exceptions -fno-rtti
LDFLAGS		=	$(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS		:=	-L$(PS3UI_DIR) -lps3ui \
				-lfreetype \
				-lfont3d -ltiny3d \
				-lrsx -lgcm_sys \
				-lpolarssl -ljson-c -lzip -lpng -lz \
				-Wl,--start-group \
				-lsysmodule -lsysutil -lsysfs -lio -lnet -lnetctl -llv2 -lrt \
				-Wl,--end-group \
				-lm

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT		:=	$(CURDIR)/$(TARGET)
export VPATH		:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR		:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD	:=	$(CXX)

export OFILES	:=	$(CFILES:.c=.o) $(CPPFILES:.cpp=.o) $(SFILES:.s=.o)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
					-I$(CURDIR)/$(BUILD) \
					-I$(PSL1GHT)/ppu/include \
					-I$(PS3DEV)/portlibs/ppu/include \
					-I$(PS3DEV)/portlibs/ppu/include/freetype2 \
					-I$(PS3UI_DIR)/include

export LIBPATHS	:=	-L$(PSL1GHT)/ppu/lib -L$(PS3DEV)/portlibs/ppu/lib

.PHONY: $(BUILD) clean run deploy launch pkg assets

$(BUILD): $(EMBEDDED_ASSETS)
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

# Regenerate embedded assets header when web source files change.
$(EMBEDDED_ASSETS): $(WEB_ASSETS)
	@echo "Embedding web assets..."
	@cd $(CURDIR) && bash tools/embed_assets.sh

assets: $(EMBEDDED_ASSETS)

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self $(OUTPUT).pkg $(OUTPUT).gnpdrm.pkg

run: $(BUILD)
	PS3LOAD=tcp:$(PS3_IP) ps3load $(OUTPUT).self

deploy: $(BUILD)
	curl -T $(OUTPUT).self ftp://$(PS3_IP)/dev_hdd0/tmp/$(TARGET).self

launch:
	curl 'http://$(PS3_IP)/play.ps3/dev_hdd0/tmp/$(TARGET).self'

pkg: $(BUILD) $(OUTPUT).gnpdrm.pkg

# Real-NPDRM .pkg via tools/build_pkg.sh: make_self_npdrm in an Ubuntu 22.04
# Docker container (host's glibc 2.39 segfaults it), then sfo.py + pkg.py +
# package_finalize natively. See .claude/skills/ps3-pkg/SKILL.md.
$(OUTPUT).gnpdrm.pkg: $(OUTPUT).self
	@echo "building pkg ... $(notdir $@)"
	@ELF=$(CURDIR)/build/build/$(TARGET).elf \
	 OUT_PKG=$@ \
	 STAGE=$(CURDIR)/build/pkg-stage \
	 TITLE="$(TITLE)" \
	 APPID="$(APPID)" \
	 CONTENTID="$(CONTENTID)" \
	 ICON0="$(ICON0)" \
	 PKGFILES="$(PKGFILES)" \
	 SFOXML="$(SFOXML)" \
	 bash $(CURDIR)/tools/build_pkg.sh

else

DEPENDS	:=	$(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf:	$(OFILES)

-include $(DEPENDS)

endif
