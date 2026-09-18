#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <esp_partition.h>
#include <functional>

class OTAManager {
public:
    enum class Stage {
        IDLE,
        STARTING,
        IN_PROGRESS,
        COMPLETE,
        FAILED
    };

    // progress is a percentage, 0-100.
    using LEDProgressCallback = std::function<void(uint8_t progress)>;
    using ScreenProgressCallback = std::function<void(uint8_t progress, Stage stage)>;
    using StartCallback = std::function<void()>;
    using EndCallback = std::function<void(bool success)>;
    // Called after every received chunk, to keep a UI responsive during the transfer.
    using RefreshCallback = std::function<void()>;

    OTAManager();
    ~OTAManager();

    OTAManager(const OTAManager&) = delete;
    OTAManager& operator=(const OTAManager&) = delete;

    // Registers GET /update and POST /ota/upload; both require basic auth unless username or password is empty.
    // partitionLabel "" refuses filesystem uploads (400). Use it whenever that partition holds user data:
    // the upload is a raw overwrite with no confirmation and no undo. Filesystem uploads are also refused unless
    // partitionLabel names the first spiffs-subtype data partition, the only one Update writes.
    bool begin(AsyncWebServer* server, const char* username = "", const char* password = "", const char* partitionLabel = "spiffs");

    // Call regularly; performs the reboot after a successful update.
    void loop();

    void setLEDProgressCallback(LEDProgressCallback callback);
    void setScreenProgressCallback(ScreenProgressCallback callback);
    void setStartCallback(StartCallback callback);
    void setEndCallback(EndCallback callback);
    void setRefreshCallback(RefreshCallback callback);
    // Default true: reboot 2 s after a successful update.
    void setAutoReboot(bool enable);

    Stage getCurrentStage() const { return currentStage_; }
    uint8_t getCurrentProgress() const { return currentProgress_; }
    bool isUpdating() const { return currentStage_ == Stage::IN_PROGRESS; }
    bool isInitialized() const { return initialized_; }

private:
    bool initialized_;
    AsyncWebServer* server_;
    AsyncCallbackWebHandler* pageHandler_;
    AsyncCallbackWebHandler* uploadHandler_;
    Stage currentStage_;
    uint8_t currentProgress_;
    size_t currentSize_;
    size_t totalSize_;
    bool autoReboot_;
    bool rebootRequested_;
    unsigned long rebootTime_;
    String username_;
    String password_;
    String partitionLabel_;

    LEDProgressCallback ledProgressCallback_;
    ScreenProgressCallback screenProgressCallback_;
    StartCallback startCallback_;
    EndCallback endCallback_;
    RefreshCallback refreshCallback_;

    // ESPAsyncWebServer can interleave two POSTs, so exactly one request owns the updater at a time.
    // Only the owner fires the start and end callbacks, and its disconnect aborts the update.
    AsyncWebServerRequest* uploadOwner_;
    bool ownerWriting_;

    unsigned long lastProgressUpdate_;

    // Content-Length may exceed the partition by at most this much: the multipart framing around the image.
    static constexpr size_t MULTIPART_FRAMING_MAX = 1152;

    void setupOTAEndpoints();

    void admitUpload(AsyncWebServerRequest* request, const String& filename);
    void writeUploadChunk(AsyncWebServerRequest* request, uint8_t* data, size_t len, bool final);
    void completeUpload(AsyncWebServerRequest* request);
    bool settleUpload(AsyncWebServerRequest* request);
    void sendRefusal(AsyncWebServerRequest* request, int code, const char* reason);

    const esp_partition_t* filesystemPartition() const;
    bool authenticate(AsyncWebServerRequest* request) const;
};
