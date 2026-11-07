#pragma once

#include <Arduino.h>

// Health supervisor for long unattended operation (enable with -D HAS_SUPERVISOR).
//
// Without it, the only self-healing was the wall-clock nightly reboot inside wsSendSysteminfo(). That code only runs
// while loop() is healthy and the clock is valid, so anything that wedged loop() (a mutex wait, a blocking call) or
// left the AP half-alive (BLE stack stuck, radio offline with content generation stopped, Wi-Fi associated but not
// passing traffic, heap exhausted) stayed dead until the power was cycled.
//
// The supervisor runs in its own task and
//  - feeds the RTC watchdog, a hardware timer that resets the whole chip (RTC domain included) if the supervisor
//    itself stops running, e.g. when interrupts or the scheduler are dead;
//  - reboots when a supervised task stops sending heartbeats, when internal RAM or PSRAM stays exhausted, when Wi-Fi
//    or the gateway stays unreachable, when the radio stays offline, or when content generation stays stopped;
//  - performs the nightly reboot, plus an uptime failsafe that does not depend on the clock;
//  - stores the reboot reason in RTC memory so the next boot can write it to /log.txt, and logs a periodic health line.

enum SupervisedTask : uint8_t {
    SUPERVISED_LOOP = 0,  // Arduino loop(): web housekeeping, content generation, DB saves
    SUPERVISED_BLE,       // BLETask: scanning and BLE tag/printer uploads
    SUPERVISED_AP,        // APTask: radio (C6) keep-alive and recovery
    SUPERVISED_COUNT
};

#ifdef HAS_SUPERVISOR
void supervisorEarlyInit();                  // in setup(), once storage is mounted: arms the RTC watchdog for boot
void supervisorStart();                      // end of setup(): starts the supervisor task
void supervisorHeartbeat(SupervisedTask t);  // "still alive", called from the supervised task's main loop
void supervisorLoopTick();                   // from loop(): heartbeat, and runs a pending graceful reboot
String supervisorBootReport();               // why the supervisor restarted the AP last time ("" if it didn't)
#else
inline void supervisorEarlyInit() {}
inline void supervisorStart() {}
inline void supervisorHeartbeat(SupervisedTask) {}
inline void supervisorLoopTick() {}
inline String supervisorBootReport() { return String(); }
#endif
