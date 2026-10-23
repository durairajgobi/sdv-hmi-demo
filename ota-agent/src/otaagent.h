// OtaAgent: on-vehicle update client for the cluster HMI UI bundle.
//
//  Idle ─poll─▶ Checking ─newer, signed─▶ UpdateAvailable ─driver consent─▶
//  Downloading ─▶ Verifying (size + SHA-256) ─▶ Installing (inactive slot) ─▶
//  Activating (switch slot, wait for HMI health) ─┬─ confirmed ─▶ Success
//                                                 └─ failure/timeout ─▶ RolledBack
//  Any integrity/authenticity problem ─▶ Failed (nothing on the vehicle changes)
#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include "manifest.h"
#include "slotmanager.h"
#include "someip/services.h"

class SomeIpProvider;
class QNetworkReply;

struct AgentConfig {
    QUrl serverUrl;          // e.g. http://ota-server:8080/
    QString bundleRoot;
    QString publicKeyPath;
    QString workDir;
    int pollIntervalMs = 10000;
    int healthTimeoutMs = 20000;
    quint16 sdPort = sdv::ota::kSdPort;
    quint16 servicePort = sdv::ota::kPort;
};

class OtaAgent : public QObject {
    Q_OBJECT
public:
    explicit OtaAgent(AgentConfig cfg, QObject *parent = nullptr);
    bool start(QString *error);

private:
    using State = sdv::ota::State;

    void setState(State s, const QString &message = {}, int progress = -1);
    void publishStatus();
    QString installedVersion() const;

    // pipeline
    void checkForUpdate();
    void onManifestFetched(const QByteArray &manifest, const QByteArray &sigB64);
    void startDownload();
    void verifyAndInstall();
    void activate();
    void commit();
    void rollback(const QString &reason);
    void fail(const QString &reason);

    void resumePendingTransaction();
    bool isBusy() const;

    AgentConfig cfg_;
    SlotManager slots_;
    SomeIpProvider *svc_ = nullptr;
    QNetworkAccessManager net_;
    QByteArray publicKey_;
    OtaPersistentState persist_;

    State state_ = State::Idle;
    QString message_;
    int progress_ = 0;
    Manifest offered_;          // verified manifest currently offered
    QByteArray lastManifestHash_;  // to avoid re-reporting the same bad manifest
    bool deferred_ = false;
    QString downloadPath_;
    QPointer<QNetworkReply> download_;

    QTimer pollTimer_;
    QTimer statusTimer_;
    QTimer healthTimer_;
};
