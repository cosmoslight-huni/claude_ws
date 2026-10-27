#include "snmp_agent.h"

#include <algorithm>

#include "util.h"

namespace bms {
namespace snmp {

namespace {

const Oid kSysUpTime = {1, 3, 6, 1, 2, 1, 1, 3, 0};
const Oid kSnmpTrapOid = {1, 3, 6, 1, 6, 3, 1, 1, 4, 1, 0};

Bytes buildMessage(int64_t version, const std::string& community, uint8_t pduType, int64_t reqId,
                   int64_t errStatus, int64_t errIndex, const Bytes& varbinds) {
    Bytes pdu = encodeInt(reqId);
    append(pdu, encodeInt(errStatus));
    append(pdu, encodeInt(errIndex));
    append(pdu, tlv(SEQUENCE, varbinds));
    Bytes msg = encodeInt(version);
    append(msg, encodeOctets(community));
    append(msg, tlv(pduType, pdu));
    return tlv(SEQUENCE, msg);
}

// v1 has a smaller error vocabulary.
int toV1Error(int err) {
    switch (err) {
        case WRONG_TYPE:
        case WRONG_VALUE: return BAD_VALUE;
        case NOT_WRITABLE:
        case NO_ACCESS: return NO_SUCH_NAME;
        default: return err;
    }
}

}  // namespace

bool Agent::begin(const SnmpConfig& cfg) {
    stop();
    cfg_ = cfg;
    if (!cfg.enable) return true;
    if (!udp_.begin(cfg.port)) {
        logf("snmp: cannot bind UDP port %u", cfg.port);
        return false;
    }
    running_ = true;
    logf("snmp: agent listening on UDP %u", cfg.port);
    return true;
}

void Agent::stop() {
    if (running_) udp_.stop();
    running_ = false;
}

void Agent::poll() {
    if (!running_) return;
    uint8_t buf[kMaxPacket];
    std::string from;
    uint16_t port;
    for (int i = 0; i < 8; i++) {  // bound the work done per tick
        int n = udp_.recvFrom(buf, sizeof(buf), from, port);
        if (n <= 0) return;
        stats_.in_pkts++;
        Bytes out;
        if (handle(buf, (size_t)n, out) && udp_.sendTo(from, port, out.data(), out.size())) stats_.out_pkts++;
    }
}

bool Agent::handle(const uint8_t* in, size_t len, Bytes& out) {
    Reader top(in, len), msg(nullptr, 0), pdu(nullptr, 0), vbl(nullptr, 0);
    int64_t version, reqId, f1, f2;
    std::string community;
    uint8_t pduType;
    if (!top.expect(SEQUENCE, msg) || !msg.readInt(version) || !msg.readOctets(community) ||
        !msg.enter(pduType, pdu) || !pdu.readInt(reqId) || !pdu.readInt(f1) || !pdu.readInt(f2) ||
        !pdu.expect(SEQUENCE, vbl)) {
        stats_.parse_errors++;
        return false;
    }
    if (version != 0 && version != 1) return false;
    bool v1 = version == 0;

    std::vector<VarBind> req;
    while (!vbl.atEnd()) {
        Reader vb(nullptr, 0);
        VarBind b;
        if (!vbl.expect(SEQUENCE, vb) || !vb.readOid(b.first) || !vb.readValue(b.second) || req.size() >= 64) {
            stats_.parse_errors++;
            return false;
        }
        req.push_back(b);
    }

    bool isSet = pduType == PDU_SET;
    bool isRead = pduType == PDU_GET || pduType == PDU_GETNEXT || (pduType == PDU_GETBULK && !v1);
    if (!isSet && !isRead) return false;
    bool rwOk = community == cfg_.rw_community;
    bool roOk = community == cfg_.ro_community;
    if (!rwOk && !roOk) {
        stats_.bad_community++;  // unknown community: drop silently
        return false;
    }

    auto encodeAll = [](const std::vector<VarBind>& list) {
        Bytes b;
        for (const auto& vb : list) append(b, encodeVarBind(vb.first, vb.second));
        return b;
    };
    auto errorReply = [&](int err, size_t idx) {
        out = buildMessage(version, community, PDU_RESPONSE, reqId, v1 ? toV1Error(err) : err, (int64_t)idx,
                           encodeAll(req));
        return true;
    };

    if (isSet) {
        if (!rwOk) return errorReply(NO_ACCESS, 1);
        for (size_t i = 0; i < req.size(); i++) {
            int err = mib_.set(req[i].first, req[i].second, false);
            if (err != NO_ERROR) return errorReply(err, i + 1);
        }
        for (const auto& vb : req) mib_.set(vb.first, vb.second, true);
        out = buildMessage(version, community, PDU_RESPONSE, reqId, 0, 0, encodeAll(req));
        return true;
    }

    std::vector<VarBind> view;
    mib_.snapshot(view);
    std::sort(view.begin(), view.end(), [](const VarBind& a, const VarBind& b) { return a.first < b.first; });
    auto cmp = [](const VarBind& a, const Oid& o) { return a.first < o; };

    auto getExact = [&](const Oid& o, VarBind& res) -> bool {
        auto it = std::lower_bound(view.begin(), view.end(), o, cmp);
        if (it != view.end() && it->first == o) {
            res = *it;
            return true;
        }
        res.first = o;
        // noSuchInstance if the object exists with another instance index.
        Oid parent(o.begin(), o.end() - (o.empty() ? 0 : 1));
        bool objectExists = !parent.empty() &&
                            ((it != view.end() && oidStartsWith(it->first, parent)) ||
                             (it != view.begin() && oidStartsWith((it - 1)->first, parent)));
        res.second = Value::Exception(objectExists ? NO_SUCH_INSTANCE : NO_SUCH_OBJECT);
        return false;
    };
    auto getNext = [&](const Oid& o, VarBind& res) -> bool {
        auto it = std::upper_bound(view.begin(), view.end(), o,
                                   [](const Oid& x, const VarBind& b) { return x < b.first; });
        if (it == view.end()) {
            res = VarBind(o, Value::Exception(END_OF_MIB_VIEW));
            return false;
        }
        res = *it;
        return true;
    };

    std::vector<VarBind> resp;
    if (pduType == PDU_GET || pduType == PDU_GETNEXT) {
        for (size_t i = 0; i < req.size(); i++) {
            VarBind r;
            bool found = pduType == PDU_GET ? getExact(req[i].first, r) : getNext(req[i].first, r);
            if (!found && v1) return errorReply(NO_SUCH_NAME, i + 1);
            resp.push_back(r);
        }
        Bytes vbs = encodeAll(resp);
        out = buildMessage(version, community, PDU_RESPONSE, reqId, 0, 0, vbs);
        if (out.size() > kMaxPacket) {
            out = buildMessage(version, community, PDU_RESPONSE, reqId, TOO_BIG, 0, Bytes());
        }
        return true;
    }

    // GETBULK: f1 = non-repeaters, f2 = max-repetitions
    size_t nonRep = (size_t)std::max<int64_t>(0, std::min<int64_t>(f1, (int64_t)req.size()));
    size_t maxRep = (size_t)std::max<int64_t>(0, std::min<int64_t>(f2, 100));
    Bytes vbs;
    const size_t budget = kMaxPacket - 64;  // room for header fields
    for (size_t i = 0; i < nonRep; i++) {
        VarBind r;
        getNext(req[i].first, r);
        append(vbs, encodeVarBind(r.first, r.second));
    }
    std::vector<Oid> cursors;
    for (size_t i = nonRep; i < req.size(); i++) cursors.push_back(req[i].first);
    bool full = false;
    for (size_t rep = 0; rep < maxRep && !cursors.empty() && !full; rep++) {
        bool allEnd = true;
        for (auto& c : cursors) {
            VarBind r;
            if (getNext(c, r)) allEnd = false;
            Bytes enc = encodeVarBind(r.first, r.second);
            if (vbs.size() + enc.size() > budget) {
                full = true;
                break;
            }
            append(vbs, enc);
            c = r.first;
        }
        if (allEnd) break;
    }
    out = buildMessage(version, community, PDU_RESPONSE, reqId, 0, 0, vbs);
    return true;
}

void Agent::sendTrap(const Oid& trapOid, const std::vector<VarBind>& extra, uint32_t uptimeTicks) {
    if (!running_ || cfg_.trap_targets.empty()) return;
    Bytes vbs = encodeVarBind(kSysUpTime, Value::Ticks(uptimeTicks));
    append(vbs, encodeVarBind(kSnmpTrapOid, Value::ObjectId(trapOid)));
    for (const auto& vb : extra) append(vbs, encodeVarBind(vb.first, vb.second));
    Bytes msg = buildMessage(1, cfg_.trap_community, PDU_TRAP_V2, trapReqId_++, 0, 0, vbs);
    for (const auto& t : cfg_.trap_targets) {
        if (udp_.sendTo(t.host, t.port, msg.data(), msg.size())) {
            stats_.traps_sent++;
        } else {
            logf("snmp: trap to %s:%u failed", t.host.c_str(), t.port);
        }
    }
}

}  // namespace snmp
}  // namespace bms
