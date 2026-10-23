#include "otaclient.h"

#include <QLoggingCategory>
#include <QMetaEnum>

#include "someip/services.h"
#include "someipclient.h"

namespace ota = sdv::ota;
Q_LOGGING_CATEGORY(lcOta, "sdv.ota")

OtaClient::OtaClient(SomeIpClient *client, QObject *parent) : QObject(parent), client_(client) {
    connect(client_, &SomeIpClient::eventReceived, this, &OtaClient::onEvent);
    connect(client_, &SomeIpClient::availableChanged, this, &OtaClient::connectedChanged);
}

bool OtaClient::connected() const { return client_->isAvailable(); }

QString OtaClient::stateName() const {
    return QString::fromLatin1(QMetaEnum::fromType<State>().valueToKey(state_));
}

void OtaClient::approveInstall() {
    qCInfo(lcOta) << "driver approved installation of" << available_;
    client_->call(ota::kMApproveInstall);
}
void OtaClient::defer() { client_->call(ota::kMDefer); }
void OtaClient::checkNow() { client_->call(ota::kMCheckNow); }

void OtaClient::confirmHealthy(const QString &version) {
    qCInfo(lcOta) << "reporting bundle" << version << "healthy";
    client_->call(ota::kMConfirmHealthy,
                  SomeIpClient::toQByteArray(someip::Writer().str(version.toStdString()).take()));
}

void OtaClient::reportBootFailure(const QString &version, const QString &reason) {
    qCWarning(lcOta) << "reporting bundle" << version << "FAILED:" << reason;
    client_->call(ota::kMReportBootFailure,
                  SomeIpClient::toQByteArray(someip::Writer()
                                                 .str(version.toStdString())
                                                 .str(reason.left(512).toStdString())
                                                 .take()));
}

void OtaClient::onEvent(quint16 id, const QByteArray &payload) {
    if (id != ota::kEvStatus) return;
    const someip::Bytes b = SomeIpClient::toBytes(payload);
    try {
        someip::Reader r(b);
        const int st = r.u8();
        const QString inst = QString::fromStdString(r.str());
        const QString avail = QString::fromStdString(r.str());
        const int prog = r.u8();
        const QString msg = QString::fromStdString(r.str());
        const QString notes = QString::fromStdString(r.str());
        const QString slot = QString(QChar(r.u8()));
        if (st == state_ && inst == installed_ && avail == available_ && prog == progress_ &&
            msg == message_ && notes == notes_ && slot == slot_)
            return;
        if (st != state_)
            qCInfo(lcOta).noquote() << "OTA state ->"
                                    << QMetaEnum::fromType<State>().valueToKey(st) << "|" << msg;
        state_ = st;
        installed_ = inst;
        available_ = avail;
        progress_ = prog;
        message_ = msg;
        notes_ = notes;
        slot_ = slot;
        emit statusChanged();
    } catch (const someip::ParseError &e) {
        qCWarning(lcOta) << "bad status payload:" << e.what();
    }
}
