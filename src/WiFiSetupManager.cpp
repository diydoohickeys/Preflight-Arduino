#include "WiFiSetupManager.h"
#include "Logger.h"
#include "html/setup_html.h"
#include "html/wifi_saved_html.h"
#include "html/factory_reset_html.h"
#include "html/factory_reset_confirm_html.h"
#include "html/style_css.h"
#include "html/theme_js.h"
#include "SameOrigin.h"
#include "HttpdRoute.h"
#include "FormBody.h"
#include "AccessGuard.h"
#include "DeviceCert.h"
#include <esp_ota_ops.h>
#include <algorithm>
#include <string>

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
    , server_(nullptr)
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
}

WiFiSetupManager::~WiFiSetupManager() {
    stopWebServer();
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

    Access.begin();

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
            startWebServer();
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
        // Persists across reconnects: the setting belongs to the station, not the association.
        WiFi.setSleep(config_.wifiPowerSave);
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

    startWebServer();

    if (config_.statusCallback) {
        config_.statusCallback->onAPMode(config_.defaultAPName, WiFi.softAPIP());
    }

    Logger.info("AP Name: %s", config_.defaultAPName.c_str());
    Logger.info("IP address: %s", WiFi.softAPIP().toString().c_str());
}

void WiFiSetupManager::handleDNS() {
    dnsServer_.processNextRequest();
}

void WiFiSetupManager::factoryReset() {
    Logger.warning("Factory reset - clearing WiFi settings and the device password");

    Preferences prefs;
    if (prefs.begin(config_.preferencesNamespace.c_str(), false)) {
        prefs.clear();
        prefs.end();
    }
    Access.clear();
    wifisetup::clearDeviceCert();

    requestRestart();
}

// The time is stored before the flag: update() reads them in the other order.
void WiFiSetupManager::requestRestart() {
    restartTime_ = millis();
    restartRequested_ = true;
}

void WiFiSetupManager::startWebServer() {
    if (server_) {
        return;
    }

    wifisetup::ServerOptions options;
    options.httpPort = config_.webServerPort;
    options.httpsPort = config_.httpsPort;
    // Never in setup mode: a captive-portal browser can't accept a self-signed certificate.
    options.https = config_.offerHttps && wifisetup::httpsChosen() && !isInSetupMode();
    options.hostname = (hostName_.isEmpty() ? config_.defaultAPName : hostName_).c_str();
    // Shared with the consumer's whole route table, one slot per method+path. Overflow
    // is silent (the last registrations just fail), so the cap is generous.
    options.maxUriHandlers = 176;
    options.stackSize = 8192;
    options.maxOpenSockets = config_.maxOpenSockets;

    if (!host_.start(options)) {
        return;
    }
    server_ = host_.handle();
    Access.setHttps(host_.isHttps());

    const httpd_uri_t routes[] = {
        wifisetup::route("/setup", HTTP_GET, handleSetupPage, this),
        wifisetup::route("/wifi-setup-style.css", HTTP_GET, handleStyleCss, this),
        wifisetup::route("/wifi-setup-theme.js", HTTP_GET, handleThemeJs, this),
        wifisetup::route("/get-networks", HTTP_GET, handleGetNetworks, this),
        wifisetup::route("/get-current-settings", HTTP_GET, handleGetCurrentSettings, this),
        wifisetup::route("/save-wifi", HTTP_POST, handleSaveWiFi, this),
        wifisetup::route("/factory-reset", HTTP_GET, handleFactoryReset, this),
        wifisetup::route("/factory-reset", HTTP_POST, handleFactoryReset, this),
    };
    for (const httpd_uri_t& r : routes) {
        httpd_register_uri_handler(server_, &r);
    }

    Access.registerEndpoints(server_);
    if (config_.offerHttps) {
        wifisetup::registerDeviceCertEndpoints(server_);
    }

    if (isInSetupMode()) {
        registerCaptivePortalRoutes();
    }
}

void WiFiSetupManager::stopWebServer() {
    host_.stop();
    server_ = nullptr;
}

// AP mode only: in STA mode the catch-all would turn the consumer's 404s into redirects to /setup.
void WiFiSetupManager::registerCaptivePortalRoutes() {
    const httpd_uri_t probes[] = {
        wifisetup::route("/", HTTP_GET, handleCaptiveRedirect, this),
        wifisetup::route("/generate_204", HTTP_GET, handleCaptiveRedirect, this),
        wifisetup::route("/fwlink", HTTP_GET, handleCaptiveRedirect, this),
        wifisetup::route("/connecttest.txt", HTTP_GET, handleCaptiveRedirect, this),
        wifisetup::route("/hotspot-detect.html", HTTP_GET, handleCaptiveRedirect, this),
        wifisetup::route("/redirect", HTTP_GET, handleCaptiveRedirect, this),
    };
    for (const httpd_uri_t& r : probes) {
        httpd_register_uri_handler(server_, &r);
    }
    // Any other URL a phone probes with also lands on the portal.
    httpd_register_err_handler(server_, HTTPD_404_NOT_FOUND, handleNotFound);
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

esp_err_t WiFiSetupManager::handleCaptiveRedirect(httpd_req_t* req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/setup");
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t WiFiSetupManager::handleNotFound(httpd_req_t* req, httpd_err_code_t) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/setup");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

esp_err_t WiFiSetupManager::handleSetupPage(httpd_req_t* req) {
    if (!Access.allowSetup(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, SETUP_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t WiFiSetupManager::handleStyleCss(httpd_req_t* req) {
    auto* self = static_cast<WiFiSetupManager*>(req->user_ctx);
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send_chunk(req, STYLE_CSS, HTTPD_RESP_USE_STRLEN);

    // Appended after the static sheet so the overrides win on equal specificity.
    const WiFiSetupTheme* theme = self ? self->config_.theme : nullptr;
    if (theme) {
        String overrides = "\n:root {\n";
        auto addVar = [&overrides](const char* name, const String& value) {
            if (value.length() > 0) {
                overrides += "  ";
                overrides += name;
                overrides += ": " + value + ";\n";
            }
        };
        addVar("--accent", theme->webPrimaryColor);
        addVar("--accent-dark", theme->webPrimaryDark);
        addVar("--bg", theme->webBackgroundColor);
        addVar("--card", theme->webSurfaceColor);
        addVar("--text", theme->webTextColor);
        addVar("--text-dim", theme->webTextSecondary);
        addVar("--border", theme->webBorderColor);
        for (const auto& var : theme->cssVariables) {
            overrides += "  --" + var.first + ": " + var.second + ";\n";
        }
        overrides += "}\n";

        // The select arrow is an SVG data URL: its fill needs its own override, with
        // the '#' percent-encoded or it terminates the URL.
        if (theme->webPrimaryColor.length() > 0) {
            String color = theme->webPrimaryColor;
            color.replace("#", "%23");
            overrides += "select { background-image: url(\"data:image/svg+xml,%3csvg xmlns='http://www.w3.org/2000/svg' fill='" + color + "' viewBox='0 0 16 16'%3e%3cpath d='M8 11L3 6h10z'/%3e%3c/svg%3e\"); }\n";
        }

        if (theme->customCSS.length() > 0) {
            overrides += theme->customCSS + "\n";
        }

        httpd_resp_send_chunk(req, overrides.c_str(), overrides.length());
    }

    return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t WiFiSetupManager::handleThemeJs(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/javascript");
    return httpd_resp_send(req, THEME_JS, HTTPD_RESP_USE_STRLEN);
}

esp_err_t WiFiSetupManager::handleGetNetworks(httpd_req_t* req) {
    auto* self = static_cast<WiFiSetupManager*>(req->user_ctx);
    if (!Access.allowSetup(req)) {
        return ESP_OK;
    }

    // Cold on a normal STA boot (the boot scan is lazy); update() runs the scan and the
    // page polls on 202.
    String networks;
    if (!self->getFreshScan(networks)) {
        self->scanRequested_ = true;
        httpd_resp_set_status(req, "202 Accepted");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "SCANNING");
    }

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, networks.c_str(), networks.length());
}

esp_err_t WiFiSetupManager::handleGetCurrentSettings(httpd_req_t* req) {
    auto* self = static_cast<WiFiSetupManager*>(req->user_ctx);
    if (!Access.allowSetup(req)) {
        return ESP_OK;
    }

    String networkName;
    String wifiSSID;
    bool hasSettings = false;
    bool hasPassword = false;

    // A Preferences handle per use: this is the httpd task, and a shared one's end()
    // would close the handle mid-read on the app task.
    Preferences prefs;
    if (prefs.begin(self->config_.preferencesNamespace.c_str(), true)) {
        hasSettings = prefs.isKey(self->config_.deviceNameKey.c_str());
        networkName = prefs.getString(self->config_.deviceNameKey.c_str(), "");
        wifiSSID = prefs.getString(self->config_.ssidKey.c_str(), "");
        hasPassword = prefs.isKey(self->config_.passwordKey.c_str()) &&
                      !prefs.getString(self->config_.passwordKey.c_str(), "").isEmpty();
        prefs.end();
    }

    // Whether a password is stored, never the password itself.
    String json = "{";
    json += "\"hasSettings\":" + String(hasSettings ? "true" : "false") + ",";
    json += "\"networkName\":\"" + jsonEscape(networkName) + "\",";
    json += "\"wifiSSID\":\"" + jsonEscape(wifiSSID) + "\",";
    json += "\"hasPassword\":" + String(hasPassword ? "true" : "false") + ",";
    json += "\"hasDevicePassword\":" + String(Access.hasPassword() ? "true" : "false");
    json += "}";

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.c_str(), json.length());
}

// Refusals before the body is read return ESP_FAIL, so httpd closes the socket instead of
// draining it.
esp_err_t WiFiSetupManager::handleSaveWiFi(httpd_req_t* req) {
    auto* self = static_cast<WiFiSetupManager*>(req->user_ctx);

    if (!wifisetup::isSameOrigin(req)) {
        wifisetup::sendCrossOriginRefused(req);
        return ESP_FAIL;
    }
    if (!Access.allowSetup(req)) {
        return ESP_FAIL;
    }

    std::string body;
    if (!wifisetup::readFormBody(req, body)) {
        return ESP_FAIL;
    }

    String networkName = wifisetup::formValue(body, "network_name").c_str();
    String wifiNetwork = wifisetup::formValue(body, "wifi_network").c_str();
    String wifiPassword = wifisetup::formValue(body, "wifi_password").c_str();
    // Only the first save chooses the device password; later ones change it on /setup's Access card.
    const bool choosingPassword = !Access.hasPassword();
    std::string devicePassword = choosingPassword ? wifisetup::formValue(body, "device_password") : std::string();
    std::fill(body.begin(), body.end(), '\0');

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
    else if (choosingPassword && (devicePassword.size() < AccessGuardClass::MIN_PASSWORD ||
                                  devicePassword.size() > AccessGuardClass::MAX_PASSWORD))
        reason = "Device password must be 8-64 characters";
    if (reason) {
        Logger.warning("Rejected WiFi settings: %s", reason);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, reason);
        return ESP_FAIL;
    }

    if (choosingPassword) {
        const bool set = Access.setPassword(devicePassword.c_str());
        std::fill(devicePassword.begin(), devicePassword.end(), '\0');
        if (!set) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save the device password");
            return ESP_FAIL;
        }
    }

    bool saved = false;
    {
        Preferences prefs;
        if (prefs.begin(self->config_.preferencesNamespace.c_str(), false)) {
            // The page leaves the field blank to keep the saved password of the saved network.
            const bool keepPassword = wifiPassword.isEmpty() && prefs.isKey(self->config_.ssidKey.c_str()) &&
                                      wifiNetwork == prefs.getString(self->config_.ssidKey.c_str(), "");
            saved = prefs.putString(self->config_.deviceNameKey.c_str(), networkName) > 0
                 && prefs.putString(self->config_.ssidKey.c_str(), wifiNetwork) > 0;
            // putString returns 0 for an empty (open-network) password, so it cannot
            // be part of the success test.
            if (!keepPassword) {
                prefs.putString(self->config_.passwordKey.c_str(), wifiPassword);
            }
            prefs.end();
        }
    }
    if (!saved) {
        Logger.error("Failed to store WiFi settings");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save settings to flash");
        return ESP_FAIL;
    }

    Logger.info("WiFi settings saved: Host=%s, SSID=%s", networkName.c_str(), wifiNetwork.c_str());

    // Escaped before templating: both values are user-supplied and land in HTML.
    String html = WIFI_SAVED_HTML;
    html.replace("%NETWORK_NAME%", htmlEscape(networkName));
    html.replace("%WIFI_NETWORK%", htmlEscape(wifiNetwork));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html.c_str(), html.length());

    self->requestRestart();
    return ESP_OK;
}

esp_err_t WiFiSetupManager::handleFactoryReset(httpd_req_t* req) {
    auto* self = static_cast<WiFiSetupManager*>(req->user_ctx);

    if (req->method != HTTP_POST) {
        // GET is the confirmation page, never the action.
        if (!Access.allowSetup(req)) {
            return ESP_OK;
        }
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, FACTORY_RESET_CONFIRM_HTML, HTTPD_RESP_USE_STRLEN);
    }

    if (!wifisetup::isSameOrigin(req)) {
        wifisetup::sendCrossOriginRefused(req);
        return ESP_FAIL;
    }
    if (!Access.allowSetup(req)) {
        return ESP_FAIL;
    }
    std::string body;
    if (!wifisetup::readFormBody(req, body)) {
        return ESP_FAIL;
    }

    // Cross-site forms are refused above; the fixed token the confirmation page posts
    // only stops an accidental reset.
    if (wifisetup::formValue(body, "confirm") != "RESET") {
        Logger.warning("Factory reset rejected - missing confirmation");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Factory reset requires confirmation");
        return ESP_FAIL;
    }

    Logger.warning("Factory reset requested via web UI");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, FACTORY_RESET_HTML, HTTPD_RESP_USE_STRLEN);

    self->factoryReset();
    return ESP_OK;
}

void WiFiSetupManager::enableLogger(size_t maxLogEntries) {
    Logger.begin(maxLogEntries, true);
    Logger.registerEndpoints(server_);
    Logger.info("Logger enabled with %zu max entries", maxLogEntries);
}
