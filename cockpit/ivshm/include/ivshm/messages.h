// SPDX-License-Identifier: MIT
// Typed payloads carried over the inter-VM rings (same SoC => host byte order).
// decode*() validate everything: the peer partition is untrusted.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "ivshm/ivshm.h"

namespace ivshm {

enum class Maneuver : uint8_t { Straight = 0, Left = 1, Right = 2, Roundabout = 3, Arrive = 4 };

struct VehicleSpeedMsg {
    float kmh = 0;
    uint8_t gear = 0;  // P R N D
};

struct NavGuidanceMsg {
    Maneuver maneuver = Maneuver::Straight;
    float meters = 0;
    std::string street;  // <= 48 bytes
    std::string eta;     // <= 32 bytes
};

inline uint16_t encode(const VehicleSpeedMsg &m, uint8_t *out) {
    std::memcpy(out, &m.kmh, 4);
    out[4] = m.gear;
    return 5;
}

inline bool decode(const MsgSlot &s, VehicleSpeedMsg &m) {
    if (s.type != uint16_t(MsgType::VehicleSpeed) || s.len != 5) return false;
    std::memcpy(&m.kmh, s.payload, 4);
    m.gear = s.payload[4];
    return m.kmh == m.kmh && m.kmh >= 0.f && m.kmh < 400.f && m.gear <= 3;  // rejects NaN
}

inline uint16_t encode(const NavGuidanceMsg &m, uint8_t *out) {
    const auto street = m.street.substr(0, 48), eta = m.eta.substr(0, 32);
    uint16_t n = 0;
    out[n++] = uint8_t(m.maneuver);
    std::memcpy(out + n, &m.meters, 4); n += 4;
    out[n++] = uint8_t(street.size());
    std::memcpy(out + n, street.data(), street.size()); n += uint16_t(street.size());
    out[n++] = uint8_t(eta.size());
    std::memcpy(out + n, eta.data(), eta.size()); n += uint16_t(eta.size());
    return n;
}

inline bool decode(const MsgSlot &s, NavGuidanceMsg &m) {
    if (s.type != uint16_t(MsgType::NavGuidance) || s.len < 7 || s.len > kMsgPayload) return false;
    const uint8_t *p = s.payload;
    if (p[0] > uint8_t(Maneuver::Arrive)) return false;
    m.maneuver = Maneuver(p[0]);
    std::memcpy(&m.meters, p + 1, 4);
    if (!(m.meters >= 0.f && m.meters < 1.0e6f)) return false;
    std::size_t off = 5;
    const uint8_t sl = p[off++];
    if (sl > 48 || off + sl + 1 > s.len) return false;
    m.street.assign(reinterpret_cast<const char *>(p + off), sl);
    off += sl;
    const uint8_t el = p[off++];
    if (el > 32 || off + el != s.len) return false;
    m.eta.assign(reinterpret_cast<const char *>(p + off), el);
    return true;
}

} // namespace ivshm
