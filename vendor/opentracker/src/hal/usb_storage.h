// OpenTracker 2.x -- USB Storage mode on the device (IUsbStorage, hal_api.h).
//
// The USB mass-storage interface (third_party/teensy3/usb_msc.h, OT_USB_MSC=1)
// is enumerated from boot; its protocol (src/core/usb/msc_bot.*) runs in the
// USB interrupt and reports "medium not present" until begin(). begin() makes
// the computer the card's owner (sdhc::Owner::kHost): FatFs is unmounted, the
// app's SD path is refused, and the card reads / writes the computer asks for
// run in service() from the main loop through the SD bounce buffer. end()
// removes the medium, lets a command in flight finish and gives the card back.
// OT_USB_MSC=0: usbStorage() returns nullptr (no USB storage in that image).
#pragma once

#include "hal_api.h"

namespace ot {
namespace hal {

// Install the USB hook (call once at boot, before the computer enumerates).
void usbStorageInit();
IUsbStorage* usbStorage();

}  // namespace hal
}  // namespace ot
