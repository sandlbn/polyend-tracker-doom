/* OpenTracker addition to the Teensy 3.x core (see PATCHES.txt).
 * Same permissive terms as the rest of this directory (LICENSE.txt).
 *
 * Bulk endpoints of the USB mass-storage interface (usb_desc.h MSC_INTERFACE,
 * OT_USB_MSC=1). The packet-level work runs in usb_isr(): every 64-byte packet
 * goes by DMA straight between the USB controller and the caller's buffer (no
 * usb_malloc() packets, no copies). The protocol layer (src/core/usb/msc_bot.*)
 * queues whole segments and polls for their completion.
 *
 * Every function here except usb_msc_set_hook() and usb_msc_kick() must be called from the USB
 * interrupt context only: from the hook below, which usb_isr() calls on every
 * MSC token, on every start-of-frame (1 kHz) and after the class / halt
 * requests. That keeps all endpoint state single-threaded without locks.
 */
#ifndef _usb_msc_h_
#define _usb_msc_h_

#include <stdint.h>
#include "usb_desc.h"

#ifdef MSC_INTERFACE

#ifdef __cplusplus
extern "C" {
#endif

// Events passed to the hook.
#define USB_MSC_EVENT_POLL   0   // a token completed or a frame started
#define USB_MSC_EVENT_RESET  1   // bus reset, SET_CONFIGURATION or Bulk-Only Mass Storage Reset:
                                 // all queued segments were dropped

// Called from usb_isr() (interrupt context).
void usb_msc_set_hook(void (*hook)(uint8_t event));

// Queue a receive (host -> device) of up to len bytes into buf. A segment
// completes when len bytes arrived or on a short packet. At most 4 segments
// per direction may be outstanding (queued or completed but not yet polled).
// Returns 0 when the queue is full.
int usb_msc_rx_queue(uint8_t *buf, uint32_t len);
// 1 when the oldest receive segment completed (*got = bytes received); pops it.
int usb_msc_rx_poll(uint32_t *got);
// Queue a transmit (device -> host) of len bytes (> 0). A length that is not a
// multiple of 64 ends with a short packet. Returns 0 when the queue is full.
int usb_msc_tx_queue(const uint8_t *buf, uint32_t len);
// 1 when the oldest transmit segment was fully sent; pops it.
int usb_msc_tx_poll(void);
// Disarm and drop every receive (transmit) segment, complete or not. Only
// while the host sends (takes) nothing on that endpoint: the endpoint is
// halted, or the data stage ended (short packet) and the CSW is not queued
// yet. A packet the controller had already started still lands in its
// buffer (within one packet time, ~50 us) and is ignored.
void usb_msc_rx_flush(void);
void usb_msc_tx_flush(void);
// Halt (STALL) the bulk IN (in = 1) or OUT (in = 0) endpoint until the host
// clears it with CLEAR_FEATURE(ENDPOINT_HALT).
void usb_msc_stall(int in);
int usb_msc_stalled(int in);
int usb_msc_configured(void);

// Main loop (any context): run the hook (USB_MSC_EVENT_POLL) right away in
// the USB interrupt, e.g. after block I/O finished, instead of at the next
// token or frame.
void usb_msc_kick(void);

// usb_dev.c internals.
void usb_msc_bus_reset(void);         // USBRST (BDT banks back to even)
void usb_msc_configure(void);         // SET_CONFIGURATION
void usb_msc_clear_halt(int in);      // CLEAR_FEATURE(ENDPOINT_HALT)
void usb_msc_class_reset(void);       // Bulk-Only Mass Storage Reset
void usb_msc_token(uint8_t stat);     // TOKDNE on an MSC endpoint
void usb_msc_sof(void);               // start of frame

#ifdef __cplusplus
}
#endif

#endif // MSC_INTERFACE
#endif // _usb_msc_h_
