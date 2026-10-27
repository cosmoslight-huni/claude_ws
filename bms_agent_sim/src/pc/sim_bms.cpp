#include "sim_bms.h"

#include <algorithm>
#include <cmath>

namespace bms {

namespace {

// LFP open-circuit voltage vs SOC
double ocv(double soc) {
    static const double pts[][2] = {{0, 2.90},  {5, 3.15},  {10, 3.22}, {30, 3.27},
                                    {70, 3.31}, {90, 3.33}, {98, 3.40}, {100, 3.45}};
    soc = std::max(0.0, std::min(100.0, soc));
    for (size_t i = 1; i < sizeof(pts) / sizeof(pts[0]); i++) {
        if (soc <= pts[i][0]) {
            double f = (soc - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0]);
            return pts[i - 1][1] + f * (pts[i][1] - pts[i - 1][1]);
        }
    }
    return pts[7][1];
}

const char* modeName(SimBms::Mode m) {
    switch (m) {
        case SimBms::Mode::Charge: return "charge";
        case SimBms::Mode::Discharge: return "discharge";
        case SimBms::Mode::Idle: return "idle";
        default: return "auto";
    }
}

}  // namespace

void SimBms::configure(uint8_t cells) {
    temp_.assign(cells, 25.0);
    mvOffset_.assign(cells, 0);
    tempOffsetDc_.assign(cells, 0);
}

void SimBms::step(double dtS) {
    switch (mode_) {
        case Mode::Auto:
            if (charging_ && soc_ >= 95) charging_ = false;
            if (!charging_ && soc_ <= 30) charging_ = true;
            currentA_ = charging_ ? kCurrentA : -kCurrentA;
            break;
        case Mode::Charge: currentA_ = soc_ >= 100 ? 0 : kCurrentA; break;
        case Mode::Discharge: currentA_ = soc_ <= 0 ? 0 : -kCurrentA; break;
        case Mode::Idle: currentA_ = 0; break;
    }
    double simS = dtS * speed_;
    soc_ += currentA_ * (simS / 3600.0) / kCapacityAh * 100.0;
    soc_ = std::max(0.0, std::min(100.0, soc_));
    double k = std::min(1.0, simS / 600.0);  // ~10 min thermal time constant
    for (size_t i = 0; i < temp_.size(); i++) {
        double target = 25.0 + std::fabs(currentA_) * 0.08 + (i % 4) * 0.3;
        temp_[i] += (target - temp_[i]) * k;
    }
}

bool SimBms::read(BmsReading& out) {
    uint64_t now = sys_.millis();
    if (lastMs_ != 0) step((now - lastMs_) / 1000.0);
    lastMs_ = now;
    if (commFail_) return false;

    std::uniform_int_distribution<int> noise(-2, 2);
    out = BmsReading();
    out.ok = true;
    double base = ocv(soc_) * 1000.0 + currentA_ * 1.0;  // 1 mOhm per cell
    for (size_t i = 0; i < temp_.size(); i++) {
        int mv = (int)std::lround(base) + (int)(i * 7 % 11) - 5 + noise(rng_) + mvOffset_[i];
        out.cell_mv.push_back((uint16_t)std::max(0, mv));
        out.cell_temp_dc.push_back((int16_t)std::lround(temp_[i] * 10) + tempOffsetDc_[i]);
        out.pack_mv += out.cell_mv.back();
    }
    out.current_ma = (int32_t)std::lround(currentA_ * 1000) + noise(rng_) * 10;
    out.soc = (uint8_t)std::lround(soc_);
    out.soh = 98;
    return true;
}

void SimBms::toJson(JsonObject o) const {
    o["mode"] = modeName(mode_);
    o["speed"] = speed_;
    o["comm_fail"] = commFail_;
    o["soc"] = std::round(soc_ * 10) / 10;
    JsonArray mv = o["cell_mv_offset"].to<JsonArray>();
    for (int v : mvOffset_) mv.add(v);
    JsonArray t = o["cell_temp_offset_dc"].to<JsonArray>();
    for (int v : tempOffsetDc_) t.add(v);
}

std::string SimBms::fromJson(JsonVariantConst v) {
    if (!v.is<JsonObjectConst>()) return "object expected";
    if (v["mode"].is<const char*>()) {
        std::string m = v["mode"].as<const char*>();
        if (m == "auto") mode_ = Mode::Auto;
        else if (m == "charge") mode_ = Mode::Charge;
        else if (m == "discharge") mode_ = Mode::Discharge;
        else if (m == "idle") mode_ = Mode::Idle;
        else return "mode: auto|charge|discharge|idle";
    }
    if (v["speed"].is<double>()) {
        double s = v["speed"].as<double>();
        if (s < 1 || s > 3600) return "speed: 1..3600";
        speed_ = s;
    }
    if (v["comm_fail"].is<bool>()) commFail_ = v["comm_fail"].as<bool>();
    if (v["soc"].is<double>()) soc_ = std::max(0.0, std::min(100.0, v["soc"].as<double>()));
    // {"cell": 3, "mv_offset": 400} / {"cell": 5, "temp_offset_dc": 350}  (cell is 1-based)
    if (!v["cell"].isNull()) {
        int c = v["cell"] | 0;
        if (c < 1 || c > (int)mvOffset_.size()) return "cell: out of range";
        if (v["mv_offset"].is<int>()) mvOffset_[c - 1] = v["mv_offset"].as<int>();
        if (v["temp_offset_dc"].is<int>()) tempOffsetDc_[c - 1] = v["temp_offset_dc"].as<int>();
    }
    if (v["reset_faults"] | false) {
        std::fill(mvOffset_.begin(), mvOffset_.end(), 0);
        std::fill(tempOffsetDc_.begin(), tempOffsetDc_.end(), 0);
        commFail_ = false;
    }
    return "";
}

}  // namespace bms
