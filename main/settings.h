#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "msx.h"

/* Machine settings kept in NVS (namespace "msx") */
typedef struct {
    uint8_t model;          /* msx_model_t */
    uint8_t video;          /* 0: 50 Hz, 1: 60 Hz */
    uint8_t floppy;         /* 1: the disk ROM (floppy drives) */
    uint8_t joystick;       /* port the keypad joystick is in: 1 or 2 */
} settings_t;

extern settings_t settings;

void settings_load(void);
void settings_save(void);
