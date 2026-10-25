// In-memory INvStore (the config EEPROM app area) for host tests.
#pragma once

#include <cstring>
#include <vector>

#include "hal_api.h"

class FakeNvStore final : public ot::INvStore {
public:
    std::vector<uint8_t> mem = std::vector<uint8_t>(4096 - 10, 0xFF);   // erased EEPROM
    bool usable = true;     // false: like a FlexRAM that is not EEPROM-ready
    int writes = 0;         // bytes actually changed
    uint32_t size() const override { return uint32_t(mem.size()); }
    bool read(uint32_t off, void* dst, uint32_t len) override {
        if (!usable || off > mem.size() || len > mem.size() - off) return false;
        std::memcpy(dst, mem.data() + off, len);
        return true;
    }
    bool write(uint32_t off, const void* src, uint32_t len) override {
        if (!usable || off > mem.size() || len > mem.size() - off) return false;
        const uint8_t* s = static_cast<const uint8_t*>(src);
        for (uint32_t i = 0; i < len; ++i)
            if (mem[off + i] != s[i]) {
                mem[off + i] = s[i];
                ++writes;
            }
        return true;
    }
};
