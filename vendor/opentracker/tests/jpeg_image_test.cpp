// IDisplay image API (spec 13 §G.6 / §I.3): the shared JPEG header probe,
// the .jpg/.jpeg base-name fallback, the default (no image support) and the
// host fake used by UI tests.
#include "test.h"

#include <string>
#include <vector>

#include "hal_api.h"
#include "ui_fakes.h"

using namespace ot;

namespace {

void be16(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}
// Header-only JPEG: SOI, optional APPn of `appLen` payload bytes, SOFn, EOI,
// padded with zeros to `total` bytes.
std::vector<uint8_t> jpeg(uint16_t w, uint16_t h, uint8_t sof = 0xC0, uint32_t appLen = 14, uint32_t total = 0) {
    std::vector<uint8_t> v = {0xFF, 0xD8};
    v.push_back(0xFF);
    v.push_back(0xE1);
    be16(v, appLen + 2);
    v.insert(v.end(), appLen, 0x11);
    v.push_back(0xFF);
    v.push_back(0xFF);   // fill byte before the marker
    v.push_back(sof);
    be16(v, 17);
    v.push_back(8);
    be16(v, h);
    be16(v, w);
    v.push_back(3);
    for (int c = 0; c < 3; ++c) { v.push_back(uint8_t(c + 1)); v.push_back(0x11); v.push_back(0); }
    v.push_back(0xFF);
    v.push_back(0xD9);
    if (total > v.size()) v.resize(total, 0);
    return v;
}

struct NullDisplay : IDisplay {
    uint16_t width() const override { return 800; }
    uint16_t height() const override { return 480; }
    void dlStart() override {}
    void clearColorRGB(uint8_t, uint8_t, uint8_t) override {}
    void clear(bool, bool, bool) override {}
    void colorRGB(uint8_t, uint8_t, uint8_t) override {}
    void colorA(uint8_t) override {}
    void lineWidth(uint16_t) override {}
    void pointSize(uint16_t) override {}
    void scissorXY(uint16_t, uint16_t) override {}
    void scissorSize(uint16_t, uint16_t) override {}
    void begin(Prim) override {}
    void end() override {}
    void vertex2f(int32_t, int32_t) override {}
    void vertex2ii(uint16_t, uint16_t, uint8_t, uint8_t) override {}
    void text(int16_t, int16_t, uint8_t, uint16_t, const char*) override {}
    void number(int16_t, int16_t, uint8_t, uint16_t, int32_t) override {}
    void swap() override {}
};

bool probe(FakeStorage& fs, const char* p, uint32_t dest, uint16_t mw, uint16_t mh, uint16_t& w, uint16_t& h) {
    uint32_t size = 0;
    return IDisplay::probeJpeg(fs, p, dest, mw, mh, w, h, size);
}

}  // namespace

TEST(jpeg_constants) {
    CHECK_EQ(IDisplay::kRamGCover, 670000u);                                  // spec 13 §I.3
    CHECK_EQ(IDisplay::kRamGImageLimit - IDisplay::kRamGCover, 330000u);      // max cover file
    CHECK(IDisplay::kRamGCover + 2u * 400u * 400u <= IDisplay::kRamGSize);
}

TEST(jpeg_probe_accepts_baseline) {
    FakeStorage fs;
    fs.files["a.jpg"] = jpeg(400, 300);
    fs.files["ext.jpg"] = jpeg(123, 45, 0xC1, 6000);   // extended sequential, big APP1
    uint16_t w = 0, h = 0;
    CHECK(probe(fs, "a.jpg", IDisplay::kRamGCover, 400, 400, w, h));
    CHECK_EQ(w, 400);
    CHECK_EQ(h, 300);
    CHECK(probe(fs, "ext.jpg", IDisplay::kRamGCover, 400, 400, w, h));
    CHECK_EQ(w, 123);
    CHECK_EQ(h, 45);
}

TEST(jpeg_probe_rejects) {
    FakeStorage fs;
    fs.files["prog.jpg"] = jpeg(64, 64, 0xC2);                       // progressive
    fs.files["wide.jpg"] = jpeg(401, 10);
    fs.files["tall.jpg"] = jpeg(10, 401);
    fs.files["zero.jpg"] = jpeg(0, 10);
    fs.files["big.jpg"] = jpeg(10, 10, 0xC0, 14, 330001);           // > 1,000,000 - 670,000
    fs.files["max.jpg"] = jpeg(10, 10, 0xC0, 14, 330000);
    std::vector<uint8_t> nosoi = jpeg(10, 10);
    nosoi[1] = 0xD9;
    fs.files["nosoi.jpg"] = nosoi;
    std::vector<uint8_t> sosFirst = {0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x08, 0, 0, 0, 0, 0, 0, 0xFF, 0xD9};
    fs.files["sos.jpg"] = sosFirst;
    fs.files["trunc.jpg"] = std::vector<uint8_t>{0xFF, 0xD8, 0xFF, 0xE0, 0x10, 0x00};
    uint16_t w = 7, h = 7;
    for (const char* p : {"prog.jpg", "wide.jpg", "tall.jpg", "zero.jpg", "big.jpg", "nosoi.jpg", "sos.jpg",
                          "trunc.jpg", "missing.jpg"}) {
        CHECK(!probe(fs, p, IDisplay::kRamGCover, 400, 400, w, h));
        CHECK_EQ(w, 0);
        CHECK_EQ(h, 0);
    }
    CHECK(probe(fs, "max.jpg", IDisplay::kRamGCover, 400, 400, w, h));
    CHECK(!probe(fs, "max.jpg", IDisplay::kRamGCover + 2, 400, 400, w, h));   // unaligned
    CHECK(!probe(fs, "max.jpg", IDisplay::kRamGImageLimit, 400, 400, w, h));
    // Decoded RGB565 must fit RAM_G (400 x 400 x 2 from 0xF0000 does not).
    fs.files["sq.jpg"] = jpeg(400, 400);
    CHECK(!probe(fs, "sq.jpg", 0xF0000, 400, 400, w, h));
}

TEST(jpeg_default_display_has_no_images) {
    NullDisplay d;
    FakeStorage fs;
    fs.files["c.jpg"] = jpeg(10, 10);
    uint16_t w = 5, h = 5;
    CHECK(!d.loadJpeg(IDisplay::kRamGCover, fs, "c.jpg", 400, 400, w, h));
    CHECK_EQ(w, 0);
    CHECK(!d.loadJpegBase(IDisplay::kRamGCover, fs, "c", 400, 400, w, h));
    d.drawImage(0, 0, IDisplay::kRamGCover, 10, 10);   // no-op
}

TEST(jpeg_fake_display_and_base_name) {
    RecordingDisplay d;
    FakeStorage fs;
    fs.dirs.insert("Projects");
    fs.dirs.insert("Projects/a");
    fs.dirs.insert("Projects/b");
    fs.files["Projects/a/cover.jpeg"] = jpeg(200, 100);
    fs.files["Projects/b/cover.jpg"] = jpeg(40, 30);
    fs.files["Projects/b/cover.jpeg"] = jpeg(50, 50);
    uint16_t w = 0, h = 0;
    CHECK(d.loadJpegBase(IDisplay::kRamGCover, fs, "Projects/a/cover", 400, 400, w, h));   // .jpeg fallback
    CHECK_EQ(w, 200);
    CHECK_EQ(h, 100);
    CHECK(d.loadJpegBase(IDisplay::kRamGCover, fs, "Projects/b/cover", 400, 400, w, h));   // .jpg first
    CHECK_EQ(w, 40);
    CHECK(!d.loadJpegBase(IDisplay::kRamGCover, fs, "Projects/c/cover", 400, 400, w, h));
    CHECK_EQ(d.loads.size(), 2u);
    CHECK_STR(d.loads[0].c_str(), "LOADJPEG 670000 200x100 Projects/a/cover.jpeg");
    d.jpegSupport = false;
    CHECK(!d.loadJpeg(IDisplay::kRamGCover, fs, "Projects/b/cover.jpg", 400, 400, w, h));
    CHECK_STR(d.loads.back().c_str(), "LOADJPEG 670000 FAIL Projects/b/cover.jpg");

    d.dlStart();
    d.drawImage(200, 40, IDisplay::kRamGCover, 40, 30);
    d.swap();
    CHECK_EQ(d.last.size(), 1u);
    CHECK_STR(d.last[0].c_str(), "IMAGE 200 40 670000 40 30");
}
