#ifndef WIFI_SETUP_SAME_ORIGIN_H
#define WIFI_SETUP_SAME_ORIGIN_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

namespace wifisetup {

// Guards state-changing POSTs against cross-site requests. Browsers send Origin (or at
// least Referer) on a cross-site POST; tools and scripts send neither, so an absent header
// passes. An "Origin: null" (sandboxed frame, file://) has no authority and is refused.
inline bool isSameOrigin(AsyncWebServerRequest* request) {
    const AsyncWebHeader* source = request->getHeader("Origin");
    if (!source) {
        source = request->getHeader("Referer");
    }
    if (!source) {
        return true;
    }
    const AsyncWebHeader* host = request->getHeader("Host");
    if (!host) {
        return false;
    }

    const String& value = source->value();
    int start = value.indexOf("://");
    if (start < 0) {
        return false;
    }
    start += 3;
    int end = value.indexOf('/', start);
    String authority = end < 0 ? value.substring(start) : value.substring(start, end);
    return authority.equalsIgnoreCase(host->value());
}

inline void sendCrossOriginRefused(AsyncWebServerRequest* request) {
    request->send(403, "text/plain", "Cross-origin request refused");
}

}  // namespace wifisetup

#endif  // WIFI_SETUP_SAME_ORIGIN_H
