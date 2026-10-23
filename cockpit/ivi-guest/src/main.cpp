// sdv-ivi-guest: the infotainment (IVI) partition.
//
// Stand-in for an Android Automotive guest VM: a QM (non-safety) partition that
// "projects" turn-by-turn navigation into the safety-relevant cluster. It
// renders the guidance tile itself and publishes it into a shared-memory frame
// surface (like a virtio-gpu / shared-buffer projection path), plus structured
// guidance data on a message ring. It receives vehicle speed from the cluster.
//
// Fault injection (sent by `sdv-hypervisor --inject ivi:<fault>`):
//   SIGUSR1      toggle CPU storm (busy threads on every CPU it may use)
//   SIGUSR2      toggle frame corruption (bad CRC)
//   SIGRTMIN     memory bomb (allocate until the partition's RAM limit)
//   SIGRTMIN+1   toggle message flood (ring overload)
//   SIGSTOP      guest hang (sent by the hypervisor CLI directly)
#include <QDateTime>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <thread>
#include <vector>

#include "ivshm/ivshm.h"
#include "ivshm/messages.h"

using namespace ivshm;

namespace {
std::atomic<bool> g_toggleStorm{false}, g_toggleCorrupt{false}, g_memBomb{false}, g_toggleFlood{false};
extern "C" void onFault(int sig) {
    if (sig == SIGUSR1) g_toggleStorm = true;
    else if (sig == SIGUSR2) g_toggleCorrupt = true;
    else if (sig == SIGRTMIN) g_memBomb = true;
    else if (sig == SIGRTMIN + 1) g_toggleFlood = true;
}

struct Leg {
    Maneuver maneuver;
    float meters;
    const char *street;
};
// A loop through Chennai, ending at the airport
const Leg kRoute[] = {
    {Maneuver::Straight, 900, "Anna Salai"},
    {Maneuver::Right, 450, "Cathedral Road"},
    {Maneuver::Left, 650, "TTK Road"},
    {Maneuver::Roundabout, 1200, "Kathipara Junction"},
    {Maneuver::Right, 1500, "GST Road"},
    {Maneuver::Arrive, 400, "Chennai Airport T1"},
};
constexpr int kLegs = sizeof(kRoute) / sizeof(kRoute[0]);
constexpr int kW = 440, kH = 260;

void logf(const char *msg, const QString &a = {}) {
    std::printf("[ivi-guest] %s %s\n", msg, qPrintable(a));
    std::fflush(stdout);
}

class IviGuest : public QObject {
public:
    bool init() {
        const std::string dir = qEnvironmentVariable("COCKPIT_SHM_DIR", "/dev/shm/sdv-cockpit").toStdString();
        std::string e;
        if (!own_.open(regionPath(dir, RegionKind::Ivi), sizeof(IviRegion), true, &e) ||
            !peer_.open(regionPath(dir, RegionKind::Cluster), sizeof(ClusterRegion), false, &e)) {
            logf("cannot map shared memory:", QString::fromStdString(e));
            return false;
        }
        me_ = own_.as<IviRegion>();
        cluster_ = peer_.as<ClusterRegion>();
        initHeader(me_->hdr, RegionKind::Ivi, sizeof(IviRegion));
        me_->hb.pid.store(uint32_t(::getpid()));
        me_->hb.state.store(uint32_t(GuestState::Booting));
        logf("partition up, generation", QString::number(me_->hdr.generation.load()));

        connect(&tick_, &QTimer::timeout, this, [this] { step(); });
        tick_.start(50);  // 20 fps projection
        frame_ = QImage(kW, kH, QImage::Format_ARGB32_Premultiplied);
        return true;
    }

private:
    void step() {
        const uint64_t now = monotonicNs();
        const double dt = lastNs_ ? (now - lastNs_) / 1e9 : 0.05;
        lastNs_ = now;
        handleFaults();
        receive();

        // advance along the route
        left_ -= float(speedKmh_ / 3.6 * dt);
        if (left_ <= 0) {
            leg_ = (leg_ + 1) % kLegs;
            left_ = kRoute[leg_].meters;
        }

        render();
        framePublish(me_->surface, frame_.constBits(), kW, kH, ++frameNo_, corrupt_);
        if (frameNo_ % 5 == 0 || flood_) sendGuidance(flood_ ? 400 : 1);

        me_->hb.counter.fetch_add(1, std::memory_order_release);
        me_->hb.monotonicNs.store(now);
        me_->hb.state.store(uint32_t(GuestState::Running), std::memory_order_release);
    }

    void receive() {
        if (!validHeader(cluster_->hdr, RegionKind::Cluster)) return;  // cluster not up (yet)
        MsgSlot msgs[64];
        const int n = ringPop(cluster_->toIvi, me_->fromCluster, cluster_->hdr.generation.load(), msgs, 64);
        for (int i = 0; i < n; ++i) {
            VehicleSpeedMsg v;
            if (decode(msgs[i], v)) speedKmh_ = v.kmh;
        }
    }

    double remainingMeters() const {
        double m = left_;
        for (int i = leg_ + 1; i < kLegs; ++i) m += kRoute[i].meters;
        return m;
    }

    QString etaText() const {
        const double avg = std::max(25.0, speedKmh_);
        const auto eta = QDateTime::currentDateTime().addSecs(qint64(remainingMeters() / (avg / 3.6)));
        return QStringLiteral("ETA %1 · %2 km").arg(eta.toString("hh:mm")).arg(remainingMeters() / 1000.0, 0, 'f', 1);
    }

    void sendGuidance(int copies) {
        NavGuidanceMsg g;
        g.maneuver = kRoute[leg_].maneuver;
        g.meters = left_;
        g.street = kRoute[leg_].street;
        g.eta = etaText().toStdString();
        uint8_t buf[kMsgPayload];
        const uint16_t len = encode(g, buf);
        for (int i = 0; i < copies; ++i) ringPush(me_->toCluster, cluster_->fromIvi, MsgType::NavGuidance, buf, len);
    }

    void handleFaults() {
        if (g_toggleCorrupt.exchange(false)) {
            corrupt_ = !corrupt_;
            logf(corrupt_ ? "FAULT: publishing corrupt frames" : "fault cleared: frames valid");
        }
        if (g_toggleFlood.exchange(false)) {
            flood_ = !flood_;
            logf(flood_ ? "FAULT: flooding the inter-VM ring" : "fault cleared: flood stopped");
        }
        if (g_toggleStorm.exchange(false)) {
            if (storm_.empty()) {
                stormStop_ = false;
                const unsigned n = std::max(2u, std::thread::hardware_concurrency()) * 2;
                for (unsigned i = 0; i < n; ++i)
                    storm_.emplace_back([this] { volatile uint64_t x = 0; while (!stormStop_) ++x; });
                logf("FAULT: CPU storm, busy threads:", QString::number(n));
            } else {
                stormStop_ = true;
                for (auto &t : storm_) t.join();
                storm_.clear();
                logf("fault cleared: CPU storm stopped");
            }
        }
        if (g_memBomb.exchange(false)) {
            logf("FAULT: memory bomb - allocating until the partition limit");
            std::vector<char *> hog;
            for (;;) {  // no catch: bad_alloc terminates the guest, as an OOM would
                char *p = new char[32 << 20];
                std::fill(p, p + (32 << 20), char(1));
                hog.push_back(p);
            }
        }
    }

    void drawArrow(QPainter &p, const QRectF &r, Maneuver m) {
        p.save();
        p.translate(r.center());
        const double s = r.width() / 100.0;
        p.scale(s, s);
        QPen pen(QColor("#ffffff"), 11, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPainterPath path;
        auto head = [&](QPointF tip, double angleDeg) {
            p.save();
            p.translate(tip);
            p.rotate(angleDeg);
            QPainterPath h;
            h.moveTo(0, -16); h.lineTo(14, 4); h.lineTo(-14, 4); h.closeSubpath();
            p.setPen(Qt::NoPen);
            p.setBrush(QColor("#ffffff"));
            p.drawPath(h);
            p.restore();
        };
        switch (m) {
        case Maneuver::Straight:
            path.moveTo(0, 40); path.lineTo(0, -28); p.drawPath(path); head({0, -36}, 0); break;
        case Maneuver::Left:
            path.moveTo(10, 40); path.lineTo(10, 0); path.quadTo(10, -18, -8, -18); path.lineTo(-24, -18);
            p.drawPath(path); head({-32, -18}, -90); break;
        case Maneuver::Right:
            path.moveTo(-10, 40); path.lineTo(-10, 0); path.quadTo(-10, -18, 8, -18); path.lineTo(24, -18);
            p.drawPath(path); head({32, -18}, 90); break;
        case Maneuver::Roundabout:
            p.drawEllipse(QPointF(0, -4), 20, 20);
            path.moveTo(0, 40); path.lineTo(0, 16); p.drawPath(path);
            path = QPainterPath(); path.moveTo(14, -18); path.lineTo(30, -34); p.drawPath(path);
            head({34, -38}, 45); break;
        case Maneuver::Arrive:
            p.drawEllipse(QPointF(0, -10), 18, 18);
            path.moveTo(0, 8); path.lineTo(0, 40); p.drawPath(path); break;
        }
        p.restore();
    }

    void render() {
        frame_.fill(Qt::transparent);
        QPainter p(&frame_);
        p.setRenderHint(QPainter::Antialiasing);
        // card
        QLinearGradient bg(0, 0, 0, kH);
        bg.setColorAt(0, QColor("#16233a"));
        bg.setColorAt(1, QColor("#0b1322"));
        p.setPen(QPen(QColor("#2d4a7a"), 2));
        p.setBrush(bg);
        p.drawRoundedRect(QRectF(1, 1, kW - 2, kH - 2), 22, 22);

        // maneuver badge
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#1e6bff"));
        p.drawRoundedRect(QRectF(22, 24, 128, 128), 20, 20);
        drawArrow(p, QRectF(22, 24, 128, 128), kRoute[leg_].maneuver);

        // distance + street
        QFont big("DejaVu Sans", 46);
        big.setWeight(QFont::Light);
        QFont mid("DejaVu Sans", 20);
        QFont small("DejaVu Sans", 13);
        p.setPen(QColor("#ffffff"));
        p.setFont(big);
        const QString dist = left_ >= 1000 ? QString::number(left_ / 1000.0, 'f', 1) + " km"
                                           : QString::number(int(left_ / 10) * 10) + " m";
        p.drawText(QRectF(170, 26, kW - 186, 70), Qt::AlignLeft | Qt::AlignVCenter, dist);
        p.setFont(mid);
        p.setPen(QColor("#c9d6ea"));
        const Maneuver m = kRoute[leg_].maneuver;
        const QString verb = m == Maneuver::Arrive ? "Arrive at" : m == Maneuver::Straight ? "Continue on"
                           : m == Maneuver::Roundabout ? "Roundabout to" : "Turn onto";
        p.drawText(QRectF(170, 96, kW - 186, 56), Qt::AlignLeft | Qt::TextWordWrap,
                   verb + QStringLiteral(" ") + QString::fromUtf8(kRoute[leg_].street));

        // progress strip + ETA
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#22324d"));
        p.drawRoundedRect(QRectF(22, 176, kW - 44, 8), 4, 4);
        const double total = kRoute[leg_].meters;
        p.setBrush(QColor("#4c8dff"));
        p.drawRoundedRect(QRectF(22, 176, (kW - 44) * (1.0 - left_ / total), 8), 4, 4);
        p.setPen(QColor("#9fb2cf"));
        p.setFont(mid);
        p.drawText(QRectF(22, 196, kW - 44, 30), Qt::AlignLeft | Qt::AlignVCenter, etaText());

        p.setFont(small);
        p.setPen(QColor("#5c7090"));
        p.drawText(QRectF(22, 228, kW - 44, 22), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("IVI partition · projected frame #%1").arg(frameNo_));
    }

    Mapping own_, peer_;
    IviRegion *me_ = nullptr;
    const ClusterRegion *cluster_ = nullptr;
    QTimer tick_;
    QImage frame_;
    uint32_t frameNo_ = 0;
    uint64_t lastNs_ = 0;
    double speedKmh_ = 0;
    int leg_ = 0;
    float left_ = kRoute[0].meters;
    bool corrupt_ = false, flood_ = false;
    std::vector<std::thread> storm_;
    std::atomic<bool> stormStop_{false};
};
} // namespace

int main(int argc, char *argv[]) {
    // The IVI renders only into shared memory; never inherit the cluster's display
    qputenv("QT_QPA_PLATFORM", qEnvironmentVariable("IVI_QPA_PLATFORM", "offscreen").toUtf8());
    QGuiApplication app(argc, argv);
    for (int s : {SIGUSR1, SIGUSR2, SIGRTMIN, SIGRTMIN + 1}) std::signal(s, onFault);
    IviGuest guest;
    if (!guest.init()) return 1;
    return app.exec();
}
