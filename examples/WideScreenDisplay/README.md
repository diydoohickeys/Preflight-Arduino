# Wide Screen Display

The boot UI on a short, wide panel — a 536×240 LilyGo T-Display AMOLED — with a custom theme
applied to both the screen and the web pages, and the OTA page once the device is connected.

## Layout selection

`WiFiSetupBootUI::initialize(title, theme, width, height)` sizes the layout from the panel
(pass `0, 0` to read it from the default LVGL display):

| Height | Title / text font | Title margin | Padding |
|---|---|---|---|
| ≥ 400 px | Montserrat 22 / 14 | 30 px | 20 px |
| ≥ 300 px | Montserrat 18 / 12 | 20 px | 15 px |
| < 300 px | Montserrat 14 / 10 | 10 px | 10 px |

An aspect ratio above 2:1 (536×240 is 2.23:1) also widens the text area to 92 % × 65 % and
thins the border (2 px, 8 px radius). Enable all five Montserrat sizes in your `lv_conf.h`.

## Pages

- Setup mode: join `WideScreen-Setup` (password `setup123`); the captive portal opens `/setup`.
- Connected: `http://<device-ip>/setup` and `http://<device-ip>/update`.
