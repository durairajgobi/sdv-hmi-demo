#include "otaagent.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>

#include "signature.h"
#include "someipprovider.h"

namespace ota = sdv::ota;
Q_LOGGING_CATEGORY(lcAgent, "sdv.ota-agent")

static const char *stateName(ota::State s) {
    static const char *n[] = {"Idle", "Checking", "UpdateAvailable", "Downloading", "Verifying",
                              "Installing", "Activating", "Success", "RolledBack", "Failed"};
    return n[static_cast<int>(s)];
}

OtaAgent::OtaAgent(AgentConfig cfg, QObject *parent)
    : QObject(parent), cfg_(std::move(cfg)), slots_(cfg_.bundleRoot) {
    svc_ = new SomeIpProvider(ota::kServiceId, ota::kInstanceId, ota::kMajor, ota::kEventgroup,
                              cfg_.sdPort, cfg_.servicePort, this);

    using Reply = SomeIpProvider::Reply;
    auto accepted = [](bool ok) { return Reply{someip::ReturnCode::Ok, someip::Writer().u8(ok ? 1 : 0).take()}; };

    svc_->setHandler(ota::kMApproveInstall, [this, accepted](const someip::Message &) {
        if (state_ != State::UpdateAvailable) return accepted(false);
        qCInfo(lcAgent) << "driver consent received for" << offered_.version;
        deferred_ = false;
        QTimer::singleShot(0, this, &OtaAgent::startDownload);
        return accepted(true);
    });
    svc_->setHandler(ota::kMDefer, [this, accepted](const someip::Message &) {
        if (state_ != State::UpdateAvailable) return accepted(false);
        deferred_ = true;
        qCInfo(lcAgent) << "update" << offered_.version << "deferred by driver";
        return accepted(true);
    });
    svc_->setHandler(ota::kMCheckNow, [this, accepted](const someip::Message &) {
        QTimer::singleShot(0, this, &OtaAgent::checkForUpdate);
        return accepted(true);
    });
    svc_->setHandler(ota::kMConfirmHealthy, [this, accepted](const someip::Message &m) {
        someip::Reader r(m.payload);
        const QString version = QString::fromStdString(r.str());
        if (state_ == State::Activating && version == persist_.pendingVersion) {
            QTimer::singleShot(0, this, &OtaAgent::commit);
            return accepted(true);
        }
        return accepted(false);  // stale / duplicate confirmations are harmless
    });
    svc_->setHandler(ota::kMReportBootFailure, [this, accepted](const someip::Message &m) {
        someip::Reader r(m.payload);
        const QString version = QString::fromStdString(r.str());
        const QString reason = QString::fromStdString(r.str());
        if (state_ == State::Activating && version == persist_.pendingVersion) {
            QTimer::singleShot(0, this, [this, reason] { rollback(QStringLiteral("HMI failed to start: ") + reason); });
            return accepted(true);
        }
        return accepted(false);
    });
    connect(svc_, &SomeIpProvider::subscriberAdded, this, &OtaAgent::publishStatus);

    pollTimer_.setInterval(cfg_.pollIntervalMs);
    connect(&pollTimer_, &QTimer::timeout, this, &OtaAgent::checkForUpdate);
    statusTimer_.setInterval(1000);  // cyclic status so late subscribers converge
    connect(&statusTimer_, &QTimer::timeout, this, &OtaAgent::publishStatus);
    healthTimer_.setSingleShot(true);
    connect(&healthTimer_, &QTimer::timeout, this, [this] {
        rollback(QStringLiteral("no health confirmation from HMI within %1 s").arg(cfg_.healthTimeoutMs / 1000));
    });
}

bool OtaAgent::start(QString *error) {
    QFile key(cfg_.publicKeyPath);
    if (!key.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("cannot read public key %1").arg(cfg_.publicKeyPath);
        return false;
    }
    publicKey_ = key.readAll();
    if (slots_.activeSlot().isEmpty()) {
        *error = QStringLiteral("bundle root %1 is not provisioned (no 'current' link)").arg(cfg_.bundleRoot);
        return false;
    }
    QDir().mkpath(cfg_.workDir);
    if (!svc_->start(error)) return false;

    persist_ = slots_.loadState();
    qCInfo(lcAgent).noquote() << "active slot" << slots_.activeSlot() << "version" << installedVersion()
                              << "| server" << cfg_.serverUrl.toString();
    resumePendingTransaction();

    statusTimer_.start();
    pollTimer_.start();
    QTimer::singleShot(500, this, &OtaAgent::checkForUpdate);
    return true;
}

// Power loss / agent restart during Activating: the new slot is still on
// probation. Give the HMI a fresh health window (bounded attempts), else roll back.
void OtaAgent::resumePendingTransaction() {
    if (persist_.pendingSlot.isEmpty()) return;
    if (++persist_.pendingAttempts > 2) {
        rollback(QStringLiteral("activation interrupted repeatedly"));
        return;
    }
    slots_.saveState(persist_);
    qCWarning(lcAgent) << "resuming unconfirmed activation of" << persist_.pendingVersion
                       << "attempt" << persist_.pendingAttempts;
    setState(State::Activating, QStringLiteral("Waiting for HMI health confirmation"));
    healthTimer_.start(cfg_.healthTimeoutMs);
}

QString OtaAgent::installedVersion() const { return slots_.versionOf(slots_.activeSlot()); }

bool OtaAgent::isBusy() const {
    return state_ == State::Downloading || state_ == State::Verifying || state_ == State::Installing ||
           state_ == State::Activating;
}

void OtaAgent::setState(State s, const QString &message, int progress) {
    if (s != state_ || message != message_)
        qCInfo(lcAgent).noquote() << "state" << stateName(state_) << "->" << stateName(s)
                                  << (message.isEmpty() ? QString() : QStringLiteral("| ") + message);
    state_ = s;
    message_ = message;
    if (progress >= 0) progress_ = progress;
    publishStatus();
}

void OtaAgent::publishStatus() {
    const QString slot = slots_.activeSlot();
    svc_->notify(ota::kEvStatus, someip::Writer()
                                     .u8(static_cast<uint8_t>(state_))
                                     .str(installedVersion().toStdString())
                                     .str(offered_.version.toStdString())
                                     .u8(static_cast<uint8_t>(progress_))
                                     .str(message_.toStdString())
                                     .str(offered_.releaseNotes.toStdString())
                                     .u8(slot.isEmpty() ? '?' : static_cast<uint8_t>(slot.at(0).toLatin1()))
                                     .take());
}

// ---- 1. check ----------------------------------------------------------------
void OtaAgent::checkForUpdate() {
    if (isBusy()) return;
    auto *mReply = net_.get(QNetworkRequest(cfg_.serverUrl.resolved(QUrl(QStringLiteral("manifest.json")))));
    connect(mReply, &QNetworkReply::finished, this, [this, mReply] {
        mReply->deleteLater();
        if (mReply->error() != QNetworkReply::NoError) {
            if (mReply->error() != QNetworkReply::ContentNotFoundError)
                qCDebug(lcAgent) << "manifest fetch failed:" << mReply->errorString();
            return;  // offline is normal for a vehicle; retry on next poll
        }
        const QByteArray manifest = mReply->readAll();
        auto *sReply = net_.get(QNetworkRequest(cfg_.serverUrl.resolved(QUrl(QStringLiteral("manifest.json.sig")))));
        connect(sReply, &QNetworkReply::finished, this, [this, sReply, manifest] {
            sReply->deleteLater();
            onManifestFetched(manifest, sReply->error() == QNetworkReply::NoError ? sReply->readAll() : QByteArray());
        });
    });
}

void OtaAgent::onManifestFetched(const QByteArray &manifest, const QByteArray &sigB64) {
    if (isBusy()) return;
    const QByteArray hash = QCryptographicHash::hash(manifest + sigB64, QCryptographicHash::Sha256);
    const bool sameAsLast = hash == lastManifestHash_;
    lastManifestHash_ = hash;

    // Authenticity first: nothing in an unsigned/tampered manifest is trusted.
    QString err;
    if (!sig::verifyEd25519(publicKey_, manifest, QByteArray::fromBase64(sigB64.trimmed()), &err)) {
        if (!sameAsLast) {
            offered_ = Manifest{};
            fail(QStringLiteral("Release manifest rejected: %1").arg(err));
        }
        return;
    }
    Manifest m;
    if (!Manifest::parse(manifest, m, err)) {
        if (!sameAsLast) fail(QStringLiteral("Invalid manifest: %1").arg(err));
        return;
    }
    if (compareVersions(m.minShellVersion, QStringLiteral(SDV_SHELL_VERSION)) > 0) {
        if (!sameAsLast) fail(QStringLiteral("Release %1 needs shell >= %2").arg(m.version, m.minShellVersion));
        return;
    }

    const QString installed = installedVersion();
    if (compareVersions(m.version, installed) <= 0 || persist_.rejectedVersions.contains(m.version)) {
        // Nothing to offer. Terminal states stay visible until something new arrives.
        if (state_ == State::UpdateAvailable) {
            offered_ = Manifest{};
            setState(State::Idle, QStringLiteral("Up to date"));
        }
        return;
    }
    if (state_ == State::UpdateAvailable && offered_.version == m.version) return;

    offered_ = m;
    deferred_ = false;
    setState(State::UpdateAvailable, QStringLiteral("Signed release %1 available").arg(m.version), 0);
}

// ---- 2. download --------------------------------------------------------------
void OtaAgent::startDownload() {
    if (state_ != State::UpdateAvailable) return;
    downloadPath_ = QDir(cfg_.workDir).filePath(QStringLiteral("download-%1.tar.gz").arg(offered_.version));
    auto *file = new QFile(downloadPath_, this);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete file;
        fail(QStringLiteral("cannot write %1").arg(downloadPath_));
        return;
    }
    setState(State::Downloading, QStringLiteral("0 / %1 kB").arg(offered_.size / 1024), 0);

    download_ = net_.get(QNetworkRequest(cfg_.serverUrl.resolved(QUrl(offered_.url))));
    connect(download_, &QNetworkReply::readyRead, this, [this, file] {
        file->write(download_->readAll());
        if (file->size() > offered_.size) download_->abort();  // never write more than announced
    });
    connect(download_, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64) {
        const int pct = int(qBound(qint64(0), got * 100 / qMax(qint64(1), offered_.size), qint64(100)));
        if (pct != progress_)
            setState(State::Downloading, QStringLiteral("%1 / %2 kB").arg(got / 1024).arg(offered_.size / 1024), pct);
    });
    connect(download_, &QNetworkReply::finished, this, [this, file] {
        file->write(download_->readAll());
        file->close();
        file->deleteLater();
        download_->deleteLater();
        if (download_->error() != QNetworkReply::NoError) {
            fail(QStringLiteral("Download failed: %1").arg(download_->errorString()));
            return;
        }
        verifyAndInstall();
    });
}

// ---- 3. verify + install -------------------------------------------------------
void OtaAgent::verifyAndInstall() {
    setState(State::Verifying, QStringLiteral("Checking SHA-256 against signed manifest"), 100);
    QFile f(downloadPath_);
    if (!f.open(QIODevice::ReadOnly)) return fail(QStringLiteral("download vanished"));
    if (f.size() != offered_.size)
        return fail(QStringLiteral("Size mismatch: got %1, manifest %2").arg(f.size()).arg(offered_.size));
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    const QString digest = QString::fromLatin1(h.result().toHex());
    if (digest != offered_.sha256)
        return fail(QStringLiteral("Integrity check failed (sha256 %1…)").arg(digest.left(12)));
    f.close();

    const QString target = slots_.inactiveSlot();
    setState(State::Installing, QStringLiteral("Writing slot %1").arg(target), 0);
    QString err;
    if (!slots_.installToInactive(downloadPath_, offered_.version, err)) return fail(QStringLiteral("Install failed: ") + err);
    QFile::remove(downloadPath_);
    setState(State::Installing, QStringLiteral("Slot %1 ready").arg(target), 100);
    QTimer::singleShot(400, this, &OtaAgent::activate);
}

// ---- 4. activate + health gate ---------------------------------------------------
void OtaAgent::activate() {
    const QString target = slots_.inactiveSlot();
    persist_.committedSlot = slots_.activeSlot();
    persist_.pendingSlot = target;
    persist_.pendingVersion = offered_.version;
    persist_.pendingAttempts = 1;
    if (!slots_.saveState(persist_)) return fail(QStringLiteral("cannot persist OTA state"));  // write-ahead

    QString err;
    if (!slots_.activate(target, err)) {
        persist_.pendingSlot.clear();
        slots_.saveState(persist_);
        return fail(err);
    }
    setState(State::Activating, QStringLiteral("Slot %1 active, waiting for HMI health confirmation").arg(target), 100);
    healthTimer_.start(cfg_.healthTimeoutMs);
}

void OtaAgent::commit() {
    healthTimer_.stop();
    const QString version = persist_.pendingVersion;
    persist_.committedSlot = persist_.pendingSlot;
    persist_.pendingSlot.clear();
    persist_.pendingVersion.clear();
    persist_.pendingAttempts = 0;
    slots_.saveState(persist_);
    setState(State::Success, QStringLiteral("Cluster software %1 committed").arg(version), 100);
}

void OtaAgent::rollback(const QString &reason) {
    healthTimer_.stop();
    const QString bad = persist_.pendingVersion;
    QString err;
    if (!persist_.committedSlot.isEmpty() && persist_.committedSlot != slots_.activeSlot())
        slots_.activate(persist_.committedSlot, err);
    if (!bad.isEmpty() && !persist_.rejectedVersions.contains(bad)) persist_.rejectedVersions << bad;
    persist_.pendingSlot.clear();
    persist_.pendingVersion.clear();
    persist_.pendingAttempts = 0;
    slots_.saveState(persist_);
    qCWarning(lcAgent).noquote() << "ROLLBACK of" << bad << "->" << installedVersion() << ":" << reason;
    setState(State::RolledBack, QStringLiteral("%1 — restored %2. %3").arg(bad, installedVersion(), reason), 0);
}

void OtaAgent::fail(const QString &reason) {
    qCWarning(lcAgent).noquote() << "update failed:" << reason;
    if (!downloadPath_.isEmpty()) QFile::remove(downloadPath_);
    setState(State::Failed, reason, 0);
}
