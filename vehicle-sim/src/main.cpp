// vehicle-sim: mock zonal ECU that provides the VehicleSignals SOME/IP service.
//
//  * Listens for SOME/IP-SD SubscribeEventgroup on the SD port and answers with
//    SubscribeEventgroupAck; subscriptions expire after their TTL.
//  * Publishes vehicle events to every live subscriber.
//  * Serves request/response methods (SetDriveMode, GetVin).
//
// Plain C++17 + POSIX sockets: the ECU side intentionally has no Qt dependency.
#include <poll.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "someip/sd.h"
#include "someip/services.h"
#include "someip/someip.h"
#include "udp.h"
#include "vehicle_model.h"

using namespace std::chrono;
namespace vs = sdv::vehicle;

namespace {
volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

uint16_t envPort(const char *name, uint16_t def) {
    const char *v = std::getenv(name);
    return v ? static_cast<uint16_t>(std::atoi(v)) : def;
}

void log(const char *fmt, const std::string &a = {}, const std::string &b = {}) {
    std::printf("[vehicle-sim] ");
    std::printf(fmt, a.c_str(), b.c_str());
    std::printf("\n");
    std::fflush(stdout);
}

class VehicleService {
public:
    VehicleService(uint16_t sdPort, uint16_t svcPort, std::string vin)
        : sd_(sdPort), svc_(svcPort), vin_(std::move(vin)) {
        log("SOME/IP-SD on udp/%s, VehicleSignals 0x1001 on udp/%s",
            std::to_string(sdPort), std::to_string(svcPort));
    }

    int run() {
        auto last = steady_clock::now();
        auto nextFast = last, nextSlow = last;
        while (!g_stop) {
            pollfd fds[2] = {{sd_.fd(), POLLIN, 0}, {svc_.fd(), POLLIN, 0}};
            ::poll(fds, 2, 10);
            if (fds[0].revents & POLLIN) handleSd();
            if (fds[1].revents & POLLIN) handleService();

            const auto now = steady_clock::now();
            model_.step(duration<double>(now - last).count());
            last = now;
            expireSubscriptions(now);

            if (now >= nextFast) {  // 20 Hz
                nextFast += milliseconds(50);
                publishFast();
            }
            if (now >= nextSlow) {  // 1 Hz cyclic + on-change below
                nextSlow += seconds(1);
                publishSlow();
            }
            publishOnChange();
        }
        log("shutting down");
        return 0;
    }

private:
    // ---- Service discovery ------------------------------------------------
    void handleSd() {
        someip::Bytes data;
        sim::Endpoint from;
        while (sd_.recv(data, from)) {
            try {
                for (const auto &msg : someip::decodeAll(data)) {
                    if (!someip::sd::isSd(msg)) continue;
                    onSdMessage(someip::sd::decode(msg), from);
                }
            } catch (const someip::ParseError &e) {
                log("dropped malformed SD datagram from %s: %s", from.str(), e.what());
            }
        }
    }

    void onSdMessage(const someip::sd::SdMessage &sdm, const sim::Endpoint &from) {
        for (const auto &e : sdm.entries) {
            if (e.type != someip::sd::EntryType::SubscribeEventgroup) continue;

            sim::Endpoint target = from;
            if (e.endpoint && e.endpoint->address != 0) target.addr = e.endpoint->address;
            if (e.endpoint && e.endpoint->port != 0) target.port = e.endpoint->port;

            const bool ok = e.serviceId == vs::kServiceId && e.eventgroupId == vs::kEventgroup &&
                            e.majorVersion == vs::kMajor;
            if (ok && e.ttl > 0) {
                const bool isNew = subs_.find(target) == subs_.end();
                subs_[target] = steady_clock::now() + seconds(e.ttl);
                if (isNew) {
                    log("subscriber added   %s", target.str());
                    forceResend_ = true;  // initial values for the new subscriber
                }
            } else if (ok && e.ttl == 0) {
                if (subs_.erase(target)) log("subscriber removed %s (StopSubscribe)", target.str());
            }

            // Ack (TTL echoed) or Nack (TTL 0)
            someip::sd::SdMessage ack;
            someip::sd::Entry a = e;
            a.type = someip::sd::EntryType::SubscribeEventgroupAck;
            a.ttl = ok ? e.ttl : 0;
            a.endpoint.reset();
            ack.entries.push_back(a);
            sd_.send(someip::encode(someip::sd::encode(ack, ++sdSession_)), from);
            if (!ok) log("NACK subscription from %s", from.str());
        }
    }

    void expireSubscriptions(steady_clock::time_point now) {
        for (auto it = subs_.begin(); it != subs_.end();) {
            if (it->second < now) {
                log("subscription expired %s (TTL elapsed, no renewal)", it->first.str());
                it = subs_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ---- Methods ----------------------------------------------------------
    void handleService() {
        someip::Bytes data;
        sim::Endpoint from;
        while (svc_.recv(data, from)) {
            try {
                for (const auto &req : someip::decodeAll(data))
                    if (req.header.messageType == someip::MessageType::Request)
                        svc_.send(someip::encode(dispatch(req)), from);
            } catch (const someip::ParseError &e) {
                log("dropped malformed request from %s: %s", from.str(), e.what());
            }
        }
    }

    someip::Message dispatch(const someip::Message &req) {
        if (req.header.serviceId != vs::kServiceId)
            return someip::makeResponse(req, {}, someip::ReturnCode::UnknownService);

        switch (req.header.methodId) {
        case vs::kMSetDriveMode: {
            someip::Reader r(req.payload);
            const uint8_t m = r.u8();
            if (m > static_cast<uint8_t>(vs::DriveMode::Sport))
                return someip::makeResponse(req, {}, someip::ReturnCode::NotOk);
            model_.setDriveMode(static_cast<vs::DriveMode>(m));
            static const char *names[] = {"ECO", "COMFORT", "SPORT"};
            log("SetDriveMode -> %s", names[m]);
            return someip::makeResponse(req, someip::Writer().u8(m).take());
        }
        case vs::kMGetVin:
            return someip::makeResponse(req, someip::Writer().str(vin_).take());
        default:
            return someip::makeResponse(req, {}, someip::ReturnCode::UnknownMethod);
        }
    }

    // ---- Events -----------------------------------------------------------
    void notify(uint16_t event, someip::Bytes payload) {
        if (subs_.empty()) return;
        const auto bytes = someip::encode(
            someip::makeNotification(vs::kServiceId, event, std::move(payload), ++session_));
        for (const auto &[ep, _] : subs_) svc_.send(bytes, ep);
    }

    void publishFast() {
        const auto s = model_.snapshot();
        notify(vs::kEvSpeed, someip::Writer().f32(s.speedKmh).take());
        notify(vs::kEvPower, someip::Writer().f32(s.powerKw).take());
    }

    void publishSlow() {
        const auto s = model_.snapshot();
        notify(vs::kEvBattery, someip::Writer().f32(s.socPct).f32(s.rangeKm).take());
        notify(vs::kEvOdometer, someip::Writer().f32(s.odoKm).f32(s.tripKm).take());
        notify(vs::kEvOutsideTemp, someip::Writer().f32(s.outsideTempC).take());
        forceResend_ = true;  // cyclic repetition of on-change signals
    }

    void publishOnChange() {
        const auto s = model_.snapshot();
        if (forceResend_ || s.gear != lastGear_)
            notify(vs::kEvGear, someip::Writer().u8(static_cast<uint8_t>(s.gear)).take());
        if (forceResend_ || s.telltales != lastTelltales_)
            notify(vs::kEvTelltales, someip::Writer().u32(s.telltales).take());
        if (forceResend_ || s.mode != lastMode_)
            notify(vs::kEvDriveMode, someip::Writer().u8(static_cast<uint8_t>(s.mode)).take());
        if (forceResend_) {
            notify(vs::kEvBattery, someip::Writer().f32(s.socPct).f32(s.rangeKm).take());
            notify(vs::kEvOdometer, someip::Writer().f32(s.odoKm).f32(s.tripKm).take());
        }
        lastGear_ = s.gear;
        lastTelltales_ = s.telltales;
        lastMode_ = s.mode;
        forceResend_ = false;
    }

    sim::UdpSocket sd_;
    sim::UdpSocket svc_;
    std::string vin_;
    sim::VehicleModel model_;
    std::map<sim::Endpoint, steady_clock::time_point> subs_;
    uint16_t session_ = 0;
    uint16_t sdSession_ = 0;
    bool forceResend_ = true;
    vs::Gear lastGear_ = vs::Gear::P;
    uint32_t lastTelltales_ = 0;
    vs::DriveMode lastMode_ = vs::DriveMode::Comfort;
};

} // namespace

int main() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    const char *vin = std::getenv("VEHICLE_VIN");
    try {
        VehicleService svc(envPort("SD_PORT", someip::sd::kDefaultPort),
                           envPort("SERVICE_PORT", vs::kPort),
                           vin ? vin : "MA1SDV0DEMO000001");
        return svc.run();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[vehicle-sim] fatal: %s\n", e.what());
        return 1;
    }
}
