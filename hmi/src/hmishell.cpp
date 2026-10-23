#include "hmishell.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QQmlApplicationEngine>
#include <QQuickWindow>

#include <algorithm>

#include "otaclient.h"
#include "vehiclestate.h"

Q_LOGGING_CATEGORY(lcShell, "sdv.shell")
Q_LOGGING_CATEGORY(lcKpi, "sdv.kpi")

HmiShell::HmiShell(QString bundleRoot, VehicleState *vehicle, OtaClient *ota, QObject *parent)
    : QObject(parent), root_(std::move(bundleRoot)), vehicle_(vehicle), ota_(ota) {
    healthTimer_.setSingleShot(true);
    connect(&healthTimer_, &QTimer::timeout, this, [this] {
        if (!window_ || !window_->isVisible()) {
            qCWarning(lcShell) << "health window elapsed but UI not visible - not confirming";
            return;
        }
        healthy_ = true;
        qCInfo(lcShell) << "bundle" << version_ << "stable for" << healthWindowMs_ << "ms";
        ota_->confirmHealthy(version_);
    });
    connect(ota_, &OtaClient::statusChanged, this, &HmiShell::onOtaStatus);
    connect(ota_, &OtaClient::connectedChanged, this, &HmiShell::onOtaStatus);
    connect(&snapTimer_, &QTimer::timeout, this, &HmiShell::takePeriodicSnapshot);
    connect(&kpiTimer_, &QTimer::timeout, this, &HmiShell::reportFrameStats);
    kpiTimer_.start(5000);
    // Scheduling-latency probe: how late does the cluster's GUI thread get to
    // run a 10 ms timer? Rises sharply if another workload steals its CPU.
    latencyProbe_.setTimerType(Qt::PreciseTimer);
    connect(&latencyProbe_, &QTimer::timeout, this, [this] {
        if (probeClock_.isValid()) latencyMs_.push_back(std::max(0.0, probeClock_.nsecsElapsed() / 1e6 - 10.0));
        probeClock_.start();
    });
    latencyProbe_.start(10);
}

HmiShell::~HmiShell() { delete engine_; }

QString HmiShell::shellVersion() const { return QStringLiteral(SDV_SHELL_VERSION); }

bool HmiShell::resolveActive(BundleInfo &out, QString &error) const {
    const QFileInfo link(QDir(root_).filePath(QStringLiteral("current")));
    const QString dir = link.canonicalFilePath();
    if (dir.isEmpty()) {
        error = QStringLiteral("no active bundle at %1").arg(link.absoluteFilePath());
        return false;
    }
    QFile meta(QDir(dir).filePath(QStringLiteral("bundle.json")));
    if (!meta.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("missing bundle.json in %1").arg(dir);
        return false;
    }
    const auto obj = QJsonDocument::fromJson(meta.readAll()).object();
    out.dir = dir;
    out.version = obj.value(QStringLiteral("version")).toString();
    out.slot = QFileInfo(dir).fileName().remove(QStringLiteral("slot"));
    if (out.version.isEmpty()) {
        error = QStringLiteral("bundle.json has no version");
        return false;
    }
    return true;
}

bool HmiShell::loadActiveBundle() {
    BundleInfo info;
    QString error;
    if (!resolveActive(info, error)) {
        qCCritical(lcShell).noquote() << error;
        return false;
    }
    qCInfo(lcShell).noquote() << "loading UI bundle" << info.version << "from slot" << info.slot
                              << (engine_ ? "(hot swap)" : "");

    // Load into a fresh engine; the currently displayed UI stays up meanwhile.
    auto *engine = new QQmlApplicationEngine(this);
    QStringList errors;
    auto conn = connect(engine, &QQmlEngine::warnings, this, [&errors](const QList<QQmlError> &w) {
        for (const auto &e : w) errors << e.toString();
    });
    engine->setInitialProperties({
        {QStringLiteral("vehicle"), QVariant::fromValue<QObject *>(vehicle_)},
        {QStringLiteral("ota"), QVariant::fromValue<QObject *>(ota_)},
        {QStringLiteral("shell"), QVariant::fromValue<QObject *>(this)},
        {QStringLiteral("cockpit"), QVariant::fromValue<QObject *>(cockpit_)},
    });
    engine->load(QUrl::fromLocalFile(QDir(info.dir).filePath(QStringLiteral("qml/Main.qml"))));
    disconnect(conn);

    QQuickWindow *win = engine->rootObjects().isEmpty()
                            ? nullptr
                            : qobject_cast<QQuickWindow *>(engine->rootObjects().constFirst());
    if (!win) {
        if (errors.isEmpty()) errors << QStringLiteral("root object is not a Window");
        const QString reason = errors.join(QStringLiteral(" | "));
        qCCritical(lcShell).noquote() << "bundle" << info.version << "failed to load:" << reason;
        delete engine;
        failedVersions_.insert(info.version);
        lastError_ = reason;
        ota_->reportBootFailure(info.version, reason);
        if (engine_) qCWarning(lcShell).noquote() << "keeping previous UI" << version_ << "on screen";
        return false;
    }

    // Swap: hide + destroy old UI only after the new one exists.
    if (engine_) {
        if (window_) window_->hide();
        engine_->deleteLater();
        previousVersion_ = version_;
    }
    engine_ = engine;
    window_ = win;
    version_ = info.version;
    slot_ = info.slot;
    ++reloadCount_;
    emit bundleChanged();
    win->show();
    connect(win, &QQuickWindow::frameSwapped, this, &HmiShell::recordFrame);
    armHealthCheck();
    return true;
}

void HmiShell::armHealthCheck() {
    healthy_ = false;
    healthTimer_.stop();
    // Start the stability window on the first frame actually presented.
    auto once = std::make_shared<QMetaObject::Connection>();
    *once = connect(window_.data(), &QQuickWindow::frameSwapped, this, [this, once] {
        disconnect(*once);
        qCInfo(lcShell) << "first frame presented for bundle" << version_;
        emit uiRendering();
        healthTimer_.start(healthWindowMs_);
    });
}

void HmiShell::onOtaStatus() {
    if (!ota_->connected()) return;
    const QString active = ota_->installedVersion();
    if (active.isEmpty()) return;

    if (active != version_ && !failedVersions_.contains(active)) {
        loadActiveBundle();  // agent activated a new slot or rolled back
        return;
    }
    if (ota_->state() == OtaClient::Activating) {
        // Re-send idempotently until the agent leaves Activating (UDP may drop).
        if (failedVersions_.contains(active))
            ota_->reportBootFailure(active, lastError_);
        else if (active == version_ && healthy_)
            ota_->confirmHealthy(version_);
    }
}

void HmiShell::setSnapshots(const QString &dir, int intervalMs) {
    snapDir_ = dir;
    QDir().mkpath(dir);
    snapTimer_.start(intervalMs);
}

bool HmiShell::saveSnapshot(const QString &path) {
    if (!window_) return false;
    const QImage img = window_->grabWindow();
    return !img.isNull() && img.save(path);
}

void HmiShell::takePeriodicSnapshot() {
    const QString name = QStringLiteral("%1/snap_%2_v%3_%4.png")
                             .arg(snapDir_)
                             .arg(++snapIndex_, 4, 10, QLatin1Char('0'))
                             .arg(version_, ota_->stateName());
    saveSnapshot(name);
}

// ---- render KPI: frame-to-frame intervals of the cluster window --------------
void HmiShell::recordFrame() {
    if (frameClock_.isValid()) intervalsMs_.push_back(frameClock_.nsecsElapsed() / 1e6);
    frameClock_.start();
}

void HmiShell::reportFrameStats() {
    if (intervalsMs_.size() < 5 || latencyMs_.size() < 5) return;
    auto take = [](std::vector<double> &src) {
        std::vector<double> v = std::move(src);
        src.clear();
        std::sort(v.begin(), v.end());
        return v;
    };
    const auto f = take(intervalsMs_), l = take(latencyMs_);
    auto pct = [](const std::vector<double> &v, double q) {
        return v[std::min(v.size() - 1, std::size_t(q * double(v.size() - 1) + 0.5))];
    };
    const auto jank = std::count_if(f.begin(), f.end(), [](double x) { return x > 50.0; });
    qCInfo(lcKpi).noquote() << QStringLiteral("frames=%1 frame_p99=%2ms frame_max=%3ms jank(>50ms)=%4 | "
                                              "ui_latency p50=%5ms p99=%6ms max=%7ms")
                                   .arg(f.size()).arg(pct(f, 0.99), 0, 'f', 1).arg(f.back(), 0, 'f', 1).arg(jank)
                                   .arg(pct(l, 0.5), 0, 'f', 2).arg(pct(l, 0.99), 0, 'f', 2).arg(l.back(), 0, 'f', 2);
}
