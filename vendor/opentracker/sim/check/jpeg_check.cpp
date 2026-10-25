// Simulator-only check of the IDisplay image path (spec 13 §G.6/§I.3):
// loads sim/fixtures/cover.jpg (64 x 48, red/green/blue/white quadrants) into
// the RAM_G model at the cover address through HostStorage, draws it with
// drawImage() and checks the swapped frame. Also checks the rejections
// (progressive JPEG, size limits, missing file) and the .jpg/.jpeg fallback.
//   jpeg-check FIXTURE_DIR [OUT.png]
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "host_storage.h"
#include "png.h"
#include "sw_display.h"

namespace {
int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "jpeg-check: FAIL %s\n", what);
        ++failures;
    }
}
bool near(uint32_t px, uint32_t want) {
    for (int s = 0; s < 24; s += 8) {
        const int a = int((px >> s) & 255), b = int((want >> s) & 255);
        if (std::abs(a - b) > 24) return false;
    }
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: jpeg-check FIXTURE_DIR [OUT.png]\n");
        return 2;
    }
    sim::HostStorage fs;
    if (!fs.init(argv[1])) return 2;
    sim::SwDisplay d;
    using D = ot::IDisplay;
    uint16_t w = 0, h = 0;

    expect(d.loadJpeg(D::kRamGCover, fs, "cover.jpg", D::kCoverMaxSide, D::kCoverMaxSide, w, h), "load cover.jpg");
    expect(w == 64 && h == 48, "cover size 64x48");
    uint16_t w2 = 0, h2 = 0;
    expect(d.loadJpegBase(D::kRamGCover, fs, "cover", 400, 400, w2, h2) && w2 == 64 && h2 == 48, "base name -> .jpg");
    expect(!d.loadJpeg(D::kRamGCover, fs, "cover_progressive.jpg", 400, 400, w2, h2) && w2 == 0, "progressive rejected");
    expect(!d.loadJpeg(D::kRamGCover, fs, "cover.jpg", 63, 400, w2, h2), "too wide rejected");
    expect(!d.loadJpeg(D::kRamGCover, fs, "missing.jpg", 400, 400, w2, h2), "missing rejected");
    expect(!d.loadJpeg(D::kRamGImageLimit - 512, fs, "cover.jpg", 400, 400, w2, h2), "file > limit - dest rejected");
    expect(!d.loadJpeg(D::kRamGCover + 2, fs, "cover.jpg", 400, 400, w2, h2), "unaligned dest rejected");

    d.dlStart();
    d.clearColorRGB(0, 0, 0);
    d.clear(true, true, true);
    d.colorRGB(255, 255, 255);
    d.drawImage(100, 50, D::kRamGCover, w, h);
    d.swap();
    std::vector<uint32_t> px(size_t(w) * h);
    expect(d.readPixels(100, 50, w, h, px.data()), "readPixels");
    expect(near(px[size_t(8) * w + 8], 0xFF0000), "top-left red");
    expect(near(px[size_t(8) * w + 56], 0x00FF00), "top-right green");
    expect(near(px[size_t(40) * w + 8], 0x0000FF), "bottom-left blue");
    expect(near(px[size_t(40) * w + 56], 0xFFFFFF), "bottom-right white");
    uint32_t outside = 1;
    d.readPixels(99, 49, 1, 1, &outside);
    expect(outside == 0, "nothing drawn outside the image");

    if (argc > 2) {
        std::vector<uint32_t> fb(size_t(sim::SwDisplay::kW) * sim::SwDisplay::kH);
        d.frontCopy(fb.data());
        sim::writePng(argv[2], fb.data(), sim::SwDisplay::kW, sim::SwDisplay::kH);
    }
    if (failures) return 1;
    std::printf("jpeg-check: ok (cover %ux%u at RAM_G %lu)\n", unsigned(w), unsigned(h),
                static_cast<unsigned long>(D::kRamGCover));
    return 0;
}
