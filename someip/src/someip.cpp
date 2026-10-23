// SPDX-License-Identifier: MIT
#include "someip/someip.h"

namespace someip {

// ---- Writer -------------------------------------------------------------
Writer &Writer::u8(uint8_t v) {
    buf_.push_back(v);
    return *this;
}
Writer &Writer::u16(uint16_t v) {
    buf_.push_back(static_cast<uint8_t>(v >> 8));
    buf_.push_back(static_cast<uint8_t>(v));
    return *this;
}
Writer &Writer::u32(uint32_t v) {
    for (int s = 24; s >= 0; s -= 8)
        buf_.push_back(static_cast<uint8_t>(v >> s));
    return *this;
}
Writer &Writer::f32(float v) {
    static_assert(sizeof(float) == 4, "IEEE-754 binary32 expected");
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    return u32(bits);
}
Writer &Writer::str(const std::string &s) {
    u32(static_cast<uint32_t>(s.size()));
    buf_.insert(buf_.end(), s.begin(), s.end());
    return *this;
}
Writer &Writer::raw(const Bytes &b) {
    buf_.insert(buf_.end(), b.begin(), b.end());
    return *this;
}

// ---- Reader -------------------------------------------------------------
void Reader::need(std::size_t n) const {
    if (pos_ + n > b_.size())
        throw ParseError("someip: payload truncated");
}
uint8_t Reader::u8() {
    need(1);
    return b_[pos_++];
}
uint16_t Reader::u16() {
    need(2);
    uint16_t v = static_cast<uint16_t>((b_[pos_] << 8) | b_[pos_ + 1]);
    pos_ += 2;
    return v;
}
uint32_t Reader::u32() {
    need(4);
    uint32_t v = (uint32_t(b_[pos_]) << 24) | (uint32_t(b_[pos_ + 1]) << 16) |
                 (uint32_t(b_[pos_ + 2]) << 8) | uint32_t(b_[pos_ + 3]);
    pos_ += 4;
    return v;
}
float Reader::f32() {
    uint32_t bits = u32();
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}
std::string Reader::str() {
    uint32_t len = u32();
    if (len > 64 * 1024)
        throw ParseError("someip: string length implausible");
    need(len);
    std::string s(reinterpret_cast<const char *>(b_.data() + pos_), len);
    pos_ += len;
    return s;
}

// ---- Message ------------------------------------------------------------
Bytes encode(const Message &m) {
    Writer w;
    w.u16(m.header.serviceId)
        .u16(m.header.methodId)
        .u32(static_cast<uint32_t>(8 + m.payload.size()))
        .u16(m.header.clientId)
        .u16(m.header.sessionId)
        .u8(m.header.protocolVersion)
        .u8(m.header.interfaceVersion)
        .u8(static_cast<uint8_t>(m.header.messageType))
        .u8(static_cast<uint8_t>(m.header.returnCode))
        .raw(m.payload);
    return w.take();
}

static Message decodeAt(const Bytes &d, std::size_t &offset) {
    if (d.size() - offset < kHeaderSize)
        throw ParseError("someip: datagram shorter than header");
    Reader r(d, offset);
    Message m;
    m.header.serviceId = r.u16();
    m.header.methodId = r.u16();
    m.header.length = r.u32();
    m.header.clientId = r.u16();
    m.header.sessionId = r.u16();
    m.header.protocolVersion = r.u8();
    m.header.interfaceVersion = r.u8();
    m.header.messageType = static_cast<MessageType>(r.u8());
    m.header.returnCode = static_cast<ReturnCode>(r.u8());

    if (m.header.protocolVersion != kProtocolVersion)
        throw ParseError("someip: unsupported protocol version");
    if (m.header.length < 8)
        throw ParseError("someip: length field < 8");
    const std::size_t payloadLen = m.header.length - 8;
    if (r.remaining() < payloadLen)
        throw ParseError("someip: length field exceeds datagram");

    auto begin = d.begin() + static_cast<std::ptrdiff_t>(r.pos());
    m.payload.assign(begin, begin + static_cast<std::ptrdiff_t>(payloadLen));
    offset = r.pos() + payloadLen;
    return m;
}

Message decode(const Bytes &datagram) {
    std::size_t off = 0;
    return decodeAt(datagram, off);
}

std::vector<Message> decodeAll(const Bytes &datagram) {
    std::vector<Message> out;
    std::size_t off = 0;
    while (off < datagram.size())
        out.push_back(decodeAt(datagram, off));
    return out;
}

Message makeNotification(uint16_t service, uint16_t event, Bytes payload,
                         uint16_t session, uint8_t ifaceVersion) {
    Message m;
    m.header.serviceId = service;
    m.header.methodId = event;
    m.header.sessionId = session;
    m.header.interfaceVersion = ifaceVersion;
    m.header.messageType = MessageType::Notification;
    m.payload = std::move(payload);
    return m;
}

Message makeRequest(uint16_t service, uint16_t method, Bytes payload,
                    uint16_t client, uint16_t session, uint8_t ifaceVersion) {
    Message m;
    m.header.serviceId = service;
    m.header.methodId = method;
    m.header.clientId = client;
    m.header.sessionId = session;
    m.header.interfaceVersion = ifaceVersion;
    m.header.messageType = MessageType::Request;
    m.payload = std::move(payload);
    return m;
}

Message makeResponse(const Message &request, Bytes payload, ReturnCode rc) {
    Message m;
    m.header = request.header;
    m.header.messageType = rc == ReturnCode::Ok ? MessageType::Response : MessageType::Error;
    m.header.returnCode = rc;
    m.payload = std::move(payload);
    return m;
}

const char *toString(MessageType t) {
    switch (t) {
    case MessageType::Request: return "REQUEST";
    case MessageType::RequestNoReturn: return "REQUEST_NO_RETURN";
    case MessageType::Notification: return "NOTIFICATION";
    case MessageType::Response: return "RESPONSE";
    case MessageType::Error: return "ERROR";
    }
    return "UNKNOWN";
}

} // namespace someip
