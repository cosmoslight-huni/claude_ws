#include "web_api.h"

#include <algorithm>

#include "util.h"

namespace bms {

namespace {

const char* stateName(PackState s) {
    switch (s) {
        case PackState::Idle: return "idle";
        case PackState::Charging: return "charging";
        case PackState::Discharging: return "discharging";
        default: return "fault";
    }
}

const char* severityName(Severity s) {
    switch (s) {
        case Severity::Critical: return "critical";
        case Severity::Major: return "major";
        default: return "minor";
    }
}

void sendJson(HttpResponse& res, const JsonDocument& doc, int status = 200) {
    res.status = status;
    res.contentType = "application/json";
    res.body.clear();
    serializeJson(doc, res.body);
}

void ok(HttpResponse& res, const char* message = nullptr) {
    JsonDocument doc;
    doc["ok"] = true;
    if (message) doc["message"] = message;
    sendJson(res, doc);
}

bool parseBody(const HttpRequest& req, HttpResponse& res, JsonDocument& doc) {
    if (deserializeJson(doc, req.body)) {
        jsonError(res, 400, "invalid JSON body");
        return false;
    }
    return true;
}

}  // namespace

void jsonError(HttpResponse& res, int status, const std::string& msg) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = msg;
    sendJson(res, doc, status);
}

bool WebApi::handle(const HttpRequest& req, HttpResponse& res) {
    if (req.path.compare(0, 5, "/api/") != 0) return false;
    if (req.path == "/api/login") {
        if (req.method != "POST") jsonError(res, 405, "method not allowed");
        else login(req, res);
        return true;
    }
    if (!authorized(req)) {
        jsonError(res, 401, "login required");
        return true;
    }
    route(req, res);
    return true;
}

bool WebApi::authorized(const HttpRequest& req) {
    const std::string prefix = "Bearer ";
    if (req.authorization.compare(0, prefix.size(), prefix) != 0) return false;
    std::string token = req.authorization.substr(prefix.size());
    uint64_t now = app_.sys().millis();
    auto it = sessions_.find(token);
    if (it == sessions_.end()) return false;
    if (now > it->second) {
        sessions_.erase(it);
        return false;
    }
    it->second = now + (uint64_t)app_.config().web.session_timeout_s * 1000;  // sliding expiry
    return true;
}

void WebApi::login(const HttpRequest& req, HttpResponse& res) {
    uint64_t now = app_.sys().millis();
    if (now < lockedUntil_) {
        jsonError(res, 429, "too many failed logins, try again later");
        return;
    }
    JsonDocument body;
    if (!parseBody(req, res, body)) return;
    std::string user = body["user"] | "";
    std::string pw = body["password"] | "";
    if (!app_.checkLogin(user, pw)) {
        if (++failedLogins_ >= 5) {
            failedLogins_ = 0;
            lockedUntil_ = now + 30000;
        }
        jsonError(res, 401, "invalid user or password");
        return;
    }
    failedLogins_ = 0;

    // Drop expired sessions, keep at most 4.
    for (auto it = sessions_.begin(); it != sessions_.end();)
        it = now > it->second ? sessions_.erase(it) : std::next(it);
    while (sessions_.size() >= 4) {
        auto oldest = std::min_element(sessions_.begin(), sessions_.end(),
                                       [](const auto& a, const auto& b) { return a.second < b.second; });
        sessions_.erase(oldest);
    }
    uint8_t raw[16];
    app_.sys().randomBytes(raw, sizeof(raw));
    std::string token = toHex(raw, sizeof(raw));
    sessions_[token] = now + (uint64_t)app_.config().web.session_timeout_s * 1000;

    JsonDocument doc;
    doc["ok"] = true;
    doc["token"] = token;
    sendJson(res, doc);
}

void WebApi::route(const HttpRequest& req, HttpResponse& res) {
    const std::string& m = req.method;
    const std::string& p = req.path;
    const Config& cfg = app_.config();
    JsonDocument body;

    if (p == "/api/logout" && m == "POST") {
        if (req.authorization.size() > 7) sessions_.erase(req.authorization.substr(7));
        return ok(res);
    }

    if (p == "/api/status" && m == "GET") {
        const BmsReading& r = app_.reading();
        JsonDocument doc;
        doc["uptime_s"] = app_.uptimeMs() / 1000;
        uint64_t epoch = app_.nowEpochMs();
        JsonObject t = doc["time"].to<JsonObject>();
        t["valid"] = epoch != 0;
        t["epoch_ms"] = epoch;
        t["local"] = epoch ? app_.localTimeString(epoch) : "";
        t["ntp_synced"] = app_.ntp().status().synced;

        JsonObject pack = doc["pack"].to<JsonObject>();
        pack["comm_ok"] = r.ok;
        pack["state"] = stateName(app_.packState());
        pack["voltage_mv"] = r.pack_mv;
        pack["current_ma"] = r.current_ma;
        pack["soc"] = r.soc;
        pack["soh"] = r.soh;
        JsonArray cells = doc["cells"].to<JsonArray>();
        for (size_t i = 0; i < r.cell_mv.size(); i++) {
            JsonObject c = cells.add<JsonObject>();
            c["mv"] = r.cell_mv[i];
            c["temp_dc"] = i < r.cell_temp_dc.size() ? r.cell_temp_dc[i] : 0;
        }
        doc["alarms_active"] = app_.alarms().activeCount();
        JsonObject th = doc["thresholds"].to<JsonObject>();
        toJson(cfg.alarm, th);
        return sendJson(res, doc);
    }

    if (p == "/api/alarms" && m == "GET") {
        JsonDocument doc;
        auto fill = [this](JsonArray arr, const AlarmEvent& a) {
            JsonObject o = arr.add<JsonObject>();
            o["code"] = (int)a.code;
            o["name"] = AlarmEngine::nameOf(a.code);
            o["severity"] = severityName(a.severity);
            o["active"] = a.active;
            o["text"] = a.text;
            o["time"] = a.epoch_ms ? app_.localTimeString(a.epoch_ms) : "";
            o["uptime_s"] = a.uptime_ms / 1000;
        };
        JsonArray act = doc["active"].to<JsonArray>();
        for (const auto& a : app_.alarms().active()) fill(act, a);
        JsonArray hist = doc["history"].to<JsonArray>();
        for (const auto& a : app_.alarms().history()) fill(hist, a);
        return sendJson(res, doc);
    }

    // ---------------------------------------------------- network
    if (p == "/api/config/network") {
        if (m == "GET") {
            JsonDocument doc;
            toJson(cfg.network, doc["config"].to<JsonObject>());
            NetworkStatus st = app_.networkStatus();
            JsonObject s = doc["status"].to<JsonObject>();
            s["link_up"] = st.link_up;
            s["dhcp"] = st.dhcp;
            s["ip"] = st.ip;
            s["mask"] = st.mask;
            s["gateway"] = st.gateway;
            s["dns"] = st.dns;
            doc["pending_confirm"] = app_.networkPending();
            doc["rollback_in_s"] = app_.networkRollbackRemainingS();
            return sendJson(res, doc);
        }
        if (m == "PUT") {
            if (!parseBody(req, res, body)) return;
            NetworkConfig c = cfg.network;
            std::string err = fromJson(body.as<JsonVariantConst>(), c);
            if (err.empty()) err = validate(c);
            if (!err.empty()) return jsonError(res, 400, err);
            app_.updateNetwork(c);
            JsonDocument doc;
            doc["ok"] = true;
            doc["rollback_in_s"] = app_.networkRollbackRemainingS();
            doc["new_address"] = c.dhcp ? "" : c.ip;
            doc["message"] = "Applied. Reconnect with the new address and confirm, or it will be rolled back.";
            return sendJson(res, doc);
        }
    }
    if (p == "/api/config/network/confirm" && m == "POST") {
        if (!app_.networkPending()) return jsonError(res, 409, "no pending network change");
        app_.confirmNetwork();
        return ok(res, "network settings confirmed");
    }

    // ---------------------------------------------------- ntp / time
    if (p == "/api/config/ntp") {
        if (m == "GET") {
            JsonDocument doc;
            toJson(cfg.ntp, doc["config"].to<JsonObject>());
            const NtpClient::Status& s = app_.ntp().status();
            JsonObject st = doc["status"].to<JsonObject>();
            st["synced"] = s.synced;
            st["in_progress"] = s.in_progress;
            st["last_server"] = s.last_server;
            st["last_error"] = s.last_error;
            st["last_sync_ago_s"] = s.synced ? (app_.sys().millis() - s.last_sync_uptime_ms) / 1000 : 0;
            st["last_adjust_ms"] = s.last_adjust_ms;
            return sendJson(res, doc);
        }
        if (m == "PUT") {
            if (!parseBody(req, res, body)) return;
            NtpConfig c = cfg.ntp;
            std::string err = fromJson(body.as<JsonVariantConst>(), c);
            if (err.empty()) err = validate(c);
            if (!err.empty()) return jsonError(res, 400, err);
            app_.updateNtp(c);
            return ok(res, "NTP settings saved");
        }
    }
    if (p == "/api/system/time-sync" && m == "POST") {
        if (!cfg.ntp.enable) return jsonError(res, 409, "NTP is disabled");
        app_.syncTimeNow();
        return ok(res, "sync started");
    }
    if (p == "/api/system/time") {
        if (m == "GET") {
            JsonDocument doc;
            uint64_t e = app_.nowEpochMs();
            doc["valid"] = e != 0;
            doc["epoch_ms"] = e;
            doc["local"] = e ? app_.localTimeString(e) : "";
            doc["tz_offset_min"] = cfg.ntp.tz_offset_min;
            return sendJson(res, doc);
        }
        if (m == "PUT") {
            if (!parseBody(req, res, body)) return;
            if (!body["epoch_ms"].is<uint64_t>()) return jsonError(res, 400, "epoch_ms: integer expected");
            uint64_t e = body["epoch_ms"].as<uint64_t>();
            if (e < 1577836800000ULL) return jsonError(res, 400, "epoch_ms: before 2020");
            app_.setTime(e);
            return ok(res, cfg.ntp.enable ? "time set (NTP will override it on next sync)" : "time set");
        }
    }

    // ---------------------------------------------------- alarm thresholds / bms
    if (p == "/api/config/alarm") {
        if (m == "GET") {
            JsonDocument doc;
            toJson(cfg.alarm, doc["config"].to<JsonObject>());
            return sendJson(res, doc);
        }
        if (m == "PUT") {
            if (!parseBody(req, res, body)) return;
            AlarmConfig c = cfg.alarm;
            std::string err = fromJson(body.as<JsonVariantConst>(), c);
            if (err.empty()) err = validate(c);
            if (!err.empty()) return jsonError(res, 400, err);
            app_.updateAlarm(c);
            return ok(res, "alarm thresholds saved");
        }
    }
    if (p == "/api/config/bms") {
        if (m == "GET") {
            JsonDocument doc;
            toJson(cfg.bms, doc["config"].to<JsonObject>());
            return sendJson(res, doc);
        }
        if (m == "PUT") {
            if (!parseBody(req, res, body)) return;
            BmsConfig c = cfg.bms;
            std::string err = fromJson(body.as<JsonVariantConst>(), c);
            if (!err.empty()) return jsonError(res, 400, err);
            app_.updateBms(c);
            return ok(res, "BMS settings saved");
        }
    }

    // ---------------------------------------------------- snmp
    if (p == "/api/config/snmp") {
        if (m == "GET") {
            JsonDocument doc;
            toJson(cfg.snmp, doc["config"].to<JsonObject>());
            const snmp::Agent::Stats& s = app_.snmpAgent().stats();
            JsonObject st = doc["stats"].to<JsonObject>();
            st["in_pkts"] = s.in_pkts;
            st["out_pkts"] = s.out_pkts;
            st["bad_community"] = s.bad_community;
            st["parse_errors"] = s.parse_errors;
            st["traps_sent"] = s.traps_sent;
            return sendJson(res, doc);
        }
        if (m == "PUT") {
            if (!parseBody(req, res, body)) return;
            SnmpConfig c = cfg.snmp;
            std::string err = fromJson(body.as<JsonVariantConst>(), c);
            if (err.empty()) err = validate(c);
            if (!err.empty()) return jsonError(res, 400, err);
            if (!app_.updateSnmp(c)) return jsonError(res, 500, "saved, but the SNMP port could not be opened");
            return ok(res, "SNMP settings saved");
        }
    }

    // ---------------------------------------------------- account / system
    if (p == "/api/config/password" && m == "PUT") {
        if (!parseBody(req, res, body)) return;
        std::string oldPw = body["old_password"] | "";
        std::string newPw = body["new_password"] | "";
        if (!app_.checkLogin(cfg.web.user, oldPw)) return jsonError(res, 403, "current password is wrong");
        if (newPw.size() < 6 || newPw.size() > 64) return jsonError(res, 400, "new password must be 6-64 characters");
        app_.setPassword(newPw);
        sessions_.clear();
        return ok(res, "password changed, please log in again");
    }
    if (p == "/api/config/backup" && m == "GET") {
        res.status = 200;
        res.contentType = "application/json";
        res.body = ConfigStore::serialize(cfg);
        return;
    }
    if (p == "/api/config/restore" && m == "POST") {
        Config c;
        std::string err = ConfigStore::deserialize(req.body, c);
        if (!err.empty()) return jsonError(res, 400, err);
        if (c.web.pw_hash.empty()) c.web = cfg.web;  // keep the current login
        app_.replaceConfig(c);
        reboot_ = true;
        return ok(res, "configuration restored, rebooting");
    }
    if (p == "/api/system/info" && m == "GET") {
        JsonDocument doc;
        doc["model"] = kModel;
        doc["firmware"] = kFirmware;
        doc["uptime_s"] = app_.uptimeMs() / 1000;
        doc["hostname"] = cfg.network.hostname;
        return sendJson(res, doc);
    }
    if (p == "/api/system/reboot" && m == "POST") {
        reboot_ = true;
        return ok(res, "rebooting");
    }
    if (p == "/api/system/factory-reset" && m == "POST") {
        app_.factoryReset();
        reboot_ = true;
        return ok(res, "factory reset, rebooting with default settings");
    }

    if (ext_ && ext_(req, res)) return;
    jsonError(res, 404, "no such endpoint: " + m + " " + p);
}

}  // namespace bms
