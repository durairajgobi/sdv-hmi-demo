// SPDX-License-Identifier: MIT
// someip-lite: a small, dependency-free, wire-compatible SOME/IP codec.
//
// Implements the 16-byte SOME/IP header (AUTOSAR PRS_SOMEIP), big-endian
// payload serialization and the subset of SOME/IP-SD needed for event
// subscription (SubscribeEventgroup / SubscribeEventgroupAck / Nack).
// Packets produced here decode correctly in Wireshark's SOME/IP dissector.
//
// It is intentionally small so the demo has no external middleware
// dependency; in a production ECU this layer would be vsomeip / CommonAPI.
#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace someip {

using Bytes = std::vector<uint8_t>;

constexpr uint8_t kProtocolVersion = 0x01;
constexpr std::size_t kHeaderSize = 16;

enum class MessageType : uint8_t {
    Request = 0x00,
    RequestNoReturn = 0x01,
    Notification = 0x02,
    Response = 0x80,
    Error = 0x81,
};

enum class ReturnCode : uint8_t {
    Ok = 0x00,
    NotOk = 0x01,
    UnknownService = 0x02,
    UnknownMethod = 0x03,
    NotReady = 0x04,
    MalformedMessage = 0x09,
};

struct Header {
    uint16_t serviceId = 0;
    uint16_t methodId = 0;
    uint32_t length = 8;  // bytes after the length field: 8 + payload size
    uint16_t clientId = 0;
    uint16_t sessionId = 0;
    uint8_t protocolVersion = kProtocolVersion;
    uint8_t interfaceVersion = 1;
    MessageType messageType = MessageType::Notification;
    ReturnCode returnCode = ReturnCode::Ok;

    bool isEvent() const { return (methodId & 0x8000) != 0; }
};

struct Message {
    Header header;
    Bytes payload;
};

class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ---- Big-endian payload serializer -------------------------------------
class Writer {
public:
    Writer &u8(uint8_t v);
    Writer &u16(uint16_t v);
    Writer &u32(uint32_t v);
    Writer &i32(int32_t v) { return u32(static_cast<uint32_t>(v)); }
    Writer &f32(float v);
    // Length-prefixed (uint32) UTF-8 string. Simplification of the SOME/IP
    // dynamic-length string (no BOM / terminator), documented in docs/.
    Writer &str(const std::string &s);
    Writer &raw(const Bytes &b);

    const Bytes &bytes() const { return buf_; }
    Bytes take() { return std::move(buf_); }

private:
    Bytes buf_;
};

class Reader {
public:
    explicit Reader(const Bytes &b, std::size_t offset = 0) : b_(b), pos_(offset) {}
    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    int32_t i32() { return static_cast<int32_t>(u32()); }
    float f32();
    std::string str();
    std::size_t remaining() const { return b_.size() - pos_; }
    std::size_t pos() const { return pos_; }

private:
    void need(std::size_t n) const;
    const Bytes &b_;
    std::size_t pos_;
};

// ---- Message encoding ---------------------------------------------------
Bytes encode(const Message &m);
// Throws ParseError on truncated / inconsistent input.
Message decode(const Bytes &datagram);
// Decodes all SOME/IP messages packed in one UDP datagram.
std::vector<Message> decodeAll(const Bytes &datagram);

Message makeNotification(uint16_t service, uint16_t event, Bytes payload,
                         uint16_t session, uint8_t ifaceVersion = 1);
Message makeRequest(uint16_t service, uint16_t method, Bytes payload,
                    uint16_t client, uint16_t session, uint8_t ifaceVersion = 1);
Message makeResponse(const Message &request, Bytes payload,
                     ReturnCode rc = ReturnCode::Ok);

const char *toString(MessageType t);

} // namespace someip
