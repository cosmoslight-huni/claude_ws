// SNMP v1/v2c agent: GET, GETNEXT, GETBULK, SET and v2c traps.
// Transport-agnostic: it uses IUdp, so the same code runs on PC and MCU.
#pragma once

#include <utility>
#include <vector>

#include "hal.h"
#include "snmp_ber.h"
#include "types.h"

namespace bms {
namespace snmp {

using VarBind = std::pair<Oid, Value>;

enum ErrorStatus : int {
    NO_ERROR = 0,
    TOO_BIG = 1,
    NO_SUCH_NAME = 2,
    BAD_VALUE = 3,
    READ_ONLY = 4,
    GEN_ERR = 5,
    NO_ACCESS = 6,
    WRONG_TYPE = 7,
    WRONG_VALUE = 10,
    NOT_WRITABLE = 17,
};

class MibProvider {
public:
    virtual ~MibProvider() = default;
    // All readable instances; the agent sorts them.
    virtual void snapshot(std::vector<VarBind>& out) = 0;
    // commit=false: only check. Returns an ErrorStatus (v2c codes).
    virtual int set(const Oid& oid, const Value& v, bool commit) = 0;
};

class Agent {
public:
    struct Stats {
        uint32_t in_pkts = 0, out_pkts = 0, bad_community = 0, parse_errors = 0, traps_sent = 0;
    };

    Agent(IUdp& udp, MibProvider& mib) : udp_(udp), mib_(mib) {}

    bool begin(const SnmpConfig& cfg);
    void stop();
    void poll();
    void sendTrap(const Oid& trapOid, const std::vector<VarBind>& extra, uint32_t uptimeTicks);
    const Stats& stats() const { return stats_; }

    // Exposed for unit tests.
    bool handle(const uint8_t* in, size_t len, Bytes& out);

private:
    static constexpr size_t kMaxPacket = 1472;

    IUdp& udp_;
    MibProvider& mib_;
    SnmpConfig cfg_;
    bool running_ = false;
    int32_t trapReqId_ = 1;
    Stats stats_;
};

}  // namespace snmp
}  // namespace bms
