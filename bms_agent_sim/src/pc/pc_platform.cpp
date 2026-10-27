#include "pc_platform.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

#include "../core/util.h"

namespace bms {

// ------------------------------------------------------------ storage

FileStorage::FileStorage(std::string dir) : dir_(std::move(dir)) { mkdir(dir_.c_str(), 0755); }

bool FileStorage::read(const std::string& name, std::string& out) {
    std::ifstream f(dir_ + "/" + name, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool FileStorage::writeAtomic(const std::string& name, const std::string& data) {
    std::string path = dir_ + "/" + name;
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << data;
        f.flush();
        if (!f) return false;
    }
    return rename(tmp.c_str(), path.c_str()) == 0;
}

bool FileStorage::remove(const std::string& name) { return ::remove((dir_ + "/" + name).c_str()) == 0; }

// ------------------------------------------------------------ udp

bool PosixUdp::begin(uint16_t localPort) {
    stop();
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;
    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(localPort);
    if (bind(fd_, (sockaddr*)&a, sizeof(a)) < 0) {
        logf("udp: bind %u failed: %s", localPort, strerror(errno));
        stop();
        return false;
    }
    return true;
}

void PosixUdp::stop() {
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
}

bool PosixUdp::sendTo(const std::string& host, uint16_t port, const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    sockaddr_in a = *(sockaddr_in*)res->ai_addr;
    freeaddrinfo(res);
    a.sin_port = htons(port);
    return sendto(fd_, data, len, 0, (sockaddr*)&a, sizeof(a)) == (ssize_t)len;
}

int PosixUdp::recvFrom(uint8_t* buf, size_t cap, std::string& fromIp, uint16_t& fromPort) {
    if (fd_ < 0) return -1;
    sockaddr_in a{};
    socklen_t al = sizeof(a);
    ssize_t n = recvfrom(fd_, buf, cap, 0, (sockaddr*)&a, &al);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    fromIp = ip;
    fromPort = ntohs(a.sin_port);
    return (int)n;
}

// ------------------------------------------------------------ network (simulated)

bool SimNetwork::apply(const NetworkConfig& cfg) {
    cfg_ = cfg;
    if (cfg.dhcp)
        logf("net(sim): DHCP enabled, hostname %s   [ESP32: ETH.config(INADDR_NONE...)]", cfg.hostname.c_str());
    else
        logf("net(sim): static ip=%s mask=%s gw=%s dns=%s   [ESP32: ETH.config(ip, gw, mask, dns)]",
             cfg.ip.c_str(), cfg.mask.c_str(), cfg.gateway.c_str(), cfg.dns.c_str());
    return true;
}

NetworkStatus SimNetwork::status() {
    NetworkStatus s;
    s.link_up = true;
    s.dhcp = cfg_.dhcp;
    if (cfg_.dhcp) {  // pretend lease
        s.ip = "192.168.1.57";
        s.mask = "255.255.255.0";
        s.gateway = "192.168.1.1";
        s.dns = "192.168.1.1";
    } else {
        s.ip = cfg_.ip;
        s.mask = cfg_.mask;
        s.gateway = cfg_.gateway;
        s.dns = cfg_.dns;
    }
    return s;
}

// ------------------------------------------------------------ system

PcSystem::PcSystem() {
    startNs_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint64_t PcSystem::millis() {
    uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count();
    return (ns - startNs_) / 1000000 + 1;  // never 0
}

void PcSystem::randomBytes(uint8_t* buf, size_t len) {
    std::random_device rd;
    for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)rd();
}

// ------------------------------------------------------------ http

HttpServer::~HttpServer() {
    if (listenFd_ >= 0) close(listenFd_);
}

bool HttpServer::begin(uint16_t port) {
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) return false;
    int one = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(listenFd_, (sockaddr*)&a, sizeof(a)) < 0 || listen(listenFd_, 8) < 0) {
        logf("http: cannot listen on %u: %s", port, strerror(errno));
        return false;
    }
    fcntl(listenFd_, F_SETFL, fcntl(listenFd_, F_GETFL) | O_NONBLOCK);
    logf("http: Web UI at http://localhost:%u/", port);
    return true;
}

void HttpServer::poll(const Handler& h) {
    if (listenFd_ < 0) return;
    for (int i = 0; i < 4; i++) {
        int fd = accept(listenFd_, nullptr, nullptr);
        if (fd < 0) return;
        serve(fd, h);
        close(fd);
    }
}

namespace {

const char* statusText(int s) {
    switch (s) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 429: return "Too Many Requests";
        default: return "Error";
    }
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

}  // namespace

void HttpServer::serve(int fd, const Handler& h) {
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::string data;
    char buf[4096];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return;
        data.append(buf, n);
        headerEnd = data.find("\r\n\r\n");
        if (data.size() > 16384 && headerEnd == std::string::npos) return;
    }

    HttpRequest req;
    size_t contentLength = 0;
    std::istringstream hs(data.substr(0, headerEnd));
    std::string line, version;
    std::getline(hs, line);
    std::istringstream rl(line);
    rl >> req.method >> req.path >> version;
    size_t q = req.path.find('?');
    if (q != std::string::npos) req.path.resize(q);
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string key = lower(line.substr(0, c));
        std::string val = line.substr(c + 1);
        val.erase(0, val.find_first_not_of(' '));
        if (key == "content-length") contentLength = strtoul(val.c_str(), nullptr, 10);
        else if (key == "authorization") req.authorization = val;
    }

    HttpResponse res;
    if (contentLength > 65536) {
        jsonError(res, 413, "body too large");
    } else {
        req.body = data.substr(headerEnd + 4);
        while (req.body.size() < contentLength) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) return;
            req.body.append(buf, n);
        }
        req.body.resize(contentLength);
        h(req, res);
    }

    std::string out = "HTTP/1.0 " + std::to_string(res.status) + " " + statusText(res.status) + "\r\n";
    out += "Content-Type: " + res.contentType + "\r\n";
    out += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
    out += "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    out += res.body;
    size_t sent = 0;
    while (sent < out.size()) {
        ssize_t n = send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += n;
    }
}

bool serveStatic(const std::string& root, const std::string& path, HttpResponse& res) {
    std::string p = path == "/" ? "/index.html" : path;
    if (p.find("..") != std::string::npos) return false;
    std::ifstream f(root + p, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    res.status = 200;
    res.body = ss.str();
    auto ends = [&p](const char* ext) {
        size_t n = strlen(ext);
        return p.size() >= n && p.compare(p.size() - n, n, ext) == 0;
    };
    res.contentType = ends(".html") ? "text/html; charset=utf-8"
                      : ends(".js") ? "application/javascript"
                      : ends(".css") ? "text/css"
                      : ends(".txt") ? "text/plain; charset=utf-8"
                                     : "application/octet-stream";
    return true;
}

}  // namespace bms
