// SPDX-License-Identifier: MIT
#include "someip/sd.h"

namespace someip::sd {

namespace {
constexpr uint8_t kOptIpv4Endpoint = 0x04;
constexpr uint16_t kIpv4OptLen = 0x0009;  // length excl. length+type fields
constexpr std::size_t kEntrySize = 16;

bool isEventgroupEntry(EntryType t) {
    return t == EntryType::SubscribeEventgroup || t == EntryType::SubscribeEventgroupAck;
}
} // namespace

Message encode(const SdMessage &sd, uint16_t session) {
    Writer entries, options;
    uint8_t optIndex = 0;

    for (const auto &e : sd.entries) {
        const bool hasOpt = e.endpoint.has_value();
        entries.u8(static_cast<uint8_t>(e.type))
            .u8(hasOpt ? optIndex : 0)  // index of 1st option run
            .u8(0)                      // index of 2nd option run
            .u8(hasOpt ? 0x10 : 0x00)   // #opt1 (hi nibble) | #opt2 (lo nibble)
            .u16(e.serviceId)
            .u16(e.instanceId)
            .u8(e.majorVersion)
            .u8(static_cast<uint8_t>(e.ttl >> 16))
            .u16(static_cast<uint16_t>(e.ttl & 0xFFFF));
        if (isEventgroupEntry(e.type))
            entries.u16(0 /* reserved + counter */).u16(e.eventgroupId);
        else
            entries.u32(e.minorVersion);

        if (hasOpt) {
            options.u16(kIpv4OptLen)
                .u8(kOptIpv4Endpoint)
                .u8(0)
                .u32(e.endpoint->address)
                .u8(0)
                .u8(e.endpoint->l4proto)
                .u16(e.endpoint->port);
            ++optIndex;
        }
    }

    Writer p;
    p.u8(sd.flags).u8(0).u16(0);  // flags + 24 bit reserved
    p.u32(static_cast<uint32_t>(entries.bytes().size())).raw(entries.bytes());
    p.u32(static_cast<uint32_t>(options.bytes().size())).raw(options.bytes());

    Message m;
    m.header.serviceId = kServiceId;
    m.header.methodId = kMethodId;
    m.header.clientId = 0x0000;
    m.header.sessionId = session;
    m.header.interfaceVersion = 0x01;
    m.header.messageType = MessageType::Notification;
    m.payload = p.take();
    return m;
}

SdMessage decode(const Message &m) {
    if (!isSd(m))
        throw ParseError("sd: not a SOME/IP-SD message");

    Reader r(m.payload);
    SdMessage sd;
    sd.flags = r.u8();
    r.u8(); r.u16();  // reserved

    const uint32_t entriesLen = r.u32();
    if (entriesLen % kEntrySize != 0 || entriesLen > r.remaining())
        throw ParseError("sd: bad entries array length");
    const std::size_t entriesStart = r.pos();

    // Parse options first so entries can resolve their indices.
    Reader ro(m.payload, entriesStart + entriesLen);
    const uint32_t optionsLen = ro.u32();
    if (optionsLen > ro.remaining())
        throw ParseError("sd: bad options array length");
    std::vector<std::optional<Ipv4Endpoint>> opts;
    const std::size_t optEnd = ro.pos() + optionsLen;
    while (ro.pos() < optEnd) {
        const uint16_t len = ro.u16();
        const uint8_t type = ro.u8();
        if (type == kOptIpv4Endpoint && len == kIpv4OptLen) {
            Ipv4Endpoint ep;
            ro.u8();
            ep.address = ro.u32();
            ro.u8();
            ep.l4proto = ro.u8();
            ep.port = ro.u16();
            opts.emplace_back(ep);
        } else {
            // Unknown option: skip (len counts bytes after the type field)
            for (uint16_t i = 0; i < len; ++i) ro.u8();
            opts.emplace_back(std::nullopt);
        }
    }

    for (uint32_t i = 0; i < entriesLen / kEntrySize; ++i) {
        Entry e;
        e.type = static_cast<EntryType>(r.u8());
        const uint8_t idx1 = r.u8();
        r.u8();  // idx2
        const uint8_t nOpts = r.u8();
        e.serviceId = r.u16();
        e.instanceId = r.u16();
        e.majorVersion = r.u8();
        e.ttl = uint32_t(r.u8()) << 16;
        e.ttl |= r.u16();
        if (isEventgroupEntry(e.type)) {
            r.u16();
            e.eventgroupId = r.u16();
        } else {
            e.minorVersion = r.u32();
        }
        if ((nOpts >> 4) > 0 && idx1 < opts.size())
            e.endpoint = opts[idx1];
        sd.entries.push_back(e);
    }
    return sd;
}

} // namespace someip::sd
