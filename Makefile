.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET		:=	3drop
THREEDROP_TEST_VERBS ?= 0
# One object tree per flavor, so switching flavors never mixes their objects.
BUILD		:=	$(if $(filter 1,$(THREEDROP_TEST_VERBS)),build-bench,build)
# A release build never compiles the bench instrument.
SOURCES		:=	src $(if $(filter 1,$(THREEDROP_TEST_VERBS)),src/bench)
CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
# src/caBundle.S: the certificate bundle in res/ as bytes of the app (it has no RomFS).
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
INCLUDES	:=	src

# The Testing Tool links only these CFILES: no production networking or install code enters.
# tools/bench/makeFixture.sh builds it again when a file it is built from is newer: keep its find in step.
ifeq ($(TARGET),testingTool)
BUILD		:=	buildTestingTool
SOURCES		:=	tools/bench src src/bench
CFILES		:=	testingTool.c led.c ledRead.c
SFILES		:=
endif

# makeCIA.sh reads these; HOME Menu shows the long field, APP_DESCRIPTION, as the title's name.
APP_TITLE		:=	3Drop
APP_DESCRIPTION	:=	3Drop
APP_AUTHOR		:=	MadeOfBees

ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

# -Wno-format-zero-length: a reply with no fields formats "" (protocol.h reply()).
# -Wframe-larger-than is half the main stack, libctru's weak __stacksize__ (32768 bytes; check.sh reads
# it from the ELF), so a large buffer that loses `static` fails the build.
CFLAGS	:=	-g -Wall -Wextra -Werror -Wno-format-zero-length -O2 -mword-relocations \
			-Wframe-larger-than=16384 \
			-DTHREEDROP_TEST_VERBS=$(THREEDROP_TEST_VERBS) \
			-ffunction-sections -fdata-sections \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -D__3DS__ -std=gnu11

# The path src/caBundle.S includes the bundle from.
ASFLAGS	:=	-g $(ARCH) -DCACERT_FILE='"$(TOPDIR)/res/cacert.pem"'

# 3Drop's own title id is the one makerom stamps, read from the RSF by the host tools' own reader.
SELF_TITLE_ID	:=	$(shell bash -c '. "$(TOPDIR)/tools/lib.sh" && . "$(TOPDIR)/tools/cia.sh" && selfTitleID')
ifeq ($(strip $(SELF_TITLE_ID)),)
$(error no 3Drop title id in res/buildCIA.rsf)
endif
CFLAGS	+=	-DSELF_TITLE_ID=0x$(SELF_TITLE_ID)ULL

# tools/build.sh stamps the display version; a bare make says dev.
APP_VERSION	?=	dev
CFLAGS	+=	-DAPP_VERSION='"$(APP_VERSION)"'

LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# A static libcurl asks for mbedTLS (tls, then x509, then crypto) and zlib after it; libctru, last,
# has the sockets and sslc under them all. The QR decoder is src/quircLib.c: no library.
LIBS	:= -lcitro2d -lcitro3d -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lctru -lm

LIBDIRS	:= $(CTRULIB) $(PORTLIBS)

# The top level recurses into the object directory, whose make takes the else branch below.
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(BUILD)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

export LD	:=	$(CC)
export OFILES	:=	$(CFILES:.c=.o) $(SFILES:.S=.o)

# devkitPro puts every object in one directory: a duplicate basename would silently replace one.
ifneq ($(words $(OFILES)),$(words $(sort $(OFILES))))
$(error source files must have unique basenames: $(OFILES))
endif

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

# The 3dsx carries libctru's default icon; the CIA's comes from res/ in makeCIA.sh.
export _3DSXDEPS	:=	$(OUTPUT).smdh
export _3DSXFLAGS	+=	--smdh=$(OUTPUT).smdh

.PHONY: all clean

# The root .elf, .3dsx and .smdh are copies: 3drop's are always the flavor built last.
all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
	@cp $(OUTPUT).elf $(OUTPUT).3dsx $(OUTPUT).smdh $(CURDIR)/

$(BUILD):
	@mkdir -p $@

clean:
	@echo clean ...
	@rm -fr build build-bench buildTestingTool \
		3drop.3dsx 3drop.smdh 3drop.elf 3drop.cia \
		testingTool.3dsx testingTool.smdh testingTool.elf

else

$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)

$(OUTPUT).elf	:	$(OFILES)

# The assembler's dependency file does not list what .incbin reads.
caBundle.o	:	$(TOPDIR)/res/cacert.pem

-include $(DEPSDIR)/*.d

endif
