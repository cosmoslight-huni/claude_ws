// Hardware abstraction layer.
// The core only talks to these interfaces. Each target (PC simulator,
// ESP32, Arduino Opta) provides its own implementation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "types.h"

namespace bms {

// Persistent storage (PC: files, ESP32: LittleFS/NVS, Opta: KVStore).
class IStorage {
public:
    virtual ~IStorage() = default;
    virtual bool read(const std::string& name, std::string& out) = 0;
    // Must be atomic: write to a temp file and rename.
    virtual bool writeAtomic(const std::string& name, const std::string& data) = 0;
    virtual bool remove(const std::string& name) = 0;
};

// Non-blocking UDP socket (PC: BSD sockets, Arduino: EthernetUDP/WiFiUDP).
class IUdp {
public:
    virtual ~IUdp() = default;
    // localPort 0 = any free port.
    virtual bool begin(uint16_t localPort) = 0;
    virtual void stop() = 0;
    virtual bool sendTo(const std::string& host, uint16_t port, const uint8_t* data, size_t len) = 0;
    // Returns received length, 0 when nothing is pending, -1 on error.
    virtual int recvFrom(uint8_t* buf, size_t cap, std::string& fromIp, uint16_t& fromPort) = 0;
};

// Network interface (PC: simulated, ESP32: ETH.config, Opta: Ethernet.begin).
class INetworkIf {
public:
    virtual ~INetworkIf() = default;
    virtual bool apply(const NetworkConfig& cfg) = 0;
    virtual NetworkStatus status() = 0;
};

// Battery data source (PC: simulator, device: RS485/Modbus/CAN driver).
class IBmsSource {
public:
    virtual ~IBmsSource() = default;
    virtual void configure(uint8_t cells) = 0;
    virtual bool read(BmsReading& out) = 0;
};

class ISystem {
public:
    virtual ~ISystem() = default;
    virtual uint64_t millis() = 0;  // monotonic
    virtual void randomBytes(uint8_t* buf, size_t len) = 0;
    virtual void reboot() = 0;
};

struct Platform {
    IStorage& storage;
    INetworkIf& net;
    IBmsSource& bms;
    IUdp& snmpUdp;
    IUdp& ntpUdp;
    ISystem& sys;
};

}  // namespace bms
