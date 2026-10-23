#include "someipclient.h"

#include <QHostInfo>
#include <QLoggingCategory>
#include <QNetworkDatagram>

#include "someip/sd.h"

Q_LOGGING_CATEGORY(lcSomeIp, "sdv.someip")

SomeIpClient::SomeIpClient(ServiceConfig cfg, quint16 clientId, QObject *parent)
    : QObject(parent), cfg_(std::move(cfg)), clientId_(clientId) {
    connect(&socket_, &QUdpSocket::readyRead, this, &SomeIpClient::onReadyRead);
    connect(&tick_, &QTimer::timeout, this, &SomeIpClient::onTick);
}

void SomeIpClient::start() {
    if (!socket_.bind(QHostAddress::AnyIPv4, 0))
        qCWarning(lcSomeIp) << cfg_.name << "bind failed:" << socket_.errorString();
    qCInfo(lcSomeIp).noquote() << cfg_.name << QStringLiteral("consumer on udp/%1 -> %2 (SD %3, svc %4)")
                                                   .arg(socket_.localPort())
                                                   .arg(cfg_.host)
                                                   .arg(cfg_.sdPort)
                                                   .arg(cfg_.servicePort);
    tick_.start(250);
    resolveHost();
}

void SomeIpClient::resolveHost() {
    QHostAddress literal(cfg_.host);
    if (!literal.isNull()) {
        address_ = literal;
        sendSubscribe();
        return;
    }
    if (resolving_) return;
    resolving_ = true;
    QHostInfo::lookupHost(cfg_.host, this, [this](const QHostInfo &info) {
        resolving_ = false;
        for (const auto &a : info.addresses()) {
            if (a.protocol() == QAbstractSocket::IPv4Protocol) {
                address_ = a;
                qCInfo(lcSomeIp) << cfg_.name << "resolved" << cfg_.host << "->" << a.toString();
                sendSubscribe();
                return;
            }
        }
        qCDebug(lcSomeIp) << cfg_.name << "cannot resolve" << cfg_.host << "- retrying";
    });
}

void SomeIpClient::sendSubscribe() {
    if (address_.isNull()) return;
    someip::sd::SdMessage sdm;
    someip::sd::Entry e;
    e.type = someip::sd::EntryType::SubscribeEventgroup;
    e.serviceId = cfg_.serviceId;
    e.instanceId = cfg_.instanceId;
    e.majorVersion = cfg_.majorVersion;
    e.eventgroupId = cfg_.eventgroupId;
    e.ttl = cfg_.ttlSeconds;
    // Address 0 => provider uses the datagram's source address (NAT/Docker friendly)
    e.endpoint = someip::sd::Ipv4Endpoint{0, socket_.localPort(), 0x11};
    sdm.entries.push_back(e);
    const auto bytes = someip::encode(someip::sd::encode(sdm, ++sdSession_));
    socket_.writeDatagram(toQByteArray(bytes), address_, cfg_.sdPort);
    sinceSubscribe_.start();
}

quint16 SomeIpClient::call(quint16 methodId, const QByteArray &payload) {
    if (address_.isNull()) return 0;
    if (++session_ == 0) session_ = 1;  // session 0 is reserved
    const auto msg = someip::makeRequest(cfg_.serviceId, methodId, toBytes(payload), clientId_, session_,
                                         cfg_.majorVersion);
    socket_.writeDatagram(toQByteArray(someip::encode(msg)), address_, cfg_.servicePort);
    return session_;
}

void SomeIpClient::onTick() {
    if (address_.isNull()) {
        resolveHost();
        return;
    }
    // Renew at TTL/2 so a single lost datagram never drops the subscription.
    if (!sinceSubscribe_.isValid() || sinceSubscribe_.elapsed() >= qint64(cfg_.ttlSeconds) * 500)
        sendSubscribe();
    if (available_ && sinceLastRx_.isValid() && sinceLastRx_.elapsed() > cfg_.livenessTimeoutMs) {
        qCWarning(lcSomeIp) << cfg_.name << "no events for" << sinceLastRx_.elapsed() << "ms - service lost";
        acked_ = false;
        setAvailable(false);
    }
}

void SomeIpClient::onReadyRead() {
    while (socket_.hasPendingDatagrams()) {
        const QByteArray dg = socket_.receiveDatagram().data();
        try {
            for (const auto &m : someip::decodeAll(toBytes(dg))) {
                if (someip::sd::isSd(m)) {
                    handleSd(m);
                    continue;
                }
                if (m.header.serviceId != cfg_.serviceId) continue;
                sinceLastRx_.start();
                if (acked_) setAvailable(true);

                const QByteArray payload = toQByteArray(m.payload);
                switch (m.header.messageType) {
                case someip::MessageType::Notification:
                    emit eventReceived(m.header.methodId, payload);
                    break;
                case someip::MessageType::Response:
                case someip::MessageType::Error:
                    emit responseReceived(m.header.methodId, m.header.sessionId,
                                          static_cast<quint8>(m.header.returnCode), payload);
                    break;
                default:
                    break;
                }
            }
        } catch (const someip::ParseError &e) {
            qCWarning(lcSomeIp) << cfg_.name << "malformed datagram:" << e.what();
        }
    }
}

void SomeIpClient::handleSd(const someip::Message &m) {
    const auto sdm = someip::sd::decode(m);
    for (const auto &e : sdm.entries) {
        if (e.type != someip::sd::EntryType::SubscribeEventgroupAck || e.serviceId != cfg_.serviceId)
            continue;
        if (e.ttl == 0) {
            qCWarning(lcSomeIp) << cfg_.name << "subscription NACKed";
            acked_ = false;
            setAvailable(false);
        } else if (!acked_) {
            qCInfo(lcSomeIp) << cfg_.name << "subscription acknowledged, TTL" << e.ttl << "s";
            acked_ = true;
            sinceLastRx_.start();
            setAvailable(true);
        }
    }
}

void SomeIpClient::setAvailable(bool a) {
    if (a == available_) return;
    available_ = a;
    emit availableChanged(a);
}

QByteArray SomeIpClient::toQByteArray(const someip::Bytes &b) {
    return QByteArray(reinterpret_cast<const char *>(b.data()), qsizetype(b.size()));
}

someip::Bytes SomeIpClient::toBytes(const QByteArray &b) {
    return someip::Bytes(b.begin(), b.end());
}
