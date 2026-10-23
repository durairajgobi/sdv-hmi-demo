// Longitudinal EV model driven by a scripted, repeating city/highway cycle.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

#include "someip/services.h"

namespace sim {

using sdv::vehicle::DriveMode;
using sdv::vehicle::Gear;

struct VehicleSnapshot {
    float speedKmh = 0;
    float powerKw = 0;
    Gear gear = Gear::P;
    float socPct = 0;
    float rangeKm = 0;
    uint32_t telltales = 0;
    DriveMode mode = DriveMode::Comfort;
    float odoKm = 0;
    float tripKm = 0;
    float outsideTempC = 0;
};

class VehicleModel {
public:
    VehicleModel() : rng_(42) {}

    void setDriveMode(DriveMode m) { mode_ = m; }
    DriveMode driveMode() const { return mode_; }

    // Advance the simulation by dt seconds.
    void step(double dt) {
        t_ += dt;
        const double cycleT = std::fmod(t_, kCycleSeconds);
        const Phase ph = phaseAt(cycleT);

        // Longitudinal dynamics with drive-mode dependent accel limits
        const double target = ph.targetKmh / 3.6;
        const double maxAccel = mode_ == DriveMode::Sport ? 3.5 : mode_ == DriveMode::Eco ? 1.3 : 2.2;
        const double maxDecel = 3.0;
        const double err = target - v_;
        double a = std::clamp(err * 0.8, -maxDecel, maxAccel);
        if (ph.gear != Gear::D) a = std::clamp(-v_ / std::max(dt, 1e-3), -maxDecel, 0.0);
        v_ = std::max(0.0, v_ + a * dt);
        gear_ = (v_ > 0.3 && ph.gear != Gear::D) ? Gear::D : ph.gear;

        // Tractive power: inertia + aero + rolling resistance (+ aux load)
        const double aero = 0.5 * kRho * kCd * kArea * v_ * v_;
        const double roll = v_ > 0.05 ? kCrr * kMass * 9.81 : 0.0;
        double powerW = (kMass * a + aero + roll) * v_;
        powerW = powerW > 0 ? powerW / kDriveEff : powerW * kRegenEff;
        powerW += kAuxW;
        power_ = powerW / 1000.0;

        // Battery & odometer
        socKwh_ = std::clamp(socKwh_ - (powerW * dt) / 3.6e6, 0.0, kBatteryKwh);
        const double km = v_ * dt / 1000.0;
        odo_ += km;
        trip_ += km;

        // Telltales
        uint32_t tt = sdv::vehicle::TtReady | sdv::vehicle::TtLowBeam;
        const bool blinkOn = std::fmod(t_, 0.8) < 0.4;
        if (ph.indicator < 0 && blinkOn) tt |= sdv::vehicle::TtLeftIndicator;
        if (ph.indicator > 0 && blinkOn) tt |= sdv::vehicle::TtRightIndicator;
        if (ph.highBeam) tt |= sdv::vehicle::TtHighBeam;
        if (gear_ == Gear::P) tt |= sdv::vehicle::TtParkingBrake;
        if (t_ < 4.0) tt |= sdv::vehicle::TtSeatBelt;
        if (cycleIndex() % 2 == 1 && cycleT > 40) tt |= sdv::vehicle::TtTyrePressure;
        if (socPct() < 15.0) tt |= sdv::vehicle::TtBatteryLow;
        telltales_ = tt;

        tempC_ += (std::normal_distribution<double>(0, 0.02)(rng_)) - (tempC_ - 31.0) * 0.001;
    }

    VehicleSnapshot snapshot() const {
        VehicleSnapshot s;
        s.speedKmh = static_cast<float>(v_ * 3.6);
        s.powerKw = static_cast<float>(power_);
        s.gear = gear_;
        s.socPct = static_cast<float>(socPct());
        s.rangeKm = static_cast<float>(socKwh_ / consumptionKwhPerKm());
        s.telltales = telltales_;
        s.mode = mode_;
        s.odoKm = static_cast<float>(odo_);
        s.tripKm = static_cast<float>(trip_);
        s.outsideTempC = static_cast<float>(tempC_);
        return s;
    }

private:
    struct Phase {
        double targetKmh;
        Gear gear;
        int indicator;  // -1 left, 0 none, +1 right
        bool highBeam;
    };

    static constexpr double kCycleSeconds = 96.0;

    // Scripted cycle: park -> city -> turn -> highway -> exit -> stop
    static Phase phaseAt(double t) {
        if (t < 6) return {0, Gear::P, 0, false};
        if (t < 8) return {0, Gear::D, -1, false};
        if (t < 26) return {48, Gear::D, 0, false};
        if (t < 32) return {22, Gear::D, +1, false};
        if (t < 40) return {65, Gear::D, 0, false};
        if (t < 66) return {112, Gear::D, 0, true};
        if (t < 72) return {80, Gear::D, -1, false};
        if (t < 86) return {35, Gear::D, 0, false};
        return {0, Gear::D, 0, false};
    }

    int cycleIndex() const { return static_cast<int>(t_ / kCycleSeconds); }
    double socPct() const { return 100.0 * socKwh_ / kBatteryKwh; }
    double consumptionKwhPerKm() const {
        return mode_ == DriveMode::Eco ? 0.145 : mode_ == DriveMode::Sport ? 0.19 : 0.165;
    }

    // Mid-size EV parameters
    static constexpr double kMass = 1950.0;      // kg
    static constexpr double kCd = 0.28;
    static constexpr double kArea = 2.3;         // m^2
    static constexpr double kCrr = 0.011;
    static constexpr double kRho = 1.18;         // kg/m^3 (warm air)
    static constexpr double kDriveEff = 0.90;
    static constexpr double kRegenEff = 0.65;
    static constexpr double kAuxW = 900.0;       // HVAC + electronics
    static constexpr double kBatteryKwh = 77.0;

    std::mt19937 rng_;
    double t_ = 0;
    double v_ = 0;  // m/s
    double power_ = 0;
    Gear gear_ = Gear::P;
    DriveMode mode_ = DriveMode::Comfort;
    double socKwh_ = kBatteryKwh * 0.78;
    double odo_ = 12847.3;
    double trip_ = 0;
    double tempC_ = 31.0;
    uint32_t telltales_ = 0;
};

} // namespace sim
