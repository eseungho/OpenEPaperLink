#ifdef HAS_SUPERVISOR

#include "supervisor.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "hal/wdt_hal.h"
#include "ping/ping_sock.h"
#include "soc/rtc.h"

#include "newproto.h"
#include "serialap.h"
#include "system.h"
#include "tag_db.h"
#include "web.h"
#include "wifimanager.h"

// ---------------------------------------------------------------------------------------------------------------------
// Tunables, all overridable with -D in platformio.ini. Times are in seconds.
// ---------------------------------------------------------------------------------------------------------------------
#ifndef SUPERVISOR_BOOT_RWDT_S
#define SUPERVISOR_BOOT_RWDT_S 180  // RTC watchdog while the rest of setup() runs (Wi-Fi connect, DB load, ...)
#endif
#ifndef SUPERVISOR_RWDT_S
#define SUPERVISOR_RWDT_S 90  // RTC watchdog once the supervisor task feeds it every second
#endif
#ifndef SUPERVISOR_LOOP_STALL_S
#define SUPERVISOR_LOOP_STALL_S 600  // loop() renders content and fetches HTTP data, give it plenty of time
#endif
#ifndef SUPERVISOR_BLE_STALL_S
#define SUPERVISOR_BLE_STALL_S 300  // longest legit BLE job (connect, discovery, upload, refresh wait) is ~2 min
#endif
#ifndef SUPERVISOR_AP_STALL_S
#define SUPERVISOR_AP_STALL_S 300
#endif
#ifndef SUPERVISOR_MAX_STALL_REBOOTS
#define SUPERVISOR_MAX_STALL_REBOOTS 2  // same task stalling right after this many restarts: stop restarting for it
#endif
#ifndef SUPERVISOR_MIN_FREE_INTERNAL
#define SUPERVISOR_MIN_FREE_INTERNAL (16 * 1024)  // Wi-Fi, lwIP and BT buffers come from internal RAM
#endif
#ifndef SUPERVISOR_MIN_BLOCK_INTERNAL
#define SUPERVISOR_MIN_BLOCK_INTERNAL (6 * 1024)
#endif
#ifndef SUPERVISOR_INSPECT_INTERNAL_BELOW
#define SUPERVISOR_INSPECT_INTERNAL_BELOW (40 * 1024)  // only then look for fragmentation (see checkMemory())
#endif
#ifndef SUPERVISOR_MIN_FREE_PSRAM
#define SUPERVISOR_MIN_FREE_PSRAM (512 * 1024)  // image buffers for large tags need hundreds of KB
#endif
#ifndef SUPERVISOR_LOW_MEM_S
#define SUPERVISOR_LOW_MEM_S 120  // memory must stay low this long; short dips during TLS or BLE jobs are normal
#endif
#ifndef SUPERVISOR_NET_DOWN_S
#define SUPERVISOR_NET_DOWN_S (20 * 60)
#endif
#ifndef SUPERVISOR_GW_PING_INTERVAL_S
#define SUPERVISOR_GW_PING_INTERVAL_S 60
#endif
#ifndef SUPERVISOR_GW_DEAD_S
#define SUPERVISOR_GW_DEAD_S (15 * 60)
#endif
#ifndef SUPERVISOR_RADIO_DOWN_S
#define SUPERVISOR_RADIO_DOWN_S (60 * 60)
#endif
#ifndef SUPERVISOR_STOPPED_S
#define SUPERVISOR_STOPPED_S (30 * 60)
#endif
#ifndef SUPERVISOR_MAX_UPTIME_S
#define SUPERVISOR_MAX_UPTIME_S (26 * 3600)  // the 03:56 reboot was missed (clock not synced, loop busy, ...)
#endif
#ifndef SUPERVISOR_HEALTH_LOG_S
#define SUPERVISOR_HEALTH_LOG_S (6 * 3600)
#endif
#ifndef SUPERVISOR_MAINTENANCE_MAX_S
#define SUPERVISOR_MAINTENANCE_MAX_S (20 * 60)  // a firmware update taking longer than this is stuck itself
#endif
#define SUPERVISOR_FIRST_HEALTH_LOG_S 600  // baseline snapshot once things have settled after boot
#define SUPERVISOR_MIN_UPTIME_FOR_MEM_S 600
#define SUPERVISOR_MEM_SAMPLE_MS 5000
#define SUPERVISOR_STALL_STREAK_CLEAR_S 3600
#define SUPERVISOR_GRACEFUL_DEADLINE_S 60  // loop() made no progress this long after the request: restart anyway
#define SUPERVISOR_GRACEFUL_MAX_S 300      // ... or this long, even if loop() is still busy

// ---------------------------------------------------------------------------------------------------------------------
// RTC memory: survives esp_restart() and panics (not a power cycle), so it can describe the reboot it causes.
// ---------------------------------------------------------------------------------------------------------------------
#define BREADCRUMB_MAGIC 0x53555056UL  // "SUPV"

struct Breadcrumb {
    uint32_t magic;
    uint32_t uptimeS;
    uint32_t freeInternal;
    uint32_t minFreeInternal;
    uint32_t largestInternal;
    uint32_t freePsram;
    char reason[96];
    uint32_t check;
};

// Consecutive stall restarts of the same task, so a stall that comes straight back after every boot (e.g. one tag that
// always hangs the BLE library) doesn't turn into a restart every few minutes that also takes the healthy parts down.
#define STALL_MEMO_MAGIC 0x5354414CUL  // "STAL"

struct StallMemo {
    uint32_t magic;
    uint32_t task;
    uint32_t count;
    uint32_t check;
};

static RTC_NOINIT_ATTR Breadcrumb s_breadcrumb;
static RTC_NOINIT_ATTR StallMemo s_stallMemo;
static String s_bootReport;

static uint32_t breadcrumbCheck(const Breadcrumb& b) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&b);
    uint32_t h = 2166136261UL;  // FNV-1a over everything but the check field itself
    for (size_t i = 0; i < offsetof(Breadcrumb, check); i++) {
        h ^= p[i];
        h *= 16777619UL;
    }
    return h;
}

static uint32_t stallMemoCheck(uint32_t task, uint32_t count) {
    return STALL_MEMO_MAGIC ^ (task * 2654435761UL) ^ (count << 16) ^ 0xA5A5A5A5UL;
}

static void stallMemoSet(uint32_t task, uint32_t count) {
    s_stallMemo.magic = STALL_MEMO_MAGIC;
    s_stallMemo.task = task;
    s_stallMemo.count = count;
    s_stallMemo.check = stallMemoCheck(task, count);
}

static void stallMemoClear() {
    stallMemoSet(SUPERVISED_COUNT, 0);
}

static uint32_t uptimeS() {
    return (uint32_t)(esp_timer_get_time() / 1000000LL);
}

static String formatUptime(uint32_t s) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%lud %02lu:%02lu", (unsigned long)(s / 86400), (unsigned long)((s / 3600) % 24),
             (unsigned long)((s / 60) % 60));
    return String(buf);
}

// ---------------------------------------------------------------------------------------------------------------------
// RTC watchdog: last line of defence. Its RESET_RTC action resets the digital core and the RTC domain, which is as
// close to a power cycle as the chip can get by itself. Only the supervisor task feeds it.
// ---------------------------------------------------------------------------------------------------------------------
static wdt_hal_context_t s_rwdt;
static bool s_rwdtArmed = false;

static void rwdtArm(uint32_t seconds) {
    const uint32_t ticks = (uint32_t)((uint64_t)seconds * rtc_clk_slow_freq_get_hz());
    wdt_hal_init(&s_rwdt, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&s_rwdt);
    wdt_hal_config_stage(&s_rwdt, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_RTC);
    wdt_hal_set_flashboot_en(&s_rwdt, false);
    wdt_hal_enable(&s_rwdt);
    wdt_hal_write_protect_enable(&s_rwdt);
    s_rwdtArmed = true;
}

static void rwdtFeed() {
    if (!s_rwdtArmed) return;
    wdt_hal_write_protect_disable(&s_rwdt);
    wdt_hal_feed(&s_rwdt);
    wdt_hal_write_protect_enable(&s_rwdt);
}

static void rwdtDisarm() {
    if (!s_rwdtArmed) return;
    wdt_hal_write_protect_disable(&s_rwdt);
    wdt_hal_disable(&s_rwdt);
    wdt_hal_write_protect_enable(&s_rwdt);
    s_rwdtArmed = false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Heartbeats. CPU starvation needs no extra check: the SDK's task watchdog already panics (and reboots) when CPU0's
// idle task starves for 5 s, and anything hogging CPU1 also starves loop(), which is pinned there.
// ---------------------------------------------------------------------------------------------------------------------
static volatile uint32_t s_beat[SUPERVISED_COUNT];
static volatile bool s_beatSeen[SUPERVISED_COUNT];
static bool s_stallSuppressed[SUPERVISED_COUNT];
static const uint32_t kStallLimitS[SUPERVISED_COUNT] = {SUPERVISOR_LOOP_STALL_S, SUPERVISOR_BLE_STALL_S,
                                                        SUPERVISOR_AP_STALL_S};
static const char* const kTaskName[SUPERVISED_COUNT] = {"loop()", "BLE task", "AP task"};

void supervisorHeartbeat(SupervisedTask t) {
    if (t >= SUPERVISED_COUNT) return;
    s_beat[t] = millis();
    s_beatSeen[t] = true;
}

// Firmware updates (ESP32 OTA, C6 flashing) must never be cut short by a restart.
static volatile bool s_maintenance = false;
static volatile uint32_t s_maintenanceSince = 0;

void supervisorMaintenance(bool active) {
    if (active) s_maintenanceSince = millis();
    s_maintenance = active;
}

// ---------------------------------------------------------------------------------------------------------------------
// Reboot requests
// ---------------------------------------------------------------------------------------------------------------------
static volatile bool s_rebootPending = false;
static volatile bool s_rebootGraceful = false;
static volatile bool s_gracefulStarted = false;
static uint32_t s_rebootRequestedAt = 0;

bool supervisorRebootPending() {
    return s_rebootPending;
}

static void writeBreadcrumb(const String& reason) {
    memset(&s_breadcrumb, 0, sizeof(s_breadcrumb));
    s_breadcrumb.magic = BREADCRUMB_MAGIC;
    s_breadcrumb.uptimeS = uptimeS();
    s_breadcrumb.freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_breadcrumb.minFreeInternal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    s_breadcrumb.largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    s_breadcrumb.freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    strlcpy(s_breadcrumb.reason, reason.c_str(), sizeof(s_breadcrumb.reason));
    s_breadcrumb.check = breadcrumbCheck(s_breadcrumb);
}

// graceful: loop() runs the usual shutdown (refresh pending, save the DB) and restarts. Not graceful: restart right
// away, for when loop() itself is the problem (it may hold the filesystem or websocket mutex).
static void requestReboot(const String& reason, bool graceful) {
    if (s_rebootPending) return;
    const bool nightly = reason.startsWith("Nightly");
    if (nightly) stallMemoClear();  // a planned restart starts a new day: stalls may trigger restarts again
    writeBreadcrumb(reason);
    Serial.println("Supervisor: " + reason + ", rebooting");
    tryLogLine(nightly ? reason : "Supervisor: " + reason + ", rebooting", 2000);
    s_rebootRequestedAt = millis();
    s_rebootGraceful = graceful;
    s_rebootPending = true;
    if (!graceful) {
        delay(100);
        esp_restart();
    }
}

void supervisorLoopTick() {
    supervisorHeartbeat(SUPERVISED_LOOP);
    if (!s_rebootPending || !s_rebootGraceful || s_gracefulStarted) return;
    s_gracefulStarted = true;
    // The shutdown sequence the nightly reboot has always used.
    wsErr("REBOOTING");
    config.runStatus = RUNSTATUS_STOP;
    ws.enable(false);
    vTaskDelay(5000 / portTICK_PERIOD_MS);
    refreshAllPending();
    saveDB("/current/tagDB.json");
    ws.closeAll();
    delay(100);
    ESP.restart();
}

// ---------------------------------------------------------------------------------------------------------------------
// Gateway ping, to catch a Wi-Fi link that reports "connected" but no longer passes traffic.
// ---------------------------------------------------------------------------------------------------------------------
static SemaphoreHandle_t s_pingDone = nullptr;
static volatile uint32_t s_pingReplies = 0;

static void pingSuccessCb(esp_ping_handle_t, void*) {
    s_pingReplies = s_pingReplies + 1;
}

static void pingEndCb(esp_ping_handle_t, void*) {
    xSemaphoreGive(s_pingDone);
}

// True if the gateway answered at least one of the pings. Failing to even start a ping session (sockets or RAM
// exhausted) counts as no answer too; the check only arms once a ping has succeeded since boot.
static bool pingGateway(const IPAddress& gw) {
    if (s_pingDone == nullptr) {
        s_pingDone = xSemaphoreCreateBinary();
        if (s_pingDone == nullptr) return false;
    }
    esp_ping_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.count = 3;
    cfg.interval_ms = 500;
    cfg.timeout_ms = 1000;
    cfg.data_size = 32;
    cfg.ttl = IP_DEFAULT_TTL;
    cfg.task_stack_size = 3072;
    cfg.task_prio = 2;
    IP_ADDR4(&cfg.target_addr, gw[0], gw[1], gw[2], gw[3]);

    esp_ping_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_ping_success = pingSuccessCb;
    cbs.on_ping_end = pingEndCb;

    xSemaphoreTake(s_pingDone, 0);
    s_pingReplies = 0;
    esp_ping_handle_t ping = nullptr;
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) return false;
    esp_ping_start(ping);
    xSemaphoreTake(s_pingDone, pdMS_TO_TICKS(8000));
    esp_ping_stop(ping);
    esp_ping_delete_session(ping);  // the ping task frees itself once it sees the session deleted
    return s_pingReplies > 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------------------------------------------------
struct SupervisorState {
    uint32_t lastMemSample = 0;
    bool memLow = false;
    uint32_t memLowSince = 0;

    bool netEverUp = false;
    uint32_t netLastUp = 0;
    bool staWasUp = false;
    bool gwEverOk = false;
    uint32_t gwLastOk = 0;
    uint32_t gwLastCheck = 0;

    bool radioEverOk = false;
    bool radioBad = false;
    uint32_t radioBadSince = 0;

    bool stopped = false;
    uint32_t stoppedSince = 0;

    uint32_t nextHealthLogS = SUPERVISOR_FIRST_HEALTH_LOG_S;
};

static void checkStalls() {
    for (uint8_t t = 0; t < SUPERVISED_COUNT; t++) {
        if (!s_beatSeen[t] || s_stallSuppressed[t]) continue;  // not running in this configuration, or given up on
        if (t == SUPERVISED_AP && apInfo.state == AP_STATE_FLASHING) continue;
        const uint32_t beat = s_beat[t];
        const uint32_t age = millis() - beat;  // read the beat first, so this can't underflow
        if (age <= kStallLimitS[t] * 1000UL) continue;

        const String what = String(kTaskName[t]) + " stalled for " + String(age / 1000) + " s";
        const uint32_t repeats = (s_stallMemo.task == t) ? s_stallMemo.count : 0;
        if (repeats >= SUPERVISOR_MAX_STALL_REBOOTS) {
            s_stallSuppressed[t] = true;
            tryLogLine("Supervisor: " + what + " again after " + String(repeats) +
                           " restarts in a row; not restarting for it until the nightly reboot",
                       1000);
            continue;
        }
        stallMemoSet(t, repeats + 1);
        requestReboot(what, t != SUPERVISED_LOOP);
        return;
    }
}

// Forget the stall streak once the task that stalled has been healthy for a while.
static void maybeClearStallStreak(uint32_t up) {
    if (s_stallMemo.count == 0 || up < SUPERVISOR_STALL_STREAK_CLEAR_S) return;
    const uint32_t t = s_stallMemo.task;
    if (t >= SUPERVISED_COUNT || !s_beatSeen[t]) {
        stallMemoClear();
        return;
    }
    const uint32_t beat = s_beat[t];
    if (!s_stallSuppressed[t] && millis() - beat <= kStallLimitS[t] * 1000UL) stallMemoClear();
}

static void checkMemory(SupervisorState& s, uint32_t now, uint32_t up) {
    if (now - s.lastMemSample < SUPERVISOR_MEM_SAMPLE_MS) return;
    s.lastMemSample = now;

    // Free sizes are O(1). heap_caps_get_largest_free_block() walks every block of a heap with interrupts disabled,
    // which for PSRAM (thousands of small blocks, each access a cache miss) can take long enough to drop bytes on the
    // 2 Mbaud radio UART. So: fragmentation is only examined for internal RAM, and only once it is getting tight.
    const size_t freeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t blockInt = SIZE_MAX;
    if (freeInt < SUPERVISOR_INSPECT_INTERNAL_BELOW) blockInt = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const bool lowInt = freeInt < SUPERVISOR_MIN_FREE_INTERNAL || blockInt < SUPERVISOR_MIN_BLOCK_INTERNAL;
    size_t freePs = SIZE_MAX;
    if (psramFound()) freePs = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const bool lowPs = freePs < SUPERVISOR_MIN_FREE_PSRAM;

    if (!lowInt && !lowPs) {
        s.memLow = false;
        return;
    }
    if (!s.memLow) {
        s.memLow = true;
        s.memLowSince = now;
        Serial.printf("Supervisor: memory low (internal free %u, block %u; psram free %u)\r\n", (unsigned)freeInt,
                      (unsigned)(blockInt == SIZE_MAX ? 0 : blockInt), (unsigned)(freePs == SIZE_MAX ? 0 : freePs));
        return;
    }
    if (up < SUPERVISOR_MIN_UPTIME_FOR_MEM_S || now - s.memLowSince < SUPERVISOR_LOW_MEM_S * 1000UL) return;

    char reason[96];
    if (lowInt) {
        snprintf(reason, sizeof(reason), "internal RAM exhausted (free %u, largest block %u)", (unsigned)freeInt,
                 (unsigned)(blockInt == SIZE_MAX ? 0 : blockInt));
    } else {
        snprintf(reason, sizeof(reason), "PSRAM exhausted (free %u)", (unsigned)freePs);
    }
    requestReboot(reason, true);
}

static void checkNetwork(SupervisorState& s, uint32_t now) {
    if (wm.wifiStatus == ETHERNET) {
        s.netEverUp = true;
        s.netLastUp = now;
        return;
    }

    const bool sta = WiFi.status() == WL_CONNECTED;
    if (sta) {
        if (!s.staWasUp) s.gwLastOk = now;  // fresh association: give the gateway a full window
        s.netEverUp = true;
        s.netLastUp = now;
    }
    s.staWasUp = sta;

    if (!sta) {
        // Only after having been connected once since boot: if the router is gone for good, rebooting in a loop
        // doesn't help, and the Wi-Fi manager keeps retrying every 5 minutes anyway. A client on the fallback
        // configuration AP postpones this (someone may be configuring the AP), but not forever: that AP is open,
        // and a device that auto-joins it would otherwise block recovery for good.
        const uint32_t down = now - s.netLastUp;
        const bool portalInUse = WifiManager::apClients != 0 && down < 3UL * SUPERVISOR_NET_DOWN_S * 1000UL;
        if (s.netEverUp && !portalInUse && down > SUPERVISOR_NET_DOWN_S * 1000UL) {
            requestReboot("Wi-Fi down for " + String(down / 60000) + " min", true);
        }
        return;
    }

    if (now - s.gwLastCheck >= SUPERVISOR_GW_PING_INTERVAL_S * 1000UL) {
        s.gwLastCheck = now;
        const IPAddress gw = WiFi.gatewayIP();
        if ((uint32_t)gw != 0 && pingGateway(gw)) {
            s.gwLastOk = millis();
            s.gwEverOk = true;
        }
    }
    // Only armed once the gateway has answered at least once, so a router that ignores ping can't cause reboots.
    if (s.gwEverOk && millis() - s.gwLastOk > SUPERVISOR_GW_DEAD_S * 1000UL) {
        requestReboot("gateway " + WiFi.gatewayIP().toString() + " unreachable for " + String(SUPERVISOR_GW_DEAD_S / 60) +
                          " min while Wi-Fi reports connected",
                      true);
    }
}

static void checkRadio(SupervisorState& s, uint32_t now) {
    const uint8_t state = apInfo.state;
    if (state == AP_STATE_ONLINE || state == AP_STATE_NORADIO) {
        s.radioEverOk = true;
        s.radioBad = false;
        return;
    }
    // While flashing, or if the radio never came up since boot (a reboot loop would not fix that), leave it alone.
    if (state == AP_STATE_FLASHING || !s.radioEverOk) {
        s.radioBad = false;
        return;
    }
    if (!s.radioBad) {
        s.radioBad = true;
        s.radioBadSince = now;
    } else if (now - s.radioBadSince > SUPERVISOR_RADIO_DOWN_S * 1000UL) {
        requestReboot("radio offline for " + String(SUPERVISOR_RADIO_DOWN_S / 60) + " min (state " + String(state) + ")",
                      true);
    }
}

static void checkRunStatus(SupervisorState& s, uint32_t now) {
    if (config.runStatus != RUNSTATUS_STOP) {
        s.stopped = false;
        return;
    }
    if (!s.stopped) {
        s.stopped = true;
        s.stoppedSince = now;
    } else if (now - s.stoppedSince > SUPERVISOR_STOPPED_S * 1000UL) {
        requestReboot("content generation stopped for " + String(SUPERVISOR_STOPPED_S / 60) + " min", true);
    }
}

static void checkNightly(uint32_t up) {
    if (config.nightlyreboot != 1) return;
    time_t now;
    time(&now);
    struct tm t;
    localtime_r(&now, &t);
    const bool clockValid = t.tm_year + 1900 >= 2024;
    if (clockValid && t.tm_hour == 3 && t.tm_min == 56 && up > 2 * 3600) {
        requestReboot("Nightly reboot", true);
    } else if (up > SUPERVISOR_MAX_UPTIME_S) {
        requestReboot("Nightly reboot (missed at 03:56, uptime failsafe)", true);
    }
}

static String healthLine(uint32_t up) {
    // Walks the heaps (see checkMemory()), but only every few hours.
    char buf[200];
    snprintf(buf, sizeof(buf), "Health: up %s, heap %uk (min %uk, block %uk), psram %uk (block %uk), rssi %d, radio %u, run %u",
             formatUptime(up).c_str(), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024), (int)WiFi.RSSI(),
             (unsigned)apInfo.state, (unsigned)config.runStatus);
    return String(buf);
}

static void supervisorTask(void*) {
    rwdtArm(SUPERVISOR_RWDT_S);  // from the boot timeout to the runtime one, now that someone feeds it
    SupervisorState s;
    s.netLastUp = millis();
    for (;;) {
        rwdtFeed();
        const uint32_t now = millis();
        const uint32_t up = uptimeS();
        const bool maintenanceFlag = s_maintenance;  // flag first: supervisorMaintenance() stores the time before it
        const uint32_t maintenanceSince = s_maintenanceSince;
        const bool maintenance = maintenanceFlag && millis() - maintenanceSince < SUPERVISOR_MAINTENANCE_MAX_S * 1000UL;

        if (s_rebootPending) {
            // loop() normally finishes the graceful shutdown within seconds. Restart without it only when loop() has
            // stopped making progress, or when it is taking far too long.
            const uint32_t sinceRequest = millis() - s_rebootRequestedAt;
            const uint32_t loopBeat = s_beat[SUPERVISED_LOOP];
            const uint32_t loopAge = millis() - loopBeat;
            if (sinceRequest > SUPERVISOR_GRACEFUL_MAX_S * 1000UL ||
                (sinceRequest > SUPERVISOR_GRACEFUL_DEADLINE_S * 1000UL && loopAge > SUPERVISOR_GRACEFUL_DEADLINE_S * 1000UL)) {
                Serial.println("Supervisor: graceful shutdown did not finish, restarting now");
                esp_restart();
            }
        } else if (maintenance) {
            // A firmware update is running: never restart underneath it. Restart the windows of the state checks, so
            // a condition has to persist for its full time after the update before it counts.
            s.memLow = false;
            s.stopped = false;
            s.radioBad = false;
            s.netLastUp = now;
            s.gwLastOk = now;
        } else {
            maybeClearStallStreak(up);
            checkStalls();
            checkMemory(s, now, up);
            checkNetwork(s, now);
            checkRadio(s, now);
            checkRunStatus(s, now);
            checkNightly(up);
            if (!s_rebootPending && up >= s.nextHealthLogS) {
                s.nextHealthLogS = up + SUPERVISOR_HEALTH_LOG_S;
                tryLogLine(healthLine(up), 1000);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------------------------------
void supervisorEarlyInit() {
    if (s_breadcrumb.magic == BREADCRUMB_MAGIC && s_breadcrumb.check == breadcrumbCheck(s_breadcrumb)) {
        s_breadcrumb.reason[sizeof(s_breadcrumb.reason) - 1] = '\0';
        char buf[220];
        snprintf(buf, sizeof(buf), "Supervisor restart: %s (was up %s, heap %luk min %luk block %luk, psram %luk)",
                 s_breadcrumb.reason, formatUptime(s_breadcrumb.uptimeS).c_str(),
                 (unsigned long)(s_breadcrumb.freeInternal / 1024), (unsigned long)(s_breadcrumb.minFreeInternal / 1024),
                 (unsigned long)(s_breadcrumb.largestInternal / 1024), (unsigned long)(s_breadcrumb.freePsram / 1024));
        s_bootReport = buf;
        Serial.println(s_bootReport);
    }
    s_breadcrumb.magic = 0;  // report once
    if (s_stallMemo.magic != STALL_MEMO_MAGIC || s_stallMemo.check != stallMemoCheck(s_stallMemo.task, s_stallMemo.count)) {
        stallMemoClear();  // power-on garbage
    }
    rwdtArm(SUPERVISOR_BOOT_RWDT_S);
}

void supervisorStart() {
    supervisorHeartbeat(SUPERVISED_LOOP);
    // Above the application tasks (loop 1, BLE/AP 5), below the radio UART tasks (11, 15). Not pinned, so it still runs
    // if one core is hogged.
    if (xTaskCreate(supervisorTask, "supervisor", 6144, NULL, 10, NULL) != pdPASS) {
        // Nobody would feed the RTC watchdog, which would turn this into a reset every boot.
        rwdtDisarm();
        Serial.println("Supervisor: could not start task, running without it");
    }
}

String supervisorBootReport() {
    return s_bootReport;
}

#endif
