// WiFi setup plus the log viewer (/logs) and OTA page (/update).
// With no saved network (or when it can't be reached) the device opens the
// "ESP32-Setup" access point, password "setup123"; the captive portal serves /setup.

#include <WiFiSetupManager.h>
#include <OTAManager.h>
#include <Logger.h>

WiFiSetupManager wifi;
OTAManager ota;

// Stops the core confirming a new OTA image at boot: WiFiSetupManager keeps it once the
// device is reachable (WiFi or setup portal) and rolls it back if that never happens.
// extern "C": the core's weak default is a C symbol, so a C++ definition wouldn't replace it.
extern "C" bool verifyRollbackLater() {
    return true;
}

void setup() {
    Serial.begin(115200);
    Logger.begin(200, /*enableSerial=*/true, /*enableWebSocket=*/false);   // no /ws here
    Logger.reportLastCrash();

    wifi.begin();

    AsyncWebServer* server = wifi.getWebServer();
    Logger.registerEndpoints(server);
    ota.begin(server, "admin", "change-me", "");   // "" label = firmware only

    if (wifi.isConnected()) {
        Logger.info("Connected to %s - http://%s/setup",
                    wifi.getSSID().c_str(), wifi.getIPAddress().c_str());
    }
}

void loop() {
    wifi.update();
    ota.loop();
    delay(10);
}
