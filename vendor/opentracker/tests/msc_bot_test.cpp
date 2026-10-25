// USB mass storage protocol (src/core/usb/msc_bot.*): Bulk-Only Transport and
// the SCSI commands, against a fake bulk endpoint pair that behaves like the
// Kinetis driver (64-byte packets, 2 segments per direction, STALL until the
// host clears it) and an in-memory card.
#include "test.h"

#include <cstring>
#include <deque>
#include <vector>

#include "usb/msc_bot.h"

using namespace ot;
using namespace ot::msc;

namespace {

struct Seg {
    uint8_t* buf;
    uint32_t len, done;
    bool complete;
};

class FakeEp : public IEndpoints {
public:
    bool isConfigured = true;
    std::deque<Seg> rx, tx;
    bool haltIn = false, haltOut = false;
    int maxSegs = 4;

    bool configured() const override { return isConfigured; }
    bool rxQueue(uint8_t* buf, uint32_t len) override {
        if (int(rx.size()) >= maxSegs || !len) return false;
        rx.push_back({buf, len, 0, false});
        return true;
    }
    bool rxPoll(uint32_t* got) override {
        if (rx.empty() || !rx.front().complete) return false;
        *got = rx.front().done;
        rx.pop_front();
        return true;
    }
    bool txQueue(const uint8_t* buf, uint32_t len) override {
        if (int(tx.size()) >= maxSegs || !len) return false;
        tx.push_back({const_cast<uint8_t*>(buf), len, 0, false});
        return true;
    }
    bool txPoll() override {
        if (tx.empty() || !tx.front().complete) return false;
        tx.pop_front();
        return true;
    }
    void rxFlush() override {
        rx.clear();
        ++rxFlushes;
    }
    void txFlush() override { tx.clear(); }
    int rxFlushes = 0;
    void stall(bool in) override { (in ? haltIn : haltOut) = true; }
    bool stalled(bool in) const override { return in ? haltIn : haltOut; }

    // ---- host side ----
    enum Tok { kAck, kNak, kStall };
    // One OUT packet (<= 64 bytes).
    Tok hostOut(const uint8_t* p, uint32_t n) {
        if (haltOut) return kStall;
        for (Seg& s : rx) {
            if (s.complete) continue;
            if (s.len - s.done < n) return kNak;   // would overflow the BDT: test bug
            std::memcpy(s.buf + s.done, p, n);
            s.done += n;
            if (s.done >= s.len || n < 64) s.complete = true;
            return kAck;
        }
        return kNak;
    }
    // One IN packet.
    Tok hostIn(std::vector<uint8_t>& out, uint32_t* n) {
        *n = 0;
        if (haltIn) return kStall;
        for (Seg& s : tx) {
            if (s.complete) continue;
            uint32_t k = s.len - s.done;
            if (k > 64) k = 64;
            out.insert(out.end(), s.buf + s.done, s.buf + s.done + k);
            s.done += k;
            if (s.done >= s.len) s.complete = true;
            *n = k;
            return kAck;
        }
        return kNak;
    }
};

class FakeCard : public IBlockDevice {
public:
    std::vector<uint8_t> data;
    int reads = 0, writes = 0;
    int failReadAt = -1, failWriteAt = -1;   // fail the n-th call
    explicit FakeCard(uint32_t blocks) : data(blocks * 512) {
        for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t(i * 7 + i / 512);
    }
    bool read(uint32_t lba, uint8_t* dst, uint32_t count) override {
        if (reads++ == failReadAt) return false;
        if ((lba + count) * 512 > data.size()) return false;
        std::memcpy(dst, &data[lba * 512], count * 512);
        return true;
    }
    bool write(uint32_t lba, const uint8_t* src, uint32_t count) override {
        if (writes++ == failWriteAt) return false;
        if ((lba + count) * 512 > data.size()) return false;
        std::memcpy(&data[lba * 512], src, count * 512);
        return true;
    }
};

struct Result {
    std::vector<uint8_t> data;
    bool stalledIn = false, stalledOut = false;
    bool gotCsw = false;
    uint32_t tag = 0, residue = 0;
    uint8_t status = 0xFF;
    std::vector<uint8_t> csw;
};

struct Rig {
    FakeEp ep;
    FakeCard card{2048};   // 1 MB card
    Bot bot;
    uint8_t buf[16384] = {};
    uint8_t small[Bot::kSmallBytes] = {};
    uint32_t tag = 100;
    bool runIo = true;

    explicit Rig(uint32_t bufBytes = 8192) { bot.init(&ep, buf, bufBytes, small); }
    void step() {
        bot.service();
        if (runIo) bot.runIo(card);
        bot.service();
    }
    void present() { bot.setMedium(true, uint32_t(card.data.size() / 512)); }

    // Send a raw CBW packet (n bytes).
    void sendRaw(const uint8_t* p, uint32_t n) {
        for (int i = 0; i < 50; ++i) {
            step();
            if (ep.hostOut(p, n) == FakeEp::kAck) return;
        }
        CHECK(!"CBW not accepted");
    }
    static void cbwBytes(uint8_t* c, uint32_t tag, uint32_t len, bool in, const std::vector<uint8_t>& cdb,
                         uint8_t lun = 0) {
        std::memset(c, 0, 31);
        c[0] = 'U'; c[1] = 'S'; c[2] = 'B'; c[3] = 'C';
        std::memcpy(c + 4, &tag, 4);
        std::memcpy(c + 8, &len, 4);
        c[12] = in ? 0x80 : 0x00;
        c[13] = lun;
        c[14] = uint8_t(cdb.size());
        std::memcpy(c + 15, cdb.data(), cdb.size());
    }
    // A full BOT transaction the way a host does it: CBW, data stage (stops
    // on a short packet or STALL; a halted pipe is cleared), CSW.
    Result cmd(const std::vector<uint8_t>& cdb, uint32_t len, bool in, const std::vector<uint8_t>& out = {},
               uint8_t lun = 0) {
        Result r;
        uint8_t c[31];
        cbwBytes(c, ++tag, len, in, cdb, lun);
        sendRaw(c, 31);
        if (len && in) {
            uint32_t got = 0;
            for (int i = 0; i < 200000 && got < len; ++i) {
                step();
                uint32_t n = 0;
                const FakeEp::Tok t = ep.hostIn(r.data, &n);
                if (t == FakeEp::kStall) {
                    r.stalledIn = true;
                    break;
                }
                if (t == FakeEp::kAck) {
                    got += n;
                    if (n < 64) break;   // short packet ends the data stage
                }
            }
        } else if (len) {
            uint32_t off = 0;
            for (int i = 0; i < 200000 && off < len; ++i) {
                step();
                const uint32_t n = len - off < 64 ? len - off : 64;
                std::vector<uint8_t> pkt(n, 0);
                if (off < out.size()) std::memcpy(pkt.data(), out.data() + off, std::min<size_t>(n, out.size() - off));
                const FakeEp::Tok t = ep.hostOut(pkt.data(), n);
                if (t == FakeEp::kStall) {
                    r.stalledOut = true;
                    ep.haltOut = false;   // CLEAR_FEATURE(HALT)
                    break;
                }
                if (t == FakeEp::kAck) off += n;
            }
        }
        // CSW
        for (int i = 0; i < 200000; ++i) {
            step();
            uint32_t n = 0;
            const FakeEp::Tok t = ep.hostIn(r.csw, &n);
            if (t == FakeEp::kStall) {
                r.stalledIn = true;
                ep.haltIn = false;   // CLEAR_FEATURE(HALT), then read the CSW again
                continue;
            }
            if (t == FakeEp::kAck) {
                r.gotCsw = n == 13;
                break;
            }
        }
        if (r.gotCsw) {
            uint32_t sig;
            std::memcpy(&sig, r.csw.data(), 4);
            CHECK_EQ(sig, Bot::kCswSig);
            std::memcpy(&r.tag, r.csw.data() + 4, 4);
            std::memcpy(&r.residue, r.csw.data() + 8, 4);
            r.status = r.csw[12];
            CHECK_EQ(r.tag, tag);
        }
        step();
        return r;
    }
    Sense requestSense() {
        Result r = cmd({op::kRequestSense, 0, 0, 0, 18, 0}, 18, true);
        Sense s;
        if (r.data.size() >= 14) {
            s.key = r.data[2] & 0x0F;
            s.asc = r.data[12];
            s.ascq = r.data[13];
        }
        return s;
    }
    static std::vector<uint8_t> rw10(uint8_t opc, uint32_t lba, uint16_t n) {
        return {opc, 0, uint8_t(lba >> 24), uint8_t(lba >> 16), uint8_t(lba >> 8), uint8_t(lba), 0,
                uint8_t(n >> 8), uint8_t(n), 0};
    }
};

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

}  // namespace

TEST(inquiry_works_without_medium) {
    Rig g;
    Result r = g.cmd({op::kInquiry, 0, 0, 0, 36, 0}, 36, true);
    CHECK(r.gotCsw);
    CHECK_EQ(int(r.status), 0);
    CHECK_EQ(r.data.size(), size_t(36));
    CHECK_EQ(int(r.data[0]), 0x00);   // direct access
    CHECK_EQ(int(r.data[1]), 0x80);   // removable
    CHECK(std::memcmp(r.data.data() + 8, "OpenTrkr", 8) == 0);
    CHECK_EQ(r.residue, 0u);
    CHECK(!r.stalledIn);
}

TEST(inquiry_longer_allocation_short_packet_then_stall) {
    Rig g;
    // Host asks for 96 bytes: 36 sent (short packet), IN halted, residue 60.
    Result r = g.cmd({op::kInquiry, 0, 0, 0, 96, 0}, 96, true);
    CHECK_EQ(r.data.size(), size_t(36));
    CHECK(r.stalledIn);
    CHECK_EQ(int(r.status), 0);
    CHECK_EQ(r.residue, 60u);
}

TEST(inquiry_evpd_is_illegal) {
    Rig g;
    Result r = g.cmd({op::kInquiry, 1, 0x80, 0, 64, 0}, 64, true);
    CHECK_EQ(int(r.status), 1);
    CHECK(r.stalledIn);
    CHECK_EQ(r.residue, 64u);
    const Sense s = g.requestSense();
    CHECK_EQ(int(s.key), int(sense::kIllegalRequest));
    CHECK_EQ(int(s.asc), int(sense::kAscInvalidField));
}

TEST(medium_not_present_outside_storage_mode) {
    Rig g;
    Result r = g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 0, false);
    CHECK_EQ(int(r.status), 1);
    Sense s = g.requestSense();
    CHECK_EQ(int(s.key), int(sense::kNotReady));
    CHECK_EQ(int(s.asc), int(sense::kAscNoMedium));
    // READ CAPACITY, READ, WRITE all fail; nothing reaches the card.
    r = g.cmd({op::kReadCapacity10, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 8, true);
    CHECK_EQ(int(r.status), 1);
    CHECK(r.stalledIn);
    CHECK_EQ(r.residue, 8u);
    r = g.cmd(Rig::rw10(op::kRead10, 0, 8), 4096, true);
    CHECK_EQ(int(r.status), 1);
    CHECK(r.stalledIn);
    CHECK(r.data.empty());
    CHECK_EQ(r.residue, 4096u);
    std::vector<uint8_t> junk(4096, 0xEE);
    const std::vector<uint8_t> before = g.card.data;
    r = g.cmd(Rig::rw10(op::kWrite10, 0, 8), 4096, false, junk);
    CHECK_EQ(int(r.status), 1);
    CHECK_EQ(r.residue, 4096u);
    CHECK(g.card.data == before);
    CHECK_EQ(g.card.reads + g.card.writes, 0);
    s = g.requestSense();
    CHECK_EQ(int(s.asc), int(sense::kAscNoMedium));
    // REQUEST SENSE with nothing stored still says "no medium".
    s = g.requestSense();
    CHECK_EQ(int(s.key), int(sense::kNotReady));
    CHECK_EQ(int(s.asc), int(sense::kAscNoMedium));
    // READ FORMAT CAPACITIES answers "no media present".
    r = g.cmd({op::kReadFormatCapacities, 0, 0, 0, 0, 0, 0, 0, 12, 0}, 12, true);
    CHECK_EQ(int(r.status), 0);
    CHECK_EQ(r.data.size(), size_t(12));
    CHECK_EQ(int(r.data[8]), 3);
    CHECK(!g.bot.hostAccessed());
}

TEST(enter_reports_unit_attention_once_then_ready) {
    Rig g;
    g.present();
    Result r = g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 0, false);
    CHECK_EQ(int(r.status), 1);
    Sense s = g.requestSense();
    CHECK_EQ(int(s.key), int(sense::kUnitAttention));
    CHECK_EQ(int(s.asc), int(sense::kAscMediumChanged));
    r = g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 0, false);
    CHECK_EQ(int(r.status), 0);
    CHECK(!g.bot.hostAccessed());   // polling alone is not "in use"
    r = g.cmd({op::kReadCapacity10, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 8, true);
    CHECK_EQ(int(r.status), 0);
    CHECK_EQ(r.data.size(), size_t(8));
    CHECK_EQ(be32(r.data.data()), 2047u);
    CHECK_EQ(be32(r.data.data() + 4), 512u);
    CHECK(g.bot.hostAccessed());
}

TEST(inquiry_does_not_consume_unit_attention) {
    Rig g;
    g.present();
    Result r = g.cmd({op::kInquiry, 0, 0, 0, 36, 0}, 36, true);
    CHECK_EQ(int(r.status), 0);
    r = g.cmd({op::kReadCapacity10, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 8, true);
    CHECK_EQ(int(r.status), 1);   // the UA comes here
    CHECK(r.stalledIn);
    r = g.cmd({op::kReadCapacity10, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 8, true);
    CHECK_EQ(int(r.status), 0);
}

static void ready(Rig& g) {
    g.present();
    g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 0, false);   // eats the UA
}

TEST(read10_multi_chunk_matches_card) {
    Rig g;
    ready(g);
    // 40 blocks = 20 KB: 5 chunks through the two 4 KB halves.
    Result r = g.cmd(Rig::rw10(op::kRead10, 17, 40), 40 * 512, true);
    CHECK_EQ(int(r.status), 0);
    CHECK_EQ(r.residue, 0u);
    CHECK(!r.stalledIn);
    CHECK_EQ(r.data.size(), size_t(40 * 512));
    CHECK(std::memcmp(r.data.data(), &g.card.data[17 * 512], 40 * 512) == 0);
    CHECK_EQ(g.card.reads, 5);   // multi-block reads, 8 blocks each
    CHECK_EQ(g.bot.bytesRead(), 40u * 512);
    CHECK(!g.bot.busy());
}

TEST(read10_single_block_and_zero_blocks) {
    Rig g;
    ready(g);
    Result r = g.cmd(Rig::rw10(op::kRead10, 2047, 1), 512, true);
    CHECK_EQ(int(r.status), 0);
    CHECK(std::memcmp(r.data.data(), &g.card.data[2047 * 512], 512) == 0);
    r = g.cmd(Rig::rw10(op::kRead10, 5, 0), 0, true);
    CHECK_EQ(int(r.status), 0);
}

TEST(read10_out_of_range) {
    Rig g;
    ready(g);
    Result r = g.cmd(Rig::rw10(op::kRead10, 2040, 16), 16 * 512, true);
    CHECK_EQ(int(r.status), 1);
    CHECK(r.stalledIn);
    CHECK_EQ(r.residue, 16u * 512);
    const Sense s = g.requestSense();
    CHECK_EQ(int(s.key), int(sense::kIllegalRequest));
    CHECK_EQ(int(s.asc), int(sense::kAscLbaOutOfRange));
    CHECK_EQ(g.card.reads, 0);
}

TEST(read10_card_error_mid_stream) {
    Rig g;
    ready(g);
    g.card.failReadAt = 2;   // third chunk
    Result r = g.cmd(Rig::rw10(op::kRead10, 0, 40), 40 * 512, true);
    CHECK_EQ(int(r.status), 1);
    CHECK(r.stalledIn);
    CHECK_EQ(r.data.size(), size_t(2 * 4096));
    CHECK_EQ(r.residue, 40u * 512 - 2 * 4096);
    const Sense s = g.requestSense();
    CHECK_EQ(int(s.key), int(sense::kMediumError));
    CHECK_EQ(int(s.asc), int(sense::kAscUnrecoveredRead));
    // The next command works.
    r = g.cmd(Rig::rw10(op::kRead10, 0, 1), 512, true);
    CHECK_EQ(int(r.status), 0);
}

TEST(write10_multi_chunk_reaches_card) {
    Rig g;
    ready(g);
    std::vector<uint8_t> d(24 * 512);
    for (size_t i = 0; i < d.size(); ++i) d[i] = uint8_t(i ^ 0x5A);
    Result r = g.cmd(Rig::rw10(op::kWrite10, 100, 24), uint32_t(d.size()), false, d);
    CHECK_EQ(int(r.status), 0);
    CHECK_EQ(r.residue, 0u);
    CHECK(std::memcmp(&g.card.data[100 * 512], d.data(), d.size()) == 0);
    CHECK_EQ(g.card.writes, 3);
    CHECK_EQ(g.bot.bytesWritten(), uint32_t(d.size()));
    CHECK(g.bot.hostAccessed());
    // Read back.
    r = g.cmd(Rig::rw10(op::kRead10, 100, 24), uint32_t(d.size()), true);
    CHECK(r.data == d);
}

TEST(write10_card_error_takes_all_data) {
    Rig g;
    ready(g);
    g.card.failWriteAt = 0;
    std::vector<uint8_t> d(16 * 512, 0x11);
    Result r = g.cmd(Rig::rw10(op::kWrite10, 0, 16), uint32_t(d.size()), false, d);
    CHECK(!r.stalledOut);
    CHECK_EQ(int(r.status), 1);
    CHECK_EQ(r.residue, uint32_t(d.size()));
    const Sense s = g.requestSense();
    CHECK_EQ(int(s.key), int(sense::kMediumError));
    CHECK_EQ(int(s.asc), int(sense::kAscWriteFault));
    CHECK_EQ(g.card.writes, 1);   // nothing more after the failure
}

TEST(mode_sense_and_format_capacities) {
    Rig g;
    ready(g);
    // Linux usb-storage: MODE SENSE(6), all pages, 192 bytes -> header +
    // caching page (WCE = 0), short packet, IN halted.
    Result r = g.cmd({op::kModeSense6, 0, 0x3F, 0, 192, 0}, 192, true);
    CHECK_EQ(int(r.status), 0);
    CHECK_EQ(r.data.size(), size_t(24));
    CHECK_EQ(int(r.data[0]), 23);
    CHECK_EQ(int(r.data[2] & 0x80), 0);   // not write protected
    CHECK_EQ(int(r.data[3]), 0);          // no block descriptors
    CHECK_EQ(int(r.data[4]), 0x08);
    CHECK_EQ(int(r.data[5]), 0x12);
    CHECK_EQ(int(r.data[6] & 0x05), 0);   // WCE = 0, RCD = 0
    CHECK(r.stalledIn);                    // 24 of 192 bytes
    CHECK_EQ(r.residue, 168u);
    // Header only when that is all the host asks for.
    r = g.cmd({op::kModeSense6, 0, 0x3F, 0, 4, 0}, 4, true);
    CHECK_EQ(int(r.status), 0);
    CHECK_EQ(r.data.size(), size_t(4));
    CHECK(!r.stalledIn);
    r = g.cmd({op::kModeSense10, 0, 0x08, 0, 0, 0, 0, 0, 28, 0}, 28, true);
    CHECK_EQ(int(r.status), 0);
    CHECK_EQ(r.data.size(), size_t(28));
    CHECK_EQ(int(r.data[1]), 26);
    CHECK_EQ(int(r.data[8]), 0x08);
    CHECK(!r.stalledIn);
    r = g.cmd({op::kModeSense10, 0, 0x3F, 0, 0, 0, 0, 0, 8, 0}, 8, true);
    CHECK_EQ(int(r.status), 0);
    CHECK_EQ(r.data.size(), size_t(8));
    CHECK(!r.stalledIn);
    // Changeable values (PC = 1): the same page, all bits 0.
    r = g.cmd({op::kModeSense6, 0, 0x48, 0, 24, 0}, 24, true);
    CHECK_EQ(int(r.status), 0);
    CHECK_EQ(r.data.size(), size_t(24));
    CHECK_EQ(int(r.data[6]), 0);
    // Other pages (Windows asks 1Ch): header only, as before.
    r = g.cmd({op::kModeSense6, 0, 0x1C, 0, 192, 0}, 192, true);
    CHECK_EQ(int(r.status), 0);
    CHECK_EQ(r.data.size(), size_t(4));
    CHECK_EQ(int(r.data[0]), 3);
    // Saved values: not supported.
    r = g.cmd({op::kModeSense6, 0, 0xFF, 0, 192, 0}, 192, true);
    CHECK_EQ(int(r.status), 1);
    Sense s = g.requestSense();
    CHECK_EQ(int(s.key), int(sense::kIllegalRequest));
    CHECK_EQ(int(s.asc), int(sense::kAscSavingNotSupported));
    r = g.cmd({op::kReadFormatCapacities, 0, 0, 0, 0, 0, 0, 0, 0xFC, 0}, 0xFC, true);
    CHECK_EQ(int(r.status), 0);
    CHECK_EQ(r.data.size(), size_t(12));
    CHECK_EQ(be32(r.data.data() + 4), 2048u);
    CHECK_EQ(int(r.data[8]), 2);
}

TEST(prevent_sync_verify_start_stop) {
    Rig g;
    ready(g);
    Result r = g.cmd({op::kPreventAllow, 0, 0, 0, 1, 0}, 0, false);
    CHECK_EQ(int(r.status), 0);
    CHECK(g.bot.hostPrevented());
    r = g.cmd({op::kSyncCache10, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, false);
    CHECK_EQ(int(r.status), 0);
    r = g.cmd({op::kVerify10, 0, 0, 0, 0, 0, 0, 0, 8, 0}, 0, false);
    CHECK_EQ(int(r.status), 0);
    r = g.cmd({op::kPreventAllow, 0, 0, 0, 0, 0}, 0, false);
    CHECK(!g.bot.hostPrevented());
    // Eject: the medium then reads as not present.
    r = g.cmd({op::kStartStopUnit, 0, 0, 0, 0x02, 0}, 0, false);
    CHECK_EQ(int(r.status), 0);
    CHECK(g.bot.hostEjected());
    r = g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 0, false);
    CHECK_EQ(int(r.status), 1);
    CHECK_EQ(int(g.requestSense().asc), int(sense::kAscNoMedium));
    // Load again.
    r = g.cmd({op::kStartStopUnit, 0, 0, 0, 0x03, 0}, 0, false);
    CHECK(!g.bot.hostEjected());
    r = g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 0, false);
    CHECK_EQ(int(r.status), 0);
}

TEST(unknown_opcode_and_bad_lun) {
    Rig g;
    ready(g);
    Result r = g.cmd({0xA0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 16, true);
    CHECK_EQ(int(r.status), 1);
    CHECK(r.stalledIn);
    CHECK_EQ(int(g.requestSense().asc), int(sense::kAscInvalidOpcode));
    r = g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 0, false, {}, 1);
    CHECK_EQ(int(r.status), 1);
    // Ho > Dn: the data is taken and dropped.
    std::vector<uint8_t> d(100, 1);
    r = g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 100, false, d);
    CHECK(!r.stalledOut);
    CHECK_EQ(int(r.status), 0);
    CHECK_EQ(r.residue, 100u);
}

TEST(phase_errors) {
    Rig g;
    ready(g);
    // Case 2 (Hn < Di): READ with no data stage.
    Result r = g.cmd(Rig::rw10(op::kRead10, 0, 1), 0, true);
    CHECK_EQ(int(r.status), 2);
    // Case 7 (Hi < Di): host expects 512, command reads 1024.
    r = g.cmd(Rig::rw10(op::kRead10, 0, 2), 512, true);
    CHECK_EQ(int(r.status), 2);
    CHECK(r.stalledIn);
    // Case 8 (Hi <> Do): WRITE with an IN data stage.
    r = g.cmd(Rig::rw10(op::kWrite10, 0, 1), 512, true);
    CHECK_EQ(int(r.status), 2);
    CHECK(r.stalledIn);
    // Case 10 (Ho <> Di): READ with an OUT data stage.
    r = g.cmd(Rig::rw10(op::kRead10, 0, 1), 512, false, std::vector<uint8_t>(512, 0));
    CHECK_EQ(int(r.status), 2);
    CHECK(r.stalledOut);
    CHECK_EQ(g.card.writes, 0);
    // Case 3 (Hn < Do).
    r = g.cmd(Rig::rw10(op::kWrite10, 0, 1), 0, false);
    CHECK_EQ(int(r.status), 2);
    // Still in step: a normal command works.
    r = g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 0, false);
    CHECK_EQ(int(r.status), 0);
}

TEST(invalid_cbw_halts_until_reset_recovery) {
    Rig g;
    uint8_t c[31];
    Rig::cbwBytes(c, 1, 0, false, {op::kTestUnitReady, 0, 0, 0, 0, 0});
    c[0] = 'X';   // bad signature
    g.sendRaw(c, 31);
    g.step();
    CHECK_EQ(int(g.bot.state()), int(Bot::kHalted));
    CHECK(g.ep.haltIn && g.ep.haltOut);
    // Clearing the halts without a reset: stays halted (BOT 6.6.1).
    g.ep.haltIn = g.ep.haltOut = false;
    g.step();
    CHECK(g.ep.haltIn && g.ep.haltOut);
    // Wrong length CBW is invalid too, after reset recovery.
    g.ep.rx.clear();
    g.ep.tx.clear();
    g.bot.onReset();
    g.ep.haltIn = g.ep.haltOut = false;
    g.step();
    CHECK_EQ(int(g.bot.state()), int(Bot::kWaitCbw));
    Result r = g.cmd({op::kInquiry, 0, 0, 0, 36, 0}, 36, true);
    CHECK_EQ(int(r.status), 0);
    Rig::cbwBytes(c, 2, 0, false, {op::kTestUnitReady, 0, 0, 0, 0, 0});
    g.sendRaw(c, 30);
    g.step();
    CHECK_EQ(int(g.bot.state()), int(Bot::kHalted));
}

TEST(not_configured_waits) {
    Rig g;
    g.ep.isConfigured = false;
    g.step();
    CHECK_EQ(int(g.bot.state()), int(Bot::kIdle));
    CHECK(g.ep.rx.empty());
    g.ep.isConfigured = true;
    g.step();
    CHECK_EQ(int(g.bot.state()), int(Bot::kWaitCbw));
}

TEST(exit_waits_for_in_flight_write) {
    Rig g;
    ready(g);
    g.runIo = false;   // the main loop is busy elsewhere
    std::vector<uint8_t> d(8 * 512, 0x42);
    uint8_t c[31];
    Rig::cbwBytes(c, 7, uint32_t(d.size()), false, Rig::rw10(op::kWrite10, 3, 8));
    g.sendRaw(c, 31);
    for (uint32_t off = 0; off < d.size(); off += 64) {
        for (int i = 0; i < 10 && g.ep.hostOut(d.data() + off, 64) != FakeEp::kAck; ++i) g.step();
    }
    g.step();
    CHECK(g.bot.busy());
    CHECK(g.bot.ioPending());
    // Exit: medium removed first; the command in flight still completes.
    g.bot.setMedium(false, 0);
    CHECK(g.bot.busy());
    CHECK(g.bot.runIo(g.card));
    g.step();
    CHECK(std::memcmp(&g.card.data[3 * 512], d.data(), d.size()) == 0);
    std::vector<uint8_t> csw;
    uint32_t n = 0;
    CHECK(g.ep.hostIn(csw, &n) == FakeEp::kAck);
    CHECK_EQ(n, 13u);
    CHECK_EQ(int(csw[12]), 0);
    g.step();
    CHECK(!g.bot.busy());
    g.runIo = true;
    // New commands no longer reach the card.
    const int w = g.card.writes;
    Result r = g.cmd(Rig::rw10(op::kWrite10, 3, 1), 512, false, std::vector<uint8_t>(512, 9));
    CHECK_EQ(int(r.status), 1);
    CHECK_EQ(g.card.writes, w);
}

TEST(reset_during_read_drops_stale_io) {
    Rig g;
    ready(g);
    g.runIo = false;
    uint8_t c[31];
    Rig::cbwBytes(c, 9, 16 * 512, true, Rig::rw10(op::kRead10, 0, 16));
    g.sendRaw(c, 31);
    g.step();
    CHECK(g.bot.ioPending());
    // Host gives up: Bulk-Only Mass Storage Reset.
    g.ep.rx.clear();
    g.ep.tx.clear();
    g.bot.onReset();
    g.step();
    CHECK(g.bot.busy());   // the stale request still owns the buffer
    g.bot.runIo(g.card);
    g.step();
    CHECK(!g.bot.busy());
    CHECK(g.ep.tx.empty());   // the stale data never went out
    g.runIo = true;
    Result r = g.cmd(Rig::rw10(op::kRead10, 4, 2), 1024, true);
    CHECK_EQ(int(r.status), 0);
    CHECK(std::memcmp(r.data.data(), &g.card.data[4 * 512], 1024) == 0);
}

TEST(medium_removed_then_present_again) {
    Rig g;
    ready(g);
    CHECK(g.bot.mediumPresent());
    g.bot.setMedium(false, 0);
    Result r = g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 0, false);
    CHECK_EQ(int(r.status), 1);
    CHECK_EQ(int(g.requestSense().asc), int(sense::kAscNoMedium));
    g.present();
    r = g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 0, false);
    CHECK_EQ(int(r.status), 1);
    CHECK_EQ(int(g.requestSense().asc), int(sense::kAscMediumChanged));
    CHECK(!g.bot.hostAccessed());
    CHECK(!g.bot.hostEjected());
}

TEST(big_buffer_untouched_without_medium) {
    // Outside storage mode the data buffer belongs to the app (device: the SD
    // bounce buffer): no command may write into it or send from it.
    Rig g;
    std::memset(g.buf, 0xA5, sizeof g.buf);
    g.cmd({op::kInquiry, 0, 0, 0, 36, 0}, 36, true);
    g.cmd({op::kRequestSense, 0, 0, 0, 18, 0}, 18, true);
    g.cmd({op::kModeSense6, 0, 0x3F, 0, 192, 0}, 192, true);
    g.cmd({op::kReadFormatCapacities, 0, 0, 0, 0, 0, 0, 0, 12, 0}, 12, true);
    g.cmd(Rig::rw10(op::kRead10, 0, 16), 16 * 512, true);
    g.cmd(Rig::rw10(op::kWrite10, 0, 16), 16 * 512, false, std::vector<uint8_t>(16 * 512, 0x33));
    g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 300, false, std::vector<uint8_t>(300, 0x44));
    g.cmd(Rig::rw10(op::kRead10, 0, 1), 512, false, std::vector<uint8_t>(512, 0x55));   // phase error
    bool clean = true;
    for (uint8_t b : g.buf) clean = clean && b == 0xA5;
    CHECK(clean);
    CHECK_EQ(g.card.reads + g.card.writes, 0);
}

TEST(abort_halts_until_reset_recovery) {
    Rig g;
    ready(g);
    g.runIo = false;
    uint8_t c[31];
    Rig::cbwBytes(c, 11, 8 * 512, false, Rig::rw10(op::kWrite10, 0, 8));
    g.sendRaw(c, 31);
    g.step();
    CHECK(g.bot.busy());   // waiting for data that never comes
    g.bot.abort();
    CHECK(!g.bot.busy());
    CHECK(g.ep.haltIn && g.ep.haltOut);
    g.step();
    CHECK_EQ(int(g.bot.state()), int(Bot::kHalted));
    // Reset recovery.
    g.ep.rx.clear();
    g.ep.tx.clear();
    g.bot.onReset();
    g.ep.haltIn = g.ep.haltOut = false;
    g.runIo = true;
    Result r = g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 0, false);
    CHECK_EQ(int(r.status), 0);
    CHECK_EQ(g.card.writes, 0);
}

TEST(stale_read_after_reset_never_reaches_next_write) {
    // Review B #1: READ times out -> Bulk-Only Reset -> WRITE(10). The stale
    // SD read must not land in the buffer the WRITE's data went to (it would
    // then be written to the card in place of the host's data).
    Rig g;
    ready(g);
    g.runIo = false;   // the main loop is stuck in the READ's card access
    uint8_t c[31];
    Rig::cbwBytes(c, 21, 8 * 512, true, Rig::rw10(op::kRead10, 0, 8));
    g.sendRaw(c, 31);
    g.step();
    CHECK(g.bot.ioPending());
    // The host times out: Bulk-Only Mass Storage Reset (the driver drops its queues).
    g.ep.rx.clear();
    g.ep.tx.clear();
    g.bot.onReset();
    // WRITE(10) of 8 blocks at LBA 100: the host sends CBW and data as soon
    // as the device takes them.
    std::vector<uint8_t> d(8 * 512);
    for (size_t i = 0; i < d.size(); ++i) d[i] = uint8_t(0xC0 ^ i);
    Rig::cbwBytes(c, 22, uint32_t(d.size()), false, Rig::rw10(op::kWrite10, 100, 8));
    bool cbwSent = false;
    uint32_t off = 0;
    auto pump = [&](int tries) {
        for (int i = 0; i < tries; ++i) {
            g.step();
            if (!cbwSent) {
                cbwSent = g.ep.hostOut(c, 31) == FakeEp::kAck;
            } else if (off < d.size()) {
                if (g.ep.hostOut(d.data() + off, 64) == FakeEp::kAck) off += 64;
            }
        }
    };
    pump(400);
    // Now the stale read finishes (into whatever buffer it was given).
    CHECK(g.bot.runIo(g.card));
    g.runIo = true;
    pump(2000);
    CHECK(cbwSent);
    CHECK_EQ(off, uint32_t(d.size()));
    std::vector<uint8_t> csw;
    uint32_t n = 0;
    for (int i = 0; i < 200 && n == 0; ++i) {
        g.step();
        g.ep.hostIn(csw, &n);
    }
    CHECK_EQ(n, 13u);
    if (n == 13) CHECK_EQ(int(csw[12]), 0);
    CHECK(std::memcmp(&g.card.data[100 * 512], d.data(), d.size()) == 0);
}

TEST(short_packet_in_write_data_stage) {
    // Review F1: the host ends a WRITE(10) data stage early. Only the whole
    // blocks it sent reach the card (no stale buffer bytes), the queued
    // receives are dropped and the next CBW is answered.
    Rig g;
    ready(g);
    std::memset(g.buf, 0x77, sizeof g.buf);   // stale buffer contents
    const std::vector<uint8_t> before = g.card.data;
    std::vector<uint8_t> d(4096 + 31);
    for (size_t i = 0; i < d.size(); ++i) d[i] = uint8_t(i * 3 + 1);
    uint8_t c[31];
    Rig::cbwBytes(c, 31, 24 * 512, false, Rig::rw10(op::kWrite10, 0, 24));
    g.sendRaw(c, 31);
    uint32_t off = 0;
    for (int i = 0; i < 2000 && off < d.size(); ++i) {
        g.step();
        const uint32_t n = d.size() - off < 64 ? uint32_t(d.size() - off) : 64;
        if (g.ep.hostOut(d.data() + off, n) == FakeEp::kAck) off += n;
    }
    CHECK_EQ(off, uint32_t(d.size()));
    std::vector<uint8_t> csw;
    uint32_t n = 0;
    for (int i = 0; i < 200 && n == 0; ++i) {
        g.step();
        g.ep.hostIn(csw, &n);
    }
    CHECK_EQ(n, 13u);
    if (n == 13) CHECK_EQ(int(csw[12]), int(Bot::kStatusPhase));
    CHECK(g.ep.rxFlushes > 0);
    CHECK(std::memcmp(&g.card.data[0], d.data(), 4096) == 0);                       // the 8 blocks sent
    CHECK(std::memcmp(&g.card.data[4096], &before[4096], 16 * 512) == 0);           // nothing stale after them
    CHECK_EQ(g.card.writes, 1);
    g.step();
    CHECK(!g.bot.busy());
    // The next command is answered.
    Result r = g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 0, false);
    CHECK(r.gotCsw);
    CHECK_EQ(int(r.status), 0);
}

TEST(abort_drops_the_queued_segments) {
    // Review F2: after abort() nothing stays queued (or armed) into the data buffer.
    Rig g;
    ready(g);
    uint8_t c[31];
    Rig::cbwBytes(c, 41, 16 * 512, false, Rig::rw10(op::kWrite10, 0, 16));
    g.sendRaw(c, 31);
    g.step();
    CHECK(!g.ep.rx.empty());
    CHECK(g.bot.busy());
    g.bot.abort();
    CHECK(g.ep.rx.empty());
    CHECK(g.ep.tx.empty());
    CHECK(!g.bot.busy());
}

TEST(host_watch_aborts_a_command_when_the_computer_is_gone) {
    // Review B #3: the cable is pulled mid-WRITE (no bus reset, frames stop).
    // After 500 ms without frames the main loop aborts the command: the unit
    // is no longer busy (a card can be re-presented, exit is immediate).
    Rig g;
    ready(g);
    std::vector<uint8_t> d(1024, 0x5C);
    uint8_t c[31];
    Rig::cbwBytes(c, 51, 16 * 512, false, Rig::rw10(op::kWrite10, 0, 16));
    g.sendRaw(c, 31);
    for (uint32_t off = 0; off < d.size(); off += 64)
        for (int i = 0; i < 10 && g.ep.hostOut(d.data() + off, 64) != FakeEp::kAck; ++i) g.step();
    g.step();
    CHECK(g.bot.busy());
    msc::HostWatch w;
    // Frames still come: never aborts.
    for (uint32_t t = 0; t < 2000; t += 10) CHECK(!w.update(true, g.bot.busy(), t));
    // Frames stop at t = 2000.
    bool fired = false;
    uint32_t firedAt = 0;
    for (uint32_t t = 2000; t < 4000 && !fired; t += 10) {
        g.step();
        if (w.update(false, g.bot.busy(), t)) {
            fired = true;
            firedAt = t;
            g.bot.abort();
            while (g.bot.ioPending()) g.bot.runIo(g.card);
        }
    }
    CHECK(fired);
    CHECK(firedAt >= 2500 && firedAt <= 2510);
    CHECK(!g.bot.busy());
    CHECK(g.ep.rx.empty());
    // Once per absence; idle units are left alone.
    CHECK(!w.update(false, g.bot.busy(), 5000));
    msc::HostWatch idle;
    CHECK(!idle.update(false, false, 0));
    CHECK(!idle.update(false, false, 10000));
    // Plugged in again: the host's reset recovery brings the unit back.
    CHECK(!w.update(true, false, 6000));
    g.ep.rx.clear();
    g.ep.tx.clear();
    g.bot.onReset();
    g.ep.haltIn = g.ep.haltOut = false;
    Result r = g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 0, false);
    CHECK_EQ(int(r.status), 0);
}

TEST(ring_of_four_chunks_read_ahead_and_write_ahead) {
    // Review B #6: with a 16 KB buffer the stream keeps 4 chunks (16 KB) in
    // flight: 4 card reads ahead of a host that does not read yet, and 16 KB
    // of host data taken while the card side is busy elsewhere.
    Rig g(16384);
    ready(g);
    uint8_t c[31];
    Rig::cbwBytes(c, 61, 64 * 512, true, Rig::rw10(op::kRead10, 100, 64));
    g.sendRaw(c, 31);
    for (int i = 0; i < 50; ++i) g.step();
    CHECK_EQ(g.card.reads, 4);
    CHECK_EQ(g.ep.tx.size(), size_t(4));
    std::vector<uint8_t> got;
    for (int i = 0; i < 100000 && got.size() < 64 * 512; ++i) {
        uint32_t n = 0;
        if (g.ep.hostIn(got, &n) != FakeEp::kAck) g.step();
    }
    CHECK(got.size() == 64 * 512 && std::memcmp(got.data(), &g.card.data[100 * 512], 64 * 512) == 0);
    std::vector<uint8_t> csw;
    uint32_t n = 0;
    for (int i = 0; i < 100 && n == 0; ++i) {
        g.step();
        g.ep.hostIn(csw, &n);
    }
    CHECK_EQ(n, 13u);
    CHECK_EQ(g.card.reads, 8);
    g.step();
    // WRITE with the main loop away: the host gets 16 KB in before a NAK.
    g.runIo = false;
    std::vector<uint8_t> d(64 * 512);
    for (size_t i = 0; i < d.size(); ++i) d[i] = uint8_t(i * 5 + 3);
    Rig::cbwBytes(c, 62, uint32_t(d.size()), false, Rig::rw10(op::kWrite10, 300, 64));
    g.sendRaw(c, 31);
    g.step();
    uint32_t off = 0;
    while (off < d.size() && g.ep.hostOut(d.data() + off, 64) == FakeEp::kAck) off += 64;
    CHECK_EQ(off, 16384u);
    g.runIo = true;
    for (int i = 0; i < 100000 && off < d.size(); ++i) {
        if (g.ep.hostOut(d.data() + off, 64) == FakeEp::kAck) off += 64;
        else g.step();
    }
    n = 0;
    csw.clear();
    for (int i = 0; i < 100 && n == 0; ++i) {
        g.step();
        g.ep.hostIn(csw, &n);
    }
    CHECK_EQ(n, 13u);
    if (n == 13) CHECK_EQ(int(csw[12]), 0);
    CHECK(std::memcmp(&g.card.data[300 * 512], d.data(), d.size()) == 0);
    CHECK_EQ(g.card.writes, 8);
}

TEST(medium_change_polling_is_standard) {
    // Review F5: the sequence Linux' TEST UNIT READY polling (and Windows /
    // macOS) uses to notice the card. REPORT LUNS does not take the UNIT
    // ATTENTION (SPC-3 5.9.7), the next TEST UNIT READY does.
    Rig g;
    Result r = g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 0, false);
    CHECK_EQ(int(r.status), 1);
    Sense s = g.requestSense();
    CHECK_EQ(int(s.key), int(sense::kNotReady));
    CHECK_EQ(int(s.asc), int(sense::kAscNoMedium));
    CHECK_EQ(int(s.ascq), 0);
    g.present();
    r = g.cmd({op::kReportLuns, 0, 0, 0, 0, 0, 0, 0, 0, 16, 0, 0}, 16, true);
    CHECK_EQ(int(r.status), 1);
    CHECK_EQ(int(g.requestSense().key), int(sense::kIllegalRequest));
    r = g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 0, false);
    CHECK_EQ(int(r.status), 1);
    CHECK_EQ(r.residue, 0u);
    s = g.requestSense();
    CHECK_EQ(int(s.key), int(sense::kUnitAttention));
    CHECK_EQ(int(s.asc), int(sense::kAscMediumChanged));
    CHECK_EQ(int(s.ascq), 0);
    r = g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 0, false);
    CHECK_EQ(int(r.status), 0);
    // The fixed-format sense data itself.
    g.bot.setMedium(false, 0);
    g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 0, false);
    r = g.cmd({op::kRequestSense, 0, 0, 0, 18, 0}, 18, true);
    CHECK_EQ(r.data.size(), size_t(18));
    CHECK_EQ(int(r.data[0]), 0x70);
    CHECK_EQ(int(r.data[2]), int(sense::kNotReady));
    CHECK_EQ(int(r.data[7]), 10);
    CHECK_EQ(int(r.data[12]), int(sense::kAscNoMedium));
}

TEST(read_capacity16) {
    Rig g;
    // No medium: NOT READY, IN halted, nothing sent.
    Result r = g.cmd({op::kServiceActionIn16, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0}, 32, true);
    CHECK_EQ(int(r.status), 1);
    CHECK(r.data.empty());
    CHECK_EQ(int(g.requestSense().asc), int(sense::kAscNoMedium));
    ready(g);
    r = g.cmd({op::kServiceActionIn16, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0}, 32, true);
    CHECK_EQ(int(r.status), 0);
    CHECK_EQ(r.data.size(), size_t(32));
    CHECK_EQ(be32(r.data.data()), 0u);
    CHECK_EQ(be32(r.data.data() + 4), 2047u);
    CHECK_EQ(be32(r.data.data() + 8), 512u);
    CHECK(!r.stalledIn);
    CHECK(g.bot.hostAccessed());
    // Another SERVICE ACTION IN service action: invalid field.
    r = g.cmd({op::kServiceActionIn16, 0x12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0}, 32, true);
    CHECK_EQ(int(r.status), 1);
    CHECK_EQ(int(g.requestSense().asc), int(sense::kAscInvalidField));
}

// What udev / udisks / smartctl / sg tools send to a USB disk besides the sd
// driver: every one ends at once with CHECK CONDITION and ILLEGAL REQUEST
// (no phase error, no halted pipe left behind, no card I/O), and the unit
// answers the next command normally. A phase error or an invalid CSW is what
// would make Linux' usb-storage do a reset recovery (and log "reset full-speed
// USB device").
TEST(unsupported_commands_fail_fast_with_illegal_request) {
    struct Probe {
        const char* what;
        std::vector<uint8_t> cdb;
        uint32_t len;
        bool in;
        uint8_t asc;
    };
    const std::vector<Probe> probes = {
        {"ATA PASS-THROUGH(16) IDENTIFY", {0x85, 0x08, 0x0E, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0xEC, 0}, 512, true,
         sense::kAscInvalidOpcode},
        {"ATA PASS-THROUGH(12) IDENTIFY", {0xA1, 0x08, 0x0E, 0, 1, 0, 0, 0, 0, 0xEC, 0, 0}, 512, true,
         sense::kAscInvalidOpcode},
        {"ATA PASS-THROUGH(16) no data", {0x85, 0x06, 0x20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xE5, 0}, 0, false,
         sense::kAscInvalidOpcode},
        {"REPORT SUPPORTED OPCODES", {0xA3, 0x0C, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0}, 512, true, sense::kAscInvalidOpcode},
        {"SYNCHRONIZE CACHE(16)", {0x91, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, false,
         sense::kAscInvalidOpcode},
        {"REPORT LUNS", {op::kReportLuns, 0, 0, 0, 0, 0, 0, 0, 0, 16, 0, 0}, 16, true, sense::kAscInvalidOpcode},
        {"INQUIRY VPD 80h", {op::kInquiry, 1, 0x80, 0, 255, 0}, 255, true, sense::kAscInvalidField},
        {"INQUIRY VPD 83h", {op::kInquiry, 1, 0x83, 0, 255, 0}, 255, true, sense::kAscInvalidField},
        {"LOG SENSE", {0x4D, 0, 0x40, 0, 0, 0, 0, 0x02, 0, 0}, 512, true, sense::kAscInvalidOpcode},
        {"WRITE SAME(10)", {0x41, 0, 0, 0, 0, 0, 0, 0, 1, 0}, 512, false, sense::kAscInvalidOpcode},
        {"GET EVENT STATUS", {0x4A, 1, 0, 0, 0x10, 0, 0, 0, 8, 0}, 8, true, sense::kAscInvalidOpcode},
    };
    Rig g;
    ready(g);
    for (const Probe& p : probes) {
        Result r = g.cmd(p.cdb, p.len, p.in, std::vector<uint8_t>(p.len, 0));
        if (!r.gotCsw || r.status != 1) std::printf("  probe: %s\n", p.what);
        CHECK(r.gotCsw);
        CHECK_EQ(int(r.status), 1);
        CHECK_EQ(r.residue, p.len);
        CHECK(r.data.empty());
        CHECK(!g.ep.haltIn && !g.ep.haltOut);   // the host's one CLEAR_FEATURE left nothing halted
        const Sense s = g.requestSense();
        CHECK_EQ(int(s.key), int(sense::kIllegalRequest));
        CHECK_EQ(int(s.asc), int(p.asc));
        CHECK_EQ(int(s.ascq), 0);
        r = g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 0, false);
        CHECK_EQ(int(r.status), 0);
    }
    CHECK_EQ(g.card.reads + g.card.writes, 0);
    CHECK_EQ(g.bot.phaseErrors(), 0u);
    CHECK(g.bot.state() != Bot::kHalted);
}

TEST(session_statistics_and_error_breakdown) {
    Rig g;
    // Outside the mode: polls fail with NOT READY ...
    for (int i = 0; i < 5; ++i) g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 0, false);
    CHECK_EQ(g.bot.errors(), 5u);
    // ... and presenting the card starts a new session from zero.
    g.bot.setReadRegions(32, 32 + 64, 32 + 128);   // reserved 0-31, FAT1 32-95, FAT2 96-159, data 160-
    g.present();
    CHECK_EQ(g.bot.errors(), 0u);
    CHECK_EQ(g.bot.commands(), 0u);
    g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 0, false);   // UNIT ATTENTION
    g.cmd({op::kRequestSense, 0, 0, 0, 18, 0}, 18, true);
    for (int i = 0; i < 3; ++i) g.cmd({0x85, 0x08, 0x0E, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0xEC, 0}, 512, true);
    g.cmd({op::kInquiry, 1, 0x80, 0, 255, 0}, 255, true);
    g.cmd(Rig::rw10(op::kRead10, 0, 1), 512, true);       // boot sector
    g.cmd(Rig::rw10(op::kRead10, 1, 1), 512, true);       // FSINFO
    g.cmd(Rig::rw10(op::kRead10, 32, 64), 64 * 512, true);   // the whole first FAT
    g.cmd(Rig::rw10(op::kRead10, 150, 20), 20 * 512, true);  // FAT2 tail + data
    g.cmd(Rig::rw10(op::kRead10, 2047, 2), 1024, true);   // out of range
    g.cmd(Rig::rw10(op::kRead10, 0, 2), 512, true);       // phase error (case 7)
    CHECK_EQ(g.bot.errors(), 6u);
    CHECK_EQ(g.bot.phaseErrors(), 1u);
    CHECK_EQ(g.bot.readCommands(), 4u);
    CHECK_EQ(g.bot.smallReads(), 2u);
    CHECK_EQ(g.bot.readBlocks(kRegionReserved), 2u);
    CHECK_EQ(g.bot.readBlocks(kRegionFat1), 64u);
    CHECK_EQ(g.bot.readBlocks(kRegionFat2), 10u);
    CHECK_EQ(g.bot.readBlocks(kRegionData), 10u);
    ErrorKind k[Bot::kErrorKinds];
    const int n = g.bot.errorKinds(k);
    CHECK_EQ(n, 4);
    CHECK_EQ(int(k[0].op), 0x85);
    CHECK_EQ(int(k[0].count), 3);
    char line[160];
    g.bot.formatErrors(line, sizeof line);
    CHECK(std::strcmp(line, "85 5/20/00 x3, 00 6/28/00 x1, 12 5/24/00 x1, 28 5/21/00 x1") == 0);
    if (std::strcmp(line, "85 5/20/00 x3, 00 6/28/00 x1, 12 5/24/00 x1, 28 5/21/00 x1") != 0)
        std::printf("  got: %s\n", line);
    // More than 8 kinds: the rest are counted as "other"; a short buffer is cut cleanly.
    for (uint8_t opc = 0xC0; opc < 0xC8; ++opc) g.cmd({opc, 0, 0, 0, 0, 0}, 0, false);
    CHECK_EQ(g.bot.errorKinds(k), Bot::kErrorKinds);
    CHECK_EQ(g.bot.otherErrors(), 4u);
    g.bot.formatErrors(line, sizeof line);
    CHECK(std::strstr(line, ", other x4") != nullptr);
    char tiny[10];
    CHECK_EQ(g.bot.formatErrors(tiny, sizeof tiny), 9u);
    CHECK_EQ(std::strlen(tiny), size_t(9));
    // A session without a FAT layout: every read counts as data.
    g.bot.setReadRegions(0, 0, 0);
    g.present();
    g.cmd({op::kTestUnitReady, 0, 0, 0, 0, 0}, 0, false);
    g.cmd(Rig::rw10(op::kRead10, 0, 4), 2048, true);
    CHECK_EQ(g.bot.readBlocks(kRegionData), 4u);
    CHECK_EQ(g.bot.readBlocks(kRegionReserved), 0u);
}
