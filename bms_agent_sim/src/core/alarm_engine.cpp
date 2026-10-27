#include "alarm_engine.h"

#include <cstdio>

#include "util.h"

namespace bms {

namespace {
// Clear only after the value is back inside the limit by this margin.
constexpr int kVoltHystMv = 50;
constexpr int kTempHystDc = 20;
constexpr int kSocHyst = 2;
}  // namespace

Severity AlarmEngine::severityOf(AlarmCode c) {
    switch (c) {
        case AlarmCode::CellOverVoltage:
        case AlarmCode::CellUnderVoltage: return Severity::Critical;
        case AlarmCode::TempHigh:
        case AlarmCode::CommLost: return Severity::Major;
        default: return Severity::Minor;
    }
}

const char* AlarmEngine::nameOf(AlarmCode c) {
    switch (c) {
        case AlarmCode::CellOverVoltage: return "Cell over-voltage";
        case AlarmCode::CellUnderVoltage: return "Cell under-voltage";
        case AlarmCode::TempHigh: return "High temperature";
        case AlarmCode::SocLow: return "Low SOC";
        case AlarmCode::CommLost: return "BMS communication lost";
    }
    return "Unknown";
}

void AlarmEngine::evaluate(const BmsReading& r, const AlarmConfig& cfg, uint64_t epoch_ms, uint64_t uptime_ms) {
    char text[96];
    if (!r.ok) {
        // Keep the other alarms as they are: we just do not know.
        update(AlarmCode::CommLost, true, false, nameOf(AlarmCode::CommLost), epoch_ms, uptime_ms);
        return;
    }
    update(AlarmCode::CommLost, false, true, nameOf(AlarmCode::CommLost), epoch_ms, uptime_ms);

    size_t maxV = 0, minV = 0, maxT = 0;
    for (size_t i = 1; i < r.cell_mv.size(); i++) {
        if (r.cell_mv[i] > r.cell_mv[maxV]) maxV = i;
        if (r.cell_mv[i] < r.cell_mv[minV]) minV = i;
    }
    for (size_t i = 1; i < r.cell_temp_dc.size(); i++)
        if (r.cell_temp_dc[i] > r.cell_temp_dc[maxT]) maxT = i;

    if (!r.cell_mv.empty()) {
        int v = r.cell_mv[maxV];
        snprintf(text, sizeof(text), "Cell over-voltage: cell %u %d mV (limit %d)", (unsigned)maxV + 1, v, cfg.cell_ov_mv);
        update(AlarmCode::CellOverVoltage, v >= cfg.cell_ov_mv, v < cfg.cell_ov_mv - kVoltHystMv, text, epoch_ms, uptime_ms);

        v = r.cell_mv[minV];
        snprintf(text, sizeof(text), "Cell under-voltage: cell %u %d mV (limit %d)", (unsigned)minV + 1, v, cfg.cell_uv_mv);
        update(AlarmCode::CellUnderVoltage, v <= cfg.cell_uv_mv, v > cfg.cell_uv_mv + kVoltHystMv, text, epoch_ms, uptime_ms);
    }
    if (!r.cell_temp_dc.empty()) {
        int t = r.cell_temp_dc[maxT];
        snprintf(text, sizeof(text), "High temperature: cell %u %.1f C (limit %.1f)", (unsigned)maxT + 1, t / 10.0,
                 cfg.temp_high_dc / 10.0);
        update(AlarmCode::TempHigh, t >= cfg.temp_high_dc, t < cfg.temp_high_dc - kTempHystDc, text, epoch_ms, uptime_ms);
    }
    snprintf(text, sizeof(text), "Low SOC: %u %% (limit %u)", r.soc, cfg.soc_low);
    update(AlarmCode::SocLow, r.soc <= cfg.soc_low, r.soc > cfg.soc_low + kSocHyst, text, epoch_ms, uptime_ms);
}

void AlarmEngine::update(AlarmCode code, bool raise, bool clear, const std::string& text,
                         uint64_t epoch_ms, uint64_t uptime_ms) {
    int i = (int)code;
    bool was = active_[i];
    if (!was && raise) {
        active_[i] = true;
    } else if (was && clear) {
        active_[i] = false;
    } else {
        if (was) state_[i].text = text;  // keep the latest values visible
        return;
    }
    AlarmEvent ev{code, severityOf(code), active_[i], text, epoch_ms, uptime_ms};
    if (active_[i]) state_[i] = ev;
    history_.push_front(ev);
    if (history_.size() > kHistorySize) history_.pop_back();
    logf("alarm %s: %s", ev.active ? "RAISED" : "cleared", text.c_str());
    if (listener_) listener_(ev);
}

void AlarmEngine::reset() {
    for (bool& a : active_) a = false;
    history_.clear();
}

std::vector<AlarmEvent> AlarmEngine::active() const {
    std::vector<AlarmEvent> out;
    for (int i = 1; i <= kAlarmCodeCount; i++)
        if (active_[i]) out.push_back(state_[i]);
    return out;
}

size_t AlarmEngine::activeCount() const {
    size_t n = 0;
    for (int i = 1; i <= kAlarmCodeCount; i++) n += active_[i];
    return n;
}

}  // namespace bms
