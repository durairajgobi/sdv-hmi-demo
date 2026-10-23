// SPDX-License-Identifier: MIT
// ivshm: inter-VM shared memory channel for a mixed-criticality cockpit.
//
// Models what a type-1 hypervisor (QNX Hypervisor, Xen, ACRN, Gunyah, COQOS)
// exposes between guests: plain shared memory pages (cf. QEMU ivshmem /
// virtio shmem), mapped with per-guest permissions by stage-2 page tables.
//
// Rule enforced here: EACH REGION HAS EXACTLY ONE WRITER. Every partition maps
// its own region read-write and all other regions read-only (PROT_READ), so a
// misbehaving guest physically cannot scribble on another guest's memory.
// Consequently an SPSC ring is split: the producer's head + slots live in the
// producer's region, the consumer's tail lives in the consumer's region.
//
//   hv region       written by the hypervisor   partition table / states
//   cluster region  written by the cluster VM   heartbeat, cluster->ivi ring, rx cursor
//   ivi region      written by the IVI VM       heartbeat, ivi->cluster ring, rx cursor,
//                                               double-buffered frame surface
//
// Everything a reader takes from another guest's region is UNTRUSTED: copy
// first, then validate (indices, lengths, dimensions, CRC) -- never act on
// data in place (TOCTOU).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ivshm {

constexpr uint32_t kVersion = 1;
constexpr char kMagic[8] = {'S', 'D', 'V', 'I', 'V', 'S', 'H', 'M'};

constexpr uint32_t kMsgPayload = 124;
constexpr uint32_t kRingSlots = 256;  // power of two
constexpr uint32_t kMaxFrameW = 640;
constexpr uint32_t kMaxFrameH = 400;
constexpr uint32_t kMaxFrameBytes = kMaxFrameW * kMaxFrameH * 4;
constexpr int kMaxPartitions = 4;

static_assert(std::atomic<uint64_t>::is_always_lock_free, "need address-free 64-bit atomics");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "need address-free 32-bit atomics");
static_assert((kRingSlots & (kRingSlots - 1)) == 0, "ring size must be power of two");

enum class RegionKind : uint32_t { Hypervisor = 1, Cluster = 2, Ivi = 3 };

struct RegionHeader {
    char magic[8];
    uint32_t version;
    RegionKind kind;
    uint64_t size;
    std::atomic<uint64_t> generation;  // bumped by the owner on every (re)start
};

// Guest liveness, written by the guest's main/render loop.
enum class GuestState : uint32_t { Down = 0, Booting = 1, Running = 2 };
struct alignas(64) Heartbeat {
    std::atomic<uint64_t> counter;
    std::atomic<uint64_t> monotonicNs;
    std::atomic<uint32_t> state;
    std::atomic<uint32_t> pid;
};

// ---- message ring (producer half / consumer half) -------------------------
enum class MsgType : uint16_t {
    VehicleSpeed = 1,    // cluster -> ivi : f32 km/h, u8 gear
    NavGuidance = 2,     // ivi -> cluster : u8 maneuver, f32 meters, u8 len + street, u8 len + eta
};

struct MsgSlot {
    uint16_t type;
    uint16_t len;
    uint8_t payload[kMsgPayload];
};

struct alignas(64) RingProducer {
    std::atomic<uint32_t> head;     // next slot to write (monotonic, wraps at 2^32)
    std::atomic<uint32_t> dropped;  // messages dropped because the consumer lagged
    MsgSlot entries[kRingSlots];  // not "slots": Qt defines that as a macro
};

struct alignas(64) RingConsumer {
    std::atomic<uint32_t> tail;      // next slot to read
    std::atomic<uint32_t> peerGeneration;  // producer generation this cursor belongs to
};

// ---- frame surface (double buffer, per-slot seqlock) ----------------------
enum class PixelFormat : uint32_t { ARGB32Premultiplied = 1 };

struct alignas(64) FrameSlot {
    std::atomic<uint64_t> seq;  // odd while being written
    uint32_t width, height, stride;
    PixelFormat format;
    uint32_t crc32;             // over width*height*4 pixel bytes
    uint32_t frameNo;
    uint64_t producedNs;
    alignas(64) uint8_t pixels[kMaxFrameBytes];
};

struct alignas(64) FrameSurface {
    std::atomic<uint32_t> latest;  // index of the most recently completed slot
    std::atomic<uint32_t> published;
    FrameSlot slot[2];
};

// ---- regions ----------------------------------------------------------------
enum class PartitionState : uint32_t {
    Stopped = 0, Booting = 1, Running = 2, Hung = 3, Crashed = 4, Restarting = 5
};

struct alignas(64) PartitionInfo {
    char name[16];
    char criticality[16];
    std::atomic<uint32_t> state;
    std::atomic<uint32_t> pid;
    std::atomic<uint32_t> restarts;
    std::atomic<uint64_t> startedNs;
    std::atomic<uint64_t> lastRecoveryMs;  // spawn -> first Running heartbeat
};

struct HvRegion {
    RegionHeader hdr;
    std::atomic<uint32_t> partitionCount;
    PartitionInfo partitions[kMaxPartitions];
};

struct ClusterRegion {
    RegionHeader hdr;
    Heartbeat hb;
    RingProducer toIvi;
    RingConsumer fromIvi;
};

struct IviRegion {
    RegionHeader hdr;
    Heartbeat hb;
    RingProducer toCluster;
    RingConsumer fromCluster;
    FrameSurface surface;
};

// ---- mapping ------------------------------------------------------------------
class Mapping {
public:
    Mapping() = default;
    ~Mapping();
    Mapping(Mapping &&o) noexcept;
    Mapping &operator=(Mapping &&o) noexcept;
    Mapping(const Mapping &) = delete;
    Mapping &operator=(const Mapping &) = delete;

    // Hypervisor: create (or truncate) and zero a region file.
    static bool create(const std::string &path, std::size_t size, std::string *error);
    // Map an existing region. writable=false maps PROT_READ only.
    bool open(const std::string &path, std::size_t size, bool writable, std::string *error);

    void *data() const { return data_; }
    bool isOpen() const { return data_ != nullptr; }
    bool writable() const { return writable_; }
    template <typename T> T *as() const { return static_cast<T *>(data_); }

private:
    void *data_ = nullptr;
    std::size_t size_ = 0;
    bool writable_ = false;
};

std::string regionPath(const std::string &dir, RegionKind kind);
std::size_t regionSize(RegionKind kind);

// Owner side: stamp header and bump generation. Called on each guest start.
void initHeader(RegionHeader &h, RegionKind kind, std::size_t size);
// Reader side: does this mapped memory look like a valid region of `kind`?
bool validHeader(const RegionHeader &h, RegionKind kind);

uint64_t monotonicNs();
uint32_t crc32(const void *data, std::size_t len, uint32_t seed = 0);

// ---- ring operations ------------------------------------------------------------
// Producer (owner of RingProducer). Returns false and counts a drop if full.
bool ringPush(RingProducer &p, const RingConsumer &c, MsgType type, const void *payload, uint16_t len);

struct RingStats {
    uint32_t received = 0;
    uint32_t resyncs = 0;   // corrupt/out-of-range indices from the producer
};
// Consumer (owner of RingConsumer): copies at most `max` messages into `out`.
// Validates producer indices and lengths; on nonsense it resynchronises.
int ringPop(const RingProducer &p, RingConsumer &c, uint64_t producerGeneration,
            MsgSlot *out, int max, RingStats *stats = nullptr);

// ---- frame operations -----------------------------------------------------------
struct FrameCopy {
    uint32_t width = 0, height = 0, frameNo = 0;
    uint64_t producedNs = 0;
    bool crcOk = false;
};
enum class ReadResult { NoNewFrame, Ok, Torn, Invalid, CrcMismatch };

// Writer: publishes one ARGB32 frame (stride must equal width*4).
bool framePublish(FrameSurface &s, const uint8_t *pixels, uint32_t width, uint32_t height,
                  uint32_t frameNo, bool corruptCrc = false);
// Reader: copies the latest frame into `dst` (capacity kMaxFrameBytes) if its
// frameNo differs from lastFrameNo, then validates it on the private copy.
ReadResult frameRead(const FrameSurface &s, uint8_t *dst, uint32_t lastFrameNo, FrameCopy &meta);

} // namespace ivshm
