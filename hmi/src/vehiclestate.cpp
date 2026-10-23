#include "vehiclestate.h"

#include <QDateTime>
#include <QLoggingCategory>

#include "someip/services.h"
#include "someipclient.h"

namespace vs = sdv::vehicle;
Q_LOGGING_CATEGORY(lcVehicle, "sdv.vehicle")

VehicleState::VehicleState(SomeIpClient *client, QObject *parent) : QObject(parent), client_(client) {
    connect(client_, &SomeIpClient::eventReceived, this, &VehicleState::onEvent);
    connect(client_, &SomeIpClient::responseReceived, this, &VehicleState::onResponse);
    connect(client_, &SomeIpClient::availableChanged, this, [this](bool up) {
        emit connectedChanged();
        if (up && vin_.isEmpty()) client_->call(vs::kMGetVin);
    });
}

bool VehicleState::connected() const { return client_->isAvailable(); }

QString VehicleState::driveModeName() const {
    switch (driveMode_) {
    case 0: return QStringLiteral("ECO");
    case 2: return QStringLiteral("SPORT");
    default: return QStringLiteral("COMFORT");
    }
}

void VehicleState::setDriveMode(int mode) {
    QByteArray p(1, char(mode));
    client_->call(vs::kMSetDriveMode, p);
}

template <typename T>
static bool assign(T &field, T value) {
    if (field == value) return false;
    field = value;
    return true;
}

void VehicleState::onEvent(quint16 id, const QByteArray &payload) {
    const someip::Bytes b = SomeIpClient::toBytes(payload);
    try {
        someip::Reader r(b);
        switch (id) {
        case vs::kEvSpeed: {
            const double v = r.f32();
            if (assign(speed_, v)) emit speedChanged();
            break;
        }
        case vs::kEvPower: {
            const double p = r.f32();
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (lastPowerMs_ > 0) {
                const double dtH = (now - lastPowerMs_) / 3.6e6;
                energyKwh_ += p * dtH;
                distKm_ += speed_ * dtH;
                if (distKm_ > 0.05) {
                    avgKwhPer100_ = 100.0 * energyKwh_ / distKm_;
                    emit consumptionChanged();
                }
            }
            lastPowerMs_ = now;
            if (assign(power_, p)) emit powerChanged();
            break;
        }
        case vs::kEvGear: {
            static const char *names[] = {"P", "R", "N", "D"};
            const uint8_t g = r.u8();
            if (g < 4 && assign(gear_, QString::fromLatin1(names[g]))) emit gearChanged();
            break;
        }
        case vs::kEvBattery:
            soc_ = r.f32();
            range_ = r.f32();
            emit batteryChanged();
            break;
        case vs::kEvTelltales:
            if (assign(telltales_, r.u32())) emit telltalesChanged();
            break;
        case vs::kEvDriveMode:
            if (assign(driveMode_, int(r.u8()))) emit driveModeChanged();
            break;
        case vs::kEvOdometer:
            odo_ = r.f32();
            trip_ = r.f32();
            emit odometerChanged();
            break;
        case vs::kEvOutsideTemp:
            if (assign(temp_, double(r.f32()))) emit outsideTempChanged();
            break;
        default:
            break;
        }
    } catch (const someip::ParseError &e) {
        qCWarning(lcVehicle) << "bad payload for event" << Qt::hex << id << e.what();
    }
}

void VehicleState::onResponse(quint16 method, quint16, quint8 rc, const QByteArray &payload) {
    if (method == vs::kMGetVin && rc == 0) {
        const someip::Bytes b = SomeIpClient::toBytes(payload);
        someip::Reader r(b);
        vin_ = QString::fromStdString(r.str());
        qCInfo(lcVehicle) << "VIN" << vin_;
        emit vinChanged();
    } else if (method == vs::kMSetDriveMode && rc != 0) {
        qCWarning(lcVehicle) << "SetDriveMode rejected, rc" << rc;
        emit driveModeRejected();
    }
}
