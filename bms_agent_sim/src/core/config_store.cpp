#include "config_store.h"

#include "util.h"

namespace bms {

namespace {

const char* kConfigFile = "config.json";
const char* kBackupFile = "config.bak";

std::string getBool(JsonVariantConst o, const char* key, bool& out) {
    JsonVariantConst v = o[key];
    if (v.isNull()) return "";
    if (!v.is<bool>()) return std::string(key) + ": boolean expected";
    out = v.as<bool>();
    return "";
}

std::string getStr(JsonVariantConst o, const char* key, std::string& out, size_t maxLen) {
    JsonVariantConst v = o[key];
    if (v.isNull()) return "";
    if (!v.is<const char*>()) return std::string(key) + ": string expected";
    std::string s = v.as<const char*>();
    if (s.size() > maxLen) return std::string(key) + ": too long";
    out = s;
    return "";
}

template <typename T>
std::string getInt(JsonVariantConst o, const char* key, T& out, long long lo, long long hi) {
    JsonVariantConst v = o[key];
    if (v.isNull()) return "";
    if (!v.is<long long>()) return std::string(key) + ": integer expected";
    long long x = v.as<long long>();
    if (x < lo || x > hi)
        return std::string(key) + ": must be " + std::to_string(lo) + ".." + std::to_string(hi);
    out = (T)x;
    return "";
}

#define TRY(expr)                        \
    do {                                 \
        std::string e_ = (expr);         \
        if (!e_.empty()) return e_;      \
    } while (0)

}  // namespace

// ------------------------------------------------------------ network

void toJson(const NetworkConfig& c, JsonObject o) {
    o["dhcp"] = c.dhcp;
    o["ip"] = c.ip;
    o["mask"] = c.mask;
    o["gateway"] = c.gateway;
    o["dns"] = c.dns;
    o["hostname"] = c.hostname;
}

std::string fromJson(JsonVariantConst v, NetworkConfig& c) {
    if (!v.is<JsonObjectConst>()) return "object expected";
    TRY(getBool(v, "dhcp", c.dhcp));
    TRY(getStr(v, "ip", c.ip, 15));
    TRY(getStr(v, "mask", c.mask, 15));
    TRY(getStr(v, "gateway", c.gateway, 15));
    TRY(getStr(v, "dns", c.dns, 15));
    TRY(getStr(v, "hostname", c.hostname, 32));
    return "";
}

std::string validate(const NetworkConfig& c) {
    if (!isValidHostname(c.hostname) || c.hostname.find('.') != std::string::npos)
        return "hostname: letters, digits and '-' only";
    if (!c.dns.empty()) {
        uint32_t dns;
        if (!parseIpv4(c.dns, dns)) return "dns: invalid IPv4 address";
    }
    if (c.dhcp) return "";
    uint32_t ip, mask, gw;
    if (!parseIpv4(c.ip, ip)) return "ip: invalid IPv4 address";
    if (!parseIpv4(c.mask, mask)) return "mask: invalid IPv4 address";
    if (mask == 0 || (~mask & (~mask + 1)) != 0) return "mask: not a contiguous netmask";
    if (mask == 0xffffffff) return "mask: /32 is not allowed";
    uint32_t host = ip & ~mask;
    if (host == 0 || host == ~mask) return "ip: network or broadcast address";
    if ((ip >> 24) == 0 || (ip >> 24) == 127 || (ip >> 24) >= 224) return "ip: reserved address";
    if (!c.gateway.empty()) {
        if (!parseIpv4(c.gateway, gw)) return "gateway: invalid IPv4 address";
        if ((gw & mask) != (ip & mask)) return "gateway: not in the same subnet as ip";
        if (gw == ip) return "gateway: same as ip";
    }
    return "";
}

// ------------------------------------------------------------ ntp

void toJson(const NtpConfig& c, JsonObject o) {
    o["enable"] = c.enable;
    o["server1"] = c.server1;
    o["server2"] = c.server2;
    o["interval_min"] = c.interval_min;
    o["tz_offset_min"] = c.tz_offset_min;
}

std::string fromJson(JsonVariantConst v, NtpConfig& c) {
    if (!v.is<JsonObjectConst>()) return "object expected";
    TRY(getBool(v, "enable", c.enable));
    TRY(getStr(v, "server1", c.server1, 63));
    TRY(getStr(v, "server2", c.server2, 63));
    TRY(getInt(v, "interval_min", c.interval_min, 1, 10080));
    TRY(getInt(v, "tz_offset_min", c.tz_offset_min, -720, 840));
    return "";
}

std::string validate(const NtpConfig& c) {
    if (!c.server1.empty() && !isValidHost(c.server1)) return "server1: invalid host";
    if (!c.server2.empty() && !isValidHost(c.server2)) return "server2: invalid host";
    if (c.enable && c.server1.empty() && c.server2.empty()) return "at least one server is required";
    return "";
}

// ------------------------------------------------------------ bms

void toJson(const BmsConfig& c, JsonObject o) {
    o["poll_ms"] = c.poll_ms;
    o["cells"] = c.cells;
}

std::string fromJson(JsonVariantConst v, BmsConfig& c) {
    if (!v.is<JsonObjectConst>()) return "object expected";
    TRY(getInt(v, "poll_ms", c.poll_ms, 100, 60000));
    TRY(getInt(v, "cells", c.cells, 1, 32));
    return "";
}

// ------------------------------------------------------------ alarm

void toJson(const AlarmConfig& c, JsonObject o) {
    o["cell_ov_mv"] = c.cell_ov_mv;
    o["cell_uv_mv"] = c.cell_uv_mv;
    o["temp_high_dc"] = c.temp_high_dc;
    o["soc_low"] = c.soc_low;
}

std::string fromJson(JsonVariantConst v, AlarmConfig& c) {
    if (!v.is<JsonObjectConst>()) return "object expected";
    TRY(getInt(v, "cell_ov_mv", c.cell_ov_mv, 2000, 5000));
    TRY(getInt(v, "cell_uv_mv", c.cell_uv_mv, 1500, 4500));
    TRY(getInt(v, "temp_high_dc", c.temp_high_dc, 0, 1200));
    TRY(getInt(v, "soc_low", c.soc_low, 0, 100));
    return "";
}

std::string validate(const AlarmConfig& c) {
    if (c.cell_uv_mv + 100 > c.cell_ov_mv) return "cell_uv_mv must be at least 100 mV below cell_ov_mv";
    return "";
}

// ------------------------------------------------------------ snmp

void toJson(const SnmpConfig& c, JsonObject o) {
    o["enable"] = c.enable;
    o["port"] = c.port;
    o["ro_community"] = c.ro_community;
    o["rw_community"] = c.rw_community;
    o["trap_community"] = c.trap_community;
    o["sys_name"] = c.sys_name;
    o["sys_contact"] = c.sys_contact;
    o["sys_location"] = c.sys_location;
    JsonArray arr = o["trap_targets"].to<JsonArray>();
    for (const auto& t : c.trap_targets) {
        JsonObject j = arr.add<JsonObject>();
        j["host"] = t.host;
        j["port"] = t.port;
    }
}

std::string fromJson(JsonVariantConst v, SnmpConfig& c) {
    if (!v.is<JsonObjectConst>()) return "object expected";
    TRY(getBool(v, "enable", c.enable));
    TRY(getInt(v, "port", c.port, 1, 65535));
    TRY(getStr(v, "ro_community", c.ro_community, 32));
    TRY(getStr(v, "rw_community", c.rw_community, 32));
    TRY(getStr(v, "trap_community", c.trap_community, 32));
    TRY(getStr(v, "sys_name", c.sys_name, 64));
    TRY(getStr(v, "sys_contact", c.sys_contact, 64));
    TRY(getStr(v, "sys_location", c.sys_location, 64));
    JsonVariantConst targets = v["trap_targets"];
    if (!targets.isNull()) {
        if (!targets.is<JsonArrayConst>()) return "trap_targets: array expected";
        if (targets.size() > 4) return "trap_targets: at most 4";
        std::vector<TrapTarget> list;
        for (JsonVariantConst t : targets.as<JsonArrayConst>()) {
            TrapTarget tt;
            if (!t.is<JsonObjectConst>()) return "trap_targets: object expected";
            TRY(getStr(t, "host", tt.host, 63));
            TRY(getInt(t, "port", tt.port, 1, 65535));
            list.push_back(tt);
        }
        c.trap_targets = list;
    }
    return "";
}

std::string validate(const SnmpConfig& c) {
    if (c.ro_community.empty()) return "ro_community: must not be empty";
    if (c.rw_community.empty()) return "rw_community: must not be empty";
    if (c.ro_community == c.rw_community) return "rw_community must differ from ro_community";
    for (const auto& t : c.trap_targets)
        if (!isValidHost(t.host)) return "trap_targets: invalid host '" + t.host + "'";
    return "";
}

// ------------------------------------------------------------ whole config

std::string ConfigStore::serialize(const Config& cfg) {
    JsonDocument doc;
    doc["version"] = 1;
    toJson(cfg.network, doc["network"].to<JsonObject>());
    toJson(cfg.ntp, doc["ntp"].to<JsonObject>());
    toJson(cfg.bms, doc["bms"].to<JsonObject>());
    toJson(cfg.alarm, doc["alarm"].to<JsonObject>());
    toJson(cfg.snmp, doc["snmp"].to<JsonObject>());
    JsonObject web = doc["web"].to<JsonObject>();
    web["user"] = cfg.web.user;
    web["pw_salt"] = cfg.web.pw_salt;
    web["pw_hash"] = cfg.web.pw_hash;
    web["session_timeout_s"] = cfg.web.session_timeout_s;
    std::string out;
    serializeJsonPretty(doc, out);
    return out;
}

std::string ConfigStore::deserialize(const std::string& json, Config& cfg) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return "invalid JSON";
    Config c;  // start from defaults so missing sections stay sane
    std::string err;
    auto section = [&](const char* name, auto& part) {
        if (!err.empty() || doc[name].isNull()) return;
        std::string e = fromJson(doc[name], part);
        if (!e.empty()) err = std::string(name) + "." + e;
    };
    section("network", c.network);
    section("ntp", c.ntp);
    section("bms", c.bms);
    section("alarm", c.alarm);
    section("snmp", c.snmp);
    if (!err.empty()) return err;
    std::string e;
    if (!(e = validate(c.network)).empty()) return "network: " + e;
    if (!(e = validate(c.ntp)).empty()) return "ntp: " + e;
    if (!(e = validate(c.alarm)).empty()) return "alarm: " + e;
    if (!(e = validate(c.snmp)).empty()) return "snmp: " + e;

    JsonVariantConst web = doc["web"];
    if (!web.isNull()) {
        if (!web.is<JsonObjectConst>()) return "web: object expected";
        TRY(getStr(web, "user", c.web.user, 32));
        TRY(getStr(web, "pw_salt", c.web.pw_salt, 64));
        TRY(getStr(web, "pw_hash", c.web.pw_hash, 64));
        TRY(getInt(web, "session_timeout_s", c.web.session_timeout_s, 60, 86400));
    }
    cfg = c;
    return "";
}

void ConfigStore::erase() {
    storage_.remove(kConfigFile);
    storage_.remove(kBackupFile);
}

bool ConfigStore::load(Config& cfg) {
    const char* files[] = {kConfigFile, kBackupFile};
    for (const char* f : files) {
        std::string json;
        if (!storage_.read(f, json)) continue;
        Config c;
        std::string err = deserialize(json, c);
        if (err.empty()) {
            cfg = c;
            logf("config loaded from %s", f);
            return true;
        }
        logf("config %s rejected: %s", f, err.c_str());
    }
    cfg = Config();
    logf("using default config");
    return false;
}

bool ConfigStore::save(const Config& cfg) {
    std::string prev;
    if (storage_.read(kConfigFile, prev)) storage_.writeAtomic(kBackupFile, prev);
    return storage_.writeAtomic(kConfigFile, serialize(cfg));
}

}  // namespace bms
