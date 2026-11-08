#include "ota.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <HTTPClient.h>
#include <MD5Builder.h>
#include <Update.h>

#include <atomic>

#include "flasher.h"
#include "espflasher.h"
#include "leds.h"
#include "serialap.h"
#include "storage.h"
#include "supervisor.h"
#include "tag_db.h"
#include "util.h"
#include "uploadinfo.h"
#include "web.h"


#ifndef BUILD_ENV_NAME
#define BUILD_ENV_NAME unknown
#endif
#ifndef BUILD_TIME
#define BUILD_TIME 0
#endif
#ifndef BUILD_VERSION
#define BUILD_VERSION custom
#endif
#ifndef SHA
#define SHA 0
#endif

#define STR_IMPL(x) #x
#define STR(x) STR_IMPL(x)
#define LOG(format, ... ) Serial.printf(format,## __VA_ARGS__)


void handleSysinfoRequest(AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["alias"] = config.alias;
    doc["env"] = STR(BUILD_ENV_NAME);
    doc["buildtime"] = STR(BUILD_TIME);
    doc["buildversion"] = STR(BUILD_VERSION);
    doc["sha"] = STR(SHA);
    doc["psramsize"] = ESP.getPsramSize();
    doc["flashsize"] = ESP.getFlashChipSize();
    doc["rollback"] = Update.canRollBack();
    doc["ap_version"] = apInfo.version;

    doc["hasC6"] = 0;
    doc["hasH2"] = 0;
    doc["hasTslr"] = 0;

#if defined HAS_H2
    doc["hasH2"] = 1;
#elif defined HAS_TSLR
    doc["hasTslr"] = 1;
#elif defined C6_OTA_FLASHING
    doc["hasC6"] = 1;
#endif

#ifdef HAS_EXT_FLASHER
    doc["hasFlasher"] = 1;
#else
    doc["hasFlasher"] = 0;
#endif
    const size_t bufferSize = measureJson(doc) + 1;
    AsyncResponseStream* response = request->beginResponseStream("application/json", bufferSize);
    serializeJson(doc, *response);
    request->send(response);
};

void handleCheckFile(AsyncWebServerRequest* request) {
    if (!request->hasParam("path")) {
        request->send(400);
        return;
    }

    const String filePath = request->getParam("path")->value();
    File file = contentFS->open(filePath, "r");
    if (!file) {
        JsonDocument doc;
        doc["filesize"] = 0;
        doc["md5"] = "";
        String jsonResponse;
        serializeJson(doc, jsonResponse);
        request->send(200, "application/json", jsonResponse);
        return;
    }

    const size_t fileSize = file.size();

    MD5Builder md5;
    md5.begin();
    md5.addStream(file, fileSize);
    md5.calculate();
    const String md5Hash = md5.toString();

    file.close();

    JsonDocument doc;
    doc["filesize"] = fileSize;
    doc["md5"] = md5Hash;
    String jsonResponse;
    serializeJson(doc, jsonResponse);
    request->send(200, "application/json", jsonResponse);
}

void handleLittleFSUpload(AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
    if (!index) {
        if (!request->hasParam("path", true)) {
            request->send(400, "text/plain", "parameters incomplete");
            return;
        }
        const String uploadfilename = request->getParam("path", true)->value();
        Serial.println("update " + uploadfilename);
        UploadInfo* uploadInfo = newUploadInfo(uploadfilename);
        if (uploadInfo == nullptr) {
            request->send(500, "text/plain", "Out of memory or path too long");
            return;
        }
        xSemaphoreTake(fsMutex, portMAX_DELAY);
        File file = contentFS->open(uploadfilename, "w");
        file.close();
        xSemaphoreGive(fsMutex);
        request->_tempObject = (void*)uploadInfo;
    }

    UploadInfo* uploadInfo = static_cast<UploadInfo*>(request->_tempObject);
    if (uploadInfo == nullptr) return;

    if (len) appendUploadInfo(uploadInfo, data, len);
    if (!final) return;

    flushUploadInfo(uploadInfo);
    const bool error = uploadInfo->error;
    if (error) logLine("Failed to write " + String(uploadInfo->filename));
    request->_tempObject = nullptr;
    free(uploadInfo);

    if (error) {
        request->send(507, "text/plain", "Error. Disk full?");
    } else {
        request->send(200, "text/plain", "Ok, file written");
    }
}

struct FirmwareUpdateParams {
    String url;
    String md5;
    size_t size;
};

void handleUpdateOTA(AsyncWebServerRequest* request) {
    if (request->hasParam("url", true) && request->hasParam("md5", true) && request->hasParam("size", true)) {
        FirmwareUpdateParams* params = new FirmwareUpdateParams;
        params->url = request->getParam("url", true)->value();
        params->md5 = request->getParam("md5", true)->value();
        params->size = request->getParam("size", true)->value().toInt();
        xTaskCreate(firmwareUpdateTask, "OTAUpdateTask", 6144, params, 10, NULL);

        request->send(200, "text/plain", "In progress");
    } else {
        request->send(400, "Bad request");
    }
}

void firmwareUpdateTask(void* parameter) {
    FirmwareUpdateParams* params = reinterpret_cast<FirmwareUpdateParams*>(parameter);

    if (ESP.getMaxAllocHeap() < 22000) {
        wsSerial("Error: Not enough memory left. Restart the esp32 and try updating again.");
        wsSerial("[reboot]");
    } else {
        const char* url = params->url.c_str();
        const char* md5 = params->md5.c_str();
        const size_t size = params->size;
        updateFirmware(url, md5, size);
    }

    delete params;
    vTaskDelete(NULL);
}

void updateFirmware(const char* url, const char* expectedMd5, const size_t size) {
    util::printHeap();

    supervisorMaintenance(true);
    config.runStatus = RUNSTATUS_STOP;
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    saveDB("/current/tagDB.json");
    // destroyDB();

    HTTPClient httpClient;

    wsSerial("start downloading");
    wsSerial(url);

    httpClient.begin(url);
    httpClient.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    util::printHeap();
    const int httpCode = httpClient.GET();
    util::printHeap();

    if (httpCode == HTTP_CODE_OK) {
        if (Update.begin(size)) {
            Update.setMD5(expectedMd5);

            unsigned long progressTimer = millis();
            Update.onProgress([&progressTimer](size_t progress, size_t total) {
                if (millis() - progressTimer > 500 || progress == total) {
                    char buffer[50];
                    sprintf(buffer, "Progress: %u%% %d %d", progress * 100 / total, progress, total);
                    wsSerial(String(buffer));
                    progressTimer = millis();
                    vTaskDelay(1 / portTICK_PERIOD_MS);
                }
            });

            // writeStream() checks the image header with a non-blocking peek() first: when the body
            // hasn't arrived yet (GET() returns after the headers), that fails at once, as a wrong
            // magic byte. Wait for the first bytes.
            WiFiClient &stream = httpClient.getStream();
            const uint32_t waitStart = millis();
            while (stream.available() == 0 && stream.connected() && millis() - waitStart < 10000) {
                vTaskDelay(10 / portTICK_PERIOD_MS);
            }
            const size_t written = Update.writeStream(stream);
            if (written == httpClient.getSize()) {
                if (Update.end(true)) {
                    wsSerial("Firmware update successful");
                    wsSerial("Reboot system now");
                    wsSerial("[reboot]");
                    vTaskDelay(1000 / portTICK_PERIOD_MS);
                } else {
                    wsSerial("Error updating firmware:");
                    wsSerial(Update.errorString());
                }
            } else {
                wsSerial("Error writing firmware data:");
                wsSerial(Update.errorString());
            }
        } else {
            wsSerial("Failed to begin firmware update");
            wsSerial(Update.errorString());
        }
    } else {
        wsSerial("Failed to download firmware file (HTTP code " + String(httpCode) + ")");
        wsSerial(httpClient.errorToString(httpCode));
    }

    httpClient.end();
    // While the AP is FLASHING, the C6 update owns runStatus (a flash still running, or one that failed after the
    // radio was put into its bootloader). Keep its STOP: the C6 task sets RUN when it finishes, or the supervisor's
    // 30-minute STOP restart brings the radio back.
    if (apInfo.state != AP_STATE_FLASHING) config.runStatus = RUNSTATUS_RUN;
    supervisorMaintenance(false);
}

void handleRollback(AsyncWebServerRequest* request) {
    if (Update.canRollBack()) {
        const bool rollbackSuccess = Update.rollBack();
        if (rollbackSuccess) {
            request->send(200, "Rollback successful");
            wsSerial("Rollback successful");
            wsSerial("Reboot system now");
            wsSerial("[reboot]");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        } else {
            wsSerial("Rollback failed");
            request->send(400, "Rollback failed");
        }
    } else {
        wsSerial("Rollback not allowed");
        request->send(400, "Rollback not allowed");
    }
}

#ifdef C6_OTA_FLASHING
// One radio update at a time: updates share the debug UART, the reset/boot pins and the firmware files
static std::atomic<bool> c6UpdateBusy{false};

void C6firmwareUpdateTask(void* parameter) {
    char* urlPtr = reinterpret_cast<char*>(parameter);

    LOG("C6firmwareUpdateTask: url '%s'\n", urlPtr);
    supervisorMaintenance(true);
    wsSerial("Stopping AP service");

    config.runStatus = RUNSTATUS_STOP;
    setAPstate(false, AP_STATE_FLASHING);
    stopApReader();  // the one radio reader (upstream 1ba8b87); bringAPOnline() starts it again
#ifndef FLASHER_DEBUG_SHARED
    extern volatile bool rxSerialStopTask2;
    rxSerialStopTask2 = true;
#endif
    vTaskDelay(500 / portTICK_PERIOD_MS);
    Serial1.end();
    // again: APTask may have overwritten FLASHING in the first 0.5 s (a recovery or an ACK that was in flight)
    setAPstate(false, AP_STATE_FLASHING);

    wsSerial(SHORT_CHIP_NAME " flash starting");

    bool radioTouched = false;
    bool result = FlashC6_H2(urlPtr, &radioTouched);

    wsSerial(SHORT_CHIP_NAME " flash end");

    // After a flash, and also when the radio was never touched (download failed, firmware refused): bring it back
    // right away instead of leaving the AP stopped until the supervisor restarts it.
    if (result || !radioTouched) {
        setAPstate(false, AP_STATE_OFFLINE);

        if (result) {
            wsSerial("Finishing config...");
            vTaskDelay(3000 / portTICK_PERIOD_MS);
        }

        wsSerial("starting monitor");
#ifndef FLASHER_DEBUG_SHARED
        rxSerialStopTask2 = false;
        xTaskCreate(rxSerialTask2, "rxSerialTask2", 1850, NULL, 2, NULL);
#endif
        vTaskDelay(1000 / portTICK_PERIOD_MS);

        if (result) apInfo.version = 0;
        wsSerial("bringing AP online");
        // if (bringAPOnline(AP_STATE_REQUIRED_POWER_CYCLE)) config.runStatus = RUNSTATUS_STOP;
        if (bringAPOnline(AP_STATE_ONLINE)) {
            config.runStatus = RUNSTATUS_RUN;
            setAPstate(true, AP_STATE_ONLINE);
        } else {
            // As after a failed start at boot: APTask's ping recovery keeps trying (it ignores OFFLINE), and with
            // runStatus RUN the BLE devices keep getting content meanwhile.
            setAPstate(false, AP_STATE_FAILED);
            config.runStatus = RUNSTATUS_RUN;
        }

        // Wait for version info to arrive
        vTaskDelay(500 / portTICK_PERIOD_MS);
        if(result && apInfo.version == 0) {
           result = false;
        }
    }

    if (!result && !radioTouched) {
       wsSerial("Nothing was flashed, the radio keeps its firmware.");
    } else if (result) {
       wsSerial("Finished!");
       char buffer[50];
       snprintf(buffer,sizeof(buffer),
                "ESP32-" SHORT_CHIP_NAME " version is now %04x", apInfo.version);
       wsSerial(String(buffer));
    }
    else if(apInfo.version == 0) {
       wsSerial("AP failed to come online. :-(");
    }
    else {
       wsSerial("Flashing failed. :-(");
    }
    // wsSerial("Reboot system now");
    // wsSerial("[reboot]");
    free(urlPtr);
    // When a flash failed after the radio was touched, the AP stays FLASHING with runStatus STOP; the supervisor
    // restarts it 30 min from here unless the user retries the update first.
    supervisorMaintenance(false);
    c6UpdateBusy = false;  // nothing below touches the radio
    vTaskDelay(30000 / portTICK_PERIOD_MS);
    vTaskDelete(NULL);
}
#endif


void handleUpdateC6(AsyncWebServerRequest* request) {
#if defined C6_OTA_FLASHING
    if (request->hasParam("url",true)) {
        bool expected = false;
        if (!c6UpdateBusy.compare_exchange_strong(expected, true)) {
            request->send(409, "text/plain", "A radio update is already running");
            return;
        }
        char* urlCopy = strdup(request->getParam("url", true)->value().c_str());
        // 10240: an HTTPS download left only 96 bytes of a 6400-byte stack on the Yellow AP (upstream 4ac63fc)
        if (urlCopy == nullptr || xTaskCreate(C6firmwareUpdateTask, "OTAUpdateTask", 10240, urlCopy, 10, NULL) != pdPASS) {
            free(urlCopy);
            c6UpdateBusy = false;
            request->send(500, "text/plain", "Could not start the update");
            return;
        }
        request->send(200, "Ok");
    }
    else {
       LOG("Sending bad request");
       request->send(400, "Bad request");
    }
#elif defined(SHORT_CHIP_NAME)
    request->send(400, SHORT_CHIP_NAME " flashing not implemented");
#else
    request->send(400, "C6/H2 flashing not implemented");
#endif
}

void handleUpdateActions(AsyncWebServerRequest* request) {
    wsSerial("Performing cleanup");
    File file = contentFS->open("/update_actions.json", "r");
    if (!file) {
        wsSerial("No update_actions.json present");
        request->send(200, "No update actions needed");
        return;
    }
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    const JsonArray deleteFiles = doc["deletefile"].as<JsonArray>();
    for (const auto& filePath : deleteFiles) {
        if (contentFS->remove(filePath.as<const char*>())) {
            wsSerial("deleted file: " + filePath.as<String>());
        }
    }
    file.close();
    wsSerial("Cleanup finished");
    request->send(200, "Clean up finished");
    contentFS->remove("/update_actions.json");
}
