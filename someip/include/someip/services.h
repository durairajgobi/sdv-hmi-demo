// SPDX-License-Identifier: MIT
// Service interface definitions shared by all SOME/IP participants.
// In a production toolchain these would be generated from Franca IDL / ARXML.
#pragma once

#include <cstdint>

namespace sdv {

// ---------------------------------------------------------------------------
// VehicleSignals  (provider: vehicle-sim / zonal ECU, consumer: HMI)
// ---------------------------------------------------------------------------
namespace vehicle {
constexpr uint16_t kServiceId = 0x1001;
constexpr uint16_t kInstanceId = 0x0001;
constexpr uint8_t kMajor = 1;
constexpr uint16_t kEventgroup = 0x0001;
constexpr uint16_t kPort = 30501;

// Events (bit 15 set)
constexpr uint16_t kEvSpeed = 0x8001;      // f32 km/h                 @ 20 Hz
constexpr uint16_t kEvPower = 0x8002;      // f32 kW (neg = regen)     @ 20 Hz
constexpr uint16_t kEvGear = 0x8003;       // u8 Gear                  on change + 1 Hz
constexpr uint16_t kEvBattery = 0x8004;    // f32 SoC %, f32 range km  @ 1 Hz
constexpr uint16_t kEvTelltales = 0x8005;  // u32 Telltale bitmask     on change + 1 Hz
constexpr uint16_t kEvDriveMode = 0x8006;  // u8 DriveMode             on change + 1 Hz
constexpr uint16_t kEvOdometer = 0x8007;   // f32 odo km, f32 trip km  @ 1 Hz
constexpr uint16_t kEvOutsideTemp = 0x8008;// f32 deg C                @ 1 Hz

// Methods
constexpr uint16_t kMSetDriveMode = 0x0001; // req: u8 mode  -> resp: u8 applied mode
constexpr uint16_t kMGetVin = 0x0002;       // req: -        -> resp: string VIN

enum class Gear : uint8_t { P = 0, R = 1, N = 2, D = 3 };
enum class DriveMode : uint8_t { Eco = 0, Comfort = 1, Sport = 2 };

enum Telltale : uint32_t {
    TtLeftIndicator = 1u << 0,
    TtRightIndicator = 1u << 1,
    TtHighBeam = 1u << 2,
    TtLowBeam = 1u << 3,
    TtSeatBelt = 1u << 4,
    TtParkingBrake = 1u << 5,
    TtTyrePressure = 1u << 6,
    TtBatteryLow = 1u << 7,
    TtReady = 1u << 8,
};
} // namespace vehicle

// ---------------------------------------------------------------------------
// OtaUpdate  (provider: ota-agent, consumer: HMI)
// ---------------------------------------------------------------------------
namespace ota {
constexpr uint16_t kServiceId = 0x2001;
constexpr uint16_t kInstanceId = 0x0001;
constexpr uint8_t kMajor = 1;
constexpr uint16_t kEventgroup = 0x0001;
constexpr uint16_t kPort = 30511;
constexpr uint16_t kSdPort = 30491;  // agent runs its own SD endpoint

// Event: u8 state, str installed, str available, u8 progress %,
//        str message, str releaseNotes, u8 activeSlot ('A'/'B')
constexpr uint16_t kEvStatus = 0x8001;

constexpr uint16_t kMApproveInstall = 0x0001;  // req: -           -> resp: u8 accepted
constexpr uint16_t kMDefer = 0x0002;           // req: -           -> resp: u8 accepted
constexpr uint16_t kMConfirmHealthy = 0x0003;  // req: str version -> resp: u8 accepted
constexpr uint16_t kMCheckNow = 0x0004;        // req: -           -> resp: u8 accepted
constexpr uint16_t kMReportBootFailure = 0x0005; // req: str version, str reason

enum class State : uint8_t {
    Idle = 0,
    Checking = 1,
    UpdateAvailable = 2,
    Downloading = 3,
    Verifying = 4,
    Installing = 5,
    Activating = 6,   // new slot active, waiting for HMI health confirmation
    Success = 7,
    RolledBack = 8,
    Failed = 9,
};
} // namespace ota

} // namespace sdv
