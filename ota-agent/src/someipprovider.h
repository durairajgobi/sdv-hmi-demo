// SomeIpProvider: Qt based SOME/IP service provider (SD subscription handling,
// event fan-out to live subscribers, request/response dispatch).
#pragma once

#include <QDeadlineTimer>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QTimer>
#include <QUdpSocket>
#include <functional>

#include "someip/someip.h"

class SomeIpProvider : public QObject {
    Q_OBJECT
public:
    struct Reply {
        someip::ReturnCode rc = someip::ReturnCode::Ok;
        someip::Bytes payload;
    };
    using Handler = std::function<Reply(const someip::Message &request)>;

    SomeIpProvider(quint16 serviceId, quint16 instanceId, quint8 major, quint16 eventgroup,
                   quint16 sdPort, quint16 servicePort, QObject *parent = nullptr);

    bool start(QString *error = nullptr);
    void setHandler(quint16 methodId, Handler h) { handlers_.insert(methodId, std::move(h)); }
    void notify(quint16 eventId, const someip::Bytes &payload);
    int subscriberCount() const { return int(subs_.size()); }

signals:
    void subscriberAdded();

private:
    struct Sub {
        QHostAddress addr;
        quint16 port;
        QDeadlineTimer expiry;
    };
    void onSd();
    void onService();
    void expire();
    static QString key(const QHostAddress &a, quint16 p) { return a.toString() + QLatin1Char(':') + QString::number(p); }

    quint16 serviceId_, instanceId_, eventgroup_, sdPort_, servicePort_;
    quint8 major_;
    QUdpSocket sd_, svc_;
    QHash<QString, Sub> subs_;
    QHash<quint16, Handler> handlers_;
    QTimer expireTimer_;
    quint16 session_ = 0, sdSession_ = 0;
};
