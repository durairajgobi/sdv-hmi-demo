#include "cockpitlink.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QMetaEnum>
#include <cstring>
#include <unistd.h>

#include "ivshm/messages.h"

Q_LOGGING_CATEGORY(lcCockpit, "sdv.cockpit")

using namespace ivshm;

static qint64 nowMs() { return qint64(monotonicNs() / 1000000ull); }

// ============================================================================
// Worker thread
// ============================================================================
void CockpitImporter::startPolling() {
    buf_.resize(kMaxFrameBytes);
    timer_ = new QTimer(this);
    timer_->setTimerType(Qt::PreciseTimer);
    connect(timer_, &QTimer::timeout, this, &CockpitImporter::poll);
    timer_->start(10);
}

void CockpitImporter::poll() {
    if (!validHeader(ivi_->hdr, RegionKind::Ivi)) return;  // IVI partition not up
    const uint64_t gen = ivi_->hdr.generation.load(std::memory_order_acquire);
    if (gen != lastGeneration_) {  // IVI (re)booted
        lastGeneration_ = gen;
        lastFrameNo_ = 0;
    }

    FrameCopy meta;
    switch (frameRead(ivi_->surface, buf_.data(), lastFrameNo_, meta)) {
    case ReadResult::Ok: {
        lastFrameNo_ = meta.frameNo;
        QImage img(buf_.data(), int(meta.width), int(meta.height), int(meta.width * 4),
                   QImage::Format_ARGB32_Premultiplied);
        emit frame(img.copy(), meta.frameNo);  // deep copy: buffer is reused
        break;
    }
    case ReadResult::CrcMismatch:
        lastFrameNo_ = meta.frameNo;
        emit rejected(QStringLiteral("CRC mismatch on frame %1").arg(meta.frameNo));
        break;
    case ReadResult::Invalid:
        emit rejected(QStringLiteral("invalid frame metadata"));
        break;
    case ReadResult::Torn:       // writer mid-update: simply retry next poll
    case ReadResult::NoNewFrame:
        break;
    }

    // Drain guidance messages; bounded work per poll even under a flood.
    MsgSlot msgs[64];
    const uint32_t resyncsBefore = ringStats_.resyncs;
    const int n = ringPop(ivi_->toCluster, me_->fromIvi, gen, msgs, 64, &ringStats_);
    if (ringStats_.resyncs != resyncsBefore) emit rejected(QStringLiteral("malformed ring data"));
    for (int i = n - 1; i >= 0; --i) {  // only the newest guidance matters
        NavGuidanceMsg g;
        if (decode(msgs[i], g)) {
            emit guidance(int(g.maneuver), g.meters, QString::fromStdString(g.street), QString::fromStdString(g.eta));
            break;
        }
    }
}

// ============================================================================
// GUI thread
// ============================================================================
CockpitLink::CockpitLink(QObject *parent) : QObject(parent) {
    beatTimer_.setTimerType(Qt::PreciseTimer);
    connect(&beatTimer_, &QTimer::timeout, this, &CockpitLink::beat);
    connect(&evalTimer_, &QTimer::timeout, this, &CockpitLink::evaluate);
    connect(&attachTimer_, &QTimer::timeout, this, [this] {
        if (attach()) attachTimer_.stop();
    });
}

CockpitLink::~CockpitLink() {
    if (me_) me_->hb.state.store(uint32_t(GuestState::Down));
    worker_.quit();
    worker_.wait();
}

QString CockpitLink::iviStateName() const {
    return QString::fromLatin1(QMetaEnum::fromType<IviState>().valueToKey(state_));
}

void CockpitLink::start() {
    dir_ = qEnvironmentVariable("COCKPIT_SHM_DIR");
    if (dir_.isEmpty()) {
        qCInfo(lcCockpit) << "standalone cluster (no hypervisor shared memory configured)";
        return;
    }
    if (!attach()) attachTimer_.start(1000);
}

bool CockpitLink::attach() {
    const std::string d = dir_.toStdString();
    std::string e;
    if (!hv_.open(regionPath(d, RegionKind::Hypervisor), sizeof(HvRegion), false, &e) ||
        !validHeader(hv_.as<HvRegion>()->hdr, RegionKind::Hypervisor) ||
        !own_.open(regionPath(d, RegionKind::Cluster), sizeof(ClusterRegion), true, &e) ||
        !ivi_.open(regionPath(d, RegionKind::Ivi), sizeof(IviRegion), false, &e)) {
        qCWarning(lcCockpit) << "waiting for hypervisor shared memory:" << e.c_str();
        return false;
    }
    hvRegion_ = hv_.as<HvRegion>();
    me_ = own_.as<ClusterRegion>();
    iviRegion_ = ivi_.as<IviRegion>();
    initHeader(me_->hdr, RegionKind::Cluster, sizeof(ClusterRegion));
    me_->hb.pid.store(uint32_t(::getpid()));
    me_->hb.state.store(uint32_t(GuestState::Booting));

    importer_ = new CockpitImporter(iviRegion_, me_);
    importer_->moveToThread(&worker_);
    connect(&worker_, &QThread::finished, importer_, &QObject::deleteLater);
    connect(&worker_, &QThread::started, importer_, &CockpitImporter::startPolling);
    connect(importer_, &CockpitImporter::frame, this, &CockpitLink::onFrame);
    connect(importer_, &CockpitImporter::rejected, this, &CockpitLink::onRejected);
    connect(importer_, &CockpitImporter::guidance, this, &CockpitLink::onGuidance);
    worker_.setObjectName(QStringLiteral("ivi-import"));
    worker_.start(QThread::LowPriority);  // validation never competes with rendering

    beatTimer_.start(100);
    evalTimer_.start(100);
    available_ = true;
    emit availableChanged();
    qCInfo(lcCockpit).noquote() << "attached to hypervisor shared memory" << dir_
                                << "| cluster generation" << me_->hdr.generation.load();
    return true;
}

void CockpitLink::beat() {
    me_->hb.counter.fetch_add(1, std::memory_order_release);
    me_->hb.monotonicNs.store(monotonicNs());
    me_->hb.state.store(uint32_t(rendering_ ? GuestState::Running : GuestState::Booting), std::memory_order_release);
}

void CockpitLink::setUiRendering(bool r) {
    rendering_ = r;
    if (me_) beat();
}

void CockpitLink::setSpeed(double kmh, const QString &gear) {
    if (!me_) return;
    const qint64 now = nowMs();
    if (now - lastSpeedPushMs_ < 100) return;  // 10 Hz is plenty for the IVI
    lastSpeedPushMs_ = now;
    VehicleSpeedMsg m;
    m.kmh = float(kmh);
    m.gear = uint8_t(QStringLiteral("PRND").indexOf(gear.isEmpty() ? QChar('P') : gear.at(0)));
    if (m.gear > 3) m.gear = 0;
    uint8_t buf[kMsgPayload];
    ringPush(me_->toIvi, iviRegion_->fromCluster, MsgType::VehicleSpeed, buf, encode(m, buf));
}

QImage CockpitLink::currentFrame() const {
    QMutexLocker l(&frameMutex_);
    return frame_;
}

void CockpitLink::onFrame(const QImage &img, quint32) {
    {
        QMutexLocker l(&frameMutex_);
        frame_ = img;
    }
    lastFrameMs_ = nowMs();
    emit frameChanged();
}

void CockpitLink::onRejected(const QString &reason) {
    static qint64 lastLog = 0;
    ++rejected_;
    lastReject_ = reason;
    lastRejectMs_ = nowMs();
    emit statsChanged();
    if (nowMs() - lastLog > 1000) {
        lastLog = nowMs();
        qCWarning(lcCockpit).noquote() << "rejected IVI data:" << reason << "| total rejected" << rejected_;
    }
}

void CockpitLink::onGuidance(int maneuver, double meters, const QString &street, const QString &eta) {
    maneuver_ = maneuver;
    distance_ = meters;
    street_ = street;
    eta_ = eta;
    lastGuidanceMs_ = nowMs();
    emit guidanceChanged();
}

void CockpitLink::evaluate() {
    // Partition view from the hypervisor (read-only)
    QString pstate;
    int restarts = 0;
    for (uint32_t i = 0; i < hvRegion_->partitionCount.load() && i < uint32_t(kMaxPartitions); ++i) {
        const auto &p = hvRegion_->partitions[i];
        if (std::strncmp(p.name, "ivi", sizeof p.name) == 0) {
            static const char *names[] = {"Stopped", "Booting", "Running", "Hung", "Crashed", "Restarting"};
            const uint32_t s = p.state.load();
            pstate = QString::fromLatin1(s < 6 ? names[s] : "?");
            restarts = int(p.restarts.load());
        }
    }
    if (pstate != partitionState_ || restarts != restarts_) {
        if (pstate != partitionState_)
            qCInfo(lcCockpit).noquote() << "hypervisor reports IVI partition" << pstate << "(restarts" << restarts << ")";
        partitionState_ = pstate;
        restarts_ = restarts;
        emit partitionChanged();
    }

    const qint64 now = nowMs();
    // Ring overload is absorbed at the producer (bounded ring, drops counted there).
    const uint32_t dropped = iviRegion_->toCluster.dropped.load(std::memory_order_relaxed);
    if (dropped != lastDropped_ && now - lastDropLogMs_ > 2000) {
        if (dropped > lastDropped_)
            qCWarning(lcCockpit) << "IVI ring overload:" << (dropped - lastDropped_)
                                 << "messages dropped at the IVI side; cluster work stays bounded";
        lastDropped_ = dropped;
        lastDropLogMs_ = now;
    }
    const bool framesFresh = lastFrameMs_ && now - lastFrameMs_ < 500;
    const bool guidanceFresh = lastGuidanceMs_ && now - lastGuidanceMs_ < 1000;
    int s;
    if (framesFresh) s = Healthy;
    else if (guidanceFresh) s = Degraded;
    else if (!everHealthy_) s = Starting;
    else s = Unavailable;
    if (s == Healthy) everHealthy_ = true;

    if (s != state_) {
        QString why;
        if (s == Degraded) why = now - lastRejectMs_ < 1000 ? lastReject_ : QStringLiteral("projection stalled");
        else if (s == Unavailable) why = QStringLiteral("no frames/guidance, partition %1").arg(pstate);
        qCInfo(lcCockpit).noquote() << "IVI projection"
                                    << QMetaEnum::fromType<IviState>().valueToKey(state_) << "->"
                                    << QMetaEnum::fromType<IviState>().valueToKey(s)
                                    << (why.isEmpty() ? QString() : QStringLiteral("(%1)").arg(why));
        state_ = s;
        emit iviStateChanged();
    }
}
