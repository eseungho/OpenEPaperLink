#include "system.h"

#include <Arduino.h>
#include <FS.h>
#include <Preferences.h>
#include <esp_sntp.h>

#include <algorithm>
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
#include <esp_core_dump.h>
#include <esp_flash.h>
#endif

#include "storage.h"
#include "supervisor.h"
#include "tag_db.h"
#include "wifimanager.h"

void timeSyncCallback(struct timeval* tv) {
    Serial.println("time succesfully synced");
}

void initTime(void* parameter) {
    if (!(WiFi.status() == WL_CONNECTED || wm.wifiStatus == ETHERNET)) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
    sntp_set_time_sync_notification_cb(timeSyncCallback);
    sntp_set_sync_interval(300 * 1000);
    configTzTime(config.timeZone, "time.cloudflare.com", "pool.ntp.org", "time.nist.gov");
    // Log first: a boot that resets again before the clock syncs (no NTP, plus an early crash) would otherwise never
    // log its reset reason, and supervisorEarlyInit() has already consumed the RTC copy of the supervisor report.
    // Lines logged before the sync carry a time without a date.
    logStartUp();
    logCoreDump();
    struct tm timeinfo;
    while (millis() < 30000) {
        if (!getLocalTime(&timeinfo)) {
            Serial.println("Waiting for valid time from NTP-server");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        } else {
            break;
        }
    }
    if (config.runStatus == RUNSTATUS_INIT) {
        config.runStatus = RUNSTATUS_RUN;
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
    vTaskDelete(NULL);
}

void logLine(const char* buffer) {
    logLine(String(buffer));
}

static bool writeLogLine(const String& text, TickType_t wait) {
    time_t now;
    time(&now);

    char timeStr[24];
    const char* format = (now < (time_t)1672531200) ? "           %H:%M:%S " : "%Y-%m-%d %H:%M:%S ";
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(timeStr, sizeof(timeStr), format, &timeinfo);

    if (xSemaphoreTake(fsMutex, wait) != pdTRUE) return false;
    File logFile = contentFS->open("/log.txt", "a");
    if (logFile) {
        if (logFile.size() >= 10 * 1024) {
            logFile.close();
            contentFS->remove("/logold.txt");
            contentFS->rename("/log.txt", "/logold.txt");
            logFile = contentFS->open("/log.txt", "a");
            if (!logFile) {
                xSemaphoreGive(fsMutex);
                return false;
            }
        }

        logFile.print(timeStr);
        logFile.println(text);
        logFile.close();
    }
    xSemaphoreGive(fsMutex);
    return true;
}

void logLine(const String& text) {
    writeLogLine(text, portMAX_DELAY);
}

bool tryLogLine(const String& text, uint32_t timeoutMs) {
    if (fsMutex == nullptr || contentFS == nullptr) return false;
    return writeLogLine(text, pdMS_TO_TICKS(timeoutMs));
}

void logStartUp() {
    esp_reset_reason_t resetReason = esp_reset_reason();

    String logEntry = "Reboot. Reason: ";
    switch (resetReason) {
        case ESP_RST_POWERON:
            logEntry += "Power-on";
            break;
        case ESP_RST_EXT:
            logEntry += "External";
            break;
        case ESP_RST_SW:
            logEntry += "Software";
            break;
        case ESP_RST_PANIC:
            logEntry += "Panic";
            break;
        case ESP_RST_INT_WDT:
            logEntry += "Watchdog";
            break;
        case ESP_RST_TASK_WDT:
            logEntry += "Task Watchdog";
            break;
        case ESP_RST_WDT:
            logEntry += "Other Watchdog";
            break;
        case ESP_RST_DEEPSLEEP:
            logEntry += "Deep Sleep";
            break;
        case ESP_RST_BROWNOUT:
            logEntry += "Brownout";
            break;
        case ESP_RST_SDIO:
            logEntry += "SDIO";
            break;
        default:
            logEntry += "Unknown";
            break;
    }

    logLine(logEntry);

    const String supervisorReport = supervisorBootReport();
    if (supervisorReport.length()) logLine(supervisorReport);
}

/// @brief A reset caused by a crash, which writes a core dump
/// @note Not ESP_RST_WDT: on the S3 that is also what a reset by the supervisor's RTC watchdog reports, and that
/// writes no dump, so the dump in flash would be an old one, logged as a new crash.
static bool resetByCrash() {
    switch (esp_reset_reason()) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
            return true;
        default:
            return false;
    }
}

/// @brief Write a summary of the core dump of a crash (stored in flash by a panic or watchdog) to the log
/// @note The dump stays in flash, for download via /coredump, until the next crash overwrites it.
/// Decode the backtrace with: xtensa-esp32s3-elf-addr2line -pfiaC -e firmware.elf <addresses>
void logCoreDump() {
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    // a dump without a crash reset is one that was logged before
    if (!resetByCrash() || esp_core_dump_image_check() != ESP_OK) return;

    esp_core_dump_summary_t* summary = static_cast<esp_core_dump_summary_t*>(malloc(sizeof(esp_core_dump_summary_t)));
    if (summary == nullptr) return;
    if (esp_core_dump_get_summary(summary) == ESP_OK) {
        char buffer[160];
#if CONFIG_IDF_TARGET_ARCH_XTENSA
        snprintf(buffer, sizeof(buffer), "Crash in task %.16s, PC 0x%08lx, cause %lu, vaddr 0x%08lx, elf %.8s",
                 summary->exc_task, (unsigned long)summary->exc_pc, (unsigned long)summary->ex_info.exc_cause,
                 (unsigned long)summary->ex_info.exc_vaddr, (const char*)summary->app_elf_sha256);
#else
        snprintf(buffer, sizeof(buffer), "Crash in task %.16s, PC 0x%08lx, elf %.8s",
                 summary->exc_task, (unsigned long)summary->exc_pc, (const char*)summary->app_elf_sha256);
#endif
        logLine(buffer);

        String backtrace = "Backtrace:";
        for (uint32_t i = 0; i < summary->exc_bt_info.depth && i < 16; i++) {
            snprintf(buffer, sizeof(buffer), " 0x%08lx", (unsigned long)summary->exc_bt_info.bt[i]);
            backtrace += buffer;
        }
        if (summary->exc_bt_info.corrupted) backtrace += " |<-CORRUPTED";
        logLine(backtrace);
    }
    free(summary);
#endif
}

size_t coreDumpSize() {
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    size_t address, size;
    if (esp_core_dump_image_check() == ESP_OK && esp_core_dump_image_get(&address, &size) == ESP_OK) return size;
#endif
    return 0;
}

size_t readCoreDump(uint8_t* buffer, size_t offset, size_t len) {
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    size_t address, size;
    if (esp_core_dump_image_get(&address, &size) != ESP_OK || offset >= size) return 0;
    len = std::min(len, size - offset);
    if (esp_flash_read(NULL, buffer, address + offset, len) == ESP_OK) return len;
#endif
    return 0;
}

bool eraseCoreDump() {
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    return esp_core_dump_image_erase() == ESP_OK;
#else
    return false;
#endif
}
