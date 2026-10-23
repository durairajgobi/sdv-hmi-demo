// SPDX-License-Identifier: MIT
#include "ivshm/ivshm.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <ctime>

namespace ivshm {

// ---- mapping ---------------------------------------------------------------
Mapping::~Mapping() {
    if (data_) ::munmap(data_, size_);
}
Mapping::Mapping(Mapping &&o) noexcept : data_(o.data_), size_(o.size_), writable_(o.writable_) {
    o.data_ = nullptr;
}
Mapping &Mapping::operator=(Mapping &&o) noexcept {
    if (this != &o) {
        if (data_) ::munmap(data_, size_);
        data_ = o.data_; size_ = o.size_; writable_ = o.writable_;
        o.data_ = nullptr;
    }
    return *this;
}

static bool err(std::string *e, const std::string &what) {
    if (e) *e = what + ": " + std::strerror(errno);
    return false;
}

bool Mapping::create(const std::string &path, std::size_t size, std::string *error) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return err(error, "open " + path);
    // Truncate to 0 then to size: the kernel hands out zero-filled pages.
    const bool ok = ::ftruncate(fd, static_cast<off_t>(size)) == 0;
    ::close(fd);
    return ok ? true : err(error, "ftruncate " + path);
}

bool Mapping::open(const std::string &path, std::size_t size, bool writable, std::string *error) {
    const int fd = ::open(path.c_str(), (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd < 0) return err(error, "open " + path);
    struct stat st {};
    if (::fstat(fd, &st) != 0 || static_cast<std::size_t>(st.st_size) < size) {
        ::close(fd);
        if (error) *error = path + ": region smaller than expected layout";
        return false;
    }
    void *p = ::mmap(nullptr, size, PROT_READ | (writable ? PROT_WRITE : 0), MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) return err(error, "mmap " + path);
    if (data_) ::munmap(data_, size_);
    data_ = p;
    size_ = size;
    writable_ = writable;
    return true;
}

std::string regionPath(const std::string &dir, RegionKind kind) {
    switch (kind) {
    case RegionKind::Hypervisor: return dir + "/hv.shm";
    case RegionKind::Cluster: return dir + "/cluster.shm";
    case RegionKind::Ivi: return dir + "/ivi.shm";
    }
    return dir + "/unknown.shm";
}

std::size_t regionSize(RegionKind kind) {
    switch (kind) {
    case RegionKind::Hypervisor: return sizeof(HvRegion);
    case RegionKind::Cluster: return sizeof(ClusterRegion);
    case RegionKind::Ivi: return sizeof(IviRegion);
    }
    return 0;
}

void initHeader(RegionHeader &h, RegionKind kind, std::size_t size) {
    std::memcpy(h.magic, kMagic, sizeof kMagic);
    h.version = kVersion;
    h.kind = kind;
    h.size = size;
    h.generation.fetch_add(1, std::memory_order_release);
}

bool validHeader(const RegionHeader &h, RegionKind kind) {
    return std::memcmp(h.magic, kMagic, sizeof kMagic) == 0 && h.version == kVersion && h.kind == kind &&
           h.size == regionSize(kind) && h.generation.load(std::memory_order_acquire) != 0;
}

uint64_t monotonicNs() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

// ---- CRC-32 (IEEE 802.3, reflected 0xEDB88320), slicing-by-4 -----------------
static const std::array<std::array<uint32_t, 256>, 4> &crcTables() {
    static const auto tables = [] {
        std::array<std::array<uint32_t, 256>, 4> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i)
            for (int s = 1; s < 4; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
        return t;
    }();
    return tables;
}

uint32_t crc32(const void *data, std::size_t len, uint32_t seed) {
    const auto &t = crcTables();
    const auto *p = static_cast<const uint8_t *>(data);
    uint32_t c = ~seed;
    while (len >= 4) {
        c ^= uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        c = t[3][c & 0xFF] ^ t[2][(c >> 8) & 0xFF] ^ t[1][(c >> 16) & 0xFF] ^ t[0][c >> 24];
        p += 4;
        len -= 4;
    }
    while (len--) c = t[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

// ---- ring --------------------------------------------------------------------
bool ringPush(RingProducer &p, const RingConsumer &c, MsgType type, const void *payload, uint16_t len) {
    if (len > kMsgPayload) return false;
    const uint32_t head = p.head.load(std::memory_order_relaxed);
    const uint32_t tail = c.tail.load(std::memory_order_acquire);
    if (head - tail >= kRingSlots) {  // full (or consumer not attached yet)
        p.dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    MsgSlot &s = p.entries[head & (kRingSlots - 1)];
    s.type = static_cast<uint16_t>(type);
    s.len = len;
    if (len) std::memcpy(s.payload, payload, len);
    p.head.store(head + 1, std::memory_order_release);
    return true;
}

int ringPop(const RingProducer &p, RingConsumer &c, uint64_t producerGeneration,
            MsgSlot *out, int max, RingStats *stats) {
    const uint32_t head = p.head.load(std::memory_order_acquire);
    // Producer restarted: its ring starts over, skip anything stale.
    if (c.peerGeneration.load(std::memory_order_relaxed) != uint32_t(producerGeneration)) {
        c.tail.store(head, std::memory_order_release);
        c.peerGeneration.store(uint32_t(producerGeneration), std::memory_order_relaxed);
        return 0;
    }
    uint32_t tail = c.tail.load(std::memory_order_relaxed);
    const uint32_t avail = head - tail;
    if (avail > kRingSlots) {  // impossible for an honest producer: resync
        if (stats) ++stats->resyncs;
        c.tail.store(head, std::memory_order_release);
        return 0;
    }
    int n = 0;
    while (n < max && tail != head) {
        MsgSlot copy;
        std::memcpy(&copy, &p.entries[tail & (kRingSlots - 1)], sizeof copy);  // copy first...
        ++tail;
        if (copy.len > kMsgPayload) {  // ...then validate the private copy
            if (stats) ++stats->resyncs;
            continue;
        }
        out[n++] = copy;
    }
    c.tail.store(tail, std::memory_order_release);
    if (stats) stats->received += uint32_t(n);
    return n;
}

// ---- frame surface --------------------------------------------------------------
bool framePublish(FrameSurface &s, const uint8_t *pixels, uint32_t width, uint32_t height,
                  uint32_t frameNo, bool corruptCrc) {
    if (width == 0 || height == 0 || width > kMaxFrameW || height > kMaxFrameH) return false;
    const uint32_t idx = 1u - (s.latest.load(std::memory_order_relaxed) & 1u);
    FrameSlot &f = s.slot[idx];
    const std::size_t bytes = std::size_t(width) * height * 4;

    const uint64_t q = f.seq.load(std::memory_order_relaxed);
    f.seq.store(q + 1, std::memory_order_relaxed);  // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);
    f.width = width;
    f.height = height;
    f.stride = width * 4;
    f.format = PixelFormat::ARGB32Premultiplied;
    f.frameNo = frameNo;
    f.producedNs = monotonicNs();
    std::memcpy(f.pixels, pixels, bytes);
    f.crc32 = crc32(pixels, bytes) ^ (corruptCrc ? 0xDEADBEEFu : 0u);
    f.seq.store(q + 2, std::memory_order_release);

    s.latest.store(idx, std::memory_order_release);
    s.published.fetch_add(1, std::memory_order_relaxed);
    return true;
}

ReadResult frameRead(const FrameSurface &s, uint8_t *dst, uint32_t lastFrameNo, FrameCopy &meta) {
    const uint32_t idx = s.latest.load(std::memory_order_acquire);
    if (idx > 1) return ReadResult::Invalid;
    const FrameSlot &f = s.slot[idx];

    const uint64_t s1 = f.seq.load(std::memory_order_acquire);
    if (s1 == 0) return ReadResult::NoNewFrame;  // never written
    if (s1 & 1) return ReadResult::Torn;

    const uint32_t frameNo = f.frameNo, w = f.width, h = f.height, stride = f.stride, crc = f.crc32;
    const PixelFormat fmt = f.format;
    const uint64_t produced = f.producedNs;
    if (frameNo == lastFrameNo) return ReadResult::NoNewFrame;

    if (w == 0 || h == 0 || w > kMaxFrameW || h > kMaxFrameH || stride != w * 4 ||
        fmt != PixelFormat::ARGB32Premultiplied) {
        std::atomic_thread_fence(std::memory_order_acquire);
        return f.seq.load(std::memory_order_relaxed) == s1 ? ReadResult::Invalid : ReadResult::Torn;
    }
    const std::size_t bytes = std::size_t(w) * h * 4;
    std::memcpy(dst, f.pixels, bytes);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (f.seq.load(std::memory_order_relaxed) != s1) return ReadResult::Torn;

    meta.width = w;
    meta.height = h;
    meta.frameNo = frameNo;
    meta.producedNs = produced;
    meta.crcOk = crc32(dst, bytes) == crc;  // validated on the private copy
    return meta.crcOk ? ReadResult::Ok : ReadResult::CrcMismatch;
}

} // namespace ivshm
