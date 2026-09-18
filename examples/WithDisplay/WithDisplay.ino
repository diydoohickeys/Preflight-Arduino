// Boot status on an LVGL screen while WiFi comes up. Needs LVGL 9 with the Montserrat
// 10/12/14/18/22 fonts enabled in your lv_conf.h, and your panel driver in displayFlush().

#include <WiFiSetupManager.h>
#include <WiFiSetupBootUI.h>
#include <Logger.h>
#include <lvgl.h>

static const uint16_t SCREEN_W = 800;
static const uint16_t SCREEN_H = 480;
// Ten rows of pixels. Heap, not a static array: classic ESP32's static DRAM can't fit it beside LVGL's pool.
static const size_t DRAW_BUF_BYTES = SCREEN_W * 10 * (LV_COLOR_DEPTH / 8);

WiFiSetupBootUI bootUI;
WiFiSetupManager* wifi = nullptr;

void displayFlush(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    // Push px_map (area->x1..x2, y1..y2) to your panel here.
    lv_display_flush_ready(disp);
}

void setup() {
    Serial.begin(115200);
    Logger.begin(100, /*enableSerial=*/true, /*enableWebSocket=*/false);

    // Initialise your panel hardware first.
    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return millis(); });
    lv_display_t* disp = lv_display_create(SCREEN_W, SCREEN_H);
    lv_display_set_flush_cb(disp, displayFlush);
    lv_display_set_buffers(disp, malloc(DRAW_BUF_BYTES), nullptr, DRAW_BUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);

    if (!bootUI.initialize("WIFI SETUP")) {
        Logger.error("Boot UI failed to initialise");
    }

    WiFiSetupConfig config;
    config.defaultAPName = "MyESP32-Setup";
    config.defaultAPPassword = "mypassword";
    config.statusCallback = &bootUI;

    wifi = new WiFiSetupManager(config);
    wifi->begin();   // the boot UI redraws itself as each step completes

    if (wifi->isConnected()) {
        delay(2000);   // leave the IP on screen for a moment
        bootUI.cleanup();
        lv_obj_clean(lv_screen_active());
        // Build your application UI here.
    }
    // In setup mode the boot UI stays up showing the AP name and address.
}

void loop() {
    wifi->update();
    lv_timer_handler();
    delay(5);
}
