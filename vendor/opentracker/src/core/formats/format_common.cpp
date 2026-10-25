#include "formats/format_common.h"

#include <cstring>

#include "formats/bytes.h"

namespace ot {

void writeFileHeader(uint8_t* out, const FileHeader& h) {
    out[0] = uint8_t(h.magic[0]);
    out[1] = uint8_t(h.magic[1]);
    wrU16(out + 2, h.type);
    std::memcpy(out + 4, h.fwVersion, 4);
    std::memcpy(out + 8, h.structVersion, 4);
    wrU16(out + 0x0C, h.size);
    out[0x0E] = 0;
    out[0x0F] = 0;
}

FileHeader readFileHeader(const uint8_t* in) {
    FileHeader h;
    h.magic[0] = char(in[0]);
    h.magic[1] = char(in[1]);
    h.type = rdU16(in + 2);
    std::memcpy(h.fwVersion, in + 4, 4);
    std::memcpy(h.structVersion, in + 8, 4);
    h.size = rdU16(in + 0x0C);
    return h;
}

}  // namespace ot
