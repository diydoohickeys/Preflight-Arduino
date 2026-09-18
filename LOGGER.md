# Logger

A buffered logger for ESP32: writes to Serial, keeps recent entries in a PSRAM-backed ring
buffer, serves them over HTTP, and optionally live-tails them over a WebSocket.

## Quick start

```cpp
#include "Logger.h"

void setup() {
    Serial.begin(115200);

    Logger.begin(300);          // ring-buffer size
    Logger.setMinLevel(LOG_INFO);
    Logger.reportLastCrash();   // decode the previous boot's core dump, if any

    Logger.info("Application started");
    Logger.error("Failed to connect: %s", ssid.c_str());

    Logger.registerEndpoints(&server);        // an AsyncWebServer*
    // or, with live tail:
    // Logger.registerEndpoints(&server, &webSocket);
}
```

`WiFiSetupManager::enableLogger(n)` is a shortcut that calls `begin(n)` and
`registerEndpoints()` against its own server.

`begin(maxEntries, enableSerial, enableWebSocket)`:

| Argument | |
|---|---|
| `maxEntries` | Entries to buffer. **0 disables buffering** — output still reaches Serial, the WebSocket and the output callback, and `/logs` serves an empty list. |
| `enableSerial` | Mirror to Serial. Default true. |
| `enableWebSocket` | Broadcast each entry to an attached `AsyncWebSocket`; the `/logs` page live-tails only while one is attached, and always at `/ws` — serve the socket there. Default true. |

## Levels

`LOG_DEBUG` · `LOG_INFO` · `LOG_WARNING` · `LOG_ERROR`, filtered by `setMinLevel()`
(default `LOG_INFO`). A filtered call returns before formatting, so a suppressed `debug()`
costs one comparison rather than formatting a String and discarding it.

Both a printf form and a `String` form exist for every level.

## Endpoints

| Route | |
|---|---|
| `GET /logs` | HTML view, newest at the bottom, colour-coded by level. |
| `GET /api/logs` | A JSON array of `{"timestamp":…,"level":"INFO","message":"…"}` |
| `POST /api/logs/clear` | Empty the buffer. A cross-origin request is refused with 403. |

**Both views return the last 300 entries by default.** Rendering a full buffer builds one
large response that a RAM-tight server cannot serve; retention is unaffected. `?max=N`
widens the window and `?max=0` returns everything — which is what you need to see the start
of a boot, including anything `reportLastCrash()` printed.

Responses are streamed in chunks rather than built whole, so peak allocation is one batch
however many entries are requested.

### Dropped entries

Entries lost to an allocation failure, or logged before `begin()` was called, are reported
as a synthetic WARNING entry at the head of the rendered range - so a gap in the history
reads as a gap rather than as a working log that happens to be missing lines.

It is an entry rather than a field wrapping the response deliberately: `/api/logs` is read
by external tooling that expects a bare array, and the notice has to be visible to whoever
reads the log, including in a downloaded copy. `droppedCount()` returns the raw number.

⚠ If you use `WiFiSetupManager` **without** calling `enableLogger()`, the manager's own log
calls land here — `/logs` is empty and the dropped-entries notice says why.

### Live tail

```cpp
AsyncWebSocket ws("/ws");
server.addHandler(&ws);
Logger.begin(300, true, /*enableWebSocket=*/true);
Logger.registerEndpoints(&server, &ws);
// or later: Logger.attachWebSocket(&ws);
```

Each entry is broadcast as `{"type":"log","timestamp":…,"level":…,"color":…,"message":…}`.
The `/logs` page appends them live.

`attachWebSocket()` and the WebSocket parameter of `registerEndpoints()` exist only in this
Arduino library, as they take ESPAsyncWebServer types; the rest of the API matches the
ESP-IDF sister exactly.

## Reading entries in code

```cpp
std::vector<LogEntry> recent = Logger.getEntries(50);   // newest 50; 0 = all
```

A copy taken under the logger's lock, so it is safe while other tasks log. The copy
allocates, so under memory pressure it throws `std::bad_alloc`.

## Forwarding elsewhere

```cpp
Logger.setOutputCallback([](LogLevel level, uint32_t ts, const char* msg) {
    // e.g. push over USB CDC to a companion app
});
```

Called for every accepted entry, on whichever task logged it. Keep it fast and
non-blocking; anything slow here is paid by every log call. An exception it throws is
caught and discarded. A line it logs itself is buffered but not passed back to it, so it
cannot recurse.

## Crash reporting

```cpp
Logger.reportLastCrash();   // once, early, after begin()
```

If the previous run ended in a panic and core-dump-to-flash is enabled, this decodes the
stored dump and logs the faulting task, the program counter and (on Xtensa) a backtrace at
ERROR level — through this Logger, so it reaches `/logs` and the WebSocket with no serial
console attached. Resolve the addresses with `addr2line` against the firmware `.elf`.

Requires `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`, a `coredump` partition, and the ELF format.
A no-op otherwise, so it is safe to call unconditionally.

A decoded dump is **erased** so it reports exactly once. A dump that could **not** be
decoded is deliberately **kept**: erasing it would destroy the only copy of the evidence and
guarantee the next panic is guesswork too, and a later build may decode it.

## Memory and exceptions

Entries are allocated from PSRAM where available, falling back to internal heap, so the
buffer does not compete with the network stack.

🚨 **The allocator throws `std::bad_alloc` and the logger catches it, so this header
requires `-fexceptions`** — and because `LogPSRAMAllocator` is a template, the throw is
instantiated in *every* translation unit that includes `Logger.h`, not just the library's
own. `library.json` sets the flag for the library, but PlatformIO library flags do not
propagate to your sources: add `-fexceptions` to your own `build_flags`.

Losing a line under memory pressure is intentional: the Serial output still happens, and
the entry is counted and reported (see Dropped entries). The logger must never crash the device it is diagnosing.
