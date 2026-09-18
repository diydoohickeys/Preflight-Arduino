#ifndef WIFI_SETUP_BOOT_UI_H
#define WIFI_SETUP_BOOT_UI_H

// Compiled only when <lvgl.h> is on the include path, so LVGL stays an optional dependency.
#if __has_include(<lvgl.h>)
#define WIFISETUP_HAS_LVGL 1

#include "WiFiSetupManager.h"
#include <lvgl.h>

// Default colours (0xRRGGBB); define any of these before including to override.
#ifndef UI_COLOR_PRIMARY
#define UI_COLOR_PRIMARY        0x4A90E2
#endif

#ifndef UI_COLOR_BACKGROUND
#define UI_COLOR_BACKGROUND     0x121212
#endif

#ifndef UI_COLOR_SURFACE
#define UI_COLOR_SURFACE        0x1E1E1E
#endif

#ifndef UI_COLOR_SURFACE_LIGHT
#define UI_COLOR_SURFACE_LIGHT  0x2A2A2A
#endif

#ifndef UI_COLOR_TEXT
#define UI_COLOR_TEXT           0xE0E0E0
#endif

#ifndef UI_COLOR_BORDER
#define UI_COLOR_BORDER         0x444444
#endif

// Set as WiFiSetupConfig::statusCallback to show setup progress on the active LVGL screen.
class WiFiSetupBootUI : public WiFiStatusCallback {
public:
    WiFiSetupBootUI();

    ~WiFiSetupBootUI();

    WiFiSetupBootUI(const WiFiSetupBootUI&) = delete;
    WiFiSetupBootUI& operator=(const WiFiSetupBootUI&) = delete;

    WiFiSetupBootUI(WiFiSetupBootUI&& other) noexcept;
    WiFiSetupBootUI& operator=(WiFiSetupBootUI&& other) noexcept;

    // 0 width/height = LVGL's display resolution; nullptr theme = the UI_COLOR_* defaults.
    bool initialize(const char* title = "ESP32 WIFI SETUP",
                   const WiFiSetupTheme* theme = nullptr,
                   uint16_t screenWidth = 0,
                   uint16_t screenHeight = 0);

    // Appends verbatim and forces a redraw (lv_refr_now).
    void addText(const char* text);

    void clearText();

    bool isInitialized() const { return initialized_; }

    // Deletes the widgets; the destructor also calls this.
    void cleanup();

    // Shown in the connected ("<name>.local") and AP messages.
    void setDeviceName(const String& deviceName) { deviceName_ = deviceName; }

    void onScanStart() override;
    void onScanComplete(int networks) override;
    void onConnecting(const String& ssid) override;
    void onConnectionProgress() override;
    void onConnected(IPAddress ip) override;
    void onAPMode(const String& apName, IPAddress ip) override;

private:
    lv_obj_t* textArea_;
    lv_obj_t* titleLabel_;
    bool initialized_;
    String deviceName_;

    uint16_t screenWidth_;
    uint16_t screenHeight_;

    uint32_t primaryColor_;
    uint32_t backgroundColor_;
    uint32_t surfaceColor_;
    uint32_t surfaceLight_;
    uint32_t textColor_;
    uint32_t borderColor_;

    struct ResponsiveConfig {
        const lv_font_t* titleFont;
        const lv_font_t* textFont;
        int16_t titleTopMargin;
        int16_t textAreaYOffset;
        int16_t textAreaWidthPercent;
        int16_t textAreaHeightPercent;
        int16_t padding;
        int16_t borderWidth;
        int16_t radius;
    };

    ResponsiveConfig calculateResponsiveConfig() const;

    void applyScreenTheme();

    // Nulls the member on LVGL's delete event, so cleanup() never frees an object a consumer already deleted.
    static void onObjectDeleted(lv_event_t* e);
    static void rebindDeleteCallback(lv_obj_t* obj, lv_obj_t** oldSlot, lv_obj_t** newSlot);
    void createTitle(const char* title, const ResponsiveConfig& config);

    void createTextArea(const ResponsiveConfig& config);
};

#endif // __has_include(<lvgl.h>)

#endif // WIFI_SETUP_BOOT_UI_H
