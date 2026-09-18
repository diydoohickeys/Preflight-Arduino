#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>
#include <deque>
#include <vector>
#include <string>
#include <new>
#include <atomic>
#include <functional>
#include <memory>
#include <ESPAsyncWebServer.h>
#include <esp_heap_caps.h>

enum LogLevel {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARNING = 2,
    LOG_ERROR = 3
};

// Prefers PSRAM, keeping the internal heap for the network stack.
template <typename T>
class LogPSRAMAllocator {
public:
    using value_type = T;
    LogPSRAMAllocator() noexcept = default;
    template <typename U> LogPSRAMAllocator(const LogPSRAMAllocator<U>&) noexcept {}

    // Must throw, never return null: std::string memcpy()s into the result with no null check.
    T* allocate(std::size_t n) {
        void* ptr = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!ptr) ptr = malloc(n * sizeof(T));
        if (!ptr) throw std::bad_alloc();
        return static_cast<T*>(ptr);
    }
    void deallocate(T* p, std::size_t) noexcept { free(p); }
};
template <typename T, typename U>
bool operator==(const LogPSRAMAllocator<T>&, const LogPSRAMAllocator<U>&) { return true; }
template <typename T, typename U>
bool operator!=(const LogPSRAMAllocator<T>&, const LogPSRAMAllocator<U>&) { return false; }

using LogString = std::basic_string<char, std::char_traits<char>, LogPSRAMAllocator<char>>;

struct LogEntry {
    unsigned long timestamp;  // ms since boot
    LogLevel level;
    LogString message;

    LogEntry(unsigned long ts, LogLevel lvl, const char* msg)
        : timestamp(ts), level(lvl), message(msg) {}
    LogEntry(unsigned long ts, LogLevel lvl, const String& msg)
        : timestamp(ts), level(lvl), message(msg.c_str()) {}
};

class LoggerClass {
public:
    LoggerClass();

    // maxEntries 0 disables buffering; serial, the WebSocket and the output callback still get every line.
    void begin(size_t maxEntries = 100, bool enableSerial = true, bool enableWebSocket = true);

    // Registers GET /logs, GET /api/logs and POST /api/logs/clear.
    void registerEndpoints(AsyncWebServer* server, AsyncWebSocket* webSocket = nullptr);

    void log(LogLevel level, const char* format, ...);

    void debug(const char* format, ...);
    void info(const char* format, ...);
    void warning(const char* format, ...);
    void error(const char* format, ...);

    void log(LogLevel level, const String& message);
    void debug(const String& message);
    void info(const String& message);
    void warning(const String& message);
    void error(const String& message);

    // A copy taken under the lock: the newest maxEntries (0 = all).
    std::vector<LogEntry> getEntries(size_t maxEntries = 0) const;

    // Most recent maxEntries (0 = all) as one contiguous string; the web endpoints stream instead.
    String getLogsJSON(size_t maxEntries = 0) const;

    String getLogsHTML(size_t maxEntries = 0) const;

    void clear();

    // Logs the previous boot's panic from a flash core dump (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH,
    // ELF format, coredump partition), then erases it. No-op otherwise; call once after begin().
    void reportLastCrash();

    size_t count() const;

    void setSerialEnabled(bool enabled) { serialEnabled_ = enabled; }

    bool isSerialEnabled() const { return serialEnabled_; }

    void setWebSocketEnabled(bool enabled) { webSocketEnabled_ = enabled; }

    bool isWebSocketEnabled() const { return webSocketEnabled_; }

    void attachWebSocket(AsyncWebSocket* webSocket);

    void setMinLevel(LogLevel level) { minLevel_ = level; }

    // timestamp is ms since boot.
    using LogOutputCallback = std::function<void(LogLevel level, uint32_t timestamp, const char* message)>;

    // Extra sink (e.g. USB CDC) called for every line on the logging task; nullptr disables.
    // Lines logged from inside the callback are buffered but not fed back to it.
    void setOutputCallback(LogOutputCallback callback);

    LogLevel getMinLevel() const { return minLevel_; }

    // Entries lost to an allocation failure or logged before begin(); shown by /logs and /api/logs.
    uint32_t droppedCount() const { return droppedEntries_.load(std::memory_order_relaxed); }

private:
    // deque, not vector: at capacity every line pops the front, which moves every vector entry.
    std::deque<LogEntry, LogPSRAMAllocator<LogEntry>> entries_;
    size_t maxEntries_;
    // Counted, not logged (logging on the failure path re-enters addEntry); any task may bump it.
    std::atomic<uint32_t> droppedEntries_{0};
    bool serialEnabled_;
    bool webSocketEnabled_;
    LogLevel minLevel_;
    SemaphoreHandle_t mutex_;
    AsyncWebSocket* webSocket_;
    // Shared so a caller can hold it across the call while another task replaces it.
    std::shared_ptr<const LogOutputCallback> outputCallback_;

    // Takes a C string: copying the message would allocate on the path that must survive OOM.
    void addEntry(LogLevel level, const char* message);
    const char* getLevelString(LogLevel level) const;
    const char* getLevelColor(LogLevel level) const;
    void printToSerial(unsigned long timestamp, LogLevel level, const char* message);
    // Takes components, not a LogEntry: building one allocates on the path that must survive OOM.
    void broadcastLogEntry(unsigned long timestamp, LogLevel level, const char* message);

    // The /logs page live-tails only when a socket is attached to feed it.
    bool pageLiveTail() const { return webSocketEnabled_ && webSocket_ != nullptr; }

    static String jsonEscape(const char* text);
    // Take components, not a LogEntry, so the dropped notice needs no LogString allocation.
    void appendEntryJson(String& out, unsigned long timestamp, LogLevel level, const char* message) const;
    void appendEntryHtml(String& out, unsigned long timestamp, LogLevel level, const char* message) const;
    void appendDroppedNotice(String& out, bool json, bool& firstOut) const;

    // Appends the next batch from `index` (advancing it); false once the range is exhausted.
    // Chunked because one contiguous ~60-90 KB response fails on a merely fragmented heap.
    bool renderLogBatch(String& out, size_t& index, size_t maxEntries,
                        bool positioned, bool json, bool& firstOut) const;

    AsyncWebServerResponse* beginLogStream(AsyncWebServerRequest* request,
                                           size_t maxEntries, bool json);
};

extern LoggerClass Logger;

#endif // LOGGER_H
