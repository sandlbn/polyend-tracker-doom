// Visual-diff gate (OpenTracker 1.9.5 "visual parity", docs/VISUAL_DIFF_1.9.4.md).
//
// For every sim/visual/*.scene: copy the SD fixture to a scratch card, replay
// the scene's script in the headless simulator, then evaluate the scene's
// pixel checks on the render. The same checks are also evaluated on the
// stock reference screenshot (self-test): a check that fails on the stock
// image is a broken check, reported as REF-FAIL.
//
//   visual-diff --scenes DIR --stock DIR --out DIR [--sd DIR --sim BIN]
//               [--projects DIR [--project NAME]]
//               [--device DIR] [--only NAME[,NAME]] [--allow-fail]
//
// --projects DIR holds stock projects (DIR/<name>/project.mt). A scene that
// selects one ('project NAME', or --project NAME as the default) boots it:
// once per run the project is copied to <out>/sd/project-<name>/Projects/
// <name> and opened in the sim through File -> Open Project (the firmware's
// own Open job, which rewrites the Workspace with name, folder, current
// pattern and instruments). Each scene card is then the SD fixture copy with
// that Workspace and the project copied into Projects/<name>, so the boot
// resumes it like the stock card did. The fixture is never modified. A
// missing project = the scene is skipped.
//
// --device DIR compares device captures (DIR/<scene>.bmp or .png, or the
// scene's stock file name) instead of simulator renders. BMP input accepts
// the stock 1,542,454-byte layout (54-byte header + 800x480x32 bottom-up +
// 6,400-byte tail) and ours (src/core/app/screenshot.cpp, no tail).
// Missing STOCK (or SD in sim mode) = skip, exit 0.
//
// Scene file (one directive per line, '#' starts a comment):
//   stock Snapshot_001.png        reference image in --stock
//   stock none                    no reference (2.x screens without a stock
//                                 counterpart): self-consistency checks only;
//                                 no REF self-test, 'diff' checks not allowed
//   project NAME                  boot the stock project --projects/NAME
//                                 (rest of the line; spaces allowed)
//   script                        sim script lines until 'end'; a trailing
//     tap DOWN x13                'xN' token repeats the line N times
//   end
//   region NAME [@tag ...]        following checks belong to region NAME
//   pix  x y RGB tol              one pixel, every channel within tol
//   fill x0 y0 x1 y1 RGB tol      every pixel of the rectangle
//   row  y x0 x1 RGB tol          every pixel of a row span
//   bbox x0 y0 x1 y1 tol          ink bounding box within tol per edge; an
//                                 edge written ~N is not checked (N only sets
//                                 the search window). Ink = pixels differing
//                                 by more than 32 from the most common colour
//                                 of the box grown by tol+2
//   diff x0 y0 x1 y1 maxpct       at most maxpct % of the pixels differ from
//                                 the stock image by more than 32 per channel
// A check line may carry its own @tags (e.g. @font: depends on font metrics,
// @content: depends on project content that differs from the stock card).
// RGB is RRGGBB hex or r,g,b. Coordinates are inclusive.
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "png.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../third_party/stb_image.h"
#pragma GCC diagnostic pop

namespace fs = std::filesystem;

namespace {

constexpr int kW = 800, kH = 480;

struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px;   // 0x00RRGGBB
    bool ok() const { return w == kW && h == kH; }
    uint32_t at(int x, int y) const { return px[size_t(y) * size_t(w) + size_t(x)]; }
};

int ch(uint32_t c, int i) { return int((c >> (16 - 8 * i)) & 255); }
int maxDelta(uint32_t a, uint32_t b) {
    int m = 0;
    for (int i = 0; i < 3; ++i) m = std::max(m, std::abs(ch(a, i) - ch(b, i)));
    return m;
}

bool loadPng(const std::string& path, Image& im) {
    int w, h, n;
    unsigned char* d = stbi_load(path.c_str(), &w, &h, &n, 3);
    if (!d) return false;
    im.w = w;
    im.h = h;
    im.px.resize(size_t(w) * size_t(h));
    for (size_t i = 0; i < im.px.size(); ++i)
        im.px[i] = uint32_t(d[3 * i]) << 16 | uint32_t(d[3 * i + 1]) << 8 | d[3 * i + 2];
    stbi_image_free(d);
    return true;
}

uint32_t le32(const unsigned char* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// 24/32-bpp BI_RGB BMP, bottom-up or top-down. The pixel offset comes from
// the header, so trailing bytes (the stock 6,400-byte tail) are ignored.
bool loadBmp(const std::string& path, Image& im) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 54 || b[0] != 'B' || b[1] != 'M') return false;
    const uint32_t off = le32(&b[10]);
    const int w = int(le32(&b[18]));
    int h = int(int32_t(le32(&b[22])));
    const int bpp = b[28] | b[29] << 8;
    const uint32_t comp = le32(&b[30]);
    const bool topDown = h < 0;
    if (topDown) h = -h;
    if ((bpp != 32 && bpp != 24) || (comp != 0 && comp != 3) || w <= 0 || h <= 0) return false;
    const size_t stride = (size_t(w) * size_t(bpp / 8) + 3) & ~size_t(3);
    if (off + stride * size_t(h) > b.size()) return false;
    im.w = w;
    im.h = h;
    im.px.resize(size_t(w) * size_t(h));
    for (int y = 0; y < h; ++y) {
        const unsigned char* row = &b[off + stride * size_t(topDown ? y : h - 1 - y)];
        for (int x = 0; x < w; ++x) {
            const unsigned char* p = row + size_t(x) * size_t(bpp / 8);
            im.px[size_t(y) * size_t(w) + size_t(x)] = uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
        }
    }
    return true;
}

bool loadAny(const std::string& path, Image& im) {
    std::string ext = fs::path(path).extension().string();
    for (auto& c : ext) c = char(std::tolower(uint8_t(c)));
    return ext == ".bmp" ? loadBmp(path, im) : loadPng(path, im);
}

// ---------------------------------------------------------------- scenes

enum Kind { kPix, kFill, kRow, kBbox, kDiff };

struct Check {
    Kind kind;
    int a[4] = {0, 0, 0, 0};       // coordinates
    bool skip[4] = {false, false, false, false};   // bbox edge written ~N
    uint32_t rgb = 0;
    double tol = 0;                 // tolerance or maxpct
    std::set<std::string> tags;
    std::string region, text;
    int line = 0;
};

struct Scene {
    std::string name, file, stock, project;
    std::vector<std::string> script;
    std::vector<Check> checks;
    std::vector<std::string> regionOrder;
};

bool parseRgb(const std::string& s, uint32_t& out) {
    int r, g, b;
    if (std::sscanf(s.c_str(), "%d,%d,%d", &r, &g, &b) == 3) {
        out = uint32_t(r & 255) << 16 | uint32_t(g & 255) << 8 | uint32_t(b & 255);
        return true;
    }
    std::string h = s;
    if (!h.empty() && h[0] == '#') h.erase(0, 1);
    if (h.size() != 6) return false;
    char* end;
    out = uint32_t(std::strtoul(h.c_str(), &end, 16));
    return *end == 0;
}

bool parseInt(const std::string& s, int& out, bool* skip = nullptr) {
    const char* p = s.c_str();
    if (*p == '~' && skip) {
        *skip = true;
        ++p;
    }
    char* end;
    out = int(std::strtol(p, &end, 10));
    return *p && *end == 0;
}

bool parseScene(const fs::path& path, Scene& sc) {
    std::ifstream f(path);
    if (!f) return false;
    sc.name = path.stem().string();
    sc.file = path.string();
    std::string region = "-";
    std::set<std::string> regionTags;
    std::string ln;
    int line = 0;
    bool inScript = false;
    auto err = [&](const char* what) {
        std::fprintf(stderr, "%s:%d: %s\n", sc.file.c_str(), line, what);
        return false;
    };
    while (std::getline(f, ln)) {
        ++line;
        const size_t hash = ln.find('#');
        if (hash != std::string::npos) ln.resize(hash);
        std::istringstream ss(ln);
        std::vector<std::string> tok;
        for (std::string t; ss >> t;) tok.push_back(t);
        if (tok.empty()) continue;
        if (inScript) {
            if (tok[0] == "end") {
                inScript = false;
                continue;
            }
            int rep = 1;
            const std::string& last = tok.back();
            if (tok.size() > 1 && last.size() > 1 && last[0] == 'x' &&
                last.find_first_not_of("0123456789", 1) == std::string::npos) {
                rep = std::atoi(last.c_str() + 1);
                tok.pop_back();
            }
            std::string s;
            for (auto& t : tok) s += (s.empty() ? "" : " ") + t;
            for (int i = 0; i < rep; ++i) sc.script.push_back(s);
            continue;
        }
        if (tok[0] == "stock") {
            if (tok.size() != 2) return err("stock FILE");
            sc.stock = tok[1];
            continue;
        }
        if (tok[0] == "project") {
            if (tok.size() < 2) return err("project NAME");
            std::string n;
            for (size_t i = 1; i < tok.size(); ++i) n += (n.empty() ? "" : " ") + tok[i];
            sc.project = n;
            continue;
        }
        if (tok[0] == "script") {
            inScript = true;
            continue;
        }
        if (tok[0] == "region") {
            if (tok.size() < 2) return err("region NAME [@tag..]");
            region = tok[1];
            regionTags.clear();
            for (size_t i = 2; i < tok.size(); ++i)
                if (tok[i][0] == '@') regionTags.insert(tok[i].substr(1));
            continue;
        }
        Check c;
        c.region = region;
        c.tags = regionTags;
        c.line = line;
        std::vector<std::string> args;
        for (size_t i = 1; i < tok.size(); ++i) {
            if (tok[i][0] == '@') c.tags.insert(tok[i].substr(1));
            else args.push_back(tok[i]);
        }
        for (auto& t : tok) c.text += (c.text.empty() ? "" : " ") + t;
        bool ok = true;
        auto ints = [&](size_t n) {
            for (size_t i = 0; i < n; ++i) ok = ok && parseInt(args[i], c.a[i], &c.skip[i]);
        };
        if (tok[0] == "pix" && args.size() == 4) {
            c.kind = kPix;
            ints(2);
            ok = ok && parseRgb(args[2], c.rgb);
            c.tol = std::atof(args[3].c_str());
        } else if (tok[0] == "fill" && args.size() == 6) {
            c.kind = kFill;
            ints(4);
            ok = ok && parseRgb(args[4], c.rgb);
            c.tol = std::atof(args[5].c_str());
        } else if (tok[0] == "row" && args.size() == 5) {
            c.kind = kRow;
            ints(3);
            ok = ok && parseRgb(args[3], c.rgb);
            c.tol = std::atof(args[4].c_str());
        } else if (tok[0] == "bbox" && args.size() == 5) {
            c.kind = kBbox;
            ints(4);
            c.tol = std::atof(args[4].c_str());
        } else if (tok[0] == "diff" && args.size() == 5) {
            c.kind = kDiff;
            ints(4);
            c.tol = std::atof(args[4].c_str());
        } else {
            return err("bad check (pix/fill/row/bbox/diff with wrong arguments?)");
        }
        if (!ok) return err("bad number or colour");
        if (std::find(sc.regionOrder.begin(), sc.regionOrder.end(), region) == sc.regionOrder.end())
            sc.regionOrder.push_back(region);
        sc.checks.push_back(c);
    }
    if (inScript) return err("script without end");
    if (sc.stock.empty()) return err("no 'stock' line");
    if (sc.stock == "none")
        for (const Check& c : sc.checks)
            if (c.kind == kDiff) {
                line = c.line;
                return err("'diff' needs a stock image");
            }
    return true;
}

// ---------------------------------------------------------------- checks

int clampX(int x) { return std::clamp(x, 0, kW - 1); }
int clampY(int y) { return std::clamp(y, 0, kH - 1); }

struct Rect {
    int x0, y0, x1, y1;
};

Rect checkRect(const Check& c) {
    switch (c.kind) {
        case kPix: return {c.a[0], c.a[1], c.a[0], c.a[1]};
        case kRow: return {c.a[1], c.a[0], c.a[2], c.a[0]};
        default: return {c.a[0], c.a[1], c.a[2], c.a[3]};
    }
}

bool inkBox(const Image& im, Rect r, int thr, Rect& out) {
    std::map<uint32_t, int> hist;
    for (int y = r.y0; y <= r.y1; ++y)
        for (int x = r.x0; x <= r.x1; ++x) ++hist[im.at(x, y)];
    uint32_t bg = 0;
    int best = -1;
    for (auto& [c, n] : hist)
        if (n > best) best = n, bg = c;
    bool any = false;
    out = {kW, kH, -1, -1};
    for (int y = r.y0; y <= r.y1; ++y)
        for (int x = r.x0; x <= r.x1; ++x)
            if (maxDelta(im.at(x, y), bg) > thr) {
                any = true;
                out.x0 = std::min(out.x0, x);
                out.y0 = std::min(out.y0, y);
                out.x1 = std::max(out.x1, x);
                out.y1 = std::max(out.y1, y);
            }
    return any;
}

// Evaluates one check; 'detail' says why it failed.
bool evaluate(const Check& c, const Image& im, const Image* ref, std::string& detail) {
    char buf[160];
    detail.clear();
    auto hex = [](uint32_t v) {
        char s[8];
        std::snprintf(s, sizeof(s), "%06X", unsigned(v & 0xFFFFFF));
        return std::string(s);
    };
    switch (c.kind) {
        case kPix:
        case kFill:
        case kRow: {
            const Rect r = checkRect(c);
            int bad = 0, total = 0, worst = -1;
            uint32_t worstPx = 0;
            int wx = 0, wy = 0;
            for (int y = clampY(r.y0); y <= clampY(r.y1); ++y)
                for (int x = clampX(r.x0); x <= clampX(r.x1); ++x) {
                    ++total;
                    const int d = maxDelta(im.at(x, y), c.rgb);
                    if (d > c.tol) ++bad;
                    if (d > worst) worst = d, worstPx = im.at(x, y), wx = x, wy = y;
                }
            if (bad == 0) return true;
            std::snprintf(buf, sizeof(buf), "%d/%d px off; worst (%d,%d)=%s want %s", bad, total, wx, wy,
                          hex(worstPx).c_str(), hex(c.rgb).c_str());
            detail = buf;
            return false;
        }
        case kBbox: {
            const int g = int(c.tol) + 2;
            const Rect want = checkRect(c);
            Rect win = {clampX(want.x0 - g), clampY(want.y0 - g), clampX(want.x1 + g), clampY(want.y1 + g)};
            Rect got;
            if (!inkBox(im, win, 32, got)) {
                detail = "no ink";
                return false;
            }
            const int gv[4] = {got.x0, got.y0, got.x1, got.y1};
            bool ok = true;
            for (int i = 0; i < 4; ++i)
                if (!c.skip[i] && std::abs(gv[i] - c.a[i]) > c.tol) ok = false;
            if (ok) return true;
            std::snprintf(buf, sizeof(buf), "ink %d,%d..%d,%d", got.x0, got.y0, got.x1, got.y1);
            detail = buf;
            return false;
        }
        case kDiff: {
            if (!ref) return true;
            const Rect r = checkRect(c);
            long bad = 0, total = 0;
            for (int y = clampY(r.y0); y <= clampY(r.y1); ++y)
                for (int x = clampX(r.x0); x <= clampX(r.x1); ++x) {
                    ++total;
                    if (maxDelta(im.at(x, y), ref->at(x, y)) > 32) ++bad;
                }
            const double pct = total ? 100.0 * double(bad) / double(total) : 0;
            if (pct <= c.tol) return true;
            std::snprintf(buf, sizeof(buf), "%.1f%% differ (max %.1f%%)", pct, c.tol);
            detail = buf;
            return false;
        }
    }
    return false;
}

// ---------------------------------------------------------------- sim run

// Scratch card: Workspace (full copy), the top-level small files, every
// folder down to depth 2 (empty), and files up to 64 KB at depth <= 2 (e.g.
// Projects/<name>/project.mt). Large sample libraries are not copied.
void copyCard(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::create_directories(dst, ec);
    for (auto& e : fs::directory_iterator(src, ec)) {
        const std::string n = e.path().filename().string();
        if (e.is_directory()) {
            if (n == "Workspace") {
                fs::copy(e.path(), dst / n, fs::copy_options::recursive, ec);
                continue;
            }
            fs::create_directories(dst / n, ec);
            for (auto& e2 : fs::directory_iterator(e.path(), ec)) {
                const fs::path t = dst / n / e2.path().filename();
                if (e2.is_directory()) {
                    fs::create_directories(t, ec);
                    for (auto& e3 : fs::directory_iterator(e2.path(), ec))
                        if (e3.is_regular_file() && e3.file_size() <= 65536)
                            fs::copy_file(e3.path(), t / e3.path().filename(), ec);
                } else if (e2.is_regular_file() && e2.file_size() <= 65536) {
                    fs::copy_file(e2.path(), t, ec);
                }
            }
        } else if (e.is_regular_file() && e.file_size() <= 65536) {
            fs::copy_file(e.path(), dst / n, ec);
        }
    }
}

bool runSim(const std::string& sim, const fs::path& card, const fs::path& script, const fs::path& png,
            const fs::path& log) {
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        setenv("SDL_VIDEODRIVER", "dummy", 1);
        setenv("SDL_AUDIODRIVER", "dummy", 1);
        const int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, 1);
            dup2(fd, 2);
            close(fd);
        }
        execl(sim.c_str(), sim.c_str(), "--no-midi", "--no-audio", "--sd", card.c_str(), "--script",
              script.c_str(), "--screenshot", png.c_str(), (char*)nullptr);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

bool fileContains(const fs::path& p, const std::string& needle) {
    std::ifstream f(p, std::ios::binary);
    const std::string b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return b.find(needle) != std::string::npos;
}

// Open the stock project <projects>/<name> once: a card holding only
// Projects/<name> (fresh copy), booted to an empty Workspace; File (F2 Open
// Project) lists "/.." and the project, DOWN selects it, F8 Open runs the
// firmware Open job. Returns the resulting Workspace or "" on failure.
fs::path prepareProject(const std::string& sim, const fs::path& projects, const std::string& name,
                        const fs::path& work) {
    std::error_code ec;
    const fs::path card = work / "card";
    fs::remove_all(work, ec);
    fs::create_directories(card / "Projects", ec);
    fs::copy(projects / name, card / "Projects" / name, fs::copy_options::recursive, ec);
    if (ec) return {};
    const fs::path script = work / "script.txt";
    {
        std::ofstream s(script);
        s << "wait 300\ntap FILE\ntap F2\nwait 300\ntap DOWN\ntap F8\nwait 3000\n";
    }
    if (!runSim(sim, card, script, work / "open.png", work / "sim.log")) return {};
    const fs::path ws = card / "Workspace";
    if (!fileContains(ws / "project.mt", name)) return {};   // name at 0x810 after Open
    return ws;
}

// ---------------------------------------------------------------- output

void blend(uint32_t& d, uint32_t c, int a) {
    uint32_t o = 0;
    for (int i = 0; i < 3; ++i) {
        const int v = (ch(d, i) * (255 - a) + ch(c, i) * a) / 255;
        o |= uint32_t(v) << (16 - 8 * i);
    }
    d = o;
}

// Diff image: candidate dimmed to 35 %; pixels differing from stock by more
// than 32 in magenta (ours brighter, e.g. our ink) or cyan (stock brighter,
// e.g. missing stock ink); check outlines green (pass) / red (fail) / yellow
// (@font fail).
void writeDiff(const fs::path& out, const Image& im, const Image& ref, const std::vector<Check>& checks,
               const std::vector<int>& result) {
    std::vector<uint32_t> px(size_t(kW) * kH);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            uint32_t c = im.at(x, y);
            const uint32_t r = ref.at(x, y);
            if (maxDelta(c, r) > 32) {
                auto lum = [](uint32_t v) { return ch(v, 0) * 3 + ch(v, 1) * 6 + ch(v, 2); };
                c = lum(c) >= lum(r) ? 0xFF40FF : 0x30D0FF;   // ours brighter / stock brighter
            } else {
                blend(c, 0, 165);
            }
            px[size_t(y) * kW + size_t(x)] = 0xFF000000u | c;
        }
    for (size_t i = 0; i < checks.size(); ++i) {
        const Rect r = checkRect(checks[i]);
        const uint32_t col = result[i] ? 0x00C000 : checks[i].tags.count("font") ? 0xFFD000 : 0xFF2020;
        auto put = [&](int x, int y) {
            if (x >= 0 && y >= 0 && x < kW && y < kH) px[size_t(y) * kW + size_t(x)] = 0xFF000000u | col;
        };
        const int x0 = r.x0 - 1, y0 = r.y0 - 1, x1 = r.x1 + 1, y1 = r.y1 + 1;
        for (int x = x0; x <= x1; ++x) put(x, y0), put(x, y1);
        for (int y = y0; y <= y1; ++y) put(x0, y), put(x1, y);
    }
    sim::writePng(out.c_str(), px.data(), kW, kH);
}

void writeImage(const fs::path& out, const Image& im) {
    std::vector<uint32_t> px(im.px.size());
    for (size_t i = 0; i < px.size(); ++i) px[i] = 0xFF000000u | im.px[i];
    sim::writePng(out.c_str(), px.data(), im.w, im.h);
}

void usage() {
    std::fprintf(stderr,
                 "usage: visual-diff --scenes DIR --stock DIR --out DIR [--sd DIR --sim BIN]\n"
                 "                   [--projects DIR [--project NAME]]\n"
                 "                   [--device DIR] [--only NAME,..] [--allow-fail] [-v]\n");
}

bool isDir(const std::string& p) {
    std::error_code ec;
    return !p.empty() && fs::is_directory(p, ec);
}

}  // namespace

int main(int argc, char** argv) {
    std::string scenesDir, stockDir, outDir, sdDir, sim, deviceDir, only, projectsDir, defProject;
    bool allowFail = false, verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& d) {
            if (i + 1 >= argc) return false;
            d = argv[++i];
            return true;
        };
        if (a == "--scenes" && next(scenesDir)) continue;
        if (a == "--stock" && next(stockDir)) continue;
        if (a == "--out" && next(outDir)) continue;
        if (a == "--sd" && next(sdDir)) continue;
        if (a == "--sim" && next(sim)) continue;
        if (a == "--projects" && next(projectsDir)) continue;
        if (a == "--project" && next(defProject)) continue;
        if (a == "--device" && next(deviceDir)) continue;
        if (a == "--only" && next(only)) continue;
        if (a == "--allow-fail") { allowFail = true; continue; }
        if (a == "-v") { verbose = true; continue; }
        usage();
        return 2;
    }
    if (scenesDir.empty() || outDir.empty()) {
        usage();
        return 2;
    }
    const bool device = !deviceDir.empty();
    if (!isDir(stockDir)) {
        std::printf("visual-diff: SKIP (stock snapshots not found: %s)\n", stockDir.c_str());
        return 0;
    }
    if (!device && !isDir(sdDir)) {
        std::printf("visual-diff: SKIP (SD fixture not found: %s)\n", sdDir.c_str());
        return 0;
    }
    if (device && !isDir(deviceDir)) {
        std::fprintf(stderr, "visual-diff: device dir not found: %s\n", deviceDir.c_str());
        return 2;
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<fs::path> files;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(scenesDir, ec))
        if (e.path().extension() == ".scene") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    std::set<std::string> onlySet;
    for (std::stringstream ss(only); std::getline(ss, only, ',');)
        if (!only.empty()) onlySet.insert(only);
    fs::create_directories(outDir, ec);

    struct Row {
        std::string scene, region, status, detail;
        bool font;
    };
    int failRegions = 0, failFont = 0, failContent = 0, refFail = 0, scenesRun = 0, parseErrors = 0;
    std::map<std::string, fs::path> prepared;   // project name -> opened Workspace ("" = failed)
    std::vector<std::string> summary;
    std::printf("%-14s %-24s %-9s %s\n", "scene", "region", "result", "detail");
    for (const auto& file : files) {
        Scene sc;
        if (!parseScene(file, sc)) {
            ++parseErrors;
            continue;
        }
        if (!onlySet.empty() && !onlySet.count(sc.name)) continue;
        Image ref, im;
        const bool noRef = sc.stock == "none";
        if (!noRef && (!loadAny((fs::path(stockDir) / sc.stock).string(), ref) || !ref.ok())) {
            std::printf("%-14s %-24s %-9s %s\n", sc.name.c_str(), "*", "SKIP", "no stock image");
            continue;
        }
        const fs::path out = fs::path(outDir);
        if (device) {
            bool got = false;
            for (const std::string& n : {sc.name + ".bmp", sc.name + ".png",
                                         fs::path(sc.stock).replace_extension(".bmp").string(), sc.stock}) {
                if (loadAny((fs::path(deviceDir) / n).string(), im) && im.ok()) {
                    got = true;
                    break;
                }
            }
            if (!got) {
                std::printf("%-14s %-24s %-9s %s\n", sc.name.c_str(), "*", "SKIP", "no device capture");
                continue;
            }
        } else {
            if (sc.project.empty()) sc.project = defProject;
            fs::path ws;
            if (!sc.project.empty()) {
                if (!isDir(projectsDir) || !fs::exists(fs::path(projectsDir) / sc.project / "project.mt", ec)) {
                    std::printf("%-14s %-24s %-9s %s\n", sc.name.c_str(), "*", "SKIP",
                                ("stock project not found: " + (fs::path(projectsDir) / sc.project).string()).c_str());
                    continue;
                }
                auto it = prepared.find(sc.project);
                if (it == prepared.end())
                    it = prepared.emplace(sc.project, prepareProject(sim, projectsDir, sc.project,
                                                                     out / "sd" / ("project-" + sc.project)))
                             .first;
                ws = it->second;
                if (ws.empty()) {
                    std::printf("%-14s %-24s %-9s %s\n", sc.name.c_str(), "*", "FAIL",
                                ("could not open stock project \"" + sc.project + "\", see " +
                                 (out / "sd" / ("project-" + sc.project) / "sim.log").string())
                                    .c_str());
                    ++failRegions;
                    continue;
                }
            }
            const fs::path work = out / "sd" / sc.name;
            const fs::path card = work / "card";
            fs::create_directories(work, ec);
            copyCard(sdDir, card);
            if (!ws.empty()) {
                fs::remove_all(card / "Workspace", ec);
                fs::copy(ws, card / "Workspace", fs::copy_options::recursive, ec);
                fs::remove_all(card / "Projects" / sc.project, ec);
                fs::create_directories(card / "Projects", ec);
                fs::copy(fs::path(projectsDir) / sc.project, card / "Projects" / sc.project,
                         fs::copy_options::recursive, ec);
            }
            const fs::path script = work / "script.txt";
            {
                std::ofstream s(script);
                for (auto& l : sc.script) s << l << "\n";
            }
            const fs::path png = out / (sc.name + ".png");
            fs::remove(png, ec);
            if (!runSim(sim, card, script, png, work / "sim.log") || !loadPng(png.string(), im) || !im.ok()) {
                std::printf("%-14s %-24s %-9s %s\n", sc.name.c_str(), "*", "FAIL",
                            ("simulator run failed, see " + (work / "sim.log").string()).c_str());
                ++failRegions;
                continue;
            }
        }
        ++scenesRun;
        if (device) writeImage(out / (sc.name + ".png"), im);
        std::vector<int> result(sc.checks.size());
        std::map<std::string, std::vector<size_t>> byRegion;
        int sceneFail = 0, sceneFont = 0, sceneContent = 0, sceneRef = 0, sceneRegions = 0;
        for (size_t i = 0; i < sc.checks.size(); ++i) byRegion[sc.checks[i].region].push_back(i);
        for (const auto& reg : sc.regionOrder) {
            auto it = byRegion.find(reg);
            if (it == byRegion.end()) continue;
            ++sceneRegions;
            int pass = 0;
            bool font = true, content = false, refBad = false;   // font: every failing check is @font
            std::string detail;
            for (size_t i : it->second) {
                const Check& c = sc.checks[i];
                std::string d;
                if (!noRef && !evaluate(c, ref, &ref, d)) {
                    refBad = true;
                    detail = "REF " + c.text + " -> " + d;
                    std::fprintf(stderr, "%s:%d: check fails on the stock image: %s (%s)\n", sc.file.c_str(),
                                 c.line, c.text.c_str(), d.c_str());
                }
                result[i] = evaluate(c, im, noRef ? nullptr : &ref, d);
                if (result[i]) ++pass;
                else {
                    if (!c.tags.count("font")) font = false;
                    if (c.tags.count("content")) content = true;
                    if (detail.empty() || verbose) {
                        if (!detail.empty()) detail += "; ";
                        detail += c.text + " -> " + d;
                    }
                }
            }
            const int n = int(it->second.size());
            std::string tags;
            {
                std::set<std::string> all;
                for (size_t i : it->second) all.insert(sc.checks[i].tags.begin(), sc.checks[i].tags.end());
                for (auto& t : all) tags += "@" + t + " ";
            }
            const char* status = refBad ? "REF-FAIL" : pass == n ? "PASS" : font ? "FAIL@font" : "FAIL";
            if (refBad) ++sceneRef;
            else if (pass != n) {
                ++sceneFail;
                if (font) ++sceneFont;
                if (content) ++sceneContent;
            }
            char cnt[16];
            std::snprintf(cnt, sizeof(cnt), "%d/%d", pass, n);
            std::printf("%-14s %-24s %-9s %-5s %s%s\n", sc.name.c_str(), reg.c_str(), status, cnt, tags.c_str(),
                        detail.c_str());
        }
        writeDiff(out / (sc.name + "-diff.png"), im, noRef ? im : ref, sc.checks, result);
        failRegions += sceneFail;
        failFont += sceneFont;
        failContent += sceneContent;
        refFail += sceneRef;
        char line[160];
        std::snprintf(line, sizeof(line), "%-17s regions %3d  fail %3d  (font-tagged %d, content-tagged %d)%s",
                      sc.name.c_str(), sceneRegions, sceneFail, sceneFont, sceneContent,
                      sceneRef ? "  REF-FAIL!" : "");
        summary.push_back(line);
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\n-- summary (%s, %.1f s, diff PNGs in %s)\n", device ? "device captures" : "simulator", secs,
                outDir.c_str());
    for (auto& s : summary) std::printf("%s\n", s.c_str());
    std::printf("total: %d scene(s), %d failing region(s) (%d font-tagged, %d content-tagged), "
                "%d broken check region(s)\n",
                scenesRun, failRegions, failFont, failContent, refFail);
    if (parseErrors) {
        std::printf("visual-diff: %d scene file(s) could not be parsed\n", parseErrors);
        return 2;
    }
    if (failRegions + refFail == 0) return 0;
    if (allowFail) {
        std::printf("visual-diff: ALLOW_FAIL=1, not failing the build\n");
        return 0;
    }
    return 1;
}
