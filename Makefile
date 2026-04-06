#---------------------------------------------------------------------------------
# 3DS YouTube Player Makefile (Dependency Fix Version)
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
  $(error "Please set DEVKITPRO in your environment")
endif

ifeq ($(strip $(DEVKITARM)),)
  DEVKITARM := $(DEVKITPRO)/devkitARM
endif

PREFIX  := $(DEVKITARM)/bin/arm-none-eabi-
CC      := $(PREFIX)gcc
CXX     := $(PREFIX)g++
LD      := $(PREFIX)g++

TARGET      := streamu
VERSION     := 1.3.0
SOURCES     := source source/network source/audio source/ui source/playlist source/ui/screens
INCLUDES    := include include/network include/ui

DEVKITPRO_LIB := $(DEVKITPRO)/libctru/lib
DEVKITPRO_INC := $(DEVKITPRO)/libctru/include
PORTLIBS      := $(DEVKITPRO)/portlibs/3ds

# --- 修正: libcurl の背後で動く mbedtls と zlib を追加 ---
# 順番が非常に重要です (依存される側を後ろに置く)
LIBS    := -lcitro2d -lcitro3d -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lctru -lm

ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS  := -g -Wall -O2 -mword-relocations -fomit-frame-pointer -ffunction-sections $(ARCH) -DARM11 -D__3DS__ -DAPP_VERSION='"v$(VERSION)"'
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
LDFLAGS := -specs=3dsx.specs -g $(ARCH)

LIBDIRS := -L$(DEVKITPRO_LIB) -L$(PORTLIBS)/lib
INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
           -I$(DEVKITPRO_INC) \
           -I$(PORTLIBS)/include

# ソースファイルの探索
CPPFILES := $(foreach dir,$(SOURCES),$(wildcard $(dir)/*.cpp))
CFILES   := $(foreach dir,$(SOURCES),$(wildcard $(dir)/*.c))
OFILES   := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)

# CIA build tools
BANNERTOOL := $(DEVKITPRO)/tools/bin/bannertool
MAKEROM    := $(DEVKITPRO)/tools/bin/makerom

.PHONY: all clean cia

all: $(TARGET).3dsx $(TARGET).cia

$(TARGET).3dsx: $(TARGET).elf $(TARGET).smdh
	@$(DEVKITPRO)/tools/bin/3dsxtool $< $@ --smdh=$(TARGET).smdh
	@echo "Built: $@"

$(TARGET).elf: $(OFILES)
	$(LD) $(LDFLAGS) $(OFILES) $(LIBDIRS) $(LIBS) -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@

# CIA build
cia: $(TARGET).cia

$(TARGET).smdh:
	$(BANNERTOOL) makesmdh -s "StreaMu" -l "StreaMu" -p " " -i assets/icon_48.png -o $@

banner.bnr:
	$(BANNERTOOL) makebanner -i assets/banner_256.png -a assets/audio_banner.wav -o $@

$(TARGET).cia: $(TARGET).elf $(TARGET).smdh banner.bnr
	$(MAKEROM) -f cia -o $@ -elf $< -rsf app.rsf -icon $(TARGET).smdh -banner banner.bnr
	@echo "Built: $@"

clean:
	rm -f $(OFILES) $(TARGET).elf $(TARGET).3dsx $(TARGET).cia $(TARGET).smdh banner.bnr