#include "app.h"

#include "bms_mib.h"
#include "util.h"

namespace bms {

App::App(Platform& p, AppOptions opts) : p_(p), store_(p.storage), ntp_(p.ntpUdp, clock_), opts_(opts) {
    mib_.reset(new BmsMib(*this));
    snmp_.reset(new snmp::Agent(p.snmpUdp, *mib_));
}

App::~App() = default;

void App::begin() {
    bootMs_ = p_.sys.millis();
    if (!store_.load(cfg_)) save();
    ensurePassword();

    p_.net.apply(cfg_.network);
    netConfirmed_ = cfg_.network;
    p_.bms.configure(cfg_.bms.cells);
    alarms_.setListener([this](const AlarmEvent& ev) { onAlarm(ev); });
    ntp_.configure(cfg_.ntp, p_.sys.millis());
    snmp_->begin(effectiveSnmp());
    logf("%s %s started", kModel, kFirmware);
}

void App::tick() {
    uint64_t now = p_.sys.millis();

    if (!polledOnce_ || now - lastPollMs_ >= cfg_.bms.poll_ms) {
        polledOnce_ = true;
        lastPollMs_ = now;
        BmsReading r;
        if (p_.bms.read(r)) {
            reading_ = r;
        } else {
            reading_.ok = false;  // keep the last values, flag them stale
        }
        alarms_.evaluate(reading_, cfg_.alarm, clock_.epochMs(now), now - bootMs_);
    }

    ntp_.tick(now);
    snmp_->poll();

    if (netPending_ && now >= netDeadlineMs_) {
        logf("network: not confirmed within %u s, rolling back to %s", opts_.networkRollbackS,
             netConfirmed_.dhcp ? "DHCP" : netConfirmed_.ip.c_str());
        netPending_ = false;
        cfg_.network = netConfirmed_;
        save();
        p_.net.apply(cfg_.network);
    }
}

PackState App::packState() const {
    if (!reading_.ok) return PackState::Fault;
    for (const AlarmEvent& a : alarms_.active())
        if (a.severity == Severity::Critical) return PackState::Fault;
    if (reading_.current_ma > 500) return PackState::Charging;
    if (reading_.current_ma < -500) return PackState::Discharging;
    return PackState::Idle;
}

std::string App::localTimeString(uint64_t epoch_ms) const {
    return formatTime((int64_t)(epoch_ms / 1000) + (int64_t)cfg_.ntp.tz_offset_min * 60);
}

void App::save() {
    if (!store_.save(cfg_)) logf("config: save failed");
}

void App::ensurePassword() {
    if (!cfg_.web.pw_hash.empty() && !cfg_.web.user.empty()) return;
    cfg_.web.user = cfg_.web.user.empty() ? "admin" : cfg_.web.user;
    setPassword("admin");
    logf("web: default login admin/admin");
}

void App::onAlarm(const AlarmEvent& ev) {
    uint32_t idx = (uint32_t)ev.code;
    auto col = [idx](uint32_t c) {
        snmp::Oid o = mib::kAlarmEntry;
        o.push_back(c);
        o.push_back(idx);
        return o;
    };
    std::vector<snmp::VarBind> vbs = {
        {col(mib::kAlarmIndex), snmp::Value::Int(idx)},
        {col(mib::kAlarmSeverity), snmp::Value::Int((int)ev.severity)},
        {col(mib::kAlarmText), snmp::Value::Str(ev.text)},
    };
    snmp_->sendTrap(ev.active ? mib::kTrapAlarmRaised : mib::kTrapAlarmCleared, vbs,
                    (uint32_t)(ev.uptime_ms / 10));
}

// ------------------------------------------------------------ config changes

void App::updateNetwork(const NetworkConfig& c) {
    if (!netPending_) netConfirmed_ = cfg_.network;
    cfg_.network = c;
    save();
    p_.net.apply(c);
    netPending_ = true;
    netDeadlineMs_ = p_.sys.millis() + (uint64_t)opts_.networkRollbackS * 1000;
    logf("network: applied, confirm within %u s or it rolls back", opts_.networkRollbackS);
}

void App::confirmNetwork() {
    if (!netPending_) return;
    netPending_ = false;
    netConfirmed_ = cfg_.network;
    logf("network: change confirmed");
}

uint32_t App::networkRollbackRemainingS() {
    if (!netPending_) return 0;
    uint64_t now = p_.sys.millis();
    return now >= netDeadlineMs_ ? 0 : (uint32_t)((netDeadlineMs_ - now + 999) / 1000);
}

void App::updateNtp(const NtpConfig& c) {
    cfg_.ntp = c;
    save();
    ntp_.configure(c, p_.sys.millis());
}

void App::updateAlarm(const AlarmConfig& c) {
    cfg_.alarm = c;
    save();
}

void App::updateBms(const BmsConfig& c) {
    cfg_.bms = c;
    save();
    p_.bms.configure(c.cells);
}

bool App::updateSnmp(const SnmpConfig& c) {
    cfg_.snmp = c;
    save();
    return snmp_->begin(effectiveSnmp());
}

SnmpConfig App::effectiveSnmp() const {
    SnmpConfig c = cfg_.snmp;
    if (opts_.snmpPortOverride) c.port = opts_.snmpPortOverride;
    return c;
}

void App::replaceConfig(const Config& c) {
    cfg_ = c;
    save();
}

void App::factoryReset() {
    store_.erase();
    logf("factory reset: configuration erased");
}

bool App::checkLogin(const std::string& user, const std::string& password) const {
    std::string h = sha256Hex(cfg_.web.pw_salt + password);
    // constant-time compare
    unsigned diff = h.size() ^ cfg_.web.pw_hash.size();
    for (size_t i = 0; i < h.size() && i < cfg_.web.pw_hash.size(); i++) diff |= h[i] ^ cfg_.web.pw_hash[i];
    return diff == 0 && user == cfg_.web.user;
}

void App::setPassword(const std::string& password) {
    uint8_t salt[16];
    p_.sys.randomBytes(salt, sizeof(salt));
    cfg_.web.pw_salt = toHex(salt, sizeof(salt));
    cfg_.web.pw_hash = sha256Hex(cfg_.web.pw_salt + password);
    save();
}

void App::setTime(uint64_t epoch_ms) {
    clock_.set(epoch_ms, p_.sys.millis());
    logf("time: set manually to %s", localTimeString(epoch_ms).c_str());
}

void App::syncTimeNow() { ntp_.syncNow(p_.sys.millis()); }

}  // namespace bms
