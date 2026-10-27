// Wall clock and a small non-blocking SNTP client (RFC 4330).
#pragma once

#include <cstdint>
#include <string>

#include "hal.h"
#include "types.h"

namespace bms {

// Wall clock kept as an offset from the monotonic millis() counter.
// On hardware with an RTC, seed it from the RTC at boot.
class Clock {
public:
    void set(uint64_t epoch_ms, uint64_t now_ms) {
        offset_ms_ = (int64_t)epoch_ms - (int64_t)now_ms;
        valid_ = true;
    }
    bool valid() const { return valid_; }
    // 0 while the clock has never been set.
    uint64_t epochMs(uint64_t now_ms) const { return valid_ ? (uint64_t)((int64_t)now_ms + offset_ms_) : 0; }

private:
    bool valid_ = false;
    int64_t offset_ms_ = 0;
};

class NtpClient {
public:
    struct Status {
        bool synced = false;          // at least one successful sync
        bool in_progress = false;
        uint64_t last_sync_uptime_ms = 0;
        std::string last_server;
        std::string last_error;
        int64_t last_adjust_ms = 0;   // how far the clock was moved
    };

    NtpClient(IUdp& udp, Clock& clock) : udp_(udp), clock_(clock) {}

    void configure(const NtpConfig& cfg, uint64_t now_ms);
    void syncNow(uint64_t now_ms);
    void tick(uint64_t now_ms);
    const Status& status() const { return status_; }

private:
    void startRequest(uint64_t now_ms);
    void fail(const std::string& err, uint64_t now_ms);

    static constexpr uint32_t kTimeoutMs = 3000;
    static constexpr uint32_t kRetryMs = 30000;

    IUdp& udp_;
    Clock& clock_;
    NtpConfig cfg_;
    Status status_;
    bool udpOpen_ = false;
    int serverIdx_ = 0;
    std::string roundError_;
    bool waiting_ = false;
    uint64_t sentAt_ = 0;
    uint64_t nextSyncAt_ = 0;
    uint8_t txStamp_[8] = {};
};

}  // namespace bms
