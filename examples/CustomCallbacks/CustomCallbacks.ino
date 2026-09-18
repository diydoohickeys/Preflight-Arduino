// Reacting to WiFi events through WiFiStatusCallback.

#include <WiFiSetupManager.h>
#include <Logger.h>

// Callbacks run on the task calling begin()/update(): keep them short and never block.
class LoggedStatus : public WiFiStatusCallback {
public:
    void onScanStart() override {
        Logger.info("Scanning...");
    }

    void onScanComplete(int networks) override {
        Logger.info("Found %d network(s)", networks);
    }

    void onConnecting(const String& ssid) override {
        Logger.info("Connecting to '%s'", ssid.c_str());
    }

    void onConnectionProgress() override {
        Logger.debug("Still connecting");
    }

    void onConnected(IPAddress ip) override {
        Logger.info("Connected: %s", ip.toString().c_str());
    }

    void onAPMode(const String& apName, IPAddress ip) override {
        Logger.info("Setup mode: join '%s', then open http://%s/setup",
                    apName.c_str(), ip.toString().c_str());
    }
};

LoggedStatus status;
WiFiSetupManager* wifi = nullptr;

void setup() {
    Serial.begin(115200);
    Logger.begin(100, /*enableSerial=*/true, /*enableWebSocket=*/false);

    WiFiSetupConfig config;
    config.defaultAPName = "CustomESP32";
    config.defaultAPPassword = "custompass123";   // at least 8 characters
    config.maxConnectionAttempts = 15;
    config.connectionTimeout = 500;               // ms between attempts
    config.statusCallback = &status;

    wifi = new WiFiSetupManager(config);
    wifi->begin();
    Logger.registerEndpoints(wifi->getWebServer());
}

void loop() {
    wifi->update();

    static unsigned long lastReport = 0;
    if (wifi->isConnected() && millis() - lastReport > 60000) {
        lastReport = millis();
        Logger.info("%s, RSSI %d dBm", wifi->getIPAddress().c_str(), WiFi.RSSI());
    }

    delay(10);
}
