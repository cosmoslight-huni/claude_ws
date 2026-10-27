// Shared data types for the BMS agent core.
// Everything under src/core is platform independent: it builds on a PC
// (simulation) and is meant to be reused as-is on ESP32 / Arduino Opta.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bms {

// ---------------------------------------------------------------- config

struct NetworkConfig {
    bool dhcp = false;
    std::string ip = "192.168.1.100";
    std::string mask = "255.255.255.0";
    std::string gateway = "192.168.1.1";
    std::string dns = "8.8.8.8";
    std::string hostname = "bms-agent";
};

struct NtpConfig {
    bool enable = true;
    std::string server1 = "pool.ntp.org";
    std::string server2 = "time.google.com";
    uint32_t interval_min = 60;
    int32_t tz_offset_min = 540;  // KST (UTC+9)
};

struct BmsConfig {
    uint32_t poll_ms = 1000;
    uint8_t cells = 16;
};

struct AlarmConfig {
    uint16_t cell_ov_mv = 3650;
    uint16_t cell_uv_mv = 2800;
    int16_t temp_high_dc = 550;  // 0.1 degC
    uint8_t soc_low = 20;        // %
};

struct TrapTarget {
    std::string host;
    uint16_t port = 162;
};

struct SnmpConfig {
    bool enable = true;
    uint16_t port = 161;
    std::string ro_community = "public";
    std::string rw_community = "private";
    std::string trap_community = "public";
    std::string sys_name = "bms-agent";
    std::string sys_contact = "admin@example.com";
    std::string sys_location = "Battery Room";
    std::vector<TrapTarget> trap_targets;
};

struct WebConfig {
    std::string user = "admin";
    std::string pw_salt;  // hex
    std::string pw_hash;  // hex sha256(salt + password)
    uint32_t session_timeout_s = 900;
};

struct Config {
    NetworkConfig network;
    NtpConfig ntp;
    BmsConfig bms;
    AlarmConfig alarm;
    SnmpConfig snmp;
    WebConfig web;
};

// ---------------------------------------------------------------- runtime

struct BmsReading {
    bool ok = false;                     // false = communication failure
    std::vector<uint16_t> cell_mv;       // per cell, mV
    std::vector<int16_t> cell_temp_dc;   // per cell, 0.1 degC
    int32_t pack_mv = 0;
    int32_t current_ma = 0;              // + charge, - discharge
    uint8_t soc = 0;                     // %
    uint8_t soh = 0;                     // %
};

enum class PackState : uint8_t { Idle = 1, Charging = 2, Discharging = 3, Fault = 4 };

enum class Severity : uint8_t { Critical = 1, Major = 2, Minor = 3 };

enum class AlarmCode : uint8_t {
    CellOverVoltage = 1,
    CellUnderVoltage = 2,
    TempHigh = 3,
    SocLow = 4,
    CommLost = 5,
};
constexpr int kAlarmCodeCount = 5;

struct AlarmEvent {
    AlarmCode code;
    Severity severity;
    bool active;           // true = raised, false = cleared
    std::string text;
    uint64_t epoch_ms;     // 0 when the clock was not set yet
    uint64_t uptime_ms;
};

struct NetworkStatus {
    bool link_up = false;
    std::string ip, mask, gateway, dns;
    bool dhcp = false;
};

}  // namespace bms
