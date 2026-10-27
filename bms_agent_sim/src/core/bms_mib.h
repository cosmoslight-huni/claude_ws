// BMS private MIB (see mib/BMS-SIM-MIB.txt) plus the MIB-II system group.
#pragma once

#include "snmp_agent.h"

namespace bms {

class App;

namespace mib {
// Placeholder enterprise number: replace 99999 with your IANA PEN.
const snmp::Oid kEnterprise = {1, 3, 6, 1, 4, 1, 99999};
const snmp::Oid kBmsAgent = {1, 3, 6, 1, 4, 1, 99999, 1};
const snmp::Oid kTrapAlarmRaised = {1, 3, 6, 1, 4, 1, 99999, 1, 0, 1};
const snmp::Oid kTrapAlarmCleared = {1, 3, 6, 1, 4, 1, 99999, 1, 0, 2};
// bmsAlarmEntry columns, used as trap varbinds (instance = alarm code)
const snmp::Oid kAlarmEntry = {1, 3, 6, 1, 4, 1, 99999, 1, 4, 1};
enum AlarmColumn : uint32_t { kAlarmIndex = 1, kAlarmSeverity = 2, kAlarmText = 3, kAlarmTime = 4 };
}  // namespace mib

class BmsMib : public snmp::MibProvider {
public:
    explicit BmsMib(App& app) : app_(app) {}
    void snapshot(std::vector<snmp::VarBind>& out) override;
    int set(const snmp::Oid& oid, const snmp::Value& v, bool commit) override;

private:
    App& app_;
};

}  // namespace bms
