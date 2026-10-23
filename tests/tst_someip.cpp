// Unit tests for the SOME/IP + SOME/IP-SD codec.
#include <QtTest>

#include "someip/sd.h"
#include "someip/services.h"
#include "someip/someip.h"

using namespace someip;

class TestSomeIp : public QObject {
    Q_OBJECT
private slots:
    void headerGoldenBytes();
    void roundTripRequestResponse();
    void payloadTypes();
    void rejectsTruncated();
    void rejectsBadLength();
    void rejectsWrongProtocolVersion();
    void multipleMessagesInOneDatagram();
    void sdSubscribeRoundTrip();
    void sdNackHasZeroTtl();
    void sdRejectsNonSd();
};

void TestSomeIp::headerGoldenBytes() {
    Writer w;
    w.f32(88.5f);
    auto m = makeNotification(0x1001, 0x8001, w.take(), 0x0007);
    const Bytes b = encode(m);
    const Bytes expected = {
        0x10, 0x01, 0x80, 0x01,  // service 0x1001, event 0x8001
        0x00, 0x00, 0x00, 0x0C,  // length = 8 + 4
        0x00, 0x00, 0x00, 0x07,  // client 0, session 7
        0x01, 0x01, 0x02, 0x00,  // proto 1, iface 1, NOTIFICATION, E_OK
        0x42, 0xB1, 0x00, 0x00,  // 88.5f big-endian IEEE-754
    };
    QCOMPARE(b, expected);
}

void TestSomeIp::roundTripRequestResponse() {
    Writer w;
    w.u8(static_cast<uint8_t>(sdv::vehicle::DriveMode::Sport));
    auto req = makeRequest(sdv::vehicle::kServiceId, sdv::vehicle::kMSetDriveMode, w.take(), 0x0042, 9);
    auto dec = decode(encode(req));
    QCOMPARE(dec.header.serviceId, sdv::vehicle::kServiceId);
    QCOMPARE(dec.header.methodId, sdv::vehicle::kMSetDriveMode);
    QCOMPARE(dec.header.clientId, uint16_t(0x0042));
    QCOMPARE(dec.header.sessionId, uint16_t(9));
    QVERIFY(dec.header.messageType == MessageType::Request);
    QVERIFY(!dec.header.isEvent());

    auto resp = makeResponse(dec, Bytes{2});
    auto decResp = decode(encode(resp));
    QVERIFY(decResp.header.messageType == MessageType::Response);
    QCOMPARE(decResp.header.sessionId, uint16_t(9));  // request id echoed
    QCOMPARE(decResp.payload, Bytes{2});

    auto err = makeResponse(dec, {}, ReturnCode::UnknownMethod);
    QVERIFY(err.header.messageType == MessageType::Error);
}

void TestSomeIp::payloadTypes() {
    Writer w;
    w.u8(0xAB).u16(0xBEEF).u32(0xDEADBEEF).i32(-5).f32(-12.25f).str("WVWZZZ1KZ9W386753").str("");
    Bytes b = w.take();
    Reader r(b);
    QCOMPARE(r.u8(), uint8_t(0xAB));
    QCOMPARE(r.u16(), uint16_t(0xBEEF));
    QCOMPARE(r.u32(), uint32_t(0xDEADBEEF));
    QCOMPARE(r.i32(), -5);
    QCOMPARE(r.f32(), -12.25f);
    QCOMPARE(QString::fromStdString(r.str()), QStringLiteral("WVWZZZ1KZ9W386753"));
    QCOMPARE(QString::fromStdString(r.str()), QString());
    QCOMPARE(r.remaining(), std::size_t(0));
    QVERIFY_THROWS_EXCEPTION(ParseError, r.u8());
}

void TestSomeIp::rejectsTruncated() {
    Bytes b = encode(makeNotification(1, 0x8001, {1, 2, 3, 4}, 1));
    b.resize(10);
    QVERIFY_THROWS_EXCEPTION(ParseError, decode(b));
}

void TestSomeIp::rejectsBadLength() {
    Bytes b = encode(makeNotification(1, 0x8001, {1, 2, 3, 4}, 1));
    b[7] = 0x40;  // claims 64 bytes
    QVERIFY_THROWS_EXCEPTION(ParseError, decode(b));
    b[7] = 0x04;  // < 8
    QVERIFY_THROWS_EXCEPTION(ParseError, decode(b));
}

void TestSomeIp::rejectsWrongProtocolVersion() {
    Bytes b = encode(makeNotification(1, 0x8001, {}, 1));
    b[12] = 0x02;
    QVERIFY_THROWS_EXCEPTION(ParseError, decode(b));
}

void TestSomeIp::multipleMessagesInOneDatagram() {
    Bytes a = encode(makeNotification(0x1001, 0x8001, {1, 2, 3, 4}, 1));
    Bytes b = encode(makeNotification(0x1001, 0x8003, {3}, 2));
    a.insert(a.end(), b.begin(), b.end());
    auto msgs = decodeAll(a);
    QCOMPARE(msgs.size(), std::size_t(2));
    QCOMPARE(msgs[1].header.methodId, uint16_t(0x8003));
    QCOMPARE(msgs[1].payload, Bytes{3});
}

void TestSomeIp::sdSubscribeRoundTrip() {
    sd::SdMessage s;
    sd::Entry e;
    e.type = sd::EntryType::SubscribeEventgroup;
    e.serviceId = sdv::vehicle::kServiceId;
    e.eventgroupId = sdv::vehicle::kEventgroup;
    e.ttl = 5;
    e.endpoint = sd::Ipv4Endpoint{0x7F000001, 40000, 0x11};
    s.entries.push_back(e);

    sd::Entry offer;
    offer.type = sd::EntryType::OfferService;
    offer.serviceId = sdv::ota::kServiceId;
    offer.ttl = 0xFFFFFF;
    offer.minorVersion = 3;
    s.entries.push_back(offer);

    auto msg = decode(encode(sd::encode(s, 17)));
    QVERIFY(sd::isSd(msg));
    auto d = sd::decode(msg);
    QCOMPARE(d.entries.size(), std::size_t(2));
    QVERIFY(d.entries[0].type == sd::EntryType::SubscribeEventgroup);
    QCOMPARE(d.entries[0].serviceId, sdv::vehicle::kServiceId);
    QCOMPARE(d.entries[0].eventgroupId, sdv::vehicle::kEventgroup);
    QCOMPARE(d.entries[0].ttl, uint32_t(5));
    QVERIFY(d.entries[0].endpoint.has_value());
    QCOMPARE(d.entries[0].endpoint->address, uint32_t(0x7F000001));
    QCOMPARE(d.entries[0].endpoint->port, uint16_t(40000));
    QCOMPARE(d.entries[1].ttl, uint32_t(0xFFFFFF));
    QCOMPARE(d.entries[1].minorVersion, uint32_t(3));
    QVERIFY(!d.entries[1].endpoint.has_value());
}

void TestSomeIp::sdNackHasZeroTtl() {
    sd::SdMessage s;
    sd::Entry e;
    e.type = sd::EntryType::SubscribeEventgroupAck;
    e.serviceId = 0x9999;
    e.ttl = 0;
    s.entries.push_back(e);
    auto d = sd::decode(decode(encode(sd::encode(s, 1))));
    QCOMPARE(d.entries.at(0).ttl, uint32_t(0));
}

void TestSomeIp::sdRejectsNonSd() {
    QVERIFY_THROWS_EXCEPTION(ParseError, sd::decode(makeNotification(0x1001, 0x8001, {}, 1)));
}

QTEST_APPLESS_MAIN(TestSomeIp)
#include "tst_someip.moc"
