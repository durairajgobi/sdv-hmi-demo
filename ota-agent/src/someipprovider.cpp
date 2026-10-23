#include "someipprovider.h"

#include <QLoggingCategory>
#include <QNetworkDatagram>

#include "someip/sd.h"

Q_LOGGING_CATEGORY(lcProv, "sdv.someip")

static QByteArray qb(const someip::Bytes &b) {
    return QByteArray(reinterpret_cast<const char *>(b.data()), qsizetype(b.size()));
}
static someip::Bytes sb(const QByteArray &b) { return someip::Bytes(b.begin(), b.end()); }

SomeIpProvider::SomeIpProvider(quint16 serviceId, quint16 instanceId, quint8 major, quint16 eventgroup,
                               quint16 sdPort, quint16 servicePort, QObject *parent)
    : QObject(parent), serviceId_(serviceId), instanceId_(instanceId), eventgroup_(eventgroup),
      sdPort_(sdPort), servicePort_(servicePort), major_(major) {
    connect(&sd_, &QUdpSocket::readyRead, this, &SomeIpProvider::onSd);
    connect(&svc_, &QUdpSocket::readyRead, this, &SomeIpProvider::onService);
    connect(&expireTimer_, &QTimer::timeout, this, &SomeIpProvider::expire);
}

bool SomeIpProvider::start(QString *error) {
    if (!sd_.bind(QHostAddress::AnyIPv4, sdPort_, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint) ||
        !svc_.bind(QHostAddress::AnyIPv4, servicePort_)) {
        if (error) *error = sd_.errorString() + QLatin1Char('/') + svc_.errorString();
        return false;
    }
    expireTimer_.start(500);
    qCInfo(lcProv, "service 0x%04x provided: SD udp/%u, service udp/%u", serviceId_, sdPort_, servicePort_);
    return true;
}

void SomeIpProvider::onSd() {
    while (sd_.hasPendingDatagrams()) {
        const auto dg = sd_.receiveDatagram();
        try {
            for (const auto &m : someip::decodeAll(sb(dg.data()))) {
                if (!someip::sd::isSd(m)) continue;
                for (const auto &e : someip::sd::decode(m).entries) {
                    if (e.type != someip::sd::EntryType::SubscribeEventgroup) continue;

                    QHostAddress addr = dg.senderAddress();
                    quint16 port = quint16(dg.senderPort());
                    if (e.endpoint && e.endpoint->address) addr = QHostAddress(e.endpoint->address);
                    if (e.endpoint && e.endpoint->port) port = e.endpoint->port;

                    const bool ok = e.serviceId == serviceId_ && e.eventgroupId == eventgroup_ &&
                                    e.majorVersion == major_;
                    const QString k = key(addr, port);
                    if (ok && e.ttl > 0) {
                        const bool isNew = !subs_.contains(k);
                        subs_[k] = Sub{addr, port, QDeadlineTimer(qint64(e.ttl) * 1000)};
                        if (isNew) {
                            qCInfo(lcProv).noquote() << "subscriber added" << k;
                            QMetaObject::invokeMethod(this, &SomeIpProvider::subscriberAdded, Qt::QueuedConnection);
                        }
                    } else if (ok) {
                        subs_.remove(k);
                    }

                    someip::sd::SdMessage ack;
                    someip::sd::Entry a = e;
                    a.type = someip::sd::EntryType::SubscribeEventgroupAck;
                    a.ttl = ok ? e.ttl : 0;
                    a.endpoint.reset();
                    ack.entries.push_back(a);
                    sd_.writeDatagram(qb(someip::encode(someip::sd::encode(ack, ++sdSession_))),
                                      dg.senderAddress(), quint16(dg.senderPort()));
                }
            }
        } catch (const someip::ParseError &e) {
            qCWarning(lcProv) << "malformed SD datagram:" << e.what();
        }
    }
}

void SomeIpProvider::onService() {
    while (svc_.hasPendingDatagrams()) {
        const auto dg = svc_.receiveDatagram();
        try {
            for (const auto &req : someip::decodeAll(sb(dg.data()))) {
                if (req.header.messageType != someip::MessageType::Request &&
                    req.header.messageType != someip::MessageType::RequestNoReturn)
                    continue;
                Reply r;
                if (req.header.serviceId != serviceId_) {
                    r.rc = someip::ReturnCode::UnknownService;
                } else if (auto it = handlers_.find(req.header.methodId); it != handlers_.end()) {
                    try {
                        r = (*it)(req);
                    } catch (const someip::ParseError &) {
                        r.rc = someip::ReturnCode::MalformedMessage;
                    }
                } else {
                    r.rc = someip::ReturnCode::UnknownMethod;
                }
                if (req.header.messageType == someip::MessageType::Request)
                    svc_.writeDatagram(qb(someip::encode(someip::makeResponse(req, r.payload, r.rc))),
                                       dg.senderAddress(), quint16(dg.senderPort()));
            }
        } catch (const someip::ParseError &e) {
            qCWarning(lcProv) << "malformed request:" << e.what();
        }
    }
}

void SomeIpProvider::notify(quint16 eventId, const someip::Bytes &payload) {
    if (subs_.isEmpty()) return;
    if (++session_ == 0) session_ = 1;
    const QByteArray bytes = qb(someip::encode(someip::makeNotification(serviceId_, eventId, payload, session_, major_)));
    for (const auto &s : std::as_const(subs_)) svc_.writeDatagram(bytes, s.addr, s.port);
}

void SomeIpProvider::expire() {
    for (auto it = subs_.begin(); it != subs_.end();) {
        if (it->expiry.hasExpired()) {
            qCInfo(lcProv).noquote() << "subscription expired" << it.key();
            it = subs_.erase(it);
        } else {
            ++it;
        }
    }
}
