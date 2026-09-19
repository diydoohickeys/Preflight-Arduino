# Preflight-Arduino

[![Build examples](https://github.com/diydoohickeys/Preflight-Arduino/actions/workflows/build.yml/badge.svg)](https://github.com/diydoohickeys/Preflight-Arduino/actions/workflows/build.yml)

**Preflight** is everything an ESP32 project needs before the interesting part starts: a
captive portal for WiFi credentials, a buffered logger with a web view and crash reports, an
OTA update page with rollback, and an optional LVGL boot screen. Built on ESPAsyncWebServer.

> This is the Arduino-framework build. The sister library **Preflight-IDF** exposes the same
> API as a native ESP-IDF component.

## Features

- **Captive portal** — a DNS server redirects every lookup to the setup page, so joining the
  device's AP pops the form automatically.
- **Network scanning** with signal strength, scanned lazily so a successful boot never pays
  for a scan nobody looks at.
- **Persistent credentials** in NVS (Preferences), with the device name normalised for mDNS.
- **mDNS** — the device is reachable at `<device-name>.local`.
- **Reconnect supervision** — a dropped AP is retried with backoff instead of leaving the
  device unreachable until a power cycle.
- **Logger** with a PSRAM-backed ring buffer, a `/logs` page (live-tail over an
  `AsyncWebSocket` you attach), `/api/logs` JSON, and previous-boot crash decoding.
- **OTA** — an `/update` page for firmware and (opt-in) filesystem images, with optional
  HTTP Basic auth, progress callbacks for LEDs/screens, and automatic rollback.
- **Optional LVGL boot screen** showing setup progress, compiled out when LVGL is absent.
- **Embedded web assets** — no SPIFFS/LittleFS partition needed to serve the UI.

## The pages

| Setup | Logs | Update |
|---|---|---|
| ![The captive-portal setup page](https://raw.githubusercontent.com/diydoohickeys/Preflight-Arduino/main/docs/images/setup.png) | ![The log viewer](https://raw.githubusercontent.com/diydoohickeys/Preflight-Arduino/main/docs/images/logs.png) | ![The OTA update page](https://raw.githubusercontent.com/diydoohickeys/Preflight-Arduino/main/docs/images/update.png) |

## Requirements

| | |
|---|---|
| **arduino-esp32 3.x** | Developed on pioarduino `55.03.38` (arduino-esp32 3.3.8). |
| **C++ exceptions** | The Logger's PSRAM allocator throws `std::bad_alloc` when both PSRAM and the internal heap are exhausted, and the logger catches it to drop the line rather than crash. PlatformIO consumers get `-fexceptions` from this library's `library.json`; in the Arduino IDE it comes from the core's own configuration. If exceptions are off, the build fails with a message naming the flag — add it to `build_flags`. |
| [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) + [AsyncTCP](https://github.com/ESP32Async/AsyncTCP) | From the **ESP32Async** org — several projects share these names, so match the links. |

Optional:

| | |
|---|---|
| **LVGL 9** | Only for `WiFiSetupBootUI`. Leave it out of your dependencies and the boot UI compiles away. |
| **PSRAM** | The logger prefers PSRAM for its buffer and falls back to internal heap. |

arduino-esp32's prebuilt configuration has a 0-byte emergency exception pool, so if the heap
is *completely* exhausted a `throw` terminates instead of being caught. Running out of PSRAM
alone is handled.

## Installation

### PlatformIO

```ini
lib_deps =
    https://github.com/diydoohickeys/Preflight-Arduino.git#v1.0.0
```

The library's `library.json` pulls in ESPAsyncWebServer and AsyncTCP.

### Arduino IDE

Install **ESP Async WebServer** and **Async TCP** (both by ESP32Async) from the Library
Manager, then download this repository as a ZIP and use *Sketch → Include Library → Add .ZIP
Library*.

## Quick start

```cpp
#include <WiFiSetupManager.h>
#include <OTAManager.h>
#include <Logger.h>

WiFiSetupManager wifi;
OTAManager ota;

void setup() {
    Serial.begin(115200);
    Logger.begin(200, /*enableSerial=*/true, /*enableWebSocket=*/false);
    Logger.reportLastCrash();

    wifi.begin();                               // connects, or brings up the portal
    Logger.registerEndpoints(wifi.getWebServer());
    ota.begin(wifi.getWebServer(), "admin", "change-me", "");
}

void loop() {
    wifi.update();                              // DNS, deferred restarts, reconnects
    ota.loop();                                 // the reboot after an update
    delay(10);
}
```

`update()` must be called regularly: it serves the captive-portal DNS, owns the deferred
restart after a settings save, and runs the reconnect backoff. Without it a saved
configuration never reboots into effect.

### First run

With no stored credentials the device starts an AP named by `defaultAPName` (`ESP32-Setup`,
password `setup123`). Join it and the captive portal opens the setup page (or browse to
`192.168.4.1/setup`). Enter a device name and pick a network; the device saves, restarts and
comes up on your WiFi at `<device-name>.local`.

## Configuration

```cpp
struct WiFiSetupConfig {
    String defaultAPName = "ESP32-Setup";
    String defaultAPPassword = "setup123";     // min 8 chars
    String preferencesNamespace = "wifi";      // NVS namespace
    String deviceNameKey = "host_name";
    String ssidKey = "ssid";
    String passwordKey = "password";
    uint16_t webServerPort = 80;
    uint16_t dnsPort = 53;
    uint8_t maxConnectionAttempts = 10;
    uint16_t connectionTimeout = 500;          // ms per attempt
    WiFiStatusCallback* statusCallback = nullptr;
    WiFiSetupTheme* theme = nullptr;           // nullptr = built-in defaults
    uint32_t rollbackTimeoutMs = 300000;       // see OTA → Rollback
};
```

## Status callbacks

```cpp
class MyCallback : public WiFiStatusCallback {
    void onScanStart() override                                  { /* ... */ }
    void onScanComplete(int networks) override                   { /* ... */ }
    void onConnecting(const String& ssid) override               { /* ... */ }
    void onConnectionProgress() override                         { /* ... */ }
    void onConnected(IPAddress ip) override                      { /* ... */ }
    void onAPMode(const String& apName, IPAddress ip) override   { /* ... */ }
};
```

Every callback fires on the task calling `begin()` or `update()` — never on the AsyncTCP
task. If another task also drives a single-owner resource (LVGL in particular, which is not
thread-safe), marshal the work onto that owner. Never block.

## Theming

`WiFiSetupTheme` covers both the LVGL boot screen and the web UI. The web fields are emitted
as CSS custom properties after the built-in stylesheet, so they override it; `cssVariables`
(names without the leading `--`) and `customCSS` are appended last.

```cpp
WiFiSetupTheme theme;
theme.primaryColor       = 0xFF6B35;   // LVGL boot screen (0xRRGGBB)
theme.webPrimaryColor    = "#FF6B35";  // web UI
theme.webBackgroundColor = "#1a1a1a";
theme.cssVariables["radius"] = "12px";
theme.customCSS = "h1 { letter-spacing: 2px; }";

WiFiSetupConfig config;
config.theme = &theme;
```

The boot screen's built-in colours can also be replaced at build time
(`build_flags = -DUI_COLOR_PRIMARY=0xFF6B35`, see `WiFiSetupBootUI.h` for the full set).

## Logger

`Logger.begin(maxEntries, enableSerial, enableWebSocket)` then
`Logger.registerEndpoints(server)`. Details, the live-tail socket and crash reports are in
[`LOGGER.md`](LOGGER.md).

| Endpoint | |
|---|---|
| `GET /logs` | HTML view. `?max=N` widens the window, `?max=0` returns everything. |
| `GET /api/logs` | A JSON array of entries. |
| `POST /api/logs/clear` | Empty the buffer. |

## OTA updates

```cpp
OTAManager ota;
ota.begin(wifi.getWebServer(), "admin", "secret");   // "", "" = no auth
ota.setScreenProgressCallback([](uint8_t pct, OTAManager::Stage stage) { /* ... */ });
ota.setLEDProgressCallback([](uint8_t pct) { /* ... */ });

// in loop()
ota.loop();     // performs the reboot after a successful update
```

Browse to `http://<device>/update`. With a username and password set, **both** `/update` and
the upload require HTTP Basic auth, and the upload is checked on its first chunk — before
any byte reaches flash. One upload runs at a time; a second one gets 409.

🚨 **Filesystem updates are a raw overwrite of a partition.** The fourth argument to
`begin()` is the partition label and defaults to `"spiffs"`. **Pass `""` whenever that
partition holds user data rather than an uploadable image** — the upload then refuses
`type=filesystem` with a 400 and the page hides the Filesystem card. arduino-esp32's updater
always writes the *first* SPIFFS-type data partition, so a label naming any other partition
also disables filesystem updates rather than overwrite the wrong one.

### Rollback and recovery

An upload is written to the inactive app slot and only booted once it validates, so an
interrupted or corrupt upload leaves the running firmware in place. A valid image that is
broken — it crashes, or never gets back on the network — is caught by rollback: the new image
boots on probation, `WiFiSetupManager` keeps it once the device is reachable — joined to
WiFi, or serving the setup portal, from which it can still be repaired — and if that hasn't
happened within `rollbackTimeoutMs` (5 minutes) or the image resets first, the bootloader
returns to the previous one. arduino-esp32 confirms every new image at boot unless the
sketch opts out, so add this to yours:

```cpp
extern "C" bool verifyRollbackLater() { return true; }   // C linkage, or it won't override the core's
```

Pair it with the task watchdog so a hang becomes a reset. If a device still ends up on a bad
image, flash it over USB from download mode (hold BOOT while connecting) — that path is in
ROM and always available.

## Factory reset

`GET /factory-reset` serves a confirmation page; the reset itself is a POST carrying a fixed
confirmation token, so following a plain link cannot wipe the device. The token is not a
secret: anything that can reach the device on the network can reset it.
`WiFiSetupManager::factoryReset()` does the same from code (it clears NVS and schedules the
restart for the next `update()`).

## Sharing the web server

`getWebServer()` returns the `AsyncWebServer*` so your application registers its own routes
on the same server. The captive-portal redirects, `/` and the catch-all `onNotFound` handler
are registered **only in AP mode**, so in normal operation your own `/` and 404s are
untouched.

## Endpoints provided

| Route | |
|---|---|
| `GET /setup` | Configuration page |
| `GET /get-networks` | Scanned networks. **202** while a scan runs (scans happen in `update()`, never on the AsyncTCP task); the page polls. A result is reused for 15 s; in setup mode the boot scan is served as-is, because scanning would pull the radio off the AP's channel under the connected phone. |
| `GET /get-current-settings` | Stored device name and SSID, and whether a password is stored (never the password), as JSON |
| `POST /save-wifi` | Validate, save and restart (400 with a reason on bad input). A blank password for the already-saved network keeps the stored one. |
| `GET /factory-reset` | Confirmation page |
| `POST /factory-reset` | Perform the reset (requires the confirmation token) |
| `GET /wifi-setup-style.css`, `GET /wifi-setup-theme.js` | Themed assets |
| `GET /update`, `POST /ota/upload` | OTA (when `OTAManager` is used) |
| `GET /logs`, `GET /api/logs`, `POST /api/logs/clear` | Logger (when registered) |

Every state-changing POST refuses a cross-site request with **403**: a browser's `Origin` (or
`Referer`) must match the `Host` it addressed. Tools and scripts send neither header and are
unaffected. This stops a web page from driving the device through the user's browser; it is
not authentication.

In AP mode the captive-portal probe URLs (`/generate_204`, `/hotspot-detect.html`,
`/connecttest.txt`, `/fwlink`, `/redirect`) and `/` redirect to `/setup`.

## Examples

Each example is a PlatformIO project that builds against this checkout, with `esp32-s3`
(any S3 with ≥ 4 MB flash, Serial on native USB) and `esp32` environments:

```
cd examples/BasicSetup
pio run -e esp32-s3 -t upload
```

- [`BasicSetup`](examples/BasicSetup) — WiFi setup with `/logs`, `/update` and rollback
- [`CustomCallbacks`](examples/CustomCallbacks) — reacting to WiFi events
- [`WithDisplay`](examples/WithDisplay) — the LVGL boot screen during setup
- [`WideScreenDisplay`](examples/WideScreenDisplay) — a 536×240 panel, a shared screen/web
  theme, and OTA

## Editing the web assets

The HTML/CSS/JS lives in `extras/html_source/` and is compiled into headers under
`src/html/` by `extras/convert_html.py`. Edit the sources and re-run the script; don't edit
the generated headers.

## Documentation

- [`LOGGER.md`](LOGGER.md) — the Logger in detail
- [`LVGL.md`](LVGL.md) — the optional boot screen

## Dependencies and licences

| Dependency | Licence | |
|---|---|---|
| [arduino-esp32](https://github.com/espressif/arduino-esp32) | LGPL-2.1 (ESP-IDF inside it: Apache-2.0) | |
| [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) | LGPL-3.0 | |
| [AsyncTCP](https://github.com/ESP32Async/AsyncTCP) | LGPL-3.0 | |
| [LVGL](https://lvgl.io) | MIT | optional |

The LGPL libraries pass their obligations on to your firmware: someone who receives it must
be able to rebuild or relink it against a modified version of them. Publishing your source
and build configuration is the simple way to meet that.

## License

MIT — see [`LICENSE`](LICENSE).
