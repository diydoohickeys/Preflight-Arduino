#include "WiFiSetupBootUI.h"
#include "Logger.h"

#if __has_include(<lvgl.h>)

WiFiSetupBootUI::WiFiSetupBootUI()
    : textArea_(nullptr)
    , titleLabel_(nullptr)
    , initialized_(false)
    , screenWidth_(0)
    , screenHeight_(0)
    , primaryColor_(UI_COLOR_PRIMARY)
    , backgroundColor_(UI_COLOR_BACKGROUND)
    , surfaceColor_(UI_COLOR_SURFACE)
    , surfaceLight_(UI_COLOR_SURFACE_LIGHT)
    , textColor_(UI_COLOR_TEXT)
    , borderColor_(UI_COLOR_BORDER)
{
}

WiFiSetupBootUI::~WiFiSetupBootUI() {
    cleanup();
}

void WiFiSetupBootUI::onObjectDeleted(lv_event_t* e) {
    auto** slot = static_cast<lv_obj_t**>(lv_event_get_user_data(e));
    if (slot) {
        *slot = nullptr;
    }
}

// The delete callback holds the member's ADDRESS, so a move must re-point it at the new owner.
void WiFiSetupBootUI::rebindDeleteCallback(lv_obj_t* obj, lv_obj_t** oldSlot, lv_obj_t** newSlot) {
    if (!obj) return;
    lv_obj_remove_event_cb_with_user_data(obj, onObjectDeleted, oldSlot);
    lv_obj_add_event_cb(obj, onObjectDeleted, LV_EVENT_DELETE, newSlot);
}

WiFiSetupBootUI::WiFiSetupBootUI(WiFiSetupBootUI&& other) noexcept
    : textArea_(other.textArea_)
    , titleLabel_(other.titleLabel_)
    , initialized_(other.initialized_)
    , deviceName_(std::move(other.deviceName_))
    , screenWidth_(other.screenWidth_)
    , screenHeight_(other.screenHeight_)
    , primaryColor_(other.primaryColor_)
    , backgroundColor_(other.backgroundColor_)
    , surfaceColor_(other.surfaceColor_)
    , surfaceLight_(other.surfaceLight_)
    , textColor_(other.textColor_)
    , borderColor_(other.borderColor_)
{
    rebindDeleteCallback(textArea_, &other.textArea_, &textArea_);
    rebindDeleteCallback(titleLabel_, &other.titleLabel_, &titleLabel_);

    other.textArea_ = nullptr;
    other.titleLabel_ = nullptr;
    other.initialized_ = false;
    other.screenWidth_ = 0;
    other.screenHeight_ = 0;
}

WiFiSetupBootUI& WiFiSetupBootUI::operator=(WiFiSetupBootUI&& other) noexcept {
    if (this != &other) {
        cleanup();

        textArea_ = other.textArea_;
        titleLabel_ = other.titleLabel_;
        initialized_ = other.initialized_;
        deviceName_ = std::move(other.deviceName_);
        screenWidth_ = other.screenWidth_;
        screenHeight_ = other.screenHeight_;
        primaryColor_ = other.primaryColor_;
        backgroundColor_ = other.backgroundColor_;
        surfaceColor_ = other.surfaceColor_;
        surfaceLight_ = other.surfaceLight_;
        textColor_ = other.textColor_;
        borderColor_ = other.borderColor_;

        rebindDeleteCallback(textArea_, &other.textArea_, &textArea_);
        rebindDeleteCallback(titleLabel_, &other.titleLabel_, &titleLabel_);

        other.textArea_ = nullptr;
        other.titleLabel_ = nullptr;
        other.initialized_ = false;
        other.screenWidth_ = 0;
        other.screenHeight_ = 0;
    }
    return *this;
}

WiFiSetupBootUI::ResponsiveConfig WiFiSetupBootUI::calculateResponsiveConfig() const {
    ResponsiveConfig config;

    float aspectRatio = (float)screenWidth_ / (float)screenHeight_;

    bool isWideScreen = aspectRatio > 2.0f;
    bool isTallScreen = aspectRatio < 1.0f;

    if (screenHeight_ >= 400) {
        config.titleFont = &lv_font_montserrat_22;
        config.textFont = &lv_font_montserrat_14;
        config.titleTopMargin = 30;
        config.padding = 20;
    } else if (screenHeight_ >= 300) {
        config.titleFont = &lv_font_montserrat_18;
        config.textFont = &lv_font_montserrat_12;
        config.titleTopMargin = 20;
        config.padding = 15;
    } else {
        config.titleFont = &lv_font_montserrat_14;
        config.textFont = &lv_font_montserrat_10;
        config.titleTopMargin = 10;
        config.padding = 10;
    }

    if (isWideScreen) {
        config.textAreaYOffset = 15;
        config.textAreaWidthPercent = 92;
        config.textAreaHeightPercent = 65;
        config.borderWidth = 2;
        config.radius = 8;
    } else if (isTallScreen) {
        config.textAreaYOffset = 50;
        config.textAreaWidthPercent = 90;
        config.textAreaHeightPercent = 55;
        config.borderWidth = 3;
        config.radius = 12;
    } else {
        config.textAreaYOffset = 40;
        config.textAreaWidthPercent = 90;
        config.textAreaHeightPercent = 60;
        config.borderWidth = 3;
        config.radius = 12;
    }

    return config;
}

void WiFiSetupBootUI::applyScreenTheme() {
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(backgroundColor_), 0);
    lv_obj_set_style_bg_grad_color(lv_scr_act(), lv_color_hex(surfaceColor_), 0);
    lv_obj_set_style_bg_grad_dir(lv_scr_act(), LV_GRAD_DIR_VER, 0);
}

void WiFiSetupBootUI::createTitle(const char* title, const ResponsiveConfig& config) {
    titleLabel_ = lv_label_create(lv_scr_act());
    if (titleLabel_) {
        lv_obj_add_event_cb(titleLabel_, onObjectDeleted, LV_EVENT_DELETE, &titleLabel_);
        lv_label_set_text(titleLabel_, title);
        lv_obj_set_style_text_color(titleLabel_, lv_color_hex(primaryColor_), 0);
        lv_obj_set_style_text_font(titleLabel_, config.titleFont, 0);
        lv_obj_align(titleLabel_, LV_ALIGN_TOP_MID, 0, config.titleTopMargin);
    }
}

void WiFiSetupBootUI::createTextArea(const ResponsiveConfig& config) {
    textArea_ = lv_textarea_create(lv_scr_act());
    if (textArea_) {
        lv_obj_add_event_cb(textArea_, onObjectDeleted, LV_EVENT_DELETE, &textArea_);
        lv_textarea_set_one_line(textArea_, false);
        lv_obj_align(textArea_, LV_ALIGN_CENTER, 0, config.textAreaYOffset);
        lv_obj_set_size(textArea_, LV_PCT(config.textAreaWidthPercent), LV_PCT(config.textAreaHeightPercent));

        lv_obj_set_style_bg_color(textArea_, lv_color_hex(surfaceColor_), 0);
        lv_obj_set_style_bg_grad_color(textArea_, lv_color_hex(surfaceLight_), 0);
        lv_obj_set_style_bg_grad_dir(textArea_, LV_GRAD_DIR_VER, 0);

        lv_obj_set_style_border_width(textArea_, config.borderWidth, 0);
        lv_obj_set_style_border_color(textArea_, lv_color_hex(borderColor_), 0);
        lv_obj_set_style_border_opa(textArea_, LV_OPA_COVER, 0);

        lv_obj_set_style_radius(textArea_, config.radius, 0);
        lv_obj_set_style_text_color(textArea_, lv_color_hex(textColor_), 0);
        lv_obj_set_style_text_font(textArea_, config.textFont, 0);
        lv_obj_set_style_pad_all(textArea_, config.padding, 0);
    }
}

bool WiFiSetupBootUI::initialize(const char* title, const WiFiSetupTheme* theme,
                                 uint16_t screenWidth, uint16_t screenHeight) {
    if (initialized_) {
        return true;
    }

    if (screenWidth == 0 || screenHeight == 0) {
        screenWidth_ = LV_HOR_RES;
        screenHeight_ = LV_VER_RES;
    } else {
        screenWidth_ = screenWidth;
        screenHeight_ = screenHeight;
    }

    if (screenWidth_ == 0 || screenHeight_ == 0) {
        Logger.error("Boot UI: invalid screen dimensions");
        return false;
    }

    Logger.info("Boot UI: initializing for %dx%d display", screenWidth_, screenHeight_);

    if (theme) {
        primaryColor_ = theme->primaryColor;
        backgroundColor_ = theme->backgroundColor;
        surfaceColor_ = theme->surfaceColor;
        surfaceLight_ = theme->surfaceLight;
        textColor_ = theme->textColor;
        borderColor_ = theme->borderColor;
    }

    ResponsiveConfig config = calculateResponsiveConfig();

    applyScreenTheme();
    createTitle(title, config);
    createTextArea(config);

    if (textArea_ && titleLabel_) {
        initialized_ = true;
        Logger.info("Boot UI: ready");
        return true;
    } else {
        cleanup();
        return false;
    }
}

void WiFiSetupBootUI::addText(const char* text) {
    if (!initialized_ || !textArea_ || !text) {
        return;
    }

    lv_textarea_add_text(textArea_, text);
    lv_refr_now(lv_display_get_default());
}

void WiFiSetupBootUI::clearText() {
    if (!initialized_ || !textArea_) {
        return;
    }

    lv_textarea_set_text(textArea_, "");
}

void WiFiSetupBootUI::cleanup() {
    if (textArea_) {
        lv_obj_delete(textArea_);
        textArea_ = nullptr;
    }

    if (titleLabel_) {
        lv_obj_delete(titleLabel_);
        titleLabel_ = nullptr;
    }

    initialized_ = false;
}

void WiFiSetupBootUI::onScanStart() {
    addText("Scanning WiFi networks...\r\n");
}

void WiFiSetupBootUI::onScanComplete(int networks) {
    String text = "Found " + String(networks) + " network";
    if (networks != 1) text += "s";
    text += "\r\n";
    addText(text.c_str());
}

void WiFiSetupBootUI::onConnecting(const String& ssid) {
    String text = "Connecting to '" + ssid + "'\r\n";
    addText(text.c_str());
}

void WiFiSetupBootUI::onConnectionProgress() {
    addText(".");
}

void WiFiSetupBootUI::onConnected(IPAddress ip) {
    String text = "\r\nConnected!\r\n";
    if (deviceName_.length() > 0) {
        text += "Device: " + deviceName_ + ".local\r\n";
    }
    text += "IP: " + ip.toString() + "\r\n";
    addText(text.c_str());
    // No delay to show the IP: a status callback must not block; the caller decides how long this screen stays up.
}

void WiFiSetupBootUI::onAPMode(const String& apName, IPAddress ip) {
    String text = "AP Mode Started\r\n";
    if (deviceName_.length() > 0) {
        text += "Device: " + deviceName_ + "\r\n";
    }
    text += "Network: " + apName + "\r\n";
    text += "IP: " + ip.toString() + "\r\n";
    text += "Connect and open browser\r\n";
    addText(text.c_str());
}

#endif // __has_include(<lvgl.h>)
