// SomeIpClient: Qt event-loop based SOME/IP consumer for one remote service.
//
// Owns one UDP socket that is used both for SOME/IP-SD (subscribe/ack) and
// for service traffic (events in, requests out, responses in). Subscriptions
// are renewed at TTL/2; "available" drops if events stop arriving, which the
// HMI uses to show a signal-loss state instead of frozen values.
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QTimer>
#include <QUdpSocket>

#include "someip/someip.h"

struct ServiceConfig {
    QString name;          // for logs
    QString host;          // provider host name or IP
    quint16 sdPort = 30490;
    quint16 servicePort = 0;
    quint16 serviceId = 0;
    quint16 instanceId = 1;
    quint8 majorVersion = 1;
    quint16 eventgroupId = 1;
    quint32 ttlSeconds = 3;
    int livenessTimeoutMs = 2500;
};

class SomeIpClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ isAvailable NOTIFY availableChanged)
public:
    explicit SomeIpClient(ServiceConfig cfg, quint16 clientId, QObject *parent = nullptr);

    void start();
    bool isAvailable() const { return available_; }
    const ServiceConfig &config() const { return cfg_; }

    // Fire a request; returns the session id used (0 if the provider is unknown).
    quint16 call(quint16 methodId, const QByteArray &payload = {});

    static QByteArray toQByteArray(const someip::Bytes &b);
    static someip::Bytes toBytes(const QByteArray &b);

signals:
    void availableChanged(bool available);
    void eventReceived(quint16 eventId, const QByteArray &payload);
    void responseReceived(quint16 methodId, quint16 session, quint8 returnCode,
                          const QByteArray &payload);

private:
    void resolveHost();
    void sendSubscribe();
    void onReadyRead();
    void onTick();
    void setAvailable(bool a);
    void handleSd(const someip::Message &m);

    ServiceConfig cfg_;
    quint16 clientId_;
    QUdpSocket socket_;
    QHostAddress address_;
    bool resolving_ = false;
    QTimer tick_;
    QElapsedTimer sinceLastRx_;
    QElapsedTimer sinceSubscribe_;
    bool acked_ = false;
    bool available_ = false;
    quint16 session_ = 0;
    quint16 sdSession_ = 0;
};
