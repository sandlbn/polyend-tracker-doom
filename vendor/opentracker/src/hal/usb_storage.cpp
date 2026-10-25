// OpenTracker 2.x -- USB Storage mode on the device (see usb_storage.h).
#include "usb_storage.h"

#include <Arduino.h>

#include "diag.h"
#include "hal.h"
#include "sd_storage.h"
#include "sdhc.h"

#if defined(OT_USB_MSC) && OT_USB_MSC

#include "dma_mem.h"
#include "usb/msc_bot.h"
#include "usb_msc.h"

#ifndef MSC_INTERFACE
#error "OT_USB_MSC=1 needs the MSC interface in usb_desc.h (USB_MIDI_SERIAL)"
#endif

namespace ot {
namespace hal {
namespace {

// Bulk endpoints: the Teensy-core driver (USB interrupt context only).
class Endpoints final : public msc::IEndpoints {
public:
    bool configured() const override { return usb_msc_configured() != 0; }
    bool rxQueue(uint8_t* buf, uint32_t len) override { return usb_msc_rx_queue(buf, len) != 0; }
    bool rxPoll(uint32_t* got) override { return usb_msc_rx_poll(got) != 0; }
    bool txQueue(const uint8_t* buf, uint32_t len) override { return usb_msc_tx_queue(buf, len) != 0; }
    bool txPoll() override { return usb_msc_tx_poll() != 0; }
    void rxFlush() override { usb_msc_rx_flush(); }
    void txFlush() override { usb_msc_tx_flush(); }
    void stall(bool in) override { usb_msc_stall(in ? 1 : 0); }
    bool stalled(bool in) const override { return usb_msc_stalled(in ? 1 : 0) != 0; }
};

// The card for the computer: only through the host-owned SD path.
class Card final : public msc::IBlockDevice {
public:
    bool read(uint32_t lba, uint8_t* dst, uint32_t count) override { return sdhc::hostReadBlocks(lba, dst, count); }
    bool write(uint32_t lba, const uint8_t* src, uint32_t count) override {
        return sdhc::hostWriteBlocks(lba, src, count);
    }
};

Endpoints gEp;
Card gCard;
msc::Bot gBot;
// CBW / CSW / short replies: the unit's own (DMA-reachable, SRAM_U). The data
// stage of READ / WRITE uses the SD bounce buffer (16 KB, SRAM_U: a ring of
// 4 chunks of 4 KB), which only the computer's transfers use while it owns
// the card (sdhc::hostBuffer()).
OT_DMA_MEM uint8_t gSmall[msc::Bot::kSmallBytes];
uint8_t* gData = nullptr;
uint32_t gDataBytes = 0;

// Main-loop access to the protocol state: USB interrupt masked.
struct UsbLock {
    UsbLock() { NVIC_DISABLE_IRQ(IRQ_USBOTG); }
    ~UsbLock() { NVIC_ENABLE_IRQ(IRQ_USBOTG); }
};

volatile uint32_t gSofTicks = 0;

void isrHook(uint8_t event) {
    if (event == USB_MSC_EVENT_RESET) gBot.onReset();
    else ++gSofTicks;   // tokens and frames; frames stop when the cable is out
    gBot.service();
}

class UsbStoragePort final : public IUsbStorage {
public:
    bool begin() override;
    bool end() override;
    Host host() const override;
    void service() override;

private:
    bool presentCard();
    void dropCommand(const char* why);
    void logLayout(const SdStorage::FatInfo& fi);
    void watchMount(uint32_t now);
    void logReads(const char* what, uint32_t ms);
    void logErrors();
    msc::HostWatch watch_;
    bool active_ = false;
    // Mount profile (diagnostics): when the computer first talked to the
    // presented card, first read it, and its first burst of reads ended.
    uint32_t presentMs_ = 0, firstCmdMs_ = 0, firstReadMs_ = 0, lastCmdMs_ = 0, lastCmds_ = 0;
    bool burstLogged_ = false;
    bool medium_ = false;
    uint32_t lastTicks_ = 0, lastTickMs_ = 0;
    uint32_t cardSeenMs_ = 0;
    uint32_t t0_ = 0;
};

bool UsbStoragePort::presentCard() {
    if (!sdhc::ready() && !sdhc::init()) return false;
    const uint32_t blocks = sdhc::sectorCount();
    if (!blocks) return false;
    {
        UsbLock l;
        gBot.setMedium(true, blocks);
    }
    medium_ = true;
    presentMs_ = millis();
    firstCmdMs_ = firstReadMs_ = 0;
    lastCmdMs_ = presentMs_;
    lastCmds_ = 0;
    burstLogged_ = false;
    diagLogf("usb storage: card presented to the computer, %lu sectors", static_cast<unsigned long>(blocks));
    return true;
}

// The computer stopped mid-command: halt the pipes (it recovers with a reset
// recovery or a new enumeration), drop every queued segment, finish a block
// request already running and drop its result.
void UsbStoragePort::dropCommand(const char* why) {
    {
        UsbLock l;
        gBot.abort();
    }
    while (gBot.ioPending()) gBot.runIo(gCard);
    delayMicroseconds(100);   // a packet the controller had started lands (~50 us)
    diagLogf("usb storage: %s, command in flight dropped", why);
}

bool UsbStoragePort::begin() {
    if (active_) return true;
    if (!sdhc::cardPresent()) return false;
    SdStorage& st = storage();
    SdStorage::FatInfo fi;
    const bool haveLayout = st.fatInfo(&fi);   // FatFs' view, before f_unmount
    st.setStorageMode(true);              // f_unmount, files closed (FSINFO synced), SDHC released
    sdhc::setOwner(sdhc::Owner::kHost);   // from here only the computer's path moves data
    gData = sdhc::hostBuffer(&gDataBytes);
    {
        UsbLock l;
        gBot.setDataBuffer(gData, gDataBytes);   // medium still absent: no command uses it yet
        if (haveLayout) gBot.setReadRegions(fi.fatStart, fi.fatStart + fi.fatSectors, fi.dataStart);
        else gBot.setReadRegions(0, 0, 0);
    }
    if (haveLayout) logLayout(fi);
    if (!presentCard()) {
        {
            UsbLock l;
            gBot.setDataBuffer(nullptr, 0);
        }
        sdhc::setOwner(sdhc::Owner::kApp);
        st.setStorageMode(false);
        st.mount();
        return false;
    }
    active_ = true;
    t0_ = millis();
    cardSeenMs_ = t0_;
    diagLog("usb storage: mode on (FatFs unmounted, card owned by the computer)");
    return true;
}

bool UsbStoragePort::end() {
    if (!active_) return storage().mounted();
    {
        UsbLock l;
        gBot.setMedium(false, 0);   // NOT READY "medium not present" from the next command on
    }
    medium_ = false;
    // A command in flight finishes (its block requests run here).
    const uint32_t t = millis();
    uint32_t ticks = gSofTicks, tickMs = t;
    for (;;) {
        bool busy;
        {
            UsbLock l;
            busy = gBot.busy();
        }
        if (!busy) break;
        if (gBot.runIo(gCard)) usb_msc_kick();
        if (gSofTicks != ticks) {
            ticks = gSofTicks;
            tickMs = millis();
        }
        // 3 s at most; at once when the computer is gone (no frames for 100 ms).
        if (millis() - tickMs > 100) {
            dropCommand("computer gone at exit");
            break;
        }
        if (millis() - t > 3000) {
            dropCommand("command did not finish within 3 s of exit");
            break;
        }
    }
    const uint32_t s = (millis() - t0_) / 1000;
    diagLogf("usb storage: mode off after %lu s, computer read %lu KB wrote %lu KB, %lu commands, %lu errors, "
             "%lu phase errors",
             static_cast<unsigned long>(s), static_cast<unsigned long>(gBot.bytesRead() / 1024),
             static_cast<unsigned long>(gBot.bytesWritten() / 1024), static_cast<unsigned long>(gBot.commands()),
             static_cast<unsigned long>(gBot.errors()), static_cast<unsigned long>(gBot.phaseErrors()));
    logErrors();
    logReads("session reads", millis() - presentMs_);
    {
        // Not busy (or aborted): no segment is queued or armed into the
        // bounce buffer any more (Bot::busy() covers queued receives).
        UsbLock l;
        gBot.setDataBuffer(nullptr, 0);   // the bounce buffer goes back to the app
    }
    gData = nullptr;
    sdhc::release();
    sdhc::setOwner(sdhc::Owner::kApp);
    SdStorage& st = storage();
    st.setStorageMode(false);
    active_ = false;
    const bool ok = st.mount();
    diagLogf("usb storage: card back to the app, mount %s", ok ? "ok" : "FAILED");
    return ok;
}

void UsbStoragePort::logLayout(const SdStorage::FatInfo& fi) {
    // Linux' vfat ignores the FSINFO free count unless mounted with
    // "usefree" and reads the whole first FAT on the first statfs (the
    // desktop's free-space query); macOS uses FSINFO. docs/HARDWARE_TESTING.md.
    const uint32_t fatKb = fi.fatSectors / 2;
    if (fi.freeClusters != 0xFFFFFFFFu)
        diagLogf("usb storage: FAT%u, %lu KB clusters, FAT %lu KB x%u, FSINFO %s, free %lu of %lu clusters",
                 unsigned(fi.type), static_cast<unsigned long>(fi.clusterBytes / 1024), static_cast<unsigned long>(fatKb),
                 unsigned(fi.fats), fi.fsinfo ? "valid" : "absent", static_cast<unsigned long>(fi.freeClusters),
                 static_cast<unsigned long>(fi.clusters));
    else
        diagLogf("usb storage: FAT%u, %lu KB clusters, FAT %lu KB x%u, FSINFO %s, free count unknown (%lu clusters)",
                 unsigned(fi.type), static_cast<unsigned long>(fi.clusterBytes / 1024), static_cast<unsigned long>(fatKb),
                 unsigned(fi.fats), fi.fsinfo ? "valid" : "absent", static_cast<unsigned long>(fi.clusters));
}

// Failed commands by cause, most frequent first: "<opcode> <key>/<ASC>/<ASCQ> x<n>".
void UsbStoragePort::logErrors() {
    char why[160];
    if (gBot.formatErrors(why, sizeof why)) diagLogf("usb storage: errors by cause: %s", why);
}

void UsbStoragePort::logReads(const char* what, uint32_t ms) {
    diagLogf("usb storage: %s: %lu KB in %lu ms, %lu READs (%lu of <= 4 KB) | boot+reserved %lu KB, FAT1 %lu KB, "
             "FAT2 %lu KB, data %lu KB",
             what, static_cast<unsigned long>(gBot.bytesRead() / 1024), static_cast<unsigned long>(ms),
             static_cast<unsigned long>(gBot.readCommands()), static_cast<unsigned long>(gBot.smallReads()),
             static_cast<unsigned long>(gBot.readBlocks(msc::kRegionReserved) / 2),
             static_cast<unsigned long>(gBot.readBlocks(msc::kRegionFat1) / 2),
             static_cast<unsigned long>(gBot.readBlocks(msc::kRegionFat2) / 2),
             static_cast<unsigned long>(gBot.readBlocks(msc::kRegionData) / 2));
}

// Mount profile: the first command and READ after presenting (the host's
// media poll), then the end of its first burst (probe + mount + whatever the
// desktop reads right after): no command for 1.5 s once it has read.
void UsbStoragePort::watchMount(uint32_t now) {
    if (!medium_ || burstLogged_) return;
    const uint32_t cmds = gBot.commands();
    if (cmds != lastCmds_) {
        lastCmds_ = cmds;
        lastCmdMs_ = now;
        if (!firstCmdMs_) firstCmdMs_ = now;
    }
    if (!firstReadMs_ && gBot.readCommands()) {
        firstReadMs_ = now;
        diagLogf("usb storage: computer's first command %lu ms, first READ %lu ms after presenting",
                 static_cast<unsigned long>(firstCmdMs_ - presentMs_), static_cast<unsigned long>(now - presentMs_));
    }
    if (firstReadMs_ && now - lastCmdMs_ > 1500) {
        burstLogged_ = true;
        logReads("first burst", lastCmdMs_ - presentMs_);
        diagLogf("usb storage: first burst: %lu commands, %lu errors, %lu phase errors",
                 static_cast<unsigned long>(cmds), static_cast<unsigned long>(gBot.errors()),
                 static_cast<unsigned long>(gBot.phaseErrors()));
        logErrors();
    }
}

IUsbStorage::Host UsbStoragePort::host() const {
    if (!usb_msc_configured()) return Host::kNoUsb;
    // No frames for 100 ms: cable out or the computer asleep.
    if (millis() - lastTickMs_ > 100) return Host::kNoUsb;
    if (gBot.hostEjected()) return Host::kEjected;
    if (gBot.hostAccessed()) return Host::kInUse;
    return Host::kWaiting;
}

void UsbStoragePort::service() {
    const uint32_t now = millis();
    if (gSofTicks != lastTicks_) {
        lastTicks_ = gSofTicks;
        lastTickMs_ = now;
    }
    if (!active_) return;
    // Cable pulled / computer gone mid-command (review B #3): abort after
    // 500 ms without frames, so the unit is not busy forever (no
    // re-presented card, no I/O spin, a fast exit).
    const bool seen = host() != Host::kNoUsb;
    bool busy;
    {
        UsbLock l;
        busy = gBot.busy();
    }
    if (watch_.update(seen, busy, now)) dropCommand("no frames from the computer for 500 ms");
    watchMount(now);
    // The card pulled / put back while the computer owns it.
    const bool present = sdhc::cardPresent();
    if (!present && medium_) {
        {
            UsbLock l;
            gBot.setMedium(false, 0);
        }
        medium_ = false;
        sdhc::release();
        diagLog("usb storage: card removed");
    } else if (present && !medium_) {
        if (!cardSeenMs_) cardSeenMs_ = now;
        if (now - cardSeenMs_ >= 500 && !gBot.busy()) presentCard();
    }
    if (!present) cardSeenMs_ = 0;
    // Block I/O for the computer: keep the ring full for up to 8 ms per loop
    // pass (the next request follows within microseconds of the kick while a
    // chunk is free; the UI keeps running between passes). With 4 chunks of
    // 4 KB (~14 ms of full-speed bus time) in flight, a pass that leaves for
    // a few ms does not idle the bus.
    const uint32_t t0 = micros();
    uint32_t idle = t0;
    for (;;) {
        const uint32_t t = micros();
        if (gBot.runIo(gCard)) {
            usb_msc_kick();
            idle = micros();
        } else if (t - idle > (seen && gBot.busy() ? 2000u : 300u)) {
            // Idle: a stream waits for USB (every chunk in flight, ~3.4 ms
            // per 4 KB at full speed) or no command runs.
            break;
        }
        if (t - t0 > 8000) break;
    }
}

UsbStoragePort gPort;

}  // namespace

void usbStorageInit() {
    gBot.init(&gEp, nullptr, 0, gSmall);
    usb_msc_set_hook(&isrHook);
}

IUsbStorage* usbStorage() { return &gPort; }

}  // namespace hal
}  // namespace ot

#else  // OT_USB_MSC=0

namespace ot {
namespace hal {
void usbStorageInit() {}
IUsbStorage* usbStorage() { return nullptr; }
}  // namespace hal
}  // namespace ot

#endif
