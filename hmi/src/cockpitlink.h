// CockpitLink: the cluster partition's end of the inter-VM channel.
//
// Imports navigation projected by the (untrusted, QM) IVI partition into the
// (safety-relevant) cluster without letting the IVI affect the cluster:
//
//  * Frames and messages are copied and validated on a worker thread, never
//    on the GUI/render thread (seqlock + CRC + bounds on a private copy).
//  * Freshness is judged by the cluster's own clock: a stalled, hung, crashed
//    or corrupting IVI is detected within ~500 ms and the tile degrades:
//       Healthy     -> show the projected frame
//       Degraded    -> frames bad but guidance data valid: cluster renders a
//                      native fallback (arrow + distance) itself
//       Unavailable -> hide guidance, show "infotainment restarting"
//  * The cluster publishes its own heartbeat from the GUI thread so the
//    hypervisor watchdog can tell a hung cluster UI from a busy one.
#pragma once

#include <QImage>
#include <QMutex>
#include <QObject>
#include <QThread>
#include <QTimer>

#include "ivshm/ivshm.h"

class CockpitImporter;

class CockpitLink : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(int iviState READ iviState NOTIFY iviStateChanged)
    Q_PROPERTY(QString iviStateName READ iviStateName NOTIFY iviStateChanged)
    Q_PROPERTY(QString iviPartitionState READ iviPartitionState NOTIFY partitionChanged)
    Q_PROPERTY(int iviRestarts READ iviRestarts NOTIFY partitionChanged)
    Q_PROPERTY(int maneuver READ maneuver NOTIFY guidanceChanged)
    Q_PROPERTY(double distance READ distance NOTIFY guidanceChanged)
    Q_PROPERTY(QString street READ street NOTIFY guidanceChanged)
    Q_PROPERTY(QString eta READ eta NOTIFY guidanceChanged)
    Q_PROPERTY(int framesRejected READ framesRejected NOTIFY statsChanged)
    Q_PROPERTY(QString lastReject READ lastReject NOTIFY statsChanged)
public:
    enum IviState { Offline = 0, Starting, Healthy, Degraded, Unavailable };
    Q_ENUM(IviState)

    explicit CockpitLink(QObject *parent = nullptr);
    ~CockpitLink() override;

    // Attach to the hypervisor's shared memory (COCKPIT_SHM_DIR). No-op if unset.
    void start();
    void setSpeed(double kmh, const QString &gear);
    void setUiRendering(bool rendering);  // first frame of the current UI presented

    bool available() const { return available_; }
    int iviState() const { return state_; }
    QString iviStateName() const;
    QString iviPartitionState() const { return partitionState_; }
    int iviRestarts() const { return restarts_; }
    int maneuver() const { return maneuver_; }
    double distance() const { return distance_; }
    QString street() const { return street_; }
    QString eta() const { return eta_; }
    int framesRejected() const { return rejected_; }
    QString lastReject() const { return lastReject_; }

    QImage currentFrame() const;  // thread-safe snapshot (implicitly shared)

signals:
    void availableChanged();
    void iviStateChanged();
    void partitionChanged();
    void guidanceChanged();
    void statsChanged();
    void frameChanged();

private:
    bool attach();
    void beat();
    void evaluate();
    void onFrame(const QImage &img, quint32 frameNo);
    void onRejected(const QString &reason);
    void onGuidance(int maneuver, double meters, const QString &street, const QString &eta);

    QString dir_;
    ivshm::Mapping own_, ivi_, hv_;
    ivshm::ClusterRegion *me_ = nullptr;
    const ivshm::IviRegion *iviRegion_ = nullptr;
    const ivshm::HvRegion *hvRegion_ = nullptr;

    QThread worker_;
    CockpitImporter *importer_ = nullptr;
    QTimer beatTimer_, evalTimer_, attachTimer_;

    mutable QMutex frameMutex_;
    QImage frame_;
    qint64 lastRejectMs_ = 0, lastDropLogMs_ = 0;
    uint32_t lastDropped_ = 0;
    qint64 lastFrameMs_ = 0, lastGuidanceMs_ = 0, lastSpeedPushMs_ = 0;
    bool everHealthy_ = false;
    bool available_ = false, rendering_ = false;
    int state_ = Offline;
    QString partitionState_;
    int restarts_ = 0;
    int maneuver_ = 0;
    double distance_ = 0;
    QString street_, eta_, lastReject_;
    int rejected_ = 0;
};

// Worker-thread side: polls the IVI region, validates, hands results to the GUI.
class CockpitImporter : public QObject {
    Q_OBJECT
public:
    CockpitImporter(const ivshm::IviRegion *ivi, ivshm::ClusterRegion *me) : ivi_(ivi), me_(me) {}
    void startPolling();

signals:
    void frame(const QImage &img, quint32 frameNo);
    void rejected(const QString &reason);
    void guidance(int maneuver, double meters, const QString &street, const QString &eta);

private:
    void poll();
    const ivshm::IviRegion *ivi_;
    ivshm::ClusterRegion *me_;  // only fromIvi (consumer cursor) is touched here
    QTimer *timer_ = nullptr;
    std::vector<uint8_t> buf_;
    quint32 lastFrameNo_ = 0;
    uint64_t lastGeneration_ = 0;
    ivshm::RingStats ringStats_;
};
