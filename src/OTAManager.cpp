#include "OTAManager.h"
#include "Logger.h"
#include "SameOrigin.h"
#include "html/ota_html.h"
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

namespace {

// Held in request->_tempObject, which AsyncWebServerRequest releases with free(): keep it trivially destructible.
struct UploadRequestState {
    int code;  // 0 while the request is accepted
    char reason[96];
};

UploadRequestState* requestState(AsyncWebServerRequest* request) {
    return static_cast<UploadRequestState*>(request->_tempObject);
}

__attribute__((format(printf, 3, 4)))
void refuse(UploadRequestState* state, int code, const char* format, ...) {
    state->code = code;
    va_list args;
    va_start(args, format);
    vsnprintf(state->reason, sizeof(state->reason), format, args);
    va_end(args);
}

}  // namespace

OTAManager::OTAManager()
    : initialized_(false)
    , server_(nullptr)
    , pageHandler_(nullptr)
    , uploadHandler_(nullptr)
    , currentStage_(Stage::IDLE)
    , currentProgress_(0)
    , currentSize_(0)
    , totalSize_(0)
    , autoReboot_(true)
    , rebootRequested_(false)
    , rebootTime_(0)
    , ledProgressCallback_(nullptr)
    , screenProgressCallback_(nullptr)
    , startCallback_(nullptr)
    , endCallback_(nullptr)
    , refreshCallback_(nullptr)
    , uploadOwner_(nullptr)
    , ownerWriting_(false)
    , lastProgressUpdate_(0)
{
}

// Each route captures `this`, so the routes go with it; the server must not have been destroyed yet.
OTAManager::~OTAManager() {
    if (uploadOwner_) {
        AsyncWebServerRequest* owner = uploadOwner_;
        uploadOwner_ = nullptr;
        ownerWriting_ = false;
        owner->onDisconnect(nullptr);
        Update.abort();
        // Its handler is deleted below, so the connection is dropped before another chunk can reach it.
        owner->abort();
    }
    if (server_) {
        server_->removeHandler(pageHandler_);
        server_->removeHandler(uploadHandler_);
    }
}

bool OTAManager::begin(AsyncWebServer* server, const char* username, const char* password, const char* partitionLabel) {
    if (initialized_) {
        return true;
    }

    if (!server) {
        Logger.error("OTAManager: No web server provided!");
        return false;
    }

    server_ = server;
    username_ = username ? username : "";
    password_ = password ? password : "";
    partitionLabel_ = partitionLabel ? partitionLabel : "spiffs";

    setupOTAEndpoints();

    Logger.info("OTA Manager initialized successfully");
    if (partitionLabel_.isEmpty()) {
        Logger.info("Filesystem updates: DISABLED");
    } else if (!filesystemPartition()) {
        Logger.warning("Filesystem updates: DISABLED - '%s' is not the first spiffs partition, the only one Update writes",
                       partitionLabel_.c_str());
    } else {
        Logger.info("Filesystem partition label: %s", partitionLabel_.c_str());
    }
    initialized_ = true;
    return true;
}

void OTAManager::loop() {
    if (!initialized_) {
        return;
    }

    if (rebootRequested_ && autoReboot_ && (millis() - rebootTime_ > 2000)) {
        Logger.info("Rebooting...");
        ESP.restart();
    }
}

void OTAManager::setLEDProgressCallback(LEDProgressCallback callback) {
    ledProgressCallback_ = callback;
}

void OTAManager::setScreenProgressCallback(ScreenProgressCallback callback) {
    screenProgressCallback_ = callback;
}

void OTAManager::setStartCallback(StartCallback callback) {
    startCallback_ = callback;
}

void OTAManager::setEndCallback(EndCallback callback) {
    endCallback_ = callback;
}

void OTAManager::setRefreshCallback(RefreshCallback callback) {
    refreshCallback_ = callback;
}

void OTAManager::setAutoReboot(bool enable) {
    autoReboot_ = enable;
}

void OTAManager::setupOTAEndpoints() {
    // The page hides its Filesystem card unless OTA_FS_ENABLED is injected at the marker (inline, so no
    // reveal flash and no extra route); a missing marker leaves it hidden, the safe default.
    pageHandler_ = &server_->on("/update", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!authenticate(request)) {
            sendRefusal(request, 401, "Unauthorized");
            return;
        }

        static const char kMarker[] = "<!--OTA_CAPS-->";
        const char* marker = strstr(OTA_HTML, kMarker);
        if (!marker) {
            request->send(200, "text/html", OTA_HTML);
            return;
        }

        const char* caps = filesystemPartition()
                               ? "<script>window.OTA_FS_ENABLED=true;</script>"
                               : "<script>window.OTA_FS_ENABLED=false;</script>";

        String html;
        html.reserve(strlen(OTA_HTML) + 48);
        html.concat(OTA_HTML, marker - OTA_HTML);
        html.concat(caps);
        html.concat(marker + (sizeof(kMarker) - 1));
        request->send(200, "text/html", html);
    });

    // ESPAsyncWebServer keeps streaming the body after a chunk refuses it and only then runs the
    // completion handler, so a refusal is carried to it per request and sent from there, once.
    uploadHandler_ = &server_->on("/ota/upload", HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            completeUpload(request);
        },
        [this](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            // Every gate, auth included, must be here: the body is written before the completion
            // handler runs, so auth checked only there still lets an unauthenticated image boot.
            if (index == 0 && !request->_tempObject) {
                admitUpload(request, filename);
            }

            // A refused request, and any request but the owner, drains without reaching the updater.
            if (request != uploadOwner_ || !ownerWriting_) {
                return;
            }

            writeUploadChunk(request, data, len, final);
        }
    );
}

void OTAManager::admitUpload(AsyncWebServerRequest* request, const String& filename) {
    auto* state = static_cast<UploadRequestState*>(calloc(1, sizeof(UploadRequestState)));
    if (!state) {
        Logger.error("OTA: upload rejected - out of memory");
        return;
    }
    request->_tempObject = state;

    if (!authenticate(request)) {
        Logger.warning("OTA: upload rejected - authentication required");
        refuse(state, 401, "Unauthorized");
        return;
    }

    // A no-cors cross-site POST needs no preflight, so without credentials any page could flash the device.
    if (!wifisetup::isSameOrigin(request)) {
        Logger.warning("OTA: upload rejected - cross-origin request");
        refuse(state, 403, "Cross-origin request refused");
        return;
    }

    // Overlapping POSTs would interleave their writes into one partition.
    if (uploadOwner_) {
        Logger.warning("OTA: upload rejected - an update is already running");
        refuse(state, 409, "An update is already in progress");
        return;
    }

    // A staged image is the boot target: a second upload would erase it, and a restart
    // mid-write boots the previous firmware instead.
    if (currentStage_ == Stage::COMPLETE) {
        Logger.warning("OTA: upload rejected - an update is waiting for a restart");
        refuse(state, 409, "An update is already installed and waiting for a restart");
        return;
    }

    String type = "firmware";
    if (request->hasParam("type", true)) {
        type = request->getParam("type", true)->value();
    } else if (request->hasParam("type")) {
        type = request->getParam("type")->value();
    }
    const bool isFilesystem = type == "filesystem";

    if (isFilesystem && partitionLabel_.isEmpty()) {
        Logger.warning("OTA: filesystem update refused - disabled on this device");
        refuse(state, 400, "Filesystem updates are disabled on this device");
        return;
    }

    const esp_partition_t* target = isFilesystem ? filesystemPartition() : esp_ota_get_next_update_partition(nullptr);
    if (!target && isFilesystem) {
        Logger.warning("OTA: filesystem update refused - '%s' is not the first spiffs partition, the only one Update writes",
                       partitionLabel_.c_str());
        refuse(state, 400, "Filesystem update refused: '%s' is not the partition the updater writes",
               partitionLabel_.c_str());
        return;
    }
    if (!target) {
        Logger.error("OTA: upload rejected - No OTA partition");
        refuse(state, 500, "No OTA partition");
        return;
    }

    // Before anything is written: an overflow found mid-write would leave the old filesystem destroyed.
    // The body wraps the image in multipart framing; the exact limit is enforced on the bytes written.
    const size_t bodySize = request->contentLength();
    if (bodySize > target->size + MULTIPART_FRAMING_MAX) {
        Logger.warning("OTA: upload rejected - %u-byte body exceeds the '%s' partition (%u bytes)",
                       (unsigned)bodySize, target->label, (unsigned)target->size);
        refuse(state, 413, "Image is larger than the '%s' partition (%u bytes)",
               target->label, (unsigned)target->size);
        return;
    }

    uploadOwner_ = request;
    request->onDisconnect([this, request]() {
        if (uploadOwner_ == request) {
            Logger.warning("OTA: client disconnected mid-upload at %u bytes", (unsigned)currentSize_);
            settleUpload(request);
        }
    });

    if (isFilesystem) {
        Logger.info("Starting filesystem OTA update (partition: %s)", partitionLabel_.c_str());
    } else {
        Logger.info("Starting firmware OTA update");
    }
    currentSize_ = 0;
    currentProgress_ = 0;
    // The whole multipart body, slightly larger than the image; the percentage is clamped.
    totalSize_ = bodySize;

    currentStage_ = Stage::STARTING;
    if (screenProgressCallback_) {
        screenProgressCallback_(0, Stage::STARTING);
    }
    if (ledProgressCallback_) {
        ledProgressCallback_(0);
    }
    if (startCallback_) {
        startCallback_();
    }

    if (!Update.begin(UPDATE_SIZE_UNKNOWN, isFilesystem ? U_SPIFFS : U_FLASH, -1, 0,
                      isFilesystem ? partitionLabel_.c_str() : nullptr)) {
        Logger.error("Update.begin failed: %s", Update.errorString());
        refuse(state, 500, "Update.begin failed: %s", Update.errorString());
        return;
    }

    ownerWriting_ = true;
    currentStage_ = Stage::IN_PROGRESS;
    Logger.info("OTA update session opened (file: %s, body: %u bytes)",
                filename.c_str(), (unsigned)totalSize_);
    lastProgressUpdate_ = 0;
}

void OTAManager::writeUploadChunk(AsyncWebServerRequest* request, uint8_t* data, size_t len, bool final) {
    if (len) {
        if (Update.write(data, len) != len) {
            Logger.error("Update.write failed: %s", Update.errorString());
            refuse(requestState(request), 500, "Update.write failed: %s", Update.errorString());
            // Frees Update's ~4 KB buffer and half-written state now, not at the next begin().
            Update.abort();
            ownerWriting_ = false;
            return;
        }

        currentSize_ += len;

        if (totalSize_ > 0) {
            size_t pct = (currentSize_ * 100) / totalSize_;
            currentProgress_ = pct > 100 ? 100 : (uint8_t)pct;

            if (millis() - lastProgressUpdate_ > 100) {
                Logger.debug("OTA Progress: %u%%", currentProgress_);

                if (screenProgressCallback_) {
                    screenProgressCallback_(currentProgress_, Stage::IN_PROGRESS);
                }
                if (ledProgressCallback_) {
                    ledProgressCallback_(currentProgress_);
                }

                lastProgressUpdate_ = millis();
            }
        }

        if (refreshCallback_) {
            refreshCallback_();
        }

        yield();
    }

    if (final) {
        ownerWriting_ = false;
        Logger.info("OTA transfer done - %u bytes written", (unsigned)currentSize_);
        if (currentSize_ == 0) {
            Update.abort();
            refuse(requestState(request), 400, "No image data received");
        } else if (Update.end(true)) {
            Logger.info("OTA update finished successfully");
        } else {
            Logger.error("Update.end failed: %s", Update.errorString());
            refuse(requestState(request), 500, "Update.end failed: %s", Update.errorString());
            Update.abort();
        }
    }
}

void OTAManager::completeUpload(AsyncWebServerRequest* request) {
    if (request == uploadOwner_) {
        if (settleUpload(request)) {
            request->send(200, "text/plain", "Update successful");
        } else {
            const UploadRequestState* state = requestState(request);
            sendRefusal(request, state->code, state->reason);
        }
        return;
    }

    const UploadRequestState* state = requestState(request);
    if (state) {
        sendRefusal(request, state->code, state->reason);
        return;
    }

    // No file part reached the upload handler, so its gates never ran.
    if (!authenticate(request)) {
        sendRefusal(request, 401, "Unauthorized");
    } else if (!wifisetup::isSameOrigin(request)) {
        sendRefusal(request, 403, "Cross-origin request refused");
    } else {
        sendRefusal(request, 400, "No file in the upload");
    }
}

// Releases the updater and fires the end callback; runs once per owned upload, from its completion or its disconnect.
bool OTAManager::settleUpload(AsyncWebServerRequest* request) {
    UploadRequestState* state = requestState(request);
    if (ownerWriting_) {
        Update.abort();
        ownerWriting_ = false;
        refuse(state, 400, "Upload incomplete");
    }

    const bool success = state->code == 0;
    uploadOwner_ = nullptr;

    if (endCallback_) {
        endCallback_(success);
    }

    if (success) {
        currentStage_ = Stage::COMPLETE;
        currentProgress_ = 100;

        if (screenProgressCallback_) {
            screenProgressCallback_(100, Stage::COMPLETE);
        }
        if (ledProgressCallback_) {
            ledProgressCallback_(100);
        }

        if (autoReboot_) {
            rebootRequested_ = true;
            rebootTime_ = millis();
        }
    } else {
        currentStage_ = Stage::FAILED;

        if (screenProgressCallback_) {
            screenProgressCallback_(currentProgress_, Stage::FAILED);
        }

        Logger.error("OTA: update failed - %s", state->reason);
    }
    return success;
}

void OTAManager::sendRefusal(AsyncWebServerRequest* request, int code, const char* reason) {
    if (code == 401) {
        request->requestAuthentication(AsyncAuthType::AUTH_BASIC, "OTA");
    } else if (code == 403) {
        wifisetup::sendCrossOriginRefused(request);
    } else {
        request->send(code ? code : 500, "text/plain", reason);
    }
}

// This core's Update.begin(U_SPIFFS) ignores its label and writes the first spiffs-subtype data partition,
// so filesystem uploads are only offered when that partition is the configured one.
const esp_partition_t* OTAManager::filesystemPartition() const {
    if (partitionLabel_.isEmpty()) {
        return nullptr;
    }
    const esp_partition_t* partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    return partition && partitionLabel_ == partition->label ? partition : nullptr;
}

// Basic only, as in the IDF sister; request->authenticate() would also accept a Digest response.
bool OTAManager::authenticate(AsyncWebServerRequest* request) const {
    if (username_.isEmpty() || password_.isEmpty()) {
        return true;
    }
    return request->authType() == AsyncAuthType::AUTH_BASIC &&
           request->authenticate(username_.c_str(), password_.c_str());
}
