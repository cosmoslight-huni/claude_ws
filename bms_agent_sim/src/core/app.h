// Application core: owns the config, data, alarms, NTP and SNMP, and is
// driven by tick() from the main loop (Arduino loop() or the PC loop).
#pragma once

#include <memory>
#include <string>

#include "alarm_engine.h"
#include "config_store.h"
#include "hal.h"
#include "ntp_client.h"
#include "snmp_agent.h"
#include "types.h"

namespace bms {

constexpr const char* kModel = "BMS-AGENT";
constexpr const char* kFirmware = "0.1.0-sim";

class BmsMib;

struct AppOptions {
    uint32_t networkRollbackS = 120;
    uint16_t snmpPortOverride = 0;  // PC: run without root (port 161 needs it)
};

class App {
public:
    explicit App(Platform& p, AppOptions opts = AppOptions());
    ~App();

    void begin();
    void tick();

    // ---- state (read by the Web API and the SNMP MIB)
    const Config& config() const { return cfg_; }
    const BmsReading& reading() const { return reading_; }
    PackState packState() const;
    const AlarmEngine& alarms() const { return alarms_; }
    const Clock& clock() const { return clock_; }
    const NtpClient& ntp() const { return ntp_; }
    const snmp::Agent& snmpAgent() const { return *snmp_; }
    NetworkStatus networkStatus() { return p_.net.status(); }
    uint64_t uptimeMs() { return p_.sys.millis() - bootMs_; }
    uint64_t nowEpochMs() { return clock_.epochMs(p_.sys.millis()); }
    std::string localTimeString(uint64_t epoch_ms) const;
    ISystem& sys() { return p_.sys; }

    // ---- configuration changes. Inputs must already be validated;
    //      each call persists and applies the new settings.
    void updateNetwork(const NetworkConfig& c);  // starts the rollback timer
    void confirmNetwork();
    bool networkPending() const { return netPending_; }
    uint32_t networkRollbackRemainingS();
    void updateNtp(const NtpConfig& c);
    void updateAlarm(const AlarmConfig& c);
    void updateBms(const BmsConfig& c);
    bool updateSnmp(const SnmpConfig& c);
    void replaceConfig(const Config& c);  // restore; caller reboots afterwards
    void factoryReset();                  // caller reboots afterwards

    bool checkLogin(const std::string& user, const std::string& password) const;
    void setPassword(const std::string& password);

    void setTime(uint64_t epoch_ms);
    void syncTimeNow();

private:
    void save();
    void ensurePassword();
    void onAlarm(const AlarmEvent& ev);
    SnmpConfig effectiveSnmp() const;

    Platform& p_;
    ConfigStore store_;
    Config cfg_;
    BmsReading reading_;
    AlarmEngine alarms_;
    Clock clock_;
    NtpClient ntp_;
    std::unique_ptr<BmsMib> mib_;
    std::unique_ptr<snmp::Agent> snmp_;

    uint64_t bootMs_ = 0;
    uint64_t lastPollMs_ = 0;
    bool polledOnce_ = false;

    AppOptions opts_;
    bool netPending_ = false;
    uint64_t netDeadlineMs_ = 0;
    NetworkConfig netConfirmed_;
};

}  // namespace bms
