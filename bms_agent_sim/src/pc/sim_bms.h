// Simulated battery pack (LFP, 100 Ah) with fault injection.
// On real hardware this is replaced by an RS485/Modbus/CAN driver.
#pragma once

#include <random>
#include <vector>

#include "../../third_party/ArduinoJson.h"
#include "../core/hal.h"

namespace bms {

class SimBms : public IBmsSource {
public:
    enum class Mode { Auto, Charge, Discharge, Idle };

    explicit SimBms(ISystem& sys) : sys_(sys), rng_(std::random_device{}()) {}

    void configure(uint8_t cells) override;
    bool read(BmsReading& out) override;

    // Control from the /api/sim endpoint.
    void toJson(JsonObject o) const;
    std::string fromJson(JsonVariantConst v);

private:
    void step(double dtS);

    static constexpr double kCapacityAh = 100.0;
    static constexpr double kCurrentA = 50.0;

    ISystem& sys_;
    std::mt19937 rng_;
    Mode mode_ = Mode::Auto;
    bool charging_ = false;  // direction in Auto mode
    double speed_ = 60.0;    // simulated seconds per real second
    bool commFail_ = false;
    double soc_ = 60.0;
    double currentA_ = 0;
    uint64_t lastMs_ = 0;
    std::vector<double> temp_;
    std::vector<int> mvOffset_;
    std::vector<int> tempOffsetDc_;
};

}  // namespace bms
