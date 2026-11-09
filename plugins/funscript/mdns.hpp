// Finds Intiface Central / Engine on the local network by mDNS.
// Intiface announces "_intiface_engine._tcp" on port 12345 when "Broadcast Server Info via mDNS" is on
// (Intiface Central > Settings > Advanced). One PTR query, then every answer naming the service counts:
// its sender is the server.
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace mdns {

inline std::vector<std::string> find(const std::string& service = "_intiface_engine._tcp.local", int timeoutMs = 1500) {
    std::vector<std::string> found;
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return found;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(5353);  // multicast answers go to 5353 (shared with a running avahi)
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof local) != 0) {
        local.sin_port = 0;  // 5353 taken without sharing: a one-shot query gets unicast answers
        bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof local);
    }
    ip_mreq mreq{};
    inet_pton(AF_INET, "224.0.0.251", &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq);

    // Query: header (id 0, flags 0, 1 question), the name as labels, type PTR, class IN + unicast-response bit.
    std::string q("\0\0\0\0\0\1\0\0\0\0\0\0", 12);
    size_t start = 0;
    while (start < service.size()) {
        size_t dot = service.find('.', start);
        if (dot == std::string::npos) dot = service.size();
        q += static_cast<char>(dot - start);
        q += service.substr(start, dot - start);
        start = dot + 1;
    }
    q += std::string("\0\0\x0c\x80\x01", 5);
    sockaddr_in group{};
    group.sin_family = AF_INET;
    group.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &group.sin_addr);
    sendto(fd, q.data(), q.size(), 0, reinterpret_cast<sockaddr*>(&group), sizeof group);

    const std::string label = service.substr(0, service.find('.'));  // e.g. "_intiface_engine"
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    char buf[4096];
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
        if (left <= 0) break;
        pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, static_cast<int>(left)) <= 0) break;
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        const ssize_t n = recvfrom(fd, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n < 12 || (static_cast<unsigned char>(buf[2]) & 0x80) == 0) continue;  // answers only
        if (std::string(buf, static_cast<size_t>(n)).find(label) == std::string::npos) continue;
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        if (std::find(found.begin(), found.end(), ip) == found.end()) found.push_back(ip);
    }
    close(fd);
    return found;
}

}  // namespace mdns
