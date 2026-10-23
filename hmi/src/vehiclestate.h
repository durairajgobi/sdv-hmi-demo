// VehicleState: QML-facing view model for the VehicleSignals SOME/IP service.
#pragma once

#include <QObject>
#include <QString>

class SomeIpClient;

class VehicleState : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(double speed READ speed NOTIFY speedChanged)
    Q_PROPERTY(double power READ power NOTIFY powerChanged)
    Q_PROPERTY(QString gear READ gear NOTIFY gearChanged)
    Q_PROPERTY(double soc READ soc NOTIFY batteryChanged)
    Q_PROPERTY(double range READ range NOTIFY batteryChanged)
    Q_PROPERTY(int telltales READ telltales NOTIFY telltalesChanged)
    Q_PROPERTY(int driveMode READ driveMode NOTIFY driveModeChanged)
    Q_PROPERTY(QString driveModeName READ driveModeName NOTIFY driveModeChanged)
    Q_PROPERTY(double odometer READ odometer NOTIFY odometerChanged)
    Q_PROPERTY(double trip READ trip NOTIFY odometerChanged)
    Q_PROPERTY(double outsideTemp READ outsideTemp NOTIFY outsideTempChanged)
    Q_PROPERTY(QString vin READ vin NOTIFY vinChanged)
    Q_PROPERTY(double avgConsumption READ avgConsumption NOTIFY consumptionChanged)
public:
    // Telltale bits, mirrored for QML (VehicleState.LeftIndicator etc.)
    enum Telltale {
        LeftIndicator = 1 << 0,
        RightIndicator = 1 << 1,
        HighBeam = 1 << 2,
        LowBeam = 1 << 3,
        SeatBelt = 1 << 4,
        ParkingBrake = 1 << 5,
        TyrePressure = 1 << 6,
        BatteryLow = 1 << 7,
        Ready = 1 << 8,
    };
    Q_ENUM(Telltale)

    explicit VehicleState(SomeIpClient *client, QObject *parent = nullptr);

    bool connected() const;
    double speed() const { return speed_; }
    double power() const { return power_; }
    QString gear() const { return gear_; }
    double soc() const { return soc_; }
    double range() const { return range_; }
    int telltales() const { return int(telltales_); }
    int driveMode() const { return driveMode_; }
    QString driveModeName() const;
    double odometer() const { return odo_; }
    double trip() const { return trip_; }
    double outsideTemp() const { return temp_; }
    QString vin() const { return vin_; }
    double avgConsumption() const { return avgKwhPer100_; }

    Q_INVOKABLE void setDriveMode(int mode);
    Q_INVOKABLE bool hasTelltale(int bit) const { return (telltales_ & quint32(bit)) != 0; }

signals:
    void connectedChanged();
    void speedChanged();
    void powerChanged();
    void gearChanged();
    void batteryChanged();
    void telltalesChanged();
    void driveModeChanged();
    void odometerChanged();
    void outsideTempChanged();
    void vinChanged();
    void consumptionChanged();
    void driveModeRejected();

private:
    void onEvent(quint16 id, const QByteArray &payload);
    void onResponse(quint16 method, quint16 session, quint8 rc, const QByteArray &payload);

    SomeIpClient *client_;
    double speed_ = 0, power_ = 0, soc_ = 0, range_ = 0, odo_ = 0, trip_ = 0, temp_ = 0;
    QString gear_ = QStringLiteral("P");
    quint32 telltales_ = 0;
    int driveMode_ = 1;
    QString vin_;
    // simple energy integrator for the v1.1 efficiency widget
    double energyKwh_ = 0, distKm_ = 0, avgKwhPer100_ = 0;
    qint64 lastPowerMs_ = 0;
};
