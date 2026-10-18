ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPPC)/wii_rules

TARGET      := wiicam
BUILD       := build
SOURCES     := source libinput-wii/source port/libusb-wii vendor/libuvc-wii/src
DATA        := data
INCLUDES    := include port/libusb-wii/include vendor/libuvc-wii/include

# libuvc and the Wii libusb compatibility backend are built in-tree.
LIBS        := -ljpeg -lfat -lwiikeyboard -lwiiuse -lbte -logc -lm
LIBDIRS     := $(PORTLIBS) $(CURDIR)/libinput-wii

CFLAGS      := -g -O2 -Wall -Wextra -ffunction-sections -D__wii__ \
               $(MACHDEP) $(INCLUDE)
CXXFLAGS    := $(CFLAGS)
LDFLAGS     := -g $(MACHDEP) -Wl,-Map,$(notdir $@).map,--gc-sections

# The .dol that goes into the Homebrew Channel package.
ifeq ($(NO_DOLXZ),1)
BOOT_DOL	=	$(TARGET).dol
else
BOOT_DOL	=	$(TARGET)-xz.dol
endif

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export VPATH  := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                 $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := camera.c jpeg_save.c main.c storage.c libusb_wii.c \
            ctrl.c ctrl-gen.c device.c diag.c frame.c init.c misc.c stream.c \
            $(notdir $(wildcard libinput-wii/source/*.c))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

export LD      := $(CC)
export OFILES  := $(addsuffix .o,$(BINFILES)) \
                  $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(LIBOGC_INC) \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib) -L$(LIBOGC_LIB)

.PHONY: all $(BUILD) clean run package

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr $(BUILD) $(TARGET).elf $(TARGET).dol $(TARGET)-xz.dol

run: all
	wiiload $(TARGET).dol

package: all
	@mkdir -p dist/apps/wiicam
	@cp $(BOOT_DOL) dist/apps/wiicam/boot.dol
	@cp meta.xml dist/apps/wiicam/

else

DEPENDS := $(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# Self-extracting xz-compressed copy of the .dol (FIX94's dolxz, see
# scripts/dolxz.sh). It is the default target; NO_DOLXZ=1 skips it.
#---------------------------------------------------------------------------------
ifneq ($(NO_DOLXZ),1)
$(OUTPUT)-xz.dol: $(OUTPUT).dol
	@echo compressing ... $(notdir $@)
	@sh $(dir $(OUTPUT))scripts/dolxz.sh $< $@
endif

$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
