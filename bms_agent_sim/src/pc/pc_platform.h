// PC (Linux/macOS/WSL) implementations of the HAL interfaces.
#pragma once

#include <functional>
#include <string>

#include "../core/hal.h"
#include "../core/web_api.h"

namespace bms {

class FileStorage : public IStorage {
public:
    explicit FileStorage(std::string dir);
    bool read(const std::string& name, std::string& out) override;
    bool writeAtomic(const std::string& name, const std::string& data) override;
    bool remove(const std::string& name) override;

private:
    std::string dir_;
};

class PosixUdp : public IUdp {
public:
    ~PosixUdp() override { stop(); }
    bool begin(uint16_t localPort) override;
    void stop() override;
    bool sendTo(const std::string& host, uint16_t port, const uint8_t* data, size_t len) override;
    int recvFrom(uint8_t* buf, size_t cap, std::string& fromIp, uint16_t& fromPort) override;

private:
    int fd_ = -1;
};

// Pretends to configure an Ethernet interface; logs what the device would do.
class SimNetwork : public INetworkIf {
public:
    bool apply(const NetworkConfig& cfg) override;
    NetworkStatus status() override;

private:
    NetworkConfig cfg_;
};

class PcSystem : public ISystem {
public:
    PcSystem();
    uint64_t millis() override;
    void randomBytes(uint8_t* buf, size_t len) override;
    void reboot() override { rebootRequested = true; }
    bool rebootRequested = false;

private:
    uint64_t startNs_;
};

// Tiny single-threaded HTTP/1.0 server; poll() from the main loop.
class HttpServer {
public:
    using Handler = std::function<void(const HttpRequest&, HttpResponse&)>;
    ~HttpServer();
    bool begin(uint16_t port);
    void poll(const Handler& h);

private:
    void serve(int fd, const Handler& h);
    int listenFd_ = -1;
};

// Serves files below `root` ("/" -> index.html). Returns false if not found.
bool serveStatic(const std::string& root, const std::string& path, HttpResponse& res);

}  // namespace bms
