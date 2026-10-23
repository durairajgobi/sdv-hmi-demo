// Minimal RAII wrapper over a POSIX UDP socket (no Qt on the "ECU" side).
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace sim {

struct Endpoint {
    uint32_t addr = 0;  // host order
    uint16_t port = 0;
    bool operator<(const Endpoint &o) const {
        return addr != o.addr ? addr < o.addr : port < o.port;
    }
    bool operator==(const Endpoint &o) const { return addr == o.addr && port == o.port; }
    std::string str() const {
        in_addr a{htonl(addr)};
        return std::string(inet_ntoa(a)) + ":" + std::to_string(port);
    }
};

class UdpSocket {
public:
    explicit UdpSocket(uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0)
            throw std::runtime_error("socket(): " + std::string(std::strerror(errno)));
        int on = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        sa.sin_port = htons(port);
        if (::bind(fd_, reinterpret_cast<sockaddr *>(&sa), sizeof sa) < 0)
            throw std::runtime_error("bind(" + std::to_string(port) +
                                     "): " + std::strerror(errno));
    }
    ~UdpSocket() { if (fd_ >= 0) ::close(fd_); }
    UdpSocket(const UdpSocket &) = delete;
    UdpSocket &operator=(const UdpSocket &) = delete;

    int fd() const { return fd_; }

    bool recv(std::vector<uint8_t> &out, Endpoint &from) {
        uint8_t buf[2048];
        sockaddr_in sa{};
        socklen_t len = sizeof sa;
        ssize_t n = ::recvfrom(fd_, buf, sizeof buf, MSG_DONTWAIT,
                               reinterpret_cast<sockaddr *>(&sa), &len);
        if (n < 0) return false;
        out.assign(buf, buf + n);
        from.addr = ntohl(sa.sin_addr.s_addr);
        from.port = ntohs(sa.sin_port);
        return true;
    }

    void send(const std::vector<uint8_t> &data, const Endpoint &to) {
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(to.addr);
        sa.sin_port = htons(to.port);
        ::sendto(fd_, data.data(), data.size(), 0,
                 reinterpret_cast<sockaddr *>(&sa), sizeof sa);
    }

private:
    int fd_ = -1;
};

} // namespace sim
