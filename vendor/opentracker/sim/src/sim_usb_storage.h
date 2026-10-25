// Simulator stand-in for the 2.x USB Storage mode (ot::IUsbStorage): there
// is no USB device, so begin() / end() only lock / unlock the host-directory
// card (any app access meanwhile is refused and counted) and the "computer"
// state is set by the script command `usbhost none|waiting|inuse|ejected`.
#pragma once

#include "hal_api.h"
#include "host_storage.h"

namespace sim {

class SimUsbStorage final : public ot::IUsbStorage {
public:
    explicit SimUsbStorage(HostStorage& fs) : fs_(fs) {}
    bool begin() override {
        fs_.setLocked(true);
        ++begins;
        return true;
    }
    bool end() override {
        fs_.setLocked(false);
        ++ends;
        return true;
    }
    Host host() const override { return host_; }
    void service() override {}
    void setHost(Host h) { host_ = h; }
    int begins = 0, ends = 0;

private:
    HostStorage& fs_;
    Host host_ = Host::kNoUsb;
};

}  // namespace sim
