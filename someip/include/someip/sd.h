// SPDX-License-Identifier: MIT
// SOME/IP Service Discovery (subset): eventgroup subscription handshake.
//
//   client                                server (provider)
//     | -- SubscribeEventgroup(TTL, ep) -->  |   SD port 30490
//     | <-- SubscribeEventgroupAck(TTL) ---  |
//     | <======== event notifications ====== |   to subscriber endpoint
//     | -- (renew before TTL expires) ---->  |
//
// A SubscribeEventgroupAck with TTL == 0 is a Nack.
#pragma once

#include "someip/someip.h"

namespace someip::sd {

constexpr uint16_t kServiceId = 0xFFFF;
constexpr uint16_t kMethodId = 0x8100;
constexpr uint16_t kDefaultPort = 30490;
constexpr uint8_t kRebootFlag = 0x80;
constexpr uint8_t kUnicastFlag = 0x40;

enum class EntryType : uint8_t {
    FindService = 0x00,
    OfferService = 0x01,  // StopOffer = Offer with TTL 0
    SubscribeEventgroup = 0x06,
    SubscribeEventgroupAck = 0x07,  // Nack = Ack with TTL 0
};

struct Ipv4Endpoint {
    uint32_t address = 0;  // host byte order; 0 => use packet source address
    uint16_t port = 0;
    uint8_t l4proto = 0x11;  // UDP
};

struct Entry {
    EntryType type = EntryType::SubscribeEventgroup;
    uint16_t serviceId = 0;
    uint16_t instanceId = 0x0001;
    uint8_t majorVersion = 1;
    uint32_t ttl = 0;          // 24 bit, seconds
    uint16_t eventgroupId = 0; // eventgroup entries
    uint32_t minorVersion = 0; // service entries
    std::optional<Ipv4Endpoint> endpoint;
};

struct SdMessage {
    uint8_t flags = kRebootFlag | kUnicastFlag;
    std::vector<Entry> entries;
};

Message encode(const SdMessage &sd, uint16_t session);
// Throws ParseError if the message is not a well-formed SD message.
SdMessage decode(const Message &m);
inline bool isSd(const Message &m) {
    return m.header.serviceId == kServiceId && m.header.methodId == kMethodId;
}

} // namespace someip::sd
