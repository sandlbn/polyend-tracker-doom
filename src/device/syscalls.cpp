// polyend-doom -- newlib system calls for Doom's stdio, POSIX file calls and
// heap on the Tracker:
//  - files: the SD card through the HAL's IStorage (FatFs). Relative paths
//    resolve under /DOOM (Doom's current directory);
//  - stdout / stderr: line-buffered into the USB serial log (diagLog);
//  - heap: _sbrk is wrapped (-Wl,--wrap=_sbrk) to hand out the SDRAM between
//    the linker symbols _sheap_sdram.._eheap_sdram once pt_heap_enable() ran
//    (the SDRAM is only up after boardInit); before that the Teensy core's
//    SRAM heap answers, as for the HAL.
// The Teensy core defines all of these weak (mk20dx128.c, Print.cpp).

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hal/diag.h"
#include "hal_api.h"

#include "device.h"

extern "C" char _sheap_sdram[], _eheap_sdram[];   // src/device/doom.ld

namespace {

ot::IStorage* gFs = nullptr;
bool gHeapSdram = false;
char* gBrk = _sheap_sdram;

constexpr int kFirstFd = 3;
constexpr int kMaxFiles = 12;
struct File {
    ot::IStorage::Handle h = -1;
    uint32_t pos = 0;
};
File gFiles[kMaxFiles];

File* fileOf(int fd) {
    const int i = fd - kFirstFd;
    if (i < 0 || i >= kMaxFiles || gFiles[i].h < 0) return nullptr;
    return &gFiles[i];
}

// "/x" stays, "x" and "./x" become "/DOOM/x".
const char* resolve(const char* path, char* buf, size_t cap) {
    if (path[0] == '/') return path;
    while (path[0] == '.' && path[1] == '/') path += 2;
    snprintf(buf, cap, "%s/%s", kDoomDir, path);
    return buf;
}

// stdout / stderr line buffer.
char gLine[160];
size_t gLineLen = 0;

void logChars(const char* p, int len) {
    for (int i = 0; i < len; ++i) {
        const char c = p[i];
        if (c == '\n' || gLineLen + 1 >= sizeof(gLine)) {
            gLine[gLineLen] = 0;
            if (gLineLen) ot::hal::diagLog(gLine);
            gLineLen = 0;
            if (c == '\n') continue;
        }
        if (c != '\r') gLine[gLineLen++] = c;
    }
}

}  // namespace

void pt_syscalls_init(ot::IStorage* fs) { gFs = fs; }

void pt_heap_enable() { gHeapSdram = true; }

uint32_t pt_heap_used() { return uint32_t(gBrk - _sheap_sdram); }
uint32_t pt_heap_size() { return uint32_t(_eheap_sdram - _sheap_sdram); }

extern "C" {

void* __real__sbrk(int incr);

void* __wrap__sbrk(int incr) {
    if (!gHeapSdram) return __real__sbrk(incr);
    char* prev = gBrk;
    if (incr > 0 ? incr > _eheap_sdram - gBrk : -incr > gBrk - _sheap_sdram) {
        errno = ENOMEM;
        return reinterpret_cast<void*>(-1);
    }
    gBrk += incr;
    return prev;
}

int _open(const char* path, int flags, int mode) {
    (void)mode;
    if (!gFs || !path) {
        errno = ENOENT;
        return -1;
    }
    int slot = 0;
    while (slot < kMaxFiles && gFiles[slot].h >= 0) ++slot;
    if (slot == kMaxFiles) {
        errno = EMFILE;
        return -1;
    }
    char buf[128];
    const char* p = resolve(path, buf, sizeof buf);
    ot::IStorage::OpenMode m;
    if ((flags & O_ACCMODE) == O_RDONLY) m = ot::IStorage::kRead;
    else if (flags & O_TRUNC) m = ot::IStorage::kWrite;
    else m = ot::IStorage::kReadWrite;
    if (m != ot::IStorage::kRead && !(flags & O_CREAT) && !gFs->exists(p)) {
        errno = ENOENT;
        return -1;
    }
    const ot::IStorage::Handle h = gFs->open(p, m);
    if (h < 0) {
        errno = ENOENT;
        return -1;
    }
    gFiles[slot].h = h;
    gFiles[slot].pos = 0;
    if (flags & O_APPEND) {
        const int32_t sz = gFs->size(h);
        if (sz > 0 && gFs->seek(h, uint32_t(sz))) gFiles[slot].pos = uint32_t(sz);
    }
    return kFirstFd + slot;
}

int _close(int fd) {
    File* f = fileOf(fd);
    if (!f) return fd >= 0 && fd < kFirstFd ? 0 : (errno = EBADF, -1);
    gFs->close(f->h);
    f->h = -1;
    return 0;
}

int _read(int fd, char* ptr, int len) {
    File* f = fileOf(fd);
    if (!f) return fd == 0 ? 0 : (errno = EBADF, -1);
    const int32_t n = gFs->read(f->h, ptr, uint32_t(len));
    if (n < 0) {
        errno = EIO;
        return -1;
    }
    f->pos += uint32_t(n);
    return int(n);
}

int _write(int fd, const char* ptr, int len) {
    if (fd == 1 || fd == 2) {
        logChars(ptr, len);
        return len;
    }
    File* f = fileOf(fd);
    if (!f) {
        errno = EBADF;
        return -1;
    }
    const int32_t n = gFs->write(f->h, ptr, uint32_t(len));
    if (n < 0) {
        errno = EIO;
        return -1;
    }
    f->pos += uint32_t(n);
    return int(n);
}

off_t _lseek(int fd, off_t off, int whence) {
    File* f = fileOf(fd);
    if (!f) {
        errno = EBADF;
        return -1;
    }
    int64_t target;
    switch (whence) {
    case SEEK_SET: target = off; break;
    case SEEK_CUR: target = int64_t(f->pos) + off; break;
    case SEEK_END: target = int64_t(gFs->size(f->h)) + off; break;
    default: errno = EINVAL; return -1;
    }
    if (target < 0 || !gFs->seek(f->h, uint32_t(target))) {
        errno = EINVAL;
        return -1;
    }
    f->pos = uint32_t(target);
    return off_t(target);
}

int _fstat(int fd, struct stat* st) {
    memset(st, 0, sizeof *st);
    if (fd >= 0 && fd < kFirstFd) {
        st->st_mode = S_IFCHR;
        return 0;
    }
    File* f = fileOf(fd);
    if (!f) {
        errno = EBADF;
        return -1;
    }
    st->st_mode = S_IFREG;
    st->st_size = gFs->size(f->h);
    return 0;
}

int _isatty(int fd) { return fd >= 0 && fd < kFirstFd; }

int _stat(const char* path, struct stat* st) {
    char buf[128];
    memset(st, 0, sizeof *st);
    if (!gFs || !gFs->exists(resolve(path, buf, sizeof buf))) {
        errno = ENOENT;
        return -1;
    }
    st->st_mode = S_IFREG;
    return 0;
}

int _unlink(const char* path) {
    char buf[128];
    if (!gFs || !gFs->remove(resolve(path, buf, sizeof buf))) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

int _link(const char* from, const char* to) {
    (void)from;
    (void)to;
    errno = EMLINK;
    return -1;
}

// newlib would build rename() from link + unlink; FatFs renames directly.
int rename(const char* from, const char* to) {
    char a[128], b[128];
    if (!gFs) return -1;
    const char* pf = resolve(from, a, sizeof a);
    const char* pt = resolve(to, b, sizeof b);
    if (gFs->exists(pt)) gFs->remove(pt);
    return gFs->rename(pf, pt) ? 0 : -1;
}

int mkdir(const char* path, mode_t mode) {
    (void)mode;
    char buf[128];
    char p[128];
    snprintf(p, sizeof p, "%s", resolve(path, buf, sizeof buf));
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') p[--n] = 0;   // Doom passes "savegames/"
    return gFs && gFs->mkdir(p) ? 0 : -1;
}

// Doom calls system() only for its GUI error box (skipped with -nogui).
int system(const char* cmd) {
    (void)cmd;
    return -1;
}

void _exit(int status) { pt_device_exit(status); }

}  // extern "C"
