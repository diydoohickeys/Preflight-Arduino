# Preflight-Arduino

[![Build examples](https://github.com/diydoohickeys/Preflight-Arduino/actions/workflows/build.yml/badge.svg)](https://github.com/diydoohickeys/Preflight-Arduino/actions/workflows/build.yml)

**Preflight** is everything an ESP32 project needs before the interesting part starts: a
captive portal for WiFi credentials, a buffered logger with a web view and crash reports, an
OTA update page with rollback, and an optional LVGL boot screen. Served by the ESP-IDF HTTP(S) server built into arduino-esp32 — no web-server library to install.

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
- **Logger** with a PSRAM-backed ring buffer, a `/logs` page (live-tail over a WebSocket you
  forward lines to), `/api/logs` JSON, and previous-boot crash decoding.
- **HTTPS** (optional, the user's switch on `/setup`) — a certificate made on the device, port 80 redirecting; `tools/preflight-certs.ps1` signs it with your own CA so browsers trust it.
- **Device password** — chosen in the setup portal; every page and endpoint sits behind a
  sign-in (session cookie) or an API key, and the application gates its own routes the same way.
- **OTA** — an `/update` page for firmware and (opt-in) filesystem images, behind the device
  password, with progress callbacks for LEDs/screens and automatic rollback.
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

### Arduino IDE

Download this repository as a ZIP and use *Sketch → Include Library → Add .ZIP Library*.

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
    ota.begin(wifi.getWebServer(), "");         // "" = firmware only
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
`192.168.4.1/setup`). Enter a device name, pick a network and choose the **device password**;
the device saves, restarts and comes up on your WiFi at `<device-name>.local`, where every page
asks you to sign in.

`defaultAPPassword` is only the setup hotspot's WiFi key. It is not a login: the device
password is chosen on that first visit, so there is no default one to change.

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

Every callback fires on the task calling `begin()` or `update()` — never on the httpd
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
`Logger.registerEndpoints(server)`. Details and crash reports are in [`LOGGER.md`](LOGGER.md).

The `/logs` page live-tails over your `/ws`: forward lines with
`Logger.setOutputCallback` and send each `Logger.liveTailJson(level, timestamp, message)` to your
WebSocket clients. The callback runs on the logging task, so hand the send to the server
(`httpd_queue_work`). Pass `enableWebSocket = false` to `begin()` when you serve no `/ws`.

| Endpoint | |
|---|---|
| `GET /logs` | HTML view. `?max=N` widens the window, `?max=0` returns everything. |
| `GET /api/logs` | A JSON array of entries. |
| `POST /api/logs/clear` | Empty the buffer. |

## OTA updates

```cpp
OTAManager ota;
ota.begin(wifi.getWebServer(), "");   // "" = firmware only (see below)
ota.setScreenProgressCallback([](uint8_t pct, OTAManager::Stage stage) { /* ... */ });
ota.setLEDProgressCallback([](uint8_t pct) { /* ... */ });

// in loop()
ota.loop();     // performs the reboot after a successful update
```

Browse to `http(s)://<device>/update`. **Both** `/update` and the upload require the device
password (a signed-in browser or the API key), and the upload is checked on its first chunk —
before any byte reaches flash. One upload runs at a time; a second one gets 409.

🚨 **Filesystem updates are a raw overwrite of a partition.** The second argument to
`begin()` is the partition label and defaults to `"spiffs"`. **Pass `""` whenever that
partition holds user data rather than an uploadable image** — the upload then refuses
`type=filesystem` with a 400 and the page hides the Filesystem card.

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
confirmation token, so following a plain link cannot wipe the device. Both need the device
password. `WiFiSetupManager::factoryReset()` does the same from code (it clears the WiFi
settings and the device password, and schedules the restart for the next `update()`). It is
the only way back from a forgotten password, so a device with a button or a touch screen should
offer it there; otherwise erase the flash over USB.

## Device password

`AccessGuard` (the global `Access`) guards every Preflight page and endpoint. It is chosen
in the setup portal on the first save, and changed later on `/setup`, which also shows the
API key and a *Sign out everywhere* button.

- **Browsers** sign in at `/login` and get one session cookie (`HttpOnly`, `SameSite=Lax`,
  a year), which survives reboots and OTA. All browsers share the token, so signing out — or
  changing the password — signs every browser out.
- **Scripts** send `Authorization: Bearer <api key>`; regenerating the key on `/setup`
  revokes the old one.
- **Until a password exists** (a device that has not been through setup), the setup routes
  are open — reachable only by joining the setup hotspot — and everything else is refused.
- The password is stored as PBKDF2-HMAC-SHA256 (10 000 iterations, random salt). A sign-in
  costs a few hundred milliseconds of the server task; after five wrong passwords in a row,
  one attempt is allowed per 30 s.

Gate your own handlers with `allow()`, which sends the refusal itself — a page navigation is
redirected to `/login?next=…`, anything else gets a 401 JSON body:

```cpp
#include <AccessGuard.h>

static esp_err_t handleStatus(httpd_req_t* req) {
    if (!Access.allow(req)) return ESP_OK;   // ESP_FAIL if a request body is left unread
    // ...
}
```

`isAuthorized(req)` checks without responding — for a WebSocket, in the URI's
`ws_pre_handshake_cb` (needs `CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT`, so a pioarduino
`custom_sdkconfig`). A browser never sends a `SameSite=Lax` cookie on another site's handshake,
so that also stops a page elsewhere from driving the device:

```cpp
httpd_uri_t ws = { .uri = "/ws", .method = HTTP_GET, .handler = handleWs, .user_ctx = nullptr,
                   .is_websocket = true };
ws.ws_pre_handshake_cb = [](httpd_req_t* r) { return Access.isAuthorized(r) ? ESP_OK : ESP_FAIL; };
httpd_register_uri_handler(wifi.getWebServer(), &ws);
```

## Sharing the web server

`getWebServer()` returns the ESP-IDF `httpd_handle_t` (valid after `begin()`) so your application
registers its own routes on the same server with `httpd_register_uri_handler` — gate each with
`Access.allow()` (see *Device password*). Wildcard matching is on (`*` at the END of a URI only),
the first registration that matches wins, and the server takes 176 handlers and 12 sockets in
all. The captive-portal redirects, `/` and the 404 handler are registered **only in AP mode**,
so in normal operation your own `/` and 404s are untouched.

## HTTPS

The server is plain HTTP by default, as ESPHome, Tasmota and WLED are: on a home network the
device password is the protection. `config.offerHttps = true` adds an HTTPS switch to `/setup`'s
Access card, off until the user turns it on (the device restarts; the choice is kept in NVS). On,
every page and route is served over TLS on `httpsPort` (443) once the device is on WiFi,
`webServerPort` (80) redirects to it, and the session cookie becomes `Secure`. The setup portal
stays plain HTTP, because a captive-portal browser can't accept a certificate. The certificate is
self-signed ECDSA P-256 for `<host>` and `<host>.local`, made on the device the first time (a few
hundred ms) and kept in NVS; it's made afresh when the hostname changes. If it can't be made, the server falls back to HTTP and
says so in the log. `isHttps()` reports which.

Each TLS connection holds about 30 KB of mbedTLS buffers. `maxOpenSockets` (12) caps the open connections; on a board without PSRAM, or without `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`, lower it (5 = two TLS connections plus the redirect).

TLS costs RAM per open socket: put mbedTLS in PSRAM with a pioarduino `custom_sdkconfig`
(`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`, `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`). In HTTPS mode the main
server gets three fewer sockets, which the redirect server uses. The certificate's private key
sits in NVS, so pair HTTPS with NVS encryption (`CONFIG_NVS_ENCRYPTION=y`, HMAC scheme).

### A certificate your browsers trust

A self-signed certificate makes every browser warn, and a browser won't cache anything from a
site whose certificate it doesn't trust, so every page load is a full fetch and every new
connection a refused-then-retried handshake. `tools/preflight-certs.ps1` (PowerShell 7, on
Windows, macOS or Linux) fixes that with a certificate authority that lives only on your
computer:

```powershell
./tools/preflight-certs.ps1 -Device mydevice          # first run also makes and trusts the CA
./tools/preflight-certs.ps1 -Broker homeassistant.local, 192.168.1.10   # an MQTT broker's certificate
./tools/preflight-certs.ps1 -Trust -CaFile ca.crt     # trust the CA on another computer
```

Turn HTTPS on at `/setup` first. The script fetches a signing request for the device's own key (the key never leaves the
device), signs it for `<host>`, `<host>.local` and the device's address, uploads it with the CA,
and waits for the device to restart serving it. It needs the API key from `/setup`
(`-ApiKey`, `$env:PREFLIGHT_API_KEY`, or a prompt). The CA lives in `~/.preflight-ca`: share
`ca.crt` with other computers and phones, keep `ca.key` private and backed up. Device
certificates last 825 days; re-run `-Device` to renew, or after a hostname change.

The device keeps the CA too, as the trust anchor for its own outbound TLS:
`wifisetup::loadTrustedCa()` returns it, for example as an MQTT client's
`broker.verification.certificate`. `/setup` shows which certificate is served and can go back
to self-signed.

## Endpoints provided

| Route | |
|---|---|
| `GET /setup` | Configuration page |
| `GET /get-networks` | Scanned networks. **202** while a scan runs (scans happen in `update()`, never on the server task); the page polls. A result is reused for 15 s; in setup mode the boot scan is served as-is, because scanning would pull the radio off the AP's channel under the connected phone. |
| `GET /get-current-settings` | Stored device name and SSID, whether a WiFi password is stored (never the password) and whether a device password exists, as JSON |
| `POST /save-wifi` | Validate, save and restart (400 with a reason on bad input). A blank password for the already-saved network keeps the stored one. The first save also takes `device_password`. |
| `GET /factory-reset` | Confirmation page |
| `POST /factory-reset` | Perform the reset (requires the confirmation token) |
| `GET /wifi-setup-style.css`, `GET /wifi-setup-theme.js` | Themed assets |
| `GET /update`, `POST /ota/upload` | OTA (when `OTAManager` is used) |
| `GET /logs`, `GET /api/logs`, `POST /api/logs/clear` | Logger (when registered) |
| `GET /login`, `POST /login` | Sign-in page; `password` form field → session cookie (401 wrong, 429 locked out) |
| `POST /logout` | Sign every browser out |
| `GET /api/access` | The API key |
| `POST /api/access/password` | Change the password (`current`, `new`) |
| `POST /api/access/api-key` | Regenerate the API key |
| `POST /api/access/https` | Turn HTTPS on or off (`enabled`); restarts the device. With `offerHttps` only, as are the four below |
| `GET /api/access/csr` | A signing request for the device's key, with its hostname and address |
| `GET /api/access/certificate` | The served certificate: subject, issuer, expiry, whether CA-signed |
| `POST /api/access/certificate` | Install a CA-signed certificate (`cert`, `ca`, PEM); restarts the device |
| `POST /api/access/certificate/reset` | Back to self-signed; restarts the device |

Everything except the themed assets, `/login` and the captive-portal probes needs the device
password (the setup routes only once one exists).

Every state-changing POST also refuses a cross-site request with **403**: a browser's `Origin`
(or `Referer`) must match the `Host` it addressed. Tools and scripts send neither header and
are unaffected.

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
| [LVGL](https://lvgl.io) | MIT | optional |

arduino-esp32's LGPL passes its obligations on to your firmware: someone who receives it must
be able to rebuild or relink it against a modified version. Publishing your source
and build configuration is the simple way to meet that.

## License

MIT — see [`LICENSE`](LICENSE).
