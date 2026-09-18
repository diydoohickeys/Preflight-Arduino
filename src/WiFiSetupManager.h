#ifndef WIFI_SETUP_MANAGER_H
#define WIFI_SETUP_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>
#include <map>

// Colours for the LVGL boot screen and the web portal; every field has a default.
struct WiFiSetupTheme {
    // LVGL colours, 0xRRGGBB
    uint32_t primaryColor = 0x4A90E2;
    uint32_t backgroundColor = 0x121212;
    uint32_t surfaceColor = 0x1E1E1E;
    uint32_t surfaceLight = 0x2A2A2A;
    uint32_t textColor = 0xE0E0E0;
    uint32_t borderColor = 0x444444;

    // Web colours, CSS "#RRGGBB"
    String webPrimaryColor = "#4A90E2";
    String webPrimaryDark = "#357ABD";
    String webBackgroundColor = "#121212";
    String webSurfaceColor = "#1E1E1E";
    String webTextColor = "#E0E0E0";
    String webTextSecondary = "#B0B0B0";
    String webBorderColor = "#444444";

    String customCSS = "";

    // CSS variable name (without the leading "--") -> CSS value
    std::map<String, String> cssVariables;
};

// All callbacks fire on the task calling begin()/update(). Never block, and marshal
// work on a single-owner resource (LVGL in particular) onto its owner.
class WiFiStatusCallback {
public:
    virtual ~WiFiStatusCallback() = default;

    virtual void onScanStart() = 0;
    virtual void onScanComplete(int networks) = 0;
    virtual void onConnecting(const String& ssid) = 0;
    virtual void onConnectionProgress() = 0;
    virtual void onConnected(IPAddress ip) = 0;
    virtual void onAPMode(const String& apName, IPAddress ip) = 0;
};

struct WiFiSetupConfig {
    String defaultAPName = "ESP32-Setup";
    String defaultAPPassword = "setup123";          // min 8 chars
    String preferencesNamespace = "wifi";           // NVS namespace
    String deviceNameKey = "host_name";
    String ssidKey = "ssid";
    String passwordKey = "password";
    uint16_t webServerPort = 80;
    uint16_t dnsPort = 53;
    uint8_t maxConnectionAttempts = 10;
    uint16_t connectionTimeout = 500;               // ms per attempt
    WiFiStatusCallback* statusCallback = nullptr;
    WiFiSetupTheme* theme = nullptr;                // nullptr = built-in defaults
    // A new OTA image on probation is kept once the device is reachable (joined WiFi, or
    // serving the setup portal), and rolled back if that hasn't happened within this many ms
    // of begin(). 0 = never roll back on a timeout.
    uint32_t rollbackTimeoutMs = 300000;
};

class WiFiSetupManager {
public:
    explicit WiFiSetupManager(const WiFiSetupConfig& config = WiFiSetupConfig());
    ~WiFiSetupManager();

    WiFiSetupManager(const WiFiSetupManager&) = delete;
    WiFiSetupManager& operator=(const WiFiSetupManager&) = delete;

    // Blocking: connects with the stored credentials, or starts the setup AP if that fails.
    void begin();

    // Call regularly from the main loop: the post-save restart and reconnects run here.
    void update();

    // "SSID|RSSI|secured,..." (secured is 1 or 0), with ',', '|' and '\' inside an SSID backslash-escaped
    String getScannedNetworks() const;

    bool isInSetupMode() const;
    bool isConnected() const;

    // STA IP when connected, AP IP in setup mode, else "0.0.0.0"
    String getIPAddress() const;

    String getHostName() const { return hostName_; }
    String getSSID() const { return wifiSsid_; }
    const WiFiSetupTheme* getTheme() const { return config_.theme; }

    // For adding the consumer's own routes
    AsyncWebServer* getWebServer() { return webServer_; }

    // Starts Logger and serves its endpoints (/logs, /api/logs, ...); call after begin().
    void enableLogger(size_t maxLogEntries = 100);

    // Clears the stored settings; the restart happens on a later update().
    void factoryReset();

private:
    WiFiSetupConfig config_;

    String hostName_;
    String wifiSsid_;
    String wifiPassword_;
    String scannedNetworks_;
    bool initialized_;

    DNSServer dnsServer_;
    AsyncWebServer* webServer_;

    // Guards the scan result and its time: written by update(), read on the AsyncTCP task.
    SemaphoreHandle_t stateMutex_;
    bool scanCompleted_;
    unsigned long lastScanMs_;

    // Deferred to update(): request->send() only queues, so restarting in a handler
    // means the page is never transmitted.
    std::atomic<bool> restartRequested_;
    std::atomic<unsigned long> restartTime_;

    // A scan blocks for seconds, which must not happen on the AsyncTCP task.
    std::atomic<bool> scanRequested_;

    bool staWantsConnection_;
    unsigned long lastReconnectMs_;
    uint8_t reconnectFailures_;

    bool firmwarePending_;
    unsigned long beginMs_;

    // Lets the queued response reach the browser before the reboot.
    static const unsigned long RESTART_DELAY_MS = 2000;
    static const unsigned long RECONNECT_BASE_MS = 5000;
    static const unsigned long RECONNECT_MAX_MS = 60000;
    // Outside setup mode, a scan result (empty or not) is served until it is this old.
    static const unsigned long SCAN_MAX_AGE_MS = 15000;

    // Not MAX_SSID_LEN: esp_wifi_types_generic.h #defines that name.
    // 802.11 caps an SSID at 32 bytes, WPA2 a passphrase at 63.
    static const size_t SSID_MAX_BYTES = 32;
    static const size_t PASSWORD_MAX_BYTES = 63;
    static const size_t HOSTNAME_MAX_BYTES = 63;

    void scanNetworks();
    // A failed scan keeps the previous list but still counts as a fresh result.
    void recordScan(bool succeeded, const String& networks = String());
    bool getFreshScan(String& networks) const;
    void loadConfiguration();
    bool connectToNetwork();
    void startAPMode();
    void handleDNS();
    void requestRestart();
    void superviseFirmware();

    static String jsonEscape(const String& in);
    static String htmlEscape(const String& in);
    static String listEscape(const String& in);

    void setupWebServer();
    void handleGetNetworks(AsyncWebServerRequest* request);
    void handleGetCurrentSettings(AsyncWebServerRequest* request);
    void handleSaveWiFi(AsyncWebServerRequest* request);
    void handleFactoryResetRequest(AsyncWebServerRequest* request);
    void registerCaptivePortalRoutes();
};

#endif // WIFI_SETUP_MANAGER_H
