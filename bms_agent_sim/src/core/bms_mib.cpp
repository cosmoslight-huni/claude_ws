#include "bms_mib.h"

#include <algorithm>

#include "app.h"
#include "config_store.h"

namespace bms {

using snmp::Oid;
using snmp::Value;
using snmp::VarBind;

namespace {

Oid sub(const Oid& base, std::initializer_list<uint32_t> arcs) {
    Oid o = base;
    o.insert(o.end(), arcs);
    return o;
}

const Oid kSystem = {1, 3, 6, 1, 2, 1, 1};
const Oid kBmsSystem = sub(mib::kBmsAgent, {1});
const Oid kBmsStatus = sub(mib::kBmsAgent, {2});
const Oid kCellEntry = sub(mib::kBmsAgent, {3, 1});
const Oid kBmsConfig = sub(mib::kBmsAgent, {5});

// Writable bmsConfig scalars: arc -> (field, min, max)
struct ConfigField {
    uint32_t arc;
    long lo, hi;
};
const ConfigField kConfigFields[] = {{1, 2000, 5000}, {2, 1500, 4500}, {3, 0, 1200}, {4, 0, 100}};

long getField(const AlarmConfig& c, uint32_t arc) {
    switch (arc) {
        case 1: return c.cell_ov_mv;
        case 2: return c.cell_uv_mv;
        case 3: return c.temp_high_dc;
        default: return c.soc_low;
    }
}

void setField(AlarmConfig& c, uint32_t arc, long v) {
    switch (arc) {
        case 1: c.cell_ov_mv = (uint16_t)v; break;
        case 2: c.cell_uv_mv = (uint16_t)v; break;
        case 3: c.temp_high_dc = (int16_t)v; break;
        default: c.soc_low = (uint8_t)v; break;
    }
}

}  // namespace

void BmsMib::snapshot(std::vector<VarBind>& out) {
    const Config& cfg = app_.config();
    const BmsReading& r = app_.reading();
    uint32_t ticks = (uint32_t)(app_.uptimeMs() / 10);

    // MIB-II system group
    out.push_back({sub(kSystem, {1, 0}), Value::Str(std::string(kModel) + " " + kFirmware + " (PC simulator)")});
    out.push_back({sub(kSystem, {2, 0}), Value::ObjectId(mib::kBmsAgent)});
    out.push_back({sub(kSystem, {3, 0}), Value::Ticks(ticks)});
    out.push_back({sub(kSystem, {4, 0}), Value::Str(cfg.snmp.sys_contact)});
    out.push_back({sub(kSystem, {5, 0}), Value::Str(cfg.snmp.sys_name)});
    out.push_back({sub(kSystem, {6, 0}), Value::Str(cfg.snmp.sys_location)});
    out.push_back({sub(kSystem, {7, 0}), Value::Int(72)});

    // bmsSystem
    uint64_t epoch = app_.nowEpochMs();
    out.push_back({sub(kBmsSystem, {1, 0}), Value::Str(kModel)});
    out.push_back({sub(kBmsSystem, {2, 0}), Value::Str(kFirmware)});
    out.push_back({sub(kBmsSystem, {3, 0}), Value::Str(epoch ? app_.localTimeString(epoch) : "")});
    out.push_back({sub(kBmsSystem, {4, 0}), Value::Int(app_.ntp().status().synced ? 1 : 2)});

    // bmsStatus
    int maxV = 0, minV = 0, maxT = 0;
    if (!r.cell_mv.empty()) {
        maxV = *std::max_element(r.cell_mv.begin(), r.cell_mv.end());
        minV = *std::min_element(r.cell_mv.begin(), r.cell_mv.end());
    }
    if (!r.cell_temp_dc.empty()) maxT = *std::max_element(r.cell_temp_dc.begin(), r.cell_temp_dc.end());
    out.push_back({sub(kBmsStatus, {1, 0}), Value::Int(r.pack_mv)});
    out.push_back({sub(kBmsStatus, {2, 0}), Value::Int(r.current_ma)});
    out.push_back({sub(kBmsStatus, {3, 0}), Value::Int(r.soc)});
    out.push_back({sub(kBmsStatus, {4, 0}), Value::Int(r.soh)});
    out.push_back({sub(kBmsStatus, {5, 0}), Value::Int(maxV)});
    out.push_back({sub(kBmsStatus, {6, 0}), Value::Int(minV)});
    out.push_back({sub(kBmsStatus, {7, 0}), Value::Int(maxT)});
    out.push_back({sub(kBmsStatus, {8, 0}), Value::Int((int)app_.packState())});
    out.push_back({sub(kBmsStatus, {9, 0}), Value::Gauge((uint32_t)app_.alarms().activeCount())});
    out.push_back({sub(kBmsStatus, {10, 0}), Value::Int(r.ok ? 1 : 2)});
    out.push_back({sub(kBmsStatus, {11, 0}), Value::Int((int)r.cell_mv.size())});

    // bmsCellTable (column 1 = bmsCellIndex is not-accessible)
    for (uint32_t i = 0; i < r.cell_mv.size(); i++) {
        out.push_back({sub(kCellEntry, {2, i + 1}), Value::Int(r.cell_mv[i])});
        if (i < r.cell_temp_dc.size()) out.push_back({sub(kCellEntry, {3, i + 1}), Value::Int(r.cell_temp_dc[i])});
    }

    // bmsAlarmTable (active alarms, indexed by alarm code)
    for (const AlarmEvent& a : app_.alarms().active()) {
        uint32_t idx = (uint32_t)a.code;
        out.push_back({sub(mib::kAlarmEntry, {mib::kAlarmIndex, idx}), Value::Int(idx)});
        out.push_back({sub(mib::kAlarmEntry, {mib::kAlarmSeverity, idx}), Value::Int((int)a.severity)});
        out.push_back({sub(mib::kAlarmEntry, {mib::kAlarmText, idx}), Value::Str(a.text)});
        out.push_back({sub(mib::kAlarmEntry, {mib::kAlarmTime, idx}),
                       Value::Str(a.epoch_ms ? app_.localTimeString(a.epoch_ms) : "")});
    }

    // bmsConfig
    for (const ConfigField& f : kConfigFields)
        out.push_back({sub(kBmsConfig, {f.arc, 0}), Value::Int(getField(cfg.alarm, f.arc))});
}

int BmsMib::set(const Oid& oid, const Value& v, bool commit) {
    const ConfigField* field = nullptr;
    for (const ConfigField& f : kConfigFields)
        if (oid == sub(kBmsConfig, {f.arc, 0})) field = &f;
    if (!field) return snmp::NOT_WRITABLE;
    if (v.type != snmp::INTEGER) return snmp::WRONG_TYPE;
    if (v.num < field->lo || v.num > field->hi) return snmp::WRONG_VALUE;

    AlarmConfig c = app_.config().alarm;
    setField(c, field->arc, (long)v.num);
    if (!validate(c).empty()) return snmp::WRONG_VALUE;
    if (commit) app_.updateAlarm(c);
    return snmp::NO_ERROR;
}

}  // namespace bms
