#include <Arduino.h>
#include <FS.h>

#include "commstructs.h"

struct PendingItem {
    struct pendingData pendingdata;
    char filename[50];
    uint8_t* data;
    uint32_t len;
};

extern void addCRC(void* p, uint8_t len);
extern bool checkCRC(void* p, uint8_t len);

extern void processBlockRequest(struct espBlockRequest* br);
extern void prepareCancelPending(const uint8_t dst[8]);
extern void prepareIdleReq(const uint8_t* dst, uint16_t nextCheckin);
extern void prepareDataAvail(const uint8_t* dst);
extern void prepareDataAvail(uint8_t* data, uint16_t len, uint8_t dataType, const uint8_t* dst);
extern bool prepareDataAvail(String& filename, uint8_t dataType, uint8_t dataTypeArgument, const uint8_t* dst, uint16_t nextCheckin, bool resend = false);
extern void prepareExternalDataAvail(struct pendingData* pending, IPAddress remoteIP);
// dataVer: the version of the item that was delivered (BLE uploads). If the queue no longer holds it (a newer image
// replaced it during the upload), nothing is dequeued and the tag hash stays. 0 means the first queued item.
extern void processXferComplete(struct espXferComplete* xfc, bool local, uint64_t dataVer = 0);
extern void processXferTimeout(struct espXferComplete* xfc, bool local);
extern void processDataReq(struct espAvailDataReq* adr, bool local, IPAddress remoteIP = IPAddress(0, 0, 0, 0));
extern void processTagReturnData(struct espTagReturnData* trd, uint8_t len, bool local);

extern bool sendTagCommand(const uint8_t* dst, uint8_t cmd, bool local, const uint8_t* payload = nullptr);
bool sendTagMac(const uint8_t* dst, const uint64_t newmac, bool local);
extern bool sendAPSegmentedData(const uint8_t* dst, String data, uint16_t icons, bool inverted, bool local);
extern bool showAPSegmentedInfo(const uint8_t* dst, bool local);
extern void updateTaginfoitem(struct TagInfo* taginfoitem, IPAddress remoteIP);
bool checkMirror(struct tagRecord* taginfo, struct pendingData* pending);

// radioTagsOnly: for a radio (re)start. BLE devices hold nothing in the radio and keep their queue, so redrawing one
// would queue its content a second time (a waiting Nemonic label is never replaced: it would print twice).
void refreshAllPending(bool radioTagsOnly = false);
void updateContent(const uint8_t* dst);
void setAPchannel();

void enqueueItem(const PendingItem& item);
bool dequeueItem(const uint8_t* targetMac);
bool dequeueItem(const uint8_t* targetMac, const uint64_t dataVer);
void dequeueAllItems(const uint8_t* targetMac);
// Removes one queued item (with its .pending file) and leaves the tag's other items queued. For BLE uploads: a label
// that can't be printed, or an image that can't be delivered, without dropping the rest.
void dropQueueItem(const uint8_t* targetMac, const uint64_t dataVer);
uint16_t countQueueItem(const uint8_t* targetMac);
// A returned PendingItem stays valid for a while after it is dequeued (see freeRetiredQueueItems)
extern PendingItem* getQueueItem(const uint8_t* targetMac);
extern PendingItem* getQueueItem(const uint8_t* targetMac, const uint64_t dataVer);
void checkQueue(const uint8_t* targetMac);
bool queueDataAvail(struct pendingData* pending, bool local);
uint8_t* getDataForFile(fs::File& file);
bool loadQueueItemData(PendingItem* queueItem);
void freeRetiredQueueItems();

// BLE displays and printers: served by BLETask, never by the 802.15.4/sub-GHz radio. Data the radio holds for one only
// times out there, and that timeout used to drop the tag's queue before BLETask had sent it.
inline bool isBleTag(const uint8_t mac[8], const uint8_t hwType) {
    if (mac[7] == 0x13 && mac[6] == 0x37) return true;  // ATC BLE OEPL
    if ((hwType & 0xF0) == 0xB0) return true;           // Gicisky family
    switch (hwType) {
        case WOLINK_BLE_EPD_213_BWRY:
        case WOLINK_BLE_EPD_350_BWRY:
        case WOLINK_BLE_EPD_750_BWRY:
        case WOLINK_BLE_UNKNOWN:
        case NEMONIC_BLE_MIP201:
        case NEMONIC_BLE_UNKNOWN:
            return true;
        default:
            return false;
    }
}
// isBleTag() for a tag in tagDB, by MAC
bool isBleTarget(const uint8_t mac[8]);
