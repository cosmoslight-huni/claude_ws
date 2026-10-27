// Unit tests for the portable core (no network needed).
#include <cstdio>

#include "../src/core/alarm_engine.h"
#include "../src/core/config_store.h"
#include "../src/core/snmp_agent.h"
#include "../src/core/util.h"

using namespace bms;

static int gFail = 0;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            gFail++;                                                \
        }                                                           \
    } while (0)

static void testUtil() {
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(formatTime(0) == "1970-01-01 00:00:00");
    CHECK(formatTime(1700000000) == "2023-11-14 22:13:20");
    uint32_t ip;
    CHECK(parseIpv4("192.168.0.1", ip) && ip == 0xC0A80001);
    CHECK(!parseIpv4("192.168.0.256", ip));
    CHECK(!parseIpv4("1.2.3", ip));
    CHECK(!parseIpv4("1.2.3.4.5", ip));
}

static void testNetworkValidation() {
    NetworkConfig c;
    CHECK(validate(c).empty());
    c.gateway = "10.0.0.1";
    CHECK(!validate(c).empty());  // other subnet
    c = NetworkConfig();
    c.mask = "255.0.255.0";
    CHECK(!validate(c).empty());  // non-contiguous
    c = NetworkConfig();
    c.ip = "192.168.1.255";
    CHECK(!validate(c).empty());  // broadcast
    c.dhcp = true;
    CHECK(validate(c).empty());   // static fields ignored with DHCP
}

static void testConfigRoundTrip() {
    Config a;
    a.network.ip = "10.1.2.3";
    a.network.mask = "255.255.0.0";
    a.network.gateway = "10.1.0.1";
    a.ntp.server1 = "time.example.org";
    a.snmp.trap_targets.push_back({"192.168.1.10", 162});
    Config b;
    CHECK(ConfigStore::deserialize(ConfigStore::serialize(a), b).empty());
    CHECK(b.network.ip == "10.1.2.3");
    CHECK(b.ntp.server1 == "time.example.org");
    CHECK(b.snmp.trap_targets.size() == 1 && b.snmp.trap_targets[0].host == "192.168.1.10");
    CHECK(!ConfigStore::deserialize("{\"alarm\":{\"soc_low\":\"x\"}}", b).empty());
    CHECK(!ConfigStore::deserialize("not json", b).empty());
}

static void testAlarmHysteresis() {
    AlarmEngine e;
    int events = 0;
    e.setListener([&](const AlarmEvent&) { events++; });
    AlarmConfig cfg;
    BmsReading r;
    r.ok = true;
    r.cell_mv = {3300, 3300};
    r.cell_temp_dc = {250, 250};
    r.soc = 50;
    e.evaluate(r, cfg, 0, 0);
    CHECK(e.activeCount() == 0);
    r.cell_mv[1] = 3700;
    e.evaluate(r, cfg, 0, 0);
    CHECK(e.activeCount() == 1 && events == 1);
    r.cell_mv[1] = 3620;  // below limit but inside hysteresis
    e.evaluate(r, cfg, 0, 0);
    CHECK(e.activeCount() == 1);
    r.cell_mv[1] = 3500;
    e.evaluate(r, cfg, 0, 0);
    CHECK(e.activeCount() == 0 && events == 2);
    r.ok = false;
    e.evaluate(r, cfg, 0, 0);
    CHECK(e.activeCount() == 1 && e.active()[0].code == AlarmCode::CommLost);
}

class FakeMib : public snmp::MibProvider {
public:
    int64_t writable = 5;
    void snapshot(std::vector<snmp::VarBind>& out) override {
        out.push_back({snmp::parseOid("1.3.6.1.2.1.1.5.0"), snmp::Value::Str("bms")});
        out.push_back({snmp::parseOid("1.3.6.1.2.1.1.3.0"), snmp::Value::Ticks(4242)});
        out.push_back({snmp::parseOid("1.3.6.1.4.1.99999.1.5.4.0"), snmp::Value::Int(writable)});
    }
    int set(const snmp::Oid& oid, const snmp::Value& v, bool commit) override {
        if (oid != snmp::parseOid("1.3.6.1.4.1.99999.1.5.4.0")) return snmp::NOT_WRITABLE;
        if (v.type != snmp::INTEGER) return snmp::WRONG_TYPE;
        if (commit) writable = v.num;
        return 0;
    }
};

class NullUdp : public IUdp {
public:
    bool begin(uint16_t) override { return true; }
    void stop() override {}
    bool sendTo(const std::string&, uint16_t, const uint8_t*, size_t) override { return true; }
    int recvFrom(uint8_t*, size_t, std::string&, uint16_t&) override { return 0; }
};

static snmp::Bytes request(uint8_t pdu, const char* community, const std::string& oid,
                           snmp::Value v = snmp::Value::Null(), int version = 1) {
    using namespace snmp;
    Bytes p = encodeInt(77);
    append(p, encodeInt(0));
    append(p, encodeInt(pdu == PDU_GETBULK ? 10 : 0));
    append(p, tlv(SEQUENCE, encodeVarBind(parseOid(oid), v)));
    Bytes m = encodeInt(version);
    append(m, encodeOctets(community));
    append(m, tlv(pdu, p));
    return tlv(SEQUENCE, m);
}

struct Parsed {
    int64_t err = -1;
    std::vector<snmp::VarBind> vbs;
};

static Parsed parse(const snmp::Bytes& b) {
    using namespace snmp;
    Parsed out;
    Reader top(b.data(), b.size()), msg(nullptr, 0), pdu(nullptr, 0), vbl(nullptr, 0);
    int64_t ver, id, ei;
    std::string comm;
    uint8_t t;
    if (!top.expect(SEQUENCE, msg) || !msg.readInt(ver) || !msg.readOctets(comm) || !msg.enter(t, pdu) ||
        !pdu.readInt(id) || !pdu.readInt(out.err) || !pdu.readInt(ei) || !pdu.expect(SEQUENCE, vbl))
        return out;
    while (!vbl.atEnd()) {
        Reader vb(nullptr, 0);
        VarBind x;
        vbl.expect(SEQUENCE, vb);
        vb.readOid(x.first);
        vb.readValue(x.second);
        out.vbs.push_back(x);
    }
    return out;
}

static void testSnmp() {
    using namespace snmp;
    CHECK(encodeInt(0) == Bytes({0x02, 0x01, 0x00}));
    CHECK(encodeInt(128) == Bytes({0x02, 0x02, 0x00, 0x80}));
    CHECK(encodeInt(-1) == Bytes({0x02, 0x01, 0xff}));
    CHECK(encodeInt(0xFFFFFFFF, GAUGE32) == Bytes({0x42, 0x05, 0x00, 0xff, 0xff, 0xff, 0xff}));
    Oid big = parseOid("1.3.6.1.4.1.99999.1.300");
    Bytes enc = encodeOid(big);
    Reader r(enc.data(), enc.size());
    Oid back;
    CHECK(r.readOid(back) && back == big);

    FakeMib mib;
    NullUdp udp;
    Agent agent(udp, mib);
    SnmpConfig cfg;
    agent.begin(cfg);
    Bytes out;

    CHECK(agent.handle(request(PDU_GET, "public", "1.3.6.1.2.1.1.5.0").data(), 0, out) == false);
    Bytes req = request(PDU_GET, "public", "1.3.6.1.2.1.1.5.0");
    CHECK(agent.handle(req.data(), req.size(), out));
    Parsed p = parse(out);
    CHECK(p.err == 0 && p.vbs.size() == 1 && p.vbs[0].second.str == "bms");

    req = request(PDU_GET, "wrong", "1.3.6.1.2.1.1.5.0");
    CHECK(!agent.handle(req.data(), req.size(), out));

    req = request(PDU_GETNEXT, "public", "1.3.6.1.2.1.1");
    agent.handle(req.data(), req.size(), out);
    p = parse(out);
    CHECK(p.vbs.size() == 1 && p.vbs[0].first == parseOid("1.3.6.1.2.1.1.3.0"));

    req = request(PDU_GETBULK, "public", "1.3.6.1.2.1.1");
    agent.handle(req.data(), req.size(), out);
    p = parse(out);
    CHECK(p.vbs.size() == 4 && p.vbs.back().second.type == END_OF_MIB_VIEW);

    req = request(PDU_GET, "public", "1.3.6.1.2.1.1.5.1");
    agent.handle(req.data(), req.size(), out);
    p = parse(out);
    CHECK(p.vbs.size() == 1 && p.vbs[0].second.type == NO_SUCH_INSTANCE);

    req = request(PDU_GET, "public", "1.3.6.1.2.1.1.5.1", Value::Null(), 0);  // v1
    agent.handle(req.data(), req.size(), out);
    CHECK(parse(out).err == NO_SUCH_NAME);

    req = request(PDU_SET, "public", "1.3.6.1.4.1.99999.1.5.4.0", Value::Int(9));
    agent.handle(req.data(), req.size(), out);  // read-only community
    CHECK(parse(out).err == NO_ACCESS && mib.writable == 5);
    req = request(PDU_SET, "private", "1.3.6.1.4.1.99999.1.5.4.0", Value::Int(9));
    agent.handle(req.data(), req.size(), out);
    CHECK(parse(out).err == 0 && mib.writable == 9);
    req = request(PDU_SET, "private", "1.3.6.1.4.1.99999.1.5.4.0", Value::Str("x"));
    agent.handle(req.data(), req.size(), out);
    CHECK(parse(out).err == WRONG_TYPE && mib.writable == 9);
    req = request(PDU_SET, "private", "1.3.6.1.2.1.1.5.0", Value::Str("x"));
    agent.handle(req.data(), req.size(), out);
    CHECK(parse(out).err == NOT_WRITABLE);

    uint8_t garbage[] = {0x30, 0x05, 0x02, 0x01};
    CHECK(!agent.handle(garbage, sizeof(garbage), out));
}

int main() {
    testUtil();
    testNetworkValidation();
    testConfigRoundTrip();
    testAlarmHysteresis();
    testSnmp();
    if (gFail) {
        printf("%d check(s) failed\n", gFail);
        return 1;
    }
    printf("all core tests passed\n");
    return 0;
}
