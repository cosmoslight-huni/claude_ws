// Evaluates readings against thresholds (with hysteresis) and keeps
// the active alarm list plus a bounded event history.
#pragma once

#include <deque>
#include <functional>
#include <vector>

#include "types.h"

namespace bms {

class AlarmEngine {
public:
    static constexpr size_t kHistorySize = 50;

    using Listener = std::function<void(const AlarmEvent&)>;
    void setListener(Listener l) { listener_ = std::move(l); }

    // epoch_ms is 0 while the clock is unknown.
    void evaluate(const BmsReading& r, const AlarmConfig& cfg, uint64_t epoch_ms, uint64_t uptime_ms);
    void reset();

    std::vector<AlarmEvent> active() const;
    const std::deque<AlarmEvent>& history() const { return history_; }
    size_t activeCount() const;

    static Severity severityOf(AlarmCode c);
    static const char* nameOf(AlarmCode c);

private:
    void update(AlarmCode code, bool raise, bool clear, const std::string& text,
                uint64_t epoch_ms, uint64_t uptime_ms);

    AlarmEvent state_[kAlarmCodeCount + 1] = {};  // indexed by AlarmCode
    bool active_[kAlarmCodeCount + 1] = {};
    std::deque<AlarmEvent> history_;
    Listener listener_;
};

}  // namespace bms
