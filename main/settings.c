#include "esp_log.h"
#include "nvs.h"
#include "settings.h"

#define NAMESPACE   "msx"

static const char *TAG = "settings";

settings_t settings;

void settings_load(void)
{
    settings.model = MSX_MODEL_MSX2;
    settings.video = 0;
    settings.floppy = 1;
    settings.joystick = 1;

    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t u8;
    if (nvs_get_u8(h, "model", &u8) == ESP_OK && u8 < MSX_MODEL_COUNT) {
        settings.model = u8;
    }
    if (nvs_get_u8(h, "video", &u8) == ESP_OK && u8 <= 1) {
        settings.video = u8;
    }
    if (nvs_get_u8(h, "floppy", &u8) == ESP_OK && u8 <= 1) {
        settings.floppy = u8;
    }
    if (nvs_get_u8(h, "joystick", &u8) == ESP_OK && (u8 == 1 || u8 == 2)) {
        settings.joystick = u8;
    }
    nvs_close(h);
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "can't open NVS");
        return;
    }
    nvs_set_u8(h, "model", settings.model);
    nvs_set_u8(h, "video", settings.video);
    nvs_set_u8(h, "floppy", settings.floppy);
    nvs_set_u8(h, "joystick", settings.joystick);
    nvs_commit(h);
    nvs_close(h);
}
