#include "WiFiSetupManager.h"
#include "Logger.h"
#include "html/setup_html.h"
#include "html/wifi_saved_html.h"
#include "html/factory_reset_html.h"
#include "html/factory_reset_confirm_html.h"
#include "html/style_css.h"
#include "html/theme_js.h"
#include "SameOrigin.h"
#include <esp_ota_ops.h>

static bool runningImagePendingVerify() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    return running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

WiFiSetupManager::WiFiSetupManager(const WiFiSetupConfig& config)
    : config_(config)
    , hostName_("esp32-device")
    , initialized_(false)
    , webServer_(nullptr)
    , stateMutex_(nullptr)
    , scanCompleted_(false)
    , lastScanMs_(0)
    , restartRequested_(false)
    , restartTime_(0)
    , scanRequested_(false)
    , staWantsConnection_(false)
    , lastReconnectMs_(0)
    , reconnectFailures_(0)
    , firmwarePending_(false)
    , beginMs_(0)
{
    stateMutex_ = xSemaphoreCreateMutex();
    webServer_ = new AsyncWebServer(config_.webServerPort);
}

WiFiSetupManager::~WiFiSetupManager() {
    if (webServer_) {
        webServer_->end();
        delete webServer_;
        webServer_ = nullptr;
    }
    dnsServer_.stop();
    if (stateMutex_) {
        vSemaphoreDelete(stateMutex_);
        stateMutex_ = nullptr;
    }
}

void WiFiSetupManager::begin() {
    if (initialized_) {
        return;
    }

    beginMs_ = millis();
    firmwarePending_ = runningImagePendingVerify();
    if (firmwarePending_) {
        Logger.warning("New firmware on probation: kept once the device is reachable, rolled back otherwise");
    }

    setupWebServer();

    loadConfiguration();

    bool hasConfig = false;
    {
        Preferences prefs;
        if (prefs.begin(config_.preferencesNamespace.c_str(), true)) {
            hasConfig = prefs.isKey(config_.deviceNameKey.c_str());
            prefs.end();
        }
    }

    // Scan only when the portal will show the list: WiFi.begin() scans internally, so a
    // scan before connecting would add seconds to every boot.
    if (!hasConfig || hostName_.isEmpty()) {
        scanNetworks();
        startAPMode();
    } else {
        if (!connectToNetwork()) {
            scanNetworks();
            startAPMode();
        } else {
            webServer_->begin();
        }
    }

    initialized_ = true;
    superviseFirmware();
}

void WiFiSetupManager::update() {
    if (!initialized_) {
        return;
    }

    superviseFirmware();

    if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
        handleDNS();
    }

    // Cleared after the scan: polls that arrive during it are answered by its result.
    if (scanRequested_) {
        scanNetworks();
        scanRequested_ = false;
    }

    if (restartRequested_ && (millis() - restartTime_) > RESTART_DELAY_MS) {
        Logger.warning("Restarting");
        ESP.restart();
    }

    if (staWantsConnection_ && !isInSetupMode() && WiFi.status() != WL_CONNECTED) {
        unsigned long now = millis();
        unsigned long backoff = RECONNECT_BASE_MS;
        for (uint8_t i = 0; i < reconnectFailures_ && backoff < RECONNECT_MAX_MS; i++) {
            backoff *= 2;
        }
        if (backoff > RECONNECT_MAX_MS) backoff = RECONNECT_MAX_MS;

        if (now - lastReconnectMs_ >= backoff) {
            lastReconnectMs_ = now;
            if (reconnectFailures_ < 255) reconnectFailures_++;
            Logger.info("Reconnecting to %s (attempt %u)", wifiSsid_.c_str(),
                        (unsigned)reconnectFailures_);
            WiFi.reconnect();
        }
    } else if (staWantsConnection_ && WiFi.status() == WL_CONNECTED) {
        reconnectFailures_ = 0;
    }
}

void WiFiSetupManager::superviseFirmware() {
    if (!firmwarePending_) {
        return;
    }

    // Reachable is enough: the setup portal and OTA page can repair a device that can't join WiFi.
    if (isConnected() || isInSetupMode()) {
        firmwarePending_ = false;
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            Logger.info("New firmware confirmed");
        } else {
            Logger.error("Confirming new firmware failed: %s", esp_err_to_name(err));
        }
        return;
    }

    if (config_.rollbackTimeoutMs && millis() - beginMs_ >= config_.rollbackTimeoutMs) {
        firmwarePending_ = false;
        Logger.error("New firmware never became reachable - rolling back");
        // Returns only on failure (no previous valid image).
        esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
        Logger.error("Rollback failed: %s", esp_err_to_name(err));
    }
}

bool WiFiSetupManager::isConnected() const {
    return WiFi.status() == WL_CONNECTED;
}

String WiFiSetupManager::getIPAddress() const {
    if (isConnected()) {
        return WiFi.localIP().toString();
    } else if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
        return WiFi.softAPIP().toString();
    }
    return "0.0.0.0";
}

bool WiFiSetupManager::isInSetupMode() const {
    return WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA;
}

String WiFiSetupManager::getScannedNetworks() const {
    String copy;
    if (stateMutex_ && xSemaphoreTake(stateMutex_, portMAX_DELAY) == pdTRUE) {
        copy = scannedNetworks_;
        xSemaphoreGive(stateMutex_);
    }
    return copy;
}

void WiFiSetupManager::recordScan(bool succeeded, const String& networks) {
    if (stateMutex_ && xSemaphoreTake(stateMutex_, portMAX_DELAY) == pdTRUE) {
        if (succeeded) scannedNetworks_ = networks;
        scanCompleted_ = true;
        lastScanMs_ = millis();
        xSemaphoreGive(stateMutex_);
    }
}

// In setup mode a scan never goes stale: scanning while the AP serves the phone hops
// channels, or fails outright on an AP-only radio.
bool WiFiSetupManager::getFreshScan(String& networks) const {
    const bool setupMode = isInSetupMode();
    bool fresh = false;
    if (stateMutex_ && xSemaphoreTake(stateMutex_, portMAX_DELAY) == pdTRUE) {
        networks = scannedNetworks_;
        fresh = scanCompleted_ && (setupMode || (millis() - lastScanMs_) < SCAN_MAX_AGE_MS);
        xSemaphoreGive(stateMutex_);
    }
    return fresh;
}

void WiFiSetupManager::scanNetworks() {
    if (config_.statusCallback) {
        config_.statusCallback->onScanStart();
    }

    int n = WiFi.scanNetworks();

    // Negative n is WIFI_SCAN_FAILED / WIFI_SCAN_RUNNING, not a count.
    if (n < 0) {
        Logger.warning("WiFi scan failed (%d)", n);
        recordScan(false);
        n = 0;
    } else {
        String list;
        for (int i = 0; i < n; ++i) {
            if (i > 0) list += ",";
            list += listEscape(WiFi.SSID(i)) + "|" + String(WiFi.RSSI(i));
            list += WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "|0" : "|1";
        }
        recordScan(true, list);
    }

    if (config_.statusCallback) {
        config_.statusCallback->onScanComplete(n);
    }
}

void WiFiSetupManager::loadConfiguration() {
    Preferences prefs;
    if (!prefs.begin(config_.preferencesNamespace.c_str(), true)) {
        return;
    }

    if (prefs.isKey(config_.deviceNameKey.c_str())) {
        hostName_ = prefs.getString(config_.deviceNameKey.c_str(), "esp32-device");
        wifiSsid_ = prefs.getString(config_.ssidKey.c_str(), "");
        wifiPassword_ = prefs.getString(config_.passwordKey.c_str(), "");
    }

    prefs.end();
}

bool WiFiSetupManager::connectToNetwork() {
    if (wifiSsid_.isEmpty()) {
        Logger.warning("No SSID configured");
        return false;
    }

    if (config_.statusCallback) {
        config_.statusCallback->onConnecting(wifiSsid_);
    }

    // mode() before setHostname(): on a stopped netif the DHCP hostname can silently
    // fall back to the default, depending on the arduino-esp32 version.
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(hostName_.c_str());
    WiFi.setAutoReconnect(true);
    WiFi.begin(wifiSsid_.c_str(), wifiPassword_.c_str());

    Logger.info("Connecting to WiFi...");

    uint8_t attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < config_.maxConnectionAttempts) {
        if (config_.statusCallback) {
            config_.statusCallback->onConnectionProgress();
        }

        delay(config_.connectionTimeout);
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (MDNS.begin(hostName_.c_str())) {
            MDNS.addService("http", "tcp", config_.webServerPort);
            Logger.info("mDNS started: %s.local", hostName_.c_str());
        } else {
            Logger.warning("mDNS failed to start");
        }

        if (config_.statusCallback) {
            config_.statusCallback->onConnected(WiFi.localIP());
        }

        // Arms update()'s reconnect supervision.
        staWantsConnection_ = true;
        reconnectFailures_ = 0;

        Logger.info("Connected to %s", wifiSsid_.c_str());
        Logger.info("IP address: %s", WiFi.localIP().toString().c_str());
        return true;
    } else {
        Logger.error("WiFi connection failed!");
        return false;
    }
}

void WiFiSetupManager::startAPMode() {
    Logger.info("Starting Access Point Mode");

    staWantsConnection_ = false;

    WiFi.mode(WIFI_AP);
    WiFi.softAP(config_.defaultAPName.c_str(), config_.defaultAPPassword.c_str());

    dnsServer_.start(config_.dnsPort, "*", WiFi.softAPIP());

    // AP mode only: in STA mode the catch-all would turn the consumer's 404s into
    // redirects to /setup.
    registerCaptivePortalRoutes();

    webServer_->begin();

    if (config_.statusCallback) {
        config_.statusCallback->onAPMode(config_.defaultAPName, WiFi.softAPIP());
    }

    Logger.info("AP Name: %s", config_.defaultAPName.c_str());
    Logger.info("Password: %s", config_.defaultAPPassword.c_str());
    Logger.info("IP address: %s", WiFi.softAPIP().toString().c_str());
}

void WiFiSetupManager::registerCaptivePortalRoutes() {
    if (!webServer_) {
        return;
    }

    webServer_->on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->redirect("/setup");
    });

    webServer_->onNotFound([](AsyncWebServerRequest* request) {
        request->redirect("/setup");
    });
}

void WiFiSetupManager::handleDNS() {
    dnsServer_.processNextRequest();
}

void WiFiSetupManager::factoryReset() {
    Logger.warning("Factory reset - clearing WiFi settings");

    Preferences prefs;
    if (prefs.begin(config_.preferencesNamespace.c_str(), false)) {
        prefs.clear();
        prefs.end();
    }

    requestRestart();
}

// The time is stored before the flag: update() reads them in the other order.
void WiFiSetupManager::requestRestart() {
    restartTime_ = millis();
    restartRequested_ = true;
}

void WiFiSetupManager::setupWebServer() {
    if (!webServer_) {
        return;
    }

    // OS captive-portal probe URLs.
    webServer_->on("/generate_204", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->redirect("/setup");
    });
    webServer_->on("/fwlink", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->redirect("/setup");
    });
    webServer_->on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->redirect("/setup");
    });
    webServer_->on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->redirect("/setup");
    });
    webServer_->on("/redirect", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->redirect("/setup");
    });

    webServer_->on("/setup", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "text/html", SETUP_HTML);
    });

    webServer_->on("/wifi-setup-style.css", HTTP_GET, [this](AsyncWebServerRequest* request) {
        String css = STYLE_CSS;

        // Appended after the static sheet so the overrides win on equal specificity.
        if (config_.theme) {
            String overrides = "\n:root {\n";

            if (config_.theme->webPrimaryColor.length() > 0) {
                overrides += "  --accent: " + config_.theme->webPrimaryColor + ";\n";
            }
            if (config_.theme->webPrimaryDark.length() > 0) {
                overrides += "  --accent-dark: " + config_.theme->webPrimaryDark + ";\n";
            }
            if (config_.theme->webBackgroundColor.length() > 0) {
                overrides += "  --bg: " + config_.theme->webBackgroundColor + ";\n";
            }
            if (config_.theme->webSurfaceColor.length() > 0) {
                overrides += "  --card: " + config_.theme->webSurfaceColor + ";\n";
            }
            if (config_.theme->webTextColor.length() > 0) {
                overrides += "  --text: " + config_.theme->webTextColor + ";\n";
            }
            if (config_.theme->webTextSecondary.length() > 0) {
                overrides += "  --text-dim: " + config_.theme->webTextSecondary + ";\n";
            }
            if (config_.theme->webBorderColor.length() > 0) {
                overrides += "  --border: " + config_.theme->webBorderColor + ";\n";
            }

            for (const auto& var : config_.theme->cssVariables) {
                overrides += "  --" + var.first + ": " + var.second + ";\n";
            }

            overrides += "}\n";

            // The select arrow is an SVG data URL: its fill needs its own override, with
            // the '#' percent-encoded or it terminates the URL.
            if (config_.theme->webPrimaryColor.length() > 0) {
                String color = config_.theme->webPrimaryColor;
                color.replace("#", "%23");
                overrides += "select { background-image: url(\"data:image/svg+xml,%3csvg xmlns='http://www.w3.org/2000/svg' fill='" + color + "' viewBox='0 0 16 16'%3e%3cpath d='M8 11L3 6h10z'/%3e%3c/svg%3e\"); }\n";
            }

            if (config_.theme->customCSS.length() > 0) {
                overrides += config_.theme->customCSS + "\n";
            }

            css += overrides;
        }

        request->send(200, "text/css", css);
    });

    webServer_->on("/wifi-setup-theme.js", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "text/javascript", THEME_JS);
    });

    webServer_->on("/get-networks", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleGetNetworks(request);
    });

    webServer_->on("/get-current-settings", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleGetCurrentSettings(request);
    });

    webServer_->on("/save-wifi", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleSaveWiFi(request);
    });

    // GET is the confirmation page, never the action.
    webServer_->on("/factory-reset", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "text/html", FACTORY_RESET_CONFIRM_HTML);
    });

    webServer_->on("/factory-reset", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleFactoryResetRequest(request);
    });
}

String WiFiSetupManager::jsonEscape(const String& in) {
    static const char* kHex = "0123456789abcdef";
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++) {
        unsigned char c = (unsigned char)in[i];
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += kHex[(c >> 4) & 0xF];
                    out += kHex[c & 0xF];
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

String WiFiSetupManager::htmlEscape(const String& in) {
    String out;
    out.reserve(in.length());
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default:   out += c;
        }
    }
    return out;
}

// ',' and '|' delimit the network list but are legal in an SSID; the page unescapes.
String WiFiSetupManager::listEscape(const String& in) {
    String out;
    out.reserve(in.length());
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '\\' || c == ',' || c == '|') out += '\\';
        out += c;
    }
    return out;
}

void WiFiSetupManager::handleGetNetworks(AsyncWebServerRequest* request) {
    // Cold on a normal STA boot (the boot scan is lazy); update() runs the scan and the
    // page polls on 202.
    String networks;
    if (!getFreshScan(networks)) {
        scanRequested_ = true;
        request->send(202, "text/plain", "SCANNING");
        return;
    }

    request->send(200, "text/plain", networks);
}

void WiFiSetupManager::handleGetCurrentSettings(AsyncWebServerRequest* request) {
    String networkName;
    String wifiSSID;
    bool hasSettings = false;
    bool hasPassword = false;

    // A Preferences handle per use: this is the AsyncTCP task, and a shared one's end()
    // would close the handle mid-read on the app task.
    Preferences prefs;
    if (prefs.begin(config_.preferencesNamespace.c_str(), true)) {
        hasSettings = prefs.isKey(config_.deviceNameKey.c_str());
        networkName = prefs.getString(config_.deviceNameKey.c_str(), "");
        wifiSSID = prefs.getString(config_.ssidKey.c_str(), "");
        hasPassword = prefs.isKey(config_.passwordKey.c_str()) &&
                      !prefs.getString(config_.passwordKey.c_str(), "").isEmpty();
        prefs.end();
    }

    // Whether a password is stored, never the password itself.
    String json = "{";
    json += "\"hasSettings\":" + String(hasSettings ? "true" : "false") + ",";
    json += "\"networkName\":\"" + jsonEscape(networkName) + "\",";
    json += "\"wifiSSID\":\"" + jsonEscape(wifiSSID) + "\",";
    json += "\"hasPassword\":" + String(hasPassword ? "true" : "false");
    json += "}";

    request->send(200, "application/json", json);
}

void WiFiSetupManager::handleSaveWiFi(AsyncWebServerRequest* request) {
    if (!wifisetup::isSameOrigin(request)) {
        wifisetup::sendCrossOriginRefused(request);
        return;
    }

    String networkName = "";
    String wifiNetwork = "";
    String wifiPassword = "";

    if (request->hasParam("network_name", true)) {
        networkName = request->getParam("network_name", true)->value();
    }
    if (request->hasParam("wifi_network", true)) {
        wifiNetwork = request->getParam("wifi_network", true)->value();
    }
    if (request->hasParam("wifi_password", true)) {
        wifiPassword = request->getParam("wifi_password", true)->value();
    }

    // mDNS labels are lowercase [a-z0-9-] and a space breaks resolution:
    // "My Device" becomes "my-device".
    networkName.trim();
    networkName.toLowerCase();
    {
        String clean;
        clean.reserve(networkName.length());
        for (size_t i = 0; i < networkName.length(); i++) {
            char c = networkName[i];
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) clean += c;
            else clean += '-';
        }
        networkName = clean;
    }

    // Never store an unusable value: begin() treats any stored device name as
    // configured, so the device would fall back to AP mode on every boot, unexplained.
    const char* reason = nullptr;
    if (networkName.isEmpty())                          reason = "Device name is required";
    else if (networkName.length() > HOSTNAME_MAX_BYTES)   reason = "Device name is too long";
    else if (wifiNetwork.isEmpty())                     reason = "WiFi network is required";
    else if (wifiNetwork.length() > SSID_MAX_BYTES)       reason = "WiFi network name is too long (max 32 bytes)";
    else if (wifiPassword.length() > PASSWORD_MAX_BYTES)  reason = "WiFi password is too long (max 63 bytes)";
    if (reason) {
        Logger.warning("Rejected WiFi settings: %s", reason);
        request->send(400, "text/plain", reason);
        return;
    }

    bool saved = false;
    {
        Preferences prefs;
        if (prefs.begin(config_.preferencesNamespace.c_str(), false)) {
            // The page leaves the field blank to keep the saved password of the saved network.
            const bool keepPassword = wifiPassword.isEmpty() && prefs.isKey(config_.ssidKey.c_str()) &&
                                      wifiNetwork == prefs.getString(config_.ssidKey.c_str(), "");
            saved = prefs.putString(config_.deviceNameKey.c_str(), networkName) > 0
                 && prefs.putString(config_.ssidKey.c_str(), wifiNetwork) > 0;
            // putString returns 0 for an empty (open-network) password, so it cannot
            // be part of the success test.
            if (!keepPassword) {
                prefs.putString(config_.passwordKey.c_str(), wifiPassword);
            }
            prefs.end();
        }
    }
    if (!saved) {
        Logger.error("Failed to store WiFi settings");
        request->send(500, "text/plain", "Could not save settings to flash");
        return;
    }

    Logger.info("WiFi settings saved: Host=%s, SSID=%s", networkName.c_str(), wifiNetwork.c_str());

    // Escaped before templating: both values are user-supplied and land in HTML.
    String html = WIFI_SAVED_HTML;
    html.replace("%NETWORK_NAME%", htmlEscape(networkName));
    html.replace("%WIFI_NETWORK%", htmlEscape(wifiNetwork));

    request->send(200, "text/html", html);

    requestRestart();
}

void WiFiSetupManager::handleFactoryResetRequest(AsyncWebServerRequest* request) {
    if (!wifisetup::isSameOrigin(request)) {
        wifisetup::sendCrossOriginRefused(request);
        return;
    }

    // Cross-site forms are refused above; the fixed token the confirmation page posts
    // only stops an accidental reset, as any client on the network can send it.
    bool confirmed = request->hasParam("confirm", true) &&
                     request->getParam("confirm", true)->value() == "RESET";
    if (!confirmed) {
        Logger.warning("Factory reset rejected - missing confirmation");
        request->send(400, "text/plain", "Factory reset requires confirmation");
        return;
    }

    Logger.warning("Factory reset requested via web UI");

    request->send(200, "text/html", FACTORY_RESET_HTML);

    factoryReset();
}

void WiFiSetupManager::enableLogger(size_t maxLogEntries) {
    Logger.begin(maxLogEntries, true);
    Logger.registerEndpoints(webServer_);
    Logger.info("Logger enabled with %zu max entries", maxLogEntries);
}
