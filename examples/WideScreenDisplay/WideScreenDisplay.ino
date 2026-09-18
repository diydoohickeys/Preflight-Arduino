// Boot UI on a short, wide panel (536x240, e.g. LilyGo T-Display AMOLED) with a custom
// theme shared by the screen and the web pages, plus the OTA page once connected.

#include <WiFiSetupManager.h>
#include <WiFiSetupBootUI.h>
#include <OTAManager.h>
#include <Logger.h>
#include <lvgl.h>

static const uint16_t SCREEN_W = 536;
static const uint16_t SCREEN_H = 240;
// Ten rows of pixels. Heap, not a static array: classic ESP32's static DRAM can't fit it beside LVGL's pool.
static const size_t DRAW_BUF_BYTES = SCREEN_W * 10 * (LV_COLOR_DEPTH / 8);

WiFiSetupBootUI bootUI;
WiFiSetupManager* wifi = nullptr;
OTAManager ota;

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

    static WiFiSetupTheme theme;
    theme.primaryColor = 0x00D9FF;
    theme.backgroundColor = 0x0A0A0A;
    theme.surfaceColor = 0x1A1A1A;
    theme.surfaceLight = 0x2A2A2A;
    theme.textColor = 0xE0E0E0;
    theme.webPrimaryColor = "#00D9FF";
    theme.webBackgroundColor = "#0A0A0A";
    theme.webTextColor = "#E0E0E0";

    // Explicit dimensions pick the compact layout (smaller fonts, tighter spacing).
    // Without them the size is read from the default LVGL display.
    bootUI.initialize("WIDE SCREEN DEVICE", &theme, SCREEN_W, SCREEN_H);

    WiFiSetupConfig config;
    config.defaultAPName = "WideScreen-Setup";
    config.defaultAPPassword = "setup123";
    config.statusCallback = &bootUI;
    config.theme = &theme;

    wifi = new WiFiSetupManager(config);
    wifi->begin();

    if (!wifi->isInSetupMode()) {
        ota.setScreenProgressCallback([](uint8_t progress, OTAManager::Stage stage) {
            Logger.debug("OTA %u%% (stage %d)", progress, static_cast<int>(stage));
        });
        ota.begin(wifi->getWebServer(), "admin", "change-me", "");
    }
}

void loop() {
    wifi->update();
    ota.loop();
    lv_timer_handler();
    delay(5);
}
