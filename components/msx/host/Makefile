# Host build of the MSX core: make -C components/msx/host
#   build/msx_host [frames] [out.ppm]   (see host_main.c)
# The Z80 core is esp32emu's, with its MSX memory (8 KB banks, $FFFF)
MSX       := ..
Z80       := ../../../../esp32emu/components/z80
EMUDBG    := ../../../../esp32emu/components/emudbg
DISKSPACE := ../../../../esp32emu/components/diskspace
BUILD     := build
CFLAGS    := -O2 -g -Wall -Wextra -Wno-unused-parameter -DMSX_HOST_HOOK \
             -DCONFIG_Z80_MSX=1 -DCONFIG_Z80_READ_SPECIAL=1 \
             -I$(MSX)/include -I$(MSX) -I$(Z80)/include -I$(EMUDBG)/include -I$(DISKSPACE)/include

MSX_SRCS  := $(wildcard $(MSX)/*.c)
MSX_OBJS  := $(patsubst $(MSX)/%.c,$(BUILD)/msx/%.o,$(MSX_SRCS)) $(BUILD)/z80.o $(BUILD)/z80_dbg.o $(BUILD)/emudbg.o $(BUILD)/emudbg_posix.o \
             $(BUILD)/diskspace_fat.o

all: $(BUILD)/msx_host

$(BUILD)/msx_host: host_main.c $(MSX_OBJS)
	$(CC) $(CFLAGS) $^ -lm -lpthread -o $@

$(BUILD)/msx/%.o: $(MSX)/%.c $(wildcard $(MSX)/*.h $(MSX)/include/*.h) $(Z80)/include/z80.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(Z80)/%.c $(Z80)/include/z80.h $(EMUDBG)/include/emudbg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(EMUDBG)/%.c $(EMUDBG)/include/emudbg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/diskspace_fat.o: $(DISKSPACE)/fat.c $(DISKSPACE)/include/diskspace.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
