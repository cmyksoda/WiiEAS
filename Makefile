#---------------------------------------------------------------------------------
# WiiEAS — GlobalEAS / CAR alerts on the Wii (DASDEC-style)
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPPC)/wii_rules

#---------------------------------------------------------------------------------
TARGET		:=	boot
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	source

# mbedTLS 3.6.3 for PPC, vendored in-repo (libs/mbedtls) so a bare clone
# builds — devkitPro ships no ppc-mbedtls package. Config enables
# MBEDTLS_ENTROPY_HARDWARE_ALT (see source/wii_entropy.c). Override:
#   make MBEDTLS_INC=/path/to/include MBEDTLS_LIB=/path/to/lib
MBEDTLS_INC	?=	$(CURDIR)/libs/mbedtls/include
MBEDTLS_LIB	?=	$(CURDIR)/libs/mbedtls/lib

#---------------------------------------------------------------------------------
CFLAGS		:=	-g -O2 -Wall -Wextra $(MACHDEP) $(INCLUDE)
CFLAGS		+=	-DGEKKO -DHW_RVL
CXXFLAGS	:=	$(CFLAGS)
ASFLAGS		:=	-g $(MACHDEP)
# Recursive '=', not ':=' — $@ has no value at assignment time, so ':=' wrote
# the link map to a file literally named ".map".
LDFLAGS		=	-g $(MACHDEP) -Wl,-Map,$(notdir $@).map

#---------------------------------------------------------------------------------
# Link order matters: mbedTLS first, then mad/asnd, then graphics deps,
# wiiuse before bte, then ogc.
#---------------------------------------------------------------------------------
LIBS	:=	-lmbedtls -lmbedx509 -lmbedcrypto \
		-lmad -lasnd \
		-lgrrlib -lfreetype -lpngu -lpng -ljpeg \
		-lbrotlidec -lbrotlicommon -lbz2 -lz \
		-lwiiuse -lbte -lfat -logc -lm

#---------------------------------------------------------------------------------
LIBDIRS	:=	$(PORTLIBS)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SOURCES	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(sFILES:.s=.o) $(SFILES:.S=.o)
export OFILES		:=	$(OFILES_BIN) $(OFILES_SOURCES)

export HFILES		:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(MBEDTLS_INC) \
			-I$(CURDIR)/$(BUILD) \
			-I$(LIBOGC_INC) \
			-I$(PORTLIBS_PATH)/ppc/include/freetype2

# Vendored mbedTLS first so it wins over any copy in portlibs.
export LIBPATHS	:=	-L$(MBEDTLS_LIB) \
			$(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
			-L$(LIBOGC_LIB)

DISTDIR	:=	$(CURDIR)/built/WiiEAS

.PHONY: $(BUILD) clean all install

#---------------------------------------------------------------------------------
all: $(BUILD)
	@$(MAKE) --no-print-directory install

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

# Ship boot.dol into built/WiiEAS; drop intermediate .elf and root-level .dol.
install:
	@mkdir -p $(DISTDIR)
	@if [ -f $(CURDIR)/$(TARGET).dol ]; then \
		cp -f $(CURDIR)/$(TARGET).dol $(DISTDIR)/boot.dol; \
		echo "installed $(DISTDIR)/boot.dol"; \
	fi
	@rm -f $(CURDIR)/$(TARGET).elf $(CURDIR)/$(TARGET).dol $(CURDIR)/$(TARGET).map
	@rm -f $(DISTDIR)/boot.elf $(DISTDIR)/*.elf

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).elf $(TARGET).dol $(TARGET).map

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

$(OFILES_SOURCES): $(HFILES)

# binary data -> .o + header (font.ttf -> font_ttf.h / font_ttf / font_ttf_size)
%.ttf.o	%_ttf.h :	%.ttf
	@echo $(notdir $<)
	$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
