// OpenTracker 2.x -- USB mass storage, Bulk-Only Transport + SCSI (see msc_bot.h).
//
// Data-stage cases follow BOT 1.0 §6.7 ("The Thirteen Cases"): the device
// sends or takes at most what both sides expect, halts the bulk IN pipe when
// it sent less than the host asked for, receives and discards host data it
// does not want, and answers a direction / length conflict with a phase error
// (status 2). An invalid CBW halts both pipes until reset recovery (§6.6.1).
#include "usb/msc_bot.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace ot {
namespace msc {

namespace {
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]); }
uint32_t be16(const uint8_t* p) { return uint32_t(p[0]) << 8 | uint32_t(p[1]); }
void putLe32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }
void putBe32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
void fence() { std::atomic_signal_fence(std::memory_order_seq_cst); }

// SPC-2 standard INQUIRY data (36 bytes): direct-access block device,
// removable medium. Vendor / product / revision are OpenTracker's own.
const uint8_t kInquiry[36] = {
    0x00, 0x80, 0x04, 0x02, 31, 0x00, 0x00, 0x00,
    'O', 'p', 'e', 'n', 'T', 'r', 'k', 'r',
    'T', 'r', 'a', 'c', 'k', 'e', 'r', ' ', 'S', 'D', ' ', 'c', 'a', 'r', 'd', ' ',
    '2', '.', '0', ' ',
};
}  // namespace

void Bot::init(IEndpoints* ep, uint8_t* buf, uint32_t bufBytes, uint8_t* small) {
    ep_ = ep;
    setDataBuffer(buf, bufBytes);
    cbw_ = small;
    csw_ = small + 64;
    reply_ = small + 80;
    state_ = kIdle;
    io_.state = kIoIdle;
}

void Bot::setDataBuffer(uint8_t* buf, uint32_t bufBytes) {
    buf_ = buf;
    ring_ = buf ? bufBytes / kChunk : 0;
    if (ring_ > kMaxChunks) ring_ = kMaxChunks;
}

bool Bot::busy() const {
    const State s = *reinterpret_cast<const volatile State*>(&state_);
    return (s != kIdle && s != kWaitCbw && s != kHalted) || io_.state == kIoPending || rxData_ != 0;
}

void Bot::setMedium(bool present, uint32_t blocks) {
    if (present && !ring_) return;   // no data buffer: stays "not present"
    if (present) {
        blocks_ = blocks;
        ejected_ = false;
        accessed_ = false;
        prevent_ = false;
        resetStats();
        unitAttention_ = true;   // SPC-2: "not ready to ready change, medium may have changed"
        fence();
        present_ = true;
    } else {
        present_ = false;
        unitAttention_ = false;
    }
}

bool Bot::runIo(IBlockDevice& dev) {
    if (io_.state != kIoPending) return false;
    fence();
    const bool ok = io_.write ? dev.write(io_.lba, io_.buf, io_.count) : dev.read(io_.lba, io_.buf, io_.count);
    fence();
    io_.state = ok ? kIoOk : kIoFail;
    return true;
}

void Bot::onReset() {
    // Reset recovery / bus reset / new configuration: drop the command. Block
    // I/O already handed to the main loop finishes there; its result is dropped.
    if (io_.state != kIoPending) io_.state = kIoIdle;
    else staleIo_ = true;
    rxData_ = 0;   // the driver dropped its queues
    state_ = kIdle;
}

void Bot::abort() {
    if (io_.state == kIoPending) staleIo_ = true;
    else io_.state = kIoIdle;
    if (ep_) {
        ep_->stall(true);
        ep_->stall(false);
        // Halted first: the host moves nothing, so the queues can go (no
        // segment may stay armed into the data buffer, review F2).
        ep_->rxFlush();
        ep_->txFlush();
    }
    rxData_ = 0;
    state_ = kHalted;
}

void Bot::service() {
    if (!ep_) return;
    if (staleIo_) {
        // A block request of a dropped command (reset / abort) still runs in
        // the main loop and owns its buffer: no new command may start (its
        // data would share that buffer) until the request is done.
        if (io_.state == kIoPending) return;
        io_.state = kIoIdle;
        staleIo_ = false;
    }
    // A few passes so that one call carries a command as far as it can go
    // (CSW sent -> next CBW queued, CBW -> reply queued).
    for (int pass = 0; pass < 4; ++pass) {
        const State before = state_;
        switch (state_) {
        case kIdle:
            if (ep_->configured() && ep_->rxQueue(cbw_, 64)) state_ = kWaitCbw;
            break;
        case kWaitCbw: {
            uint32_t got = 0;
            if (ep_->rxPoll(&got)) startCommand(got);
            break;
        }
        case kDataIn:
            if (ep_->txPoll()) finish();
            break;
        case kReadStream: serviceRead(); break;
        case kWriteStream: serviceWrite(); break;
        case kWaitUnstall:
            if ((!stallIn_ || !ep_->stalled(true)) && (!stallOut_ || !ep_->stalled(false))) sendCsw();
            break;
        case kSendCsw:
            if (cswQueued_) {
                if (ep_->txPoll()) state_ = kIdle;
            } else {
                sendCsw();
            }
            break;
        case kHalted:
            // BOT 6.6.1: both pipes stay halted until a Bulk-Only Mass Storage Reset.
            if (!ep_->stalled(true)) ep_->stall(true);
            if (!ep_->stalled(false)) ep_->stall(false);
            break;
        }
        if (state_ == before) break;
    }
}

void Bot::startCommand(uint32_t got) {
    ++commands_;
    if (got != kCbwLen || le32(cbw_) != kCbwSig) {
        // BOT 6.6.1: not a valid CBW.
        ++errors_;
        ep_->stall(true);
        ep_->stall(false);
        state_ = kHalted;
        return;
    }
    tag_ = le32(cbw_ + 4);
    hostLen_ = le32(cbw_ + 8);
    hostIn_ = (cbw_[12] & 0x80) != 0;
    const uint8_t lun = cbw_[13] & 0x0F;
    uint8_t len = cbw_[14] & 0x1F;
    if (len > 16) len = 16;
    std::memset(cb_, 0, sizeof cb_);
    std::memcpy(cb_, cbw_ + 15, len);
    status_ = kStatusPassed;
    residue_ = hostLen_;
    processed_ = 0;
    xferred_ = 0;
    stallIn_ = stallOut_ = false;
    cswQueued_ = false;
    outDirection_ = !hostIn_;
    if (lun != 0 || len == 0) {   // CBW valid but not meaningful (BOT 6.2.2)
        fail(sense::kIllegalRequest, sense::kAscInvalidField);
        noData();
        return;
    }
    execute();
}

void Bot::fail(uint8_t key, uint8_t asc, uint8_t ascq) {
    if (status_ == kStatusPassed) status_ = kStatusFailed;
    sense_.key = key;
    sense_.asc = asc;
    sense_.ascq = ascq;
    ++errors_;
    const uint8_t opc = cb_[0];
    for (ErrorKind& k : kinds_) {
        if (k.count && (k.op != opc || k.key != key || k.asc != asc || k.ascq != ascq)) continue;
        if (!k.count) k = ErrorKind{opc, key, asc, ascq, 0};
        if (k.count != 0xFFFF) ++k.count;
        return;
    }
    ++otherErrors_;
}

void Bot::resetStats() {
    commands_ = bytesRead_ = bytesWritten_ = errors_ = 0;
    phaseErrors_ = readCmds_ = smallReads_ = otherErrors_ = 0;
    for (ErrorKind& k : kinds_) k = ErrorKind{};
    for (uint32_t& b : regionBlocks_) b = 0;
}

int Bot::errorKinds(ErrorKind* out) const {
    int n = 0;
    for (const ErrorKind& k : kinds_)
        if (k.count) out[n++] = k;
    for (int i = 1; i < n; ++i)   // most frequent first (stable)
        for (int j = i; j > 0 && out[j].count > out[j - 1].count; --j) {
            const ErrorKind t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return n;
}

uint32_t Bot::formatErrors(char* out, uint32_t n) const {
    if (!n) return 0;
    out[0] = 0;
    ErrorKind k[kErrorKinds];
    const int kinds = errorKinds(k);
    uint32_t len = 0;
    for (int i = 0; i < kinds && len < n; ++i) {
        const int w = std::snprintf(out + len, n - len, "%s%02X %X/%02X/%02X x%u", i ? ", " : "", k[i].op, k[i].key,
                                    k[i].asc, k[i].ascq, unsigned(k[i].count));
        if (w < 0) break;
        len += uint32_t(w);
    }
    if (otherErrors_ && len < n) {
        const int w = std::snprintf(out + len, n - len, ", other x%lu", static_cast<unsigned long>(otherErrors_));
        if (w > 0) len += uint32_t(w);
    }
    return len < n ? len : n - 1;
}

void Bot::setReadRegions(uint32_t fatStart, uint32_t fat2Start, uint32_t dataStart) {
    regionStart_[kRegionReserved] = 0;
    regionStart_[kRegionFat1] = fatStart;
    regionStart_[kRegionFat2] = fat2Start;
    regionStart_[kRegionData] = dataStart;
}

void Bot::countRead(uint32_t lba, uint32_t n) {
    ++readCmds_;
    if (n <= kChunk / kBlock) ++smallReads_;
    if (!regionStart_[kRegionData]) {   // no FAT layout known: all "data"
        regionBlocks_[kRegionData] += n;
        return;
    }
    // Split [lba, lba + n) over the regions (bounds ascending).
    for (int r = kRegions - 1; r >= 0 && n; --r) {
        const uint32_t start = regionStart_[r];
        const uint32_t end = lba + n;
        if (end <= start) continue;
        const uint32_t from = lba > start ? lba : start;
        regionBlocks_[r] += end - from;
        n = from - lba;
    }
}

bool Bot::unitReady() {
    if (!mediaOk()) {
        fail(sense::kNotReady, sense::kAscNoMedium);
        return false;
    }
    return true;
}

void Bot::execute() {
    const uint8_t opc = cb_[0];
    if (opc != op::kRequestSense) sense_ = Sense{};
    // SPC-2 5.6.5 / SPC-3 5.9.7: a pending UNIT ATTENTION fails every command
    // except INQUIRY, REQUEST SENSE and REPORT LUNS, once, with CHECK
    // CONDITION and sense 06h / 28h / 00h ("not ready to ready change, medium
    // may have changed"). This is what Linux' TEST UNIT READY polling
    // (sd_check_events, udisks) and Windows / macOS look for to notice a new
    // medium: TUR -> CHECK CONDITION, REQUEST SENSE -> 06/28/00, TUR -> GOOD.
    // No medium: TUR -> CHECK CONDITION, sense 02h / 3Ah / 00h.
    if (opc != op::kInquiry && opc != op::kRequestSense && opc != op::kReportLuns && unitAttention_ && mediaOk()) {
        unitAttention_ = false;
        fail(sense::kUnitAttention, sense::kAscMediumChanged);
        if (opc == op::kWrite10) dataOut(be16(cb_ + 7) * kBlock, false);
        else noData();
        return;
    }
    switch (opc) {
    case op::kTestUnitReady:
        unitReady();
        noData();
        return;
    case op::kRequestSense: {
        Sense s = sense_;
        if (s.key == sense::kNoSense && !mediaOk()) s = Sense{sense::kNotReady, sense::kAscNoMedium, 0};
        sense_ = Sense{};
        uint8_t* r = reply_;
        std::memset(r, 0, 18);
        r[0] = 0x70;   // current error, fixed format
        r[2] = s.key;
        r[7] = 10;     // additional sense length
        r[12] = s.asc;
        r[13] = s.ascq;
        const uint32_t alloc = cb_[4];
        replyIn(alloc < 18 ? alloc : 18);
        return;
    }
    case op::kInquiry: {
        if (cb_[1] & 0x01) {   // EVPD: no vital product data pages
            fail(sense::kIllegalRequest, sense::kAscInvalidField);
            replyIn(0);
            return;
        }
        std::memcpy(reply_, kInquiry, sizeof kInquiry);
        const uint32_t alloc = be16(cb_ + 3);
        replyIn(alloc < sizeof kInquiry ? alloc : sizeof kInquiry);
        return;
    }
    case op::kReadCapacity10: {
        if (!unitReady()) {
            replyIn(0);
            return;
        }
        accessed_ = true;
        putBe32(reply_, blocks_ ? blocks_ - 1 : 0);
        putBe32(reply_ + 4, kBlock);
        replyIn(8);
        return;
    }
    case op::kReadFormatCapacities: {
        uint8_t* r = reply_;
        std::memset(r, 0, 12);
        r[3] = 8;   // capacity list length
        putBe32(r + 4, mediaOk() ? blocks_ : 0);
        r[8] = mediaOk() ? 0x02 : 0x03;   // formatted media / no media present
        r[10] = uint8_t(kBlock >> 8);
        r[11] = uint8_t(kBlock & 0xFF);
        const uint32_t alloc = be16(cb_ + 7);
        replyIn(alloc < 12 ? alloc : 12);
        return;
    }
    case op::kModeSense6:
    case op::kModeSense10: {
        // No block descriptors, not write protected. Page 08h (caching,
        // SBC-2 6.3.3) alone or within "all pages" (3Fh): WCE = 0 (a WRITE's
        // data is on the card before its CSW), read cache enabled. Linux'
        // usb-storage asks MODE SENSE(6) 3Fh with 192 bytes for the write-
        // protect flag and the caching page (sd: "Write cache: disabled"
        // instead of "Assuming drive cache: write through"). Any other page:
        // header only, as before (hosts ask for pages they can do without).
        // Saved values (PC = 3) are not supported (SPC-3 6.9).
        const bool ten = opc == op::kModeSense10;
        const uint8_t pc = cb_[2] >> 6, page = cb_[2] & 0x3F;
        if (pc == 3) {
            fail(sense::kIllegalRequest, sense::kAscSavingNotSupported);
            replyIn(0);
            return;
        }
        const uint32_t hdr = ten ? 8 : 4;
        uint32_t len = hdr;
        std::memset(reply_, 0, hdr + 20);
        if (page == 0x08 || page == 0x3F) {
            uint8_t* p = reply_ + hdr;
            p[0] = 0x08;   // caching page, not savable
            p[1] = 0x12;   // page length 18; every field 0 (current = default = changeable mask)
            len += 20;
        }
        if (ten) reply_[1] = uint8_t(len - 2);
        else reply_[0] = uint8_t(len - 1);
        const uint32_t alloc = ten ? be16(cb_ + 7) : cb_[4];
        replyIn(alloc < len ? alloc : len);
        return;
    }
    case op::kServiceActionIn16: {
        // READ CAPACITY(16) (SBC-2 5.11): Linux tries it only when READ
        // CAPACITY(10) says FFFFFFFFh (> 2 TB) or for a SPC-3 device; cheap
        // to answer right. Other service actions: invalid field.
        if ((cb_[1] & 0x1F) != op::kSaReadCapacity16) {
            fail(sense::kIllegalRequest, sense::kAscInvalidField);
            replyIn(0);
            return;
        }
        if (!unitReady()) {
            replyIn(0);
            return;
        }
        accessed_ = true;
        std::memset(reply_, 0, 32);
        putBe32(reply_ + 4, blocks_ ? blocks_ - 1 : 0);   // 64-bit last LBA, high word 0
        putBe32(reply_ + 8, kBlock);
        const uint32_t alloc = be32(cb_ + 10);
        replyIn(alloc < 32 ? alloc : 32);
        return;
    }
    case op::kPreventAllow:
        prevent_ = (cb_[4] & 0x01) != 0;
        noData();
        return;
    case op::kStartStopUnit:
        // LoEj with Start = 0 ejects (the computer's "Eject"), with Start = 1 loads.
        if (cb_[4] & 0x02) ejected_ = (cb_[4] & 0x01) == 0;
        noData();
        return;
    case op::kSyncCache10:
        unitReady();   // writes reach the card before their CSW: nothing to flush
        noData();
        return;
    case op::kVerify10:
        if (unitReady() && (cb_[1] & 0x02)) fail(sense::kIllegalRequest, sense::kAscInvalidField);   // BYTCHK
        noData();
        return;
    case op::kRead10:
    case op::kWrite10: {
        const bool write = opc == op::kWrite10;
        const uint32_t lba = be32(cb_ + 2), n = be16(cb_ + 7);
        bool ok = unitReady();
        if (ok && (lba > blocks_ || n > blocks_ - lba)) {
            fail(sense::kIllegalRequest, sense::kAscLbaOutOfRange);
            ok = false;
        }
        if (ok) accessed_ = true;
        lba_ = lba;
        if (n == 0) {
            noData();
        } else if (write) {
            dataOut(n * kBlock, ok);
        } else if (ok) {
            dataIn(n * kBlock);
        } else {
            replyIn(0);
        }
        return;
    }
    default:
        fail(sense::kIllegalRequest, sense::kAscInvalidOpcode);
        noData();
        return;
    }
}

void Bot::noData() {
    if (hostLen_ == 0) {   // case 1
        finish();
    } else if (hostIn_) {  // case 4: Hi > Dn -> halt IN
        finish();
    } else {               // case 9: Ho > Dn -> take the data and drop it
        startWriteStream(hostLen_, 0);
    }
}

void Bot::replyIn(uint32_t len) {
    if (len == 0) {
        noData();
        return;
    }
    if (hostLen_ == 0) {   // case 2: Hn < Di
        status_ = kStatusPhase;
        finish();
        return;
    }
    if (!hostIn_) {        // case 10: Ho <> Di
        status_ = kStatusPhase;
        stallOut_ = true;
        ep_->stall(false);
        finish();
        return;
    }
    if (len > hostLen_) {  // case 7: Hi < Di
        status_ = kStatusPhase;
        len = hostLen_;
    }
    if (!ep_->txQueue(reply_, len)) {   // cannot happen: the IN queue is empty between commands
        status_ = kStatusPhase;
        finish();
        return;
    }
    xferred_ = len;
    state_ = kDataIn;
}

void Bot::dataIn(uint32_t bytes) {
    if (hostLen_ == 0 || !hostIn_ || bytes > hostLen_) {
        // cases 2, 10, 7
        status_ = kStatusPhase;
        if (hostLen_ && !hostIn_) {
            stallOut_ = true;
            ep_->stall(false);
        }
        finish();
        return;
    }
    countRead(lba_, bytes / kBlock);
    total_ = bytes;
    chunk_ = kChunk;
    chunks_ = (bytes + chunk_ - 1) / chunk_;
    issued_ = ready_ = sent_ = 0;
    ioError_ = false;
    state_ = kReadStream;
    serviceRead();
}

void Bot::dataOut(uint32_t bytes, bool write) {
    if (hostLen_ == 0) {   // case 3: Hn < Do
        status_ = kStatusPhase;
        finish();
        return;
    }
    if (hostIn_) {         // case 8: Hi <> Do
        status_ = kStatusPhase;
        finish();
        return;
    }
    if (bytes > hostLen_) {   // case 13: Ho < Do -> take what comes, drop it
        status_ = kStatusPhase;
        write = false;
    }
    startWriteStream(hostLen_, write ? bytes : 0);
}

void Bot::startWriteStream(uint32_t total, uint32_t writeBytes) {
    total_ = total;
    writeBytes_ = writeBytes;
    // Data for the card goes through the ring chunks of buf_; data that is
    // only dropped never touches buf_ (it may belong to the app outside
    // storage mode) and comes in 64-byte pieces into reply_.
    chunk_ = writeBytes ? kChunk : 64;
    chunks_ = (total + chunk_ - 1) / chunk_;
    issued_ = ready_ = sent_ = 0;
    ioError_ = false;
    state_ = kWriteStream;
    serviceWrite();
}

uint32_t Bot::chunkBytes(uint32_t k, uint32_t total) const {
    const uint32_t off = k * chunk_;
    return total - off < chunk_ ? total - off : chunk_;
}

void Bot::serviceRead() {
    // USB side: chunks sent.
    while (sent_ < ready_ && ep_->txPoll()) {
        xferred_ += chunkBytes(sent_, total_);
        ++sent_;
    }
    // Card side: a finished read goes out over USB.
    const uint8_t io = io_.state;
    if (io == kIoOk || io == kIoFail) {
        if (io == kIoFail) {
            ioError_ = true;
            io_.state = kIoIdle;
        } else if (ep_->txQueue(chunkBuf(ready_), chunkBytes(ready_, total_))) {
            bytesRead_ += chunkBytes(ready_, total_);
            ++ready_;
            io_.state = kIoIdle;
        }
    }
    // Next read into a free chunk (chunk k reuses the slot of chunk k - ring_).
    if (io_.state == kIoIdle && !ioError_ && issued_ < chunks_ && issued_ < sent_ + ring_) {
        const uint32_t k = issued_++;
        io_.write = false;
        io_.lba = lba_ + k * (chunk_ / kBlock);
        io_.count = chunkBytes(k, total_) / kBlock;
        io_.buf = chunkBuf(k);
        fence();
        io_.state = kIoPending;
    }
    if (sent_ == ready_ && io_.state == kIoIdle && (sent_ == chunks_ || ioError_)) {
        if (ioError_) fail(sense::kMediumError, sense::kAscUnrecoveredRead);
        finish();
    }
}

void Bot::serviceWrite() {
    // USB side: queue receives into free chunks, collect finished ones.
    // Discarded data (64-byte pieces into reply_) keeps 2 in flight.
    const uint32_t ahead = chunk_ == kChunk ? ring_ : 2;
    while (issued_ < chunks_ && issued_ < sent_ + ahead &&
           ep_->rxQueue(chunkBuf(issued_), chunkBytes(issued_, total_))) {
        ++issued_;
        ++rxData_;
    }
    uint32_t got = 0;
    while (ready_ < issued_ && ep_->rxPoll(&got)) {
        --rxData_;
        xferred_ += got;
        if (got < chunkBytes(ready_, total_)) {
            // Short packet: the host ended the transfer early (review F1).
            // Only the whole blocks it sent go to the card, and the
            // segments queued behind this one are dropped: one could still
            // be armed into this chunk, and the next CBW must not land in a
            // data chunk. The host sends nothing more before the CSW.
            status_ = kStatusPhase;
            const uint32_t have = ready_ * chunk_ + (got & ~(kBlock - 1));
            if (writeBytes_ > have) writeBytes_ = have;
            chunks_ = issued_ = ready_ + 1;
            ep_->rxFlush();
            rxData_ = 0;
        }
        ++ready_;
    }
    // Card side.
    const uint8_t io = io_.state;
    if (io == kIoOk || io == kIoFail) {
        if (io == kIoFail) ioError_ = true;
        else bytesWritten_ += io_.count * kBlock;
        io_.state = kIoIdle;
        ++sent_;
    }
    while (io_.state == kIoIdle && sent_ < ready_) {
        const uint32_t off = sent_ * chunk_;
        uint32_t n = 0;
        if (!ioError_ && off < writeBytes_) {
            n = writeBytes_ - off;
            if (n > chunk_) n = chunk_;
        }
        if (n == 0) {   // discarded data
            ++sent_;
            continue;
        }
        io_.write = true;
        io_.lba = lba_ + sent_ * (chunk_ / kBlock);
        io_.count = n / kBlock;
        io_.buf = chunkBuf(sent_);
        fence();
        io_.state = kIoPending;
    }
    if (sent_ == chunks_ && io_.state == kIoIdle) {
        if (ioError_) fail(sense::kMediumError, sense::kAscWriteFault);
        processed_ = writeBytes_ && !ioError_ ? writeBytes_ : 0;
        finish();
    }
}

void Bot::finish() {
    if (status_ == kStatusPhase) ++phaseErrors_;
    // Review F4: Hi > Di (case 4 / 5: e.g. INQUIRY or MODE SENSE with a larger
    // allocation length) ends with a short packet and then halts the IN pipe
    // (BOT 6.7.2 allows either; the Linux gadget stack does the same). The
    // host clears the halt and reads the CSW: one extra STALL + CLEAR_FEATURE
    // round trip per such command, handled by Linux, macOS and Windows. Small
    // stacks (TinyUSB, ST) send only the short packet; if a host is found
    // that does not cope, skip the stall when xferred_ % 64 != 0.
    if (outDirection_ && state_ == kWriteStream) {
        residue_ = hostLen_ - processed_;
    } else if (hostIn_) {
        residue_ = hostLen_ - xferred_;
        if (xferred_ < hostLen_ && !stallIn_) {   // less than asked for: halt IN (BOT 6.7.2)
            stallIn_ = true;
            ep_->stall(true);
        }
    } else {
        residue_ = hostLen_;
    }
    if (stallIn_ || stallOut_) {
        state_ = kWaitUnstall;
        return;
    }
    sendCsw();
}

void Bot::sendCsw() {
    putLe32(csw_, kCswSig);
    putLe32(csw_ + 4, tag_);
    putLe32(csw_ + 8, residue_);
    csw_[12] = status_;
    cswQueued_ = ep_->txQueue(csw_, kCswLen);
    state_ = kSendCsw;
}

}  // namespace msc
}  // namespace ot
