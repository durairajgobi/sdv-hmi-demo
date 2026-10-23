// Unit tests for the inter-VM shared memory channel.
#include <QtTest>

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <memory>
#include <thread>
#include <vector>

#include "ivshm/ivshm.h"

using namespace ivshm;

class TestIvshm : public QObject {
    Q_OBJECT
private slots:
    void crcKnownVector() {
        QCOMPARE(crc32("123456789", 9), 0xCBF43926u);
        QCOMPARE(crc32("", 0), 0u);
        const char odd[] = "abcdefg";  // exercises slicing tail
        QCOMPARE(crc32(odd, 7), 0x312A6AA6u);
    }

    void ringRoundTripAndOrder() {
        auto p = std::make_unique<RingProducer>();
        auto c = std::make_unique<RingConsumer>();
        MsgSlot out[8];
        QCOMPARE(ringPop(*p, *c, 1, out, 8), 0);  // attaches to generation 1
        for (uint8_t i = 0; i < 5; ++i) QVERIFY(ringPush(*p, *c, MsgType::VehicleSpeed, &i, 1));
        QCOMPARE(ringPop(*p, *c, 1, out, 8), 5);
        for (uint8_t i = 0; i < 5; ++i) QCOMPARE(out[i].payload[0], i);
    }

    void ringFullCountsDrops() {
        auto p = std::make_unique<RingProducer>();
        auto c = std::make_unique<RingConsumer>();
        MsgSlot out[1];
        ringPop(*p, *c, 1, out, 1);
        for (uint32_t i = 0; i < kRingSlots; ++i) QVERIFY(ringPush(*p, *c, MsgType::VehicleSpeed, nullptr, 0));
        QVERIFY(!ringPush(*p, *c, MsgType::VehicleSpeed, nullptr, 0));
        QCOMPARE(p->dropped.load(), 1u);
    }

    void ringWrapsAt32Bits() {
        auto p = std::make_unique<RingProducer>();
        auto c = std::make_unique<RingConsumer>();
        p->head = 0xFFFFFFF0u;
        c->tail = 0xFFFFFFF0u;
        c->peerGeneration = 7;
        std::vector<MsgSlot> out(64);
        for (uint32_t i = 0; i < 40; ++i) QVERIFY(ringPush(*p, *c, MsgType::NavGuidance, &i, 4));
        QCOMPARE(ringPop(*p, *c, 7, out.data(), 64), 40);
        uint32_t last;
        std::memcpy(&last, out[39].payload, 4);
        QCOMPARE(last, 39u);
    }

    void ringResyncsOnProducerRestart() {
        auto p = std::make_unique<RingProducer>();
        auto c = std::make_unique<RingConsumer>();
        MsgSlot out[8];
        ringPop(*p, *c, 1, out, 8);
        ringPush(*p, *c, MsgType::VehicleSpeed, nullptr, 0);
        // producer restarts with a new generation; stale content must be skipped
        QCOMPARE(ringPop(*p, *c, 2, out, 8), 0);
        ringPush(*p, *c, MsgType::NavGuidance, nullptr, 0);
        QCOMPARE(ringPop(*p, *c, 2, out, 8), 1);
        QCOMPARE(out[0].type, uint16_t(MsgType::NavGuidance));
    }

    void ringSurvivesHostileProducer() {
        auto p = std::make_unique<RingProducer>();
        auto c = std::make_unique<RingConsumer>();
        MsgSlot out[8];
        RingStats st;
        ringPop(*p, *c, 1, out, 8, &st);
        p->head = 123456;  // garbage index
        QCOMPARE(ringPop(*p, *c, 1, out, 8, &st), 0);
        QCOMPARE(st.resyncs, 1u);
        // oversized length field is dropped, valid neighbour still delivered
        ringPush(*p, *c, MsgType::VehicleSpeed, nullptr, 0);
        p->entries[(p->head - 1) & (kRingSlots - 1)].len = 9999;
        ringPush(*p, *c, MsgType::NavGuidance, nullptr, 0);
        QCOMPARE(ringPop(*p, *c, 1, out, 8, &st), 1);
        QCOMPARE(out[0].type, uint16_t(MsgType::NavGuidance));
        QCOMPARE(st.resyncs, 2u);
    }

    void frameRoundTripAndValidation() {
        auto s = std::make_unique<FrameSurface>();
        std::vector<uint8_t> px(64 * 32 * 4, 0x5A), dst(kMaxFrameBytes);
        FrameCopy meta;
        QCOMPARE(frameRead(*s, dst.data(), 0, meta), ReadResult::NoNewFrame);
        QVERIFY(framePublish(*s, px.data(), 64, 32, 1));
        QCOMPARE(frameRead(*s, dst.data(), 0, meta), ReadResult::Ok);
        QCOMPARE(meta.width, 64u);
        QCOMPARE(meta.frameNo, 1u);
        QCOMPARE(dst[100], uint8_t(0x5A));
        QCOMPARE(frameRead(*s, dst.data(), 1, meta), ReadResult::NoNewFrame);

        QVERIFY(framePublish(*s, px.data(), 64, 32, 2, /*corruptCrc*/ true));
        QCOMPARE(frameRead(*s, dst.data(), 1, meta), ReadResult::CrcMismatch);

        QVERIFY(!framePublish(*s, px.data(), kMaxFrameW + 1, 2, 3));  // writer-side bound
        s->slot[s->latest].width = 100000;                              // hostile metadata
        s->slot[s->latest].frameNo = 77;
        QCOMPARE(frameRead(*s, dst.data(), 2, meta), ReadResult::Invalid);
    }

    // A reader racing a writer must never accept a torn frame.
    void seqlockNeverReturnsTornFrame() {
        auto s = std::make_unique<FrameSurface>();
        constexpr uint32_t W = 320, H = 200;
        std::atomic<bool> stop{false};
        std::thread writer([&] {
            std::vector<uint8_t> px(W * H * 4);
            for (uint32_t n = 1; n <= 3000; ++n) {
                std::fill(px.begin(), px.end(), uint8_t(n));
                framePublish(*s, px.data(), W, H, n);
            }
            stop = true;
        });
        std::vector<uint8_t> dst(kMaxFrameBytes);
        uint32_t last = 0, ok = 0, torn = 0, bad = 0;
        while (!stop) {
            FrameCopy m;
            const auto r = frameRead(*s, dst.data(), last, m);
            if (r == ReadResult::Ok) {
                ++ok;
                last = m.frameNo;
                const uint8_t v = uint8_t(m.frameNo);
                for (uint32_t i = 0; i < W * H * 4; i += 997)
                    if (dst[i] != v) { ++bad; break; }
            } else if (r == ReadResult::Torn || r == ReadResult::CrcMismatch) {
                ++torn;  // detected and rejected: correct behaviour
            }
        }
        writer.join();
        qInfo("accepted %u frames, rejected %u torn reads", ok, torn);
        QVERIFY(ok > 10);
        QCOMPARE(bad, 0u);
    }

    // Read-only mappings are enforced by the MMU, like stage-2 permissions.
    void readOnlyMappingFaultsOnWrite() {
        QTemporaryDir dir;
        const std::string path = regionPath(dir.path().toStdString(), RegionKind::Ivi);
        std::string e;
        QVERIFY2(Mapping::create(path, regionSize(RegionKind::Ivi), &e), e.c_str());
        Mapping owner, peer;
        QVERIFY(owner.open(path, regionSize(RegionKind::Ivi), true, &e));
        QVERIFY(peer.open(path, regionSize(RegionKind::Ivi), false, &e));
        initHeader(owner.as<IviRegion>()->hdr, RegionKind::Ivi, regionSize(RegionKind::Ivi));
        QVERIFY(validHeader(peer.as<IviRegion>()->hdr, RegionKind::Ivi));   // shared view
        QVERIFY(!validHeader(peer.as<IviRegion>()->hdr, RegionKind::Cluster));

        const pid_t pid = fork();
        if (pid == 0) {
            std::signal(SIGSEGV, SIG_DFL);  // bypass QtTest crash handler in the child
            peer.as<IviRegion>()->hb.counter.store(42);  // must SIGSEGV
            _exit(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        QVERIFY(WIFSIGNALED(status));
        QCOMPARE(WTERMSIG(status), SIGSEGV);
        QCOMPARE(owner.as<IviRegion>()->hb.counter.load(), 0ull);
    }
};

QTEST_APPLESS_MAIN(TestIvshm)
#include "tst_ivshm.moc"
