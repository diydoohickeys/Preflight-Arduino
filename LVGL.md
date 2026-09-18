# Optional LVGL boot screen

`WiFiSetupBootUI` puts WiFi setup progress on an attached display: scanning, connecting,
the assigned IP, or the AP name and address to join. It implements `WiFiStatusCallback`, so
wiring it up is one assignment.

It is entirely optional — see *Headless builds* below.

## Using it

Bring LVGL up yourself (display driver, buffers, tick source, `lv_timer_handler` pumping —
this library does none of that), then:

```cpp
#include <lvgl.h>
#include <WiFiSetupManager.h>
#include <WiFiSetupBootUI.h>

WiFiSetupBootUI bootUI;
WiFiSetupManager* wifiSetup = nullptr;

void setup() {
    Serial.begin(115200);

    // ... lv_init(), display driver, lv_display_create(), buffers, lv_tick_set_cb ...

    bootUI.initialize("MY DEVICE");        // optional custom title and theme

    WiFiSetupConfig config;                // copied by the manager's constructor
    config.defaultAPName  = "MyDevice-Setup";
    config.statusCallback = &bootUI;       // progress now renders on screen
    wifiSetup = new WiFiSetupManager(config);
    wifiSetup->begin();

    bootUI.cleanup();                      // remove the widgets, hand the screen back

    // ... your own UI ...
}

void loop() {
    wifiSetup->update();
    lv_timer_handler();
    delay(5);
}
```

`initialize()` optionally takes the screen dimensions; without them it reads the default
display and sizes the title, text area and fonts to fit. `setDeviceName()` adds a `Device:`
line: `<name>.local` on the connected screen, the bare name on the AP screen.

## 🚨 Threading

`WiFiStatusCallback` methods fire on the task calling `begin()` or `update()`, and
`WiFiSetupBootUI` calls LVGL directly from them. LVGL is not thread-safe.

So `WiFiSetupBootUI` is safe **only** where that same task owns LVGL, which is the case it is
built for: the boot window, before your own LVGL task starts. If your application pumps LVGL
from another task, do not point `statusCallback` at this class — implement
`WiFiStatusCallback` yourself and marshal each update onto whichever task owns LVGL.

None of the callbacks block. In particular `onConnected()` does **not** pause to let you
read the IP; keep the boot screen up for as long as you want from your own code.

Note that `addText()` calls `lv_refr_now()` per line, which is a full display flush. That is
appropriate during boot, where nothing else is driving the screen, and is another reason
this class belongs to the boot window only.

## Object lifetime

The widgets belong to LVGL, which can free them behind this class's back — a consumer that
calls `lv_obj_clean(lv_scr_act())` or switches screens does exactly that. Each widget
carries an `LV_EVENT_DELETE` callback that nulls the matching member, so `cleanup()` and the
destructor can never delete a dangling pointer, whichever runs first.

## Theming

```cpp
WiFiSetupTheme theme;
theme.primaryColor    = 0xFF6B35;   // title text
theme.backgroundColor = 0x0A0A0A;
theme.surfaceColor    = 0x1A1A1A;   // text area
theme.textColor       = 0xF0F0F0;
theme.borderColor     = 0x333333;

bootUI.initialize("MY DEVICE", &theme);
```

The same struct carries the web-UI colours; see the README.

## Headless builds

`WiFiSetupBootUI.cpp` is wrapped in `#if __has_include(<lvgl.h>)`, so it compiles to an
empty translation unit whenever LVGL is not on the include path, and nothing else in the
library references it. The library doesn't depend on LVGL, so a project without it in its
`lib_deps` is headless with no further change.

## LVGL configuration

The layout uses Montserrat 10, 12, 14, 18 and 22 — enable all five in your `lv_conf.h`, or
the link fails.

The `lv_conf.h` at the repo root exists for **developing this library** (and building its
examples). A consumer should supply its own; if both are reachable, whichever include path
comes first wins, which is a confusing way to configure LVGL:

```ini
build_flags =
    -I include        ; your own lv_conf.h wins
```

Put your project's `lv_conf.h` in `include/` and list that path first.
