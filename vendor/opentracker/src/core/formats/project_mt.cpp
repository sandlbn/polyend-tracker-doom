// project.mt codec, spec 02 §3.
#include "formats/project_mt.h"

#include <cmath>
#include <cstring>

#include "formats/bytes.h"
#include "formats/crc32.h"
#include "formats/legacy_upgrade.h"

namespace ot {

namespace {
BlockUpgradeFn g_upgrade = nullptr;
}

void setProjectUpgradeHook(BlockUpgradeFn fn) { g_upgrade = fn; }

void encodeProject(const Project& p, uint8_t out[mt::kFileSize]) {
    std::memset(out, 0, mt::kFileSize);
    // spec 02 §3.1 header: "MT", type 0, fw 1.9.2.255, structure 17x4, size 0x914.
    FileHeader h{{'M', 'T'}, 0, {}, {}, uint16_t(mt::kFileSize)};
    std::memcpy(h.fwVersion, kProjectFwVersion, 4);
    std::memset(h.structVersion, kProjectStructVersion, 4);
    writeFileHeader(out, h);
    // spec 02 §3.2 song block.
    std::memcpy(out + mt::kSongOff, p.playlist, kSongSlots);
    out[mt::kSongOff + kSongSlots] = p.playlistPos;
    // spec 02 §3.3 values block.
    std::memcpy(out + mt::kValuesOff, p.values, mt::kValuesSize);
    // Name/folder with forced NULs at 0x830 / 0x90E.
    wrStr(out + mt::kNameOff, mt::kNameMax + 1, p.name);
    wrStr(out + mt::kFolderOff, mt::kFolderMax + 1, p.folder);
    out[mt::kNameNulOff] = 0;
    out[mt::kFolderNulOff] = 0;
    // spec 02 §2: CRC over 0x000..0x90F stored at 0x910.
    wrU32(out + mt::kCrcOff, crc32(out, mt::kCrcOff));
}

FmtStatus decodeProject(const uint8_t* file, size_t len, Project& p, ProjectLoadInfo* info,
                        uint8_t trackIndexLimit) {
    // spec 10 §0.1/§0.2: the reader zero-fills the whole 0x914-byte buffer,
    // so spec 02 §3.1 steps 1-2 (defaults pre-fill) are ineffective: bytes
    // beyond the end of a short file are 0.
    uint8_t buf[mt::kFileSize];
    std::memset(buf, 0, sizeof(buf));
    size_t n = len < mt::kFileSize ? len : mt::kFileSize;
    if (n) std::memcpy(buf, file, n);

    FileHeader h = readFileHeader(buf);
    ProjectLoadInfo li;
    li.structVersion = buf[0x008];
    li.sizeField = h.size;
    li.storedCrc = rdU32(buf + mt::kCrcOff);
    // Step 4: the CRC is computed and ignored.
    li.computedCrc = crc32(buf, mt::kCrcOff);

    // Step 3: type must be 0 (tracker-lib writes 1; firmware rejects it, spec 02 §11).
    if (h.type != 0) {
        if (info) *info = li;
        return FmtStatus::WrongType;
    }

    // spec 10 §0.3: structure version 0 is rejected (stock would jump to 0).
    if (li.structVersion == 0) {
        if (info) *info = li;
        return FmtStatus::Unsupported;
    }

    // Step 5: structure <= 16 -> upgrade chain (spec 10 §1.2), in place on
    // the file image. An installed hook replaces it (tests).
    if (li.structVersion <= 16) {
        li.upgraded = true;
        if (g_upgrade) g_upgrade(buf + mt::kSongOff, mt::kSongSize + mt::kValuesSize, li.structVersion);
        else legacy::upgradeProjectImage(buf, li.structVersion);
    }

    // Step 6: name/folder or "Unsaved Project".
    if (h.size == mt::kFileSize) {
        buf[mt::kNameNulOff] = 0;
        buf[mt::kFolderNulOff] = 0;
        rdStr(buf + mt::kNameOff, mt::kNameMax + 1, p.name, sizeof(p.name));
        rdStr(buf + mt::kFolderOff, mt::kFolderMax + 1, p.folder, sizeof(p.folder));
    } else {
        li.sizeMismatch = true;
        p.setName("Unsaved Project");
        p.setFolder("");
        buf[mt::kSizeMismatchFlag] = 3;
    }

    // Step 7: copy song + values, sanitise.
    std::memcpy(p.playlist, buf + mt::kSongOff, kSongSlots);
    p.playlistPos = buf[mt::kSongOff + kSongSlots];
    std::memcpy(p.values, buf + mt::kValuesOff, mt::kValuesSize);
    li.synthSlotsReset = sanitizeProject(p, trackIndexLimit);

    if (info) *info = li;
    return FmtStatus::Ok;
}

uint8_t sanitizeProject(Project& p, uint8_t trackIndexLimit) {
    // spec 02 §3.3 "Validation on load" column.
    if (int8_t(p.u8(0x110)) < 0) p.setU8(0x110, 48);
    if (p.u8(0x111) > 63) p.setU8(0x111, 0);
    if (p.u8(0x113) > 49) p.setU8(0x113, 0);
    for (uint32_t i = 0; i < 8; ++i)
        if (p.u8(mt::kTrackBool8 + i) > 1) p.setU8(mt::kTrackBool8 + i, 0);
    // 0x134 = Sample Recorder source (spec 05 appendix C §2.1/§2.4: 0 line in,
    // 1 radio, 2 mic low gain, 3 mic high gain). The original Tracker keeps
    // the stock 1.9.2 range 0..3 (`> 3 -> 0`). The Mini's `> 2 -> 0` (spec 21
    // §3.2) belongs to hardware without a tuner and would reset a stored
    // "Mic high gain" to line in on every load here.
    if (p.u8(mt::kMode134) > 3) p.setU8(mt::kMode134, 0);
    for (uint32_t i = 0; i < 8; ++i) {
        uint16_t v = p.u16(mt::kTrackWord8 + 2 * i);
        if (v < 1 || v > 255) p.setU16(mt::kTrackWord8 + 2 * i, 1);
    }
    if (trackIndexLimit) {
        // "each must be < N, else reset": the reset value is not given by the
        // spec; OpenTracker resets to the default (i+1) if valid, else 0.
        for (uint32_t i = 0; i < 12; ++i)
            if (p.u8(mt::kTrackIndex12 + i) >= trackIndexLimit)
                p.setU8(mt::kTrackIndex12 + i, uint8_t(i + 1 < trackIndexLimit ? i + 1 : 0));
    }
    for (uint32_t t = 0; t < 12; ++t) {
        uint32_t base = mt::kTrackRecord12 + t * 8;
        p.setU16(base, 0);
        for (uint32_t k = 1; k < 4; ++k) {
            int16_t v = int16_t(p.u16(base + 2 * k));
            if (v < -255 || v > 255) p.setU16(base + 2 * k, 0);
        }
    }
    // Tempo > 1000 -> 130. NaN is also reset (OpenTracker choice; the spec
    // compare would let NaN through).
    float tempo = p.tempo();
    if (!(tempo <= 1000.0f)) p.setTempo(130.0f);
    if (p.u16(mt::kDefaultPatternLength) > 255) p.setU16(mt::kDefaultPatternLength, 32);
    for (uint32_t i = 0; i < 12; ++i)
        if (p.u8(mt::kTrackMode12 + i) > 3) p.setU8(mt::kTrackMode12 + i, 0);
    for (int r = 0; r < kMidiInstrumentCount; ++r) {
        MidiInstrument m = p.midiInstrument(r);
        if (m.channel > 16) m.channel = uint8_t(r);
        if (m.velocity & 0x80) m.velocity = 127;
        for (auto& cc : m.cc)
            if (cc > 130) cc = 0;
        p.setMidiInstrument(r, m);
    }
    uint16_t v5fe = p.u16(mt::kU16_5FE);
    if (v5fe < 100 || v5fe > 20000) p.setU16(mt::kU16_5FE, 20000);
    if (p.u8(mt::kFlag600) > 1) p.setU8(mt::kFlag600, 0);
    if (p.u8(mt::kSel601) > 4) p.setU8(mt::kSel601, 0);
    if (p.u8(mt::kFlag602) > 1) p.setU8(mt::kFlag602, 0);
    for (uint32_t i = 0; i < 8; ++i)
        if (p.u8(mt::kTrackFlags8 + i) > 1) p.setU8(mt::kTrackFlags8 + i, 0);
    if (p.u8(mt::kPercent80C) > 100) p.setU8(mt::kPercent80C, 0);
    return sanitizeSynthSlots(p);
}

uint8_t sanitizeSynthSlots(Project& p, const uint8_t* bipolarMask) {
    uint8_t reset = 0;
    for (int s = 0; s < kSynthSlotCount; ++s) {
        if (p.synthSlotValid(s)) continue;
        p.resetSynthSlot(s, bipolarMask ? bipolarMask[s] : 0);
        reset = uint8_t(reset | (1u << s));
    }
    return reset;
}

}  // namespace ot
