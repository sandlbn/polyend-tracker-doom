// USB Storage Mode screen (spec 01 §6.1, spec 05 §1.6 "USB Storage Mode";
// stock strings 0xa2c7c / 0xa2c90 / 0xa2d34, screen 0x29670 / 0x295a8).
//
// 2.x: the screen of a real USB Storage mode (Tracker Mini manual §13.10:
// Config > USB > Storage, Enter -> "USB Storage Mode" and a warning to keep
// the power connected during transfers; Exit returns to normal operation).
// While it is shown the SD card belongs to the computer (App::enterUsbStorage,
// IUsbStorage) and the page takes every button, pad and encoder event. A line
// under the warning says what the computer does with the drive. Exit while the
// computer still has the drive mounted first asks to eject it there; a second
// Exit leaves anyway.
#pragma once

#include <cstdint>

#include "hal_api.h"

namespace ot {

class Gfx;
struct Theme;

class UsbStorageMode {
public:
    static constexpr const char* kTitle = "USB Storage Mode";
    // [I] The Mini manual paraphrases its warning ("ensure power is retained
    // in this mode and during data transfer"); this wording is OpenTracker's.
    // The first line keeps the stock 1.9.2 "Do not unplug…" start (0xa2c90).
    static constexpr const char* kWarning = "Do not unplug the USB cable or remove the SD card";
    static constexpr const char* kWarning2 = "and keep the power connected while files are transferred.";
    static constexpr const char* kEjectFirst = "Eject the drive on the computer first";
    static constexpr const char* kEjectFirst2 = "Press Exit again to exit anyway.";
    static constexpr const char* kExit = "Exit";
    // SPEC GAP: the Exit control's position is not recovered; it sits over F8.
    static constexpr uint8_t kExitKey = 7;   // btn::F8

    enum class Action : uint8_t { kNone = 0, kRedraw, kExit };

    void show(bool on) {
        active_ = on;
        warned_ = false;
    }
    bool active() const { return active_; }
    // True when the host state changed (the page needs a redraw).
    bool setHost(IUsbStorage::Host h) {
        if (h == host_) return false;
        host_ = h;
        if (h != IUsbStorage::Host::kInUse) warned_ = false;
        return true;
    }
    IUsbStorage::Host host() const { return host_; }
    bool ejectWarningShown() const { return warned_; }
    static const char* hostText(IUsbStorage::Host h);
    Action onButton(uint8_t id, uint8_t state);
    void draw(Gfx& g, const Theme& th) const;

private:
    bool active_ = false;
    bool warned_ = false;
    IUsbStorage::Host host_ = IUsbStorage::Host::kNoUsb;
};

}  // namespace ot
