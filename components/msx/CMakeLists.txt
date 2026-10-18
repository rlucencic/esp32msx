# MSX1 / MSX2, platform independent (see include/msx.h); the Z80 is esp32emu's
idf_component_register(SRCS "msx.c" "vdp.c" "psg.c" "fdc.c" "ide.c" "cart.c" "rtc.c"
                       INCLUDE_DIRS "include"
                       REQUIRES z80)

# Runs in the emulation loop: jump tables for the switches (the IDF turns
# them off for code that may run with the flash cache disabled; the
# emulation task never does: flash writes stall it)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fjump-tables -ftree-switch-conversion)
