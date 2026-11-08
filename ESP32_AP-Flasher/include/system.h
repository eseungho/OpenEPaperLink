#include <Arduino.h>

#define WAKEUP_REASON_TIMED 0
#define WAKEUP_REASON_BOOT 1
#define WAKEUP_REASON_GPIO 2
#define WAKEUP_REASON_NFC 3
#define WAKEUP_REASON_BUTTON1 4
#define WAKEUP_REASON_BUTTON2 5
#define WAKEUP_REASON_BUTTON3 6
#define WAKEUP_REASON_FAILED_OTA_FW 0xE0
#define WAKEUP_REASON_FIRSTBOOT 0xFC
#define WAKEUP_REASON_NETWORK_SCAN 0xFD
#define WAKEUP_REASON_WDT_RESET 0xFE

void initTime(void* parameter);
void logLine(const char* buffer);
void logLine(const String& text);
// Like logLine(), but gives up after timeoutMs instead of waiting forever for the filesystem mutex.
bool tryLogLine(const String& text, uint32_t timeoutMs);
void logStartUp();
void logCoreDump();
size_t coreDumpSize();
size_t readCoreDump(uint8_t* buffer, size_t offset, size_t len);
bool eraseCoreDump();
