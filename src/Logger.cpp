#include "Logger.h"
#include "SameOrigin.h"
#include "html/logs_html.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#include "esp_partition.h"
#endif

namespace {
    const char* const ENTRIES_MARKER = "%LOG_ENTRIES%";

    // Set while this task runs the output callback, so a line it logs is not fed back to it.
    thread_local bool inOutputCallback = false;

    // Releases on every exit, including a bad_alloc thrown while held: a leaked lock blocks every later log call.
    class MutexLock {
    public:
        explicit MutexLock(SemaphoreHandle_t mutex)
            : mutex_(mutex)
            , held_(mutex != nullptr && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {}
        ~MutexLock() {
            if (held_) xSemaphoreGive(mutex_);
        }
        MutexLock(const MutexLock&) = delete;
        MutexLock& operator=(const MutexLock&) = delete;

        bool held() const { return held_; }

    private:
        SemaphoreHandle_t mutex_;
        bool held_;
    };

    // Only template text is substituted, never the entries: a logged message may contain a marker.
    String logsPageSlice(const char* text, size_t length, size_t total, bool webSocket) {
        String slice(text, (unsigned int)length);
        slice.replace("%LOG_COUNT%", String((unsigned long)total));
        slice.replace("%LOG_WS%", webSocket ? "true" : "false");
        return slice;
    }
}

LoggerClass Logger;

LoggerClass::LoggerClass()
    : maxEntries_(100)
    , serialEnabled_(true)
    , webSocketEnabled_(true)
    , minLevel_(LOG_INFO)
    , mutex_(nullptr)
    , webSocket_(nullptr)
{
}

void LoggerClass::begin(size_t maxEntries, bool enableSerial, bool enableWebSocket) {
    maxEntries_ = maxEntries;
    serialEnabled_ = enableSerial;
    webSocketEnabled_ = enableWebSocket;

    if (mutex_ == nullptr) {
        mutex_ = xSemaphoreCreateMutex();
    }

    info("Logger started, buffering %u entries", (unsigned)maxEntries);
}

// Filter before formatting: a filtered debug() costs one compare, not a vsnprintf.
void LoggerClass::log(LogLevel level, const char* format, ...) {
    if (level < minLevel_) return;
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    addEntry(level, buffer);
}

void LoggerClass::debug(const char* format, ...) {
    if (LOG_DEBUG < minLevel_) return;
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    addEntry(LOG_DEBUG, buffer);
}

void LoggerClass::info(const char* format, ...) {
    if (LOG_INFO < minLevel_) return;
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    addEntry(LOG_INFO, buffer);
}

void LoggerClass::warning(const char* format, ...) {
    if (LOG_WARNING < minLevel_) return;
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    addEntry(LOG_WARNING, buffer);
}

void LoggerClass::error(const char* format, ...) {
    if (LOG_ERROR < minLevel_) return;
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    addEntry(LOG_ERROR, buffer);
}

void LoggerClass::log(LogLevel level, const String& message) {
    addEntry(level, message.c_str());
}

void LoggerClass::debug(const String& message) {
    addEntry(LOG_DEBUG, message.c_str());
}

void LoggerClass::info(const String& message) {
    addEntry(LOG_INFO, message.c_str());
}

void LoggerClass::warning(const String& message) {
    addEntry(LOG_WARNING, message.c_str());
}

void LoggerClass::error(const String& message) {
    addEntry(LOG_ERROR, message.c_str());
}

void LoggerClass::addEntry(LogLevel level, const char* message) {
    if (level < minLevel_) {
        return;
    }

    const unsigned long timestamp = millis();

    // Runs from any context, including C callbacks where an escaping exception terminate()s:
    // an allocation failure drops the line (counted). Never log from the catch - it re-enters.
    bool buffered = false;
    if (mutex_ && maxEntries_ > 0) {
        try {
            LogEntry newEntry(timestamp, level, message);
            MutexLock lock(mutex_);
            if (lock.held()) {
                while (entries_.size() >= maxEntries_) {
                    entries_.pop_front();
                }
                entries_.push_back(std::move(newEntry));
                buffered = true;
            }
        } catch (...) {
        }
    }
    // begin(0) turns buffering off by choice; that is not a loss.
    if (!buffered && maxEntries_ > 0) {
        droppedEntries_.fetch_add(1, std::memory_order_relaxed);
    }

    if (serialEnabled_) {
        printToSerial(timestamp, level, message);
    }

    try {
        broadcastLogEntry(timestamp, level, message);
    } catch (...) {
        // Only the live push is lost; the buffered copy survives, so it is not counted as dropped.
    }

    if (inOutputCallback) {
        return;
    }
    try {
        std::shared_ptr<const LogOutputCallback> callback;
        {
            MutexLock lock(mutex_);
            callback = outputCallback_;
        }
        if (callback && *callback) {
            inOutputCallback = true;
            (*callback)(level, timestamp, message);
            inOutputCallback = false;
        }
    } catch (...) {
        inOutputCallback = false;
        // Only this sink misses the line; the buffered copy survives, so it is not counted as dropped.
    }
}

void LoggerClass::setOutputCallback(LogOutputCallback callback) {
    std::shared_ptr<const LogOutputCallback> next;
    if (callback) {
        next = std::make_shared<LogOutputCallback>(std::move(callback));
    }
    // Swapped, not assigned: the old callback is destroyed after the lock is released.
    MutexLock lock(mutex_);
    outputCallback_.swap(next);
}

std::vector<LogEntry> LoggerClass::getEntries(size_t maxEntries) const {
    std::vector<LogEntry> snapshot;
    MutexLock lock(mutex_);
    if (!lock.held()) return snapshot;
    const size_t total = entries_.size();
    const size_t start = (maxEntries > 0 && total > maxEntries) ? total - maxEntries : 0;
    snapshot.assign(entries_.begin() + static_cast<std::ptrdiff_t>(start), entries_.end());
    return snapshot;
}

void LoggerClass::printToSerial(unsigned long ms, LogLevel level, const char* message) {
    unsigned long seconds = ms / 1000;
    unsigned long minutes = seconds / 60;
    unsigned long hours = minutes / 60;
    unsigned long days = hours / 24;

    seconds %= 60;
    minutes %= 60;
    hours %= 24;

    Serial.print("[");

    if (days > 0) {
        Serial.print(days);
        Serial.print("d ");
    }

    if (hours > 0 || days > 0) {
        Serial.print(hours);
        Serial.print("h ");
    }

    if (minutes > 0 || hours > 0 || days > 0) {
        Serial.print(minutes);
        Serial.print("m ");
    }

    Serial.print(seconds);
    Serial.print(".");
    unsigned long milliseconds = ms % 1000;
    if (milliseconds < 100) Serial.print("0");
    if (milliseconds < 10) Serial.print("0");
    Serial.print(milliseconds);
    Serial.print("s] [");
    Serial.print(getLevelString(level));
    Serial.print("] ");
    Serial.println(message);
}

const char* LoggerClass::getLevelString(LogLevel level) const {
    switch (level) {
        case LOG_DEBUG:   return "DEBUG";
        case LOG_INFO:    return "INFO";
        case LOG_WARNING: return "WARN";
        case LOG_ERROR:   return "ERROR";
        default:          return "UNKNOWN";
    }
}

const char* LoggerClass::getLevelColor(LogLevel level) const {
    switch (level) {
        case LOG_DEBUG:   return "#888888";
        case LOG_INFO:    return "#4A90E2";
        case LOG_WARNING: return "#FFA500";
        case LOG_ERROR:   return "#E74C3C";
        default:          return "#FFFFFF";
    }
}

// Control bytes must be \uXXXX-escaped: one raw byte invalidates the whole /api/logs payload.
String LoggerClass::jsonEscape(const char* text) {
    static const char* kHex = "0123456789abcdef";
    String out;
    for (const char* p = text; *p; ++p) {
        unsigned char c = (unsigned char)*p;
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

void LoggerClass::appendEntryJson(String& out, unsigned long timestamp, LogLevel level,
                                  const char* message) const {
    out += "{";
    out += "\"timestamp\":" + String(timestamp) + ",";
    out += "\"level\":\"" + String(getLevelString(level)) + "\",";
    out += "\"message\":\"" + jsonEscape(message) + "\"";
    out += "}";
}

void LoggerClass::appendEntryHtml(String& out, unsigned long timestamp, LogLevel level,
                                  const char* message) const {
    out += "<div class='log-entry'>";
    out += "<span class='timestamp'>" + String(timestamp) + "ms</span>";
    out += "<span class='level' style='color:" + String(getLevelColor(level)) + "'>";
    out += String(getLevelString(level)) + "</span>";

    String escapedMsg = message;
    escapedMsg.replace("&", "&amp;");
    escapedMsg.replace("<", "&lt;");
    escapedMsg.replace(">", "&gt;");
    out += "<span class='message'>" + escapedMsg + "</span>";

    out += "</div>";
}

// Keeps peak allocation at a few KB however many entries are requested.
static constexpr size_t LOG_STREAM_BATCH = 24;

// Reported as a synthetic entry, not a response field: /api/logs readers expect a bare
// array, and the gap must survive in a downloaded copy.
void LoggerClass::appendDroppedNotice(String& out, bool json, bool& firstOut) const {
    uint32_t dropped = droppedCount();
    if (dropped == 0) return;

    char notice[128];
    snprintf(notice, sizeof(notice),
             "[logger] %u earlier entries were dropped (out of memory, or logged before "
             "Logger.begin())", (unsigned)dropped);
    if (json) {
        if (!firstOut) out += ',';
        firstOut = false;
        appendEntryJson(out, millis(), LOG_WARNING, notice);
    } else {
        appendEntryHtml(out, millis(), LOG_WARNING, notice);
    }
}

bool LoggerClass::renderLogBatch(String& out, size_t& index, size_t maxEntries,
                                 bool positioned, bool json, bool& firstOut) const {
    if (!positioned) {
        appendDroppedNotice(out, json, firstOut);
    }

    size_t produced = 0;
    MutexLock lock(mutex_);
    if (!lock.held()) return false;
    const size_t total = entries_.size();
    if (!positioned) {
        index = (maxEntries > 0 && total > maxEntries) ? total - maxEntries : 0;
    }
    // The mutex is released between batches, so trimming can shift indices (a repeated or
    // skipped line); holding it across a network send would stall every task that logs.
    for (; index < total && produced < LOG_STREAM_BATCH; index++, produced++) {
        const LogEntry& entry = entries_[index];
        if (json) {
            if (!firstOut) out += ',';
            firstOut = false;
            appendEntryJson(out, entry.timestamp, entry.level, entry.message.c_str());
        } else {
            appendEntryHtml(out, entry.timestamp, entry.level, entry.message.c_str());
        }
    }

    return produced == LOG_STREAM_BATCH;
}

String LoggerClass::getLogsJSON(size_t maxEntries) const {
    String json = "[";
    bool wroteAny = false;
    appendDroppedNotice(json, /*json=*/true, wroteAny);

    {
        MutexLock lock(mutex_);
        if (lock.held()) {
            size_t count = entries_.size();
            size_t start = 0;

            if (maxEntries > 0 && count > maxEntries) {
                start = count - maxEntries;
            }

            for (size_t i = start; i < count; i++) {
                if (wroteAny) json += ",";
                wroteAny = true;
                const LogEntry& entry = entries_[i];
                appendEntryJson(json, entry.timestamp, entry.level, entry.message.c_str());
            }
        }
    }

    json += "]";
    return json;
}

String LoggerClass::getLogsHTML(size_t maxEntries) const {
    String logEntries;
    bool unusedFirst = true;
    appendDroppedNotice(logEntries, /*json=*/false, unusedFirst);
    size_t totalEntries = 0;

    {
        MutexLock lock(mutex_);
        if (lock.held()) {
            totalEntries = entries_.size();
            size_t start = 0;

            if (maxEntries > 0 && totalEntries > maxEntries) {
                start = totalEntries - maxEntries;
            }

            for (size_t i = start; i < totalEntries; i++) {
                const LogEntry& entry = entries_[i];
                appendEntryHtml(logEntries, entry.timestamp, entry.level, entry.message.c_str());
            }
        }
    }

    // logs_html.h is generated from logs.html by extras/convert_html.py; edit the .html.
    const char* tpl = LOGS_HTML;
    const char* mark = strstr(tpl, ENTRIES_MARKER);
    if (!mark) {
        return logsPageSlice(tpl, strlen(tpl), totalEntries, pageLiveTail());
    }
    const char* after = mark + strlen(ENTRIES_MARKER);
    String html = logsPageSlice(tpl, (size_t)(mark - tpl), totalEntries, pageLiveTail());
    html += logEntries;
    html += logsPageSlice(after, strlen(after), totalEntries, pageLiveTail());
    return html;
}

size_t LoggerClass::count() const {
    MutexLock lock(mutex_);
    return lock.held() ? entries_.size() : 0;
}

void LoggerClass::clear() {
    {
        MutexLock lock(mutex_);
        if (lock.held()) {
            entries_.clear();
        }
    }

    info("Logs cleared");
}

void LoggerClass::reportLastCrash() {
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    if (esp_core_dump_image_check() != ESP_OK) {
        return;
    }

    // A few hundred bytes: heap it rather than grow the caller's stack.
    esp_core_dump_summary_t* summary =
        (esp_core_dump_summary_t*)malloc(sizeof(esp_core_dump_summary_t));
    if (!summary) {
        error("Crash report: no heap for coredump summary; leaving dump on flash");
        return;
    }

    bool decoded = false;
    if (esp_core_dump_get_summary(summary) == ESP_OK) {
        error("CRASH (prev boot): task '%s' faulted at PC=0x%08x - addr2line against the .elf",
              summary->exc_task, (unsigned)summary->exc_pc);
#if defined(__XTENSA__)
        // Only Xtensa fills the backtrace; on RISC-V the PC above is the anchor.
        uint32_t depth = summary->exc_bt_info.depth;
        if (depth > 16) depth = 16;
        for (uint32_t i = 0; i < depth; i++) {
            error("CRASH:   bt[%u] 0x%08x", (unsigned)i,
                  (unsigned)summary->exc_bt_info.bt[i]);
        }
        if (summary->exc_bt_info.corrupted) {
            warning("CRASH: backtrace flagged corrupted (partial/unreliable)");
        }
#endif
        decoded = true;
    } else {
        // get_summary() returns a bare ESP_FAIL for both a corrupt ELF and a failed mmap (which
        // needs a free 64 KB MMU window); probe the mmap to say which, as the fixes differ.
        size_t dumpAddr = 0, dumpSize = 0;
        esp_err_t ierr = esp_core_dump_image_get(&dumpAddr, &dumpSize);
        error("Crash report: coredump present but summary decode failed "
              "(image_get=%s addr=0x%08x size=%u)",
              esp_err_to_name(ierr), (unsigned)dumpAddr, (unsigned)dumpSize);

        const esp_partition_t* part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
        if (!part) {
            error("Crash report:   no coredump partition in the table");
        } else {
            const void* mapAddr = nullptr;
            esp_partition_mmap_handle_t mapHandle = 0;
            esp_err_t merr = esp_partition_mmap(part, 0, part->size,
                                                ESP_PARTITION_MMAP_DATA, &mapAddr, &mapHandle);
            if (merr == ESP_OK) {
                esp_partition_munmap(mapHandle);
                error("Crash report:   partition mmap OK - so it is the ELF that did not parse");
            } else {
                error("Crash report:   partition mmap FAILED (%s) - the dump is INTACT but "
                      "this firmware has no free MMU window to read it through",
                      esp_err_to_name(merr));
            }
        }
    }

    free(summary);

    if (decoded) {
        if (esp_core_dump_image_erase() != ESP_OK) {
            warning("Crash report: failed to erase coredump image");
        }
    } else {
        // Never erase a dump that could not be read: a later build may still decode it.
        warning("Crash report: KEEPING the coredump image (it could not be read, so it is "
                "not spent). It will report once per boot until a build decodes it.");
    }
#endif  // CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
}

// nullptr when out of memory; the handler then sends nothing and the server answers itself.
AsyncWebServerResponse* LoggerClass::beginLogStream(AsyncWebServerRequest* request,
                                                    size_t maxEntries, bool json) {
    // Shared so the state outlives this call: AsyncWebServer calls the filler until it returns 0.
    struct StreamState {
        String pending;
        String tail;
        size_t index = 0;
        bool positioned = false;
        bool firstEntry = true;
        bool entriesDone = false;
        bool tailQueued = false;
    };

    try {
        auto state = std::make_shared<StreamState>();

        if (json) {
            state->pending = "[";
            state->tail = "]";
        } else {
            const char* tpl = LOGS_HTML;
            const char* mark = strstr(tpl, ENTRIES_MARKER);
            const size_t total = this->count();
            if (!mark) {
                state->pending = logsPageSlice(tpl, strlen(tpl), total, pageLiveTail());
                state->entriesDone = true;
            } else {
                const char* after = mark + strlen(ENTRIES_MARKER);
                state->pending = logsPageSlice(tpl, (size_t)(mark - tpl), total, pageLiveTail());
                state->tail = logsPageSlice(after, strlen(after), total, pageLiveTail());
            }
        }

        return request->beginChunkedResponse(json ? "application/json" : "text/html",
            [this, state, maxEntries, json](uint8_t* buffer, size_t maxLen, size_t) -> size_t {
                while (state->pending.length() == 0) {
                    if (!state->entriesDone) {
                        bool more = this->renderLogBatch(state->pending, state->index, maxEntries,
                                                         state->positioned, json, state->firstEntry);
                        state->positioned = true;
                        if (!more) state->entriesDone = true;
                        continue;
                    }
                    if (!state->tailQueued) {
                        state->tailQueued = true;
                        state->pending = state->tail;
                        state->tail = String();
                        continue;
                    }
                    return 0;
                }

                size_t take = state->pending.length() < maxLen ? state->pending.length() : maxLen;
                memcpy(buffer, state->pending.c_str(), take);
                state->pending.remove(0, take);
                return take;
            });
    } catch (...) {
        return nullptr;
    }
}

void LoggerClass::registerEndpoints(AsyncWebServer* server, AsyncWebSocket* webSocket) {
    if (!server) {
        return;
    }

    if (webSocket) {
        attachWebSocket(webSocket);
    }

    // Default window is the most recent 300, which can hide the start of a boot (where the
    // crash report prints). ?max=N widens it, ?max=0 returns everything.
    auto logWindow = [](AsyncWebServerRequest* request) -> size_t {
        if (!request->hasParam("max")) return 300;
        long v = request->getParam("max")->value().toInt();
        return (v >= 0) ? (size_t)v : 300;
    };

    server->on("/logs", HTTP_GET, [this, logWindow](AsyncWebServerRequest* request) {
        if (AsyncWebServerResponse* response = this->beginLogStream(request, logWindow(request), /*json=*/false)) {
            request->send(response);
        }
    });

    server->on("/api/logs", HTTP_GET, [this, logWindow](AsyncWebServerRequest* request) {
        if (AsyncWebServerResponse* response = this->beginLogStream(request, logWindow(request), /*json=*/true)) {
            request->send(response);
        }
    });

    server->on("/api/logs/clear", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (!wifisetup::isSameOrigin(request)) {
            wifisetup::sendCrossOriginRefused(request);
            return;
        }
        this->clear();
        request->send(200, "application/json", "{\"status\":\"success\",\"message\":\"Logs cleared\"}");
    });

    info("Log endpoints registered (/logs, /api/logs)");
}

void LoggerClass::attachWebSocket(AsyncWebSocket* webSocket) {
    webSocket_ = webSocket;
    if (webSocket_) {
        info("Log WebSocket attached");
    }
}

void LoggerClass::broadcastLogEntry(unsigned long timestamp, LogLevel level,
                                    const char* message) {
    if (!webSocket_ || !webSocketEnabled_) {
        return;
    }

    String json = "{";
    json += "\"type\":\"log\",";
    json += "\"timestamp\":" + String(timestamp) + ",";
    json += "\"level\":\"" + String(getLevelString(level)) + "\",";
    json += "\"color\":\"" + String(getLevelColor(level)) + "\",";

    json += "\"message\":\"" + jsonEscape(message) + "\"";

    json += "}";

    webSocket_->textAll(json);
}
