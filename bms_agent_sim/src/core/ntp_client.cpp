#include "ntp_client.h"

#include <cstring>

#include "util.h"

namespace bms {

namespace {
constexpr uint64_t kNtpToUnix = 2208988800ULL;  // seconds 1900 -> 1970

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
}  // namespace

void NtpClient::configure(const NtpConfig& cfg, uint64_t now_ms) {
    cfg_ = cfg;
    waiting_ = false;
    status_.in_progress = false;
    nextSyncAt_ = now_ms;  // sync as soon as possible after a change
    if (!cfg_.enable && udpOpen_) {
        udp_.stop();
        udpOpen_ = false;
    }
}

void NtpClient::syncNow(uint64_t now_ms) {
    waiting_ = false;
    serverIdx_ = 0;
    roundError_.clear();
    startRequest(now_ms);
}

void NtpClient::tick(uint64_t now_ms) {
    if (!cfg_.enable) return;
    if (!waiting_) {
        if (now_ms >= nextSyncAt_) {
            serverIdx_ = 0;
            roundError_.clear();
            startRequest(now_ms);
        }
        return;
    }

    uint8_t buf[68];
    std::string from;
    uint16_t fromPort;
    int n;
    while ((n = udp_.recvFrom(buf, sizeof(buf), from, fromPort)) > 0) {
        if (n < 48 || (buf[0] & 0x07) != 4 || buf[1] == 0 || buf[1] > 15) continue;
        if (memcmp(buf + 24, txStamp_, 8) != 0) continue;  // not an answer to our request
        uint64_t secs = be32(buf + 40);
        uint64_t frac = be32(buf + 44);
        uint64_t serverMs = (secs - kNtpToUnix) * 1000 + ((frac * 1000) >> 32);
        uint64_t rtt = now_ms - sentAt_;
        uint64_t newEpoch = serverMs + rtt / 2;
        uint64_t oldEpoch = clock_.epochMs(now_ms);
        clock_.set(newEpoch, now_ms);
        waiting_ = false;
        status_.in_progress = false;
        status_.synced = true;
        status_.last_sync_uptime_ms = now_ms;
        status_.last_server = serverIdx_ == 0 ? cfg_.server1 : cfg_.server2;
        status_.last_error.clear();
        status_.last_adjust_ms = oldEpoch ? (int64_t)newEpoch - (int64_t)oldEpoch : 0;
        nextSyncAt_ = now_ms + (uint64_t)cfg_.interval_min * 60000;
        logf("ntp: synced with %s, %s UTC (adjust %lld ms)", status_.last_server.c_str(),
             formatTime(newEpoch / 1000).c_str(), (long long)status_.last_adjust_ms);
        return;
    }
    if (now_ms - sentAt_ > kTimeoutMs) {
        serverIdx_++;
        startRequest(now_ms);
    }
}

void NtpClient::startRequest(uint64_t now_ms) {
    const std::string* servers[2] = {&cfg_.server1, &cfg_.server2};
    while (serverIdx_ < 2 && servers[serverIdx_]->empty()) serverIdx_++;
    if (serverIdx_ >= 2) {
        fail("no response from NTP servers", now_ms);
        return;
    }
    if (!udpOpen_) {
        if (!udp_.begin(0)) {
            fail("cannot open UDP socket", now_ms);
            return;
        }
        udpOpen_ = true;
    }
    uint8_t pkt[48] = {};
    pkt[0] = 0x23;  // LI=0, VN=4, Mode=3 (client)
    // Transmit timestamp: any unique value; the server echoes it as "origin".
    uint64_t stamp = now_ms * 0x10C6F7A0B5EDULL + 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 8; i++) txStamp_[i] = (uint8_t)(stamp >> (56 - i * 8));
    memcpy(pkt + 40, txStamp_, 8);

    const std::string& host = *servers[serverIdx_];
    if (!udp_.sendTo(host, 123, pkt, sizeof(pkt))) {
        roundError_ = "cannot resolve or reach " + host;
        serverIdx_++;
        startRequest(now_ms);
        return;
    }
    waiting_ = true;
    status_.in_progress = true;
    sentAt_ = now_ms;
}

void NtpClient::fail(const std::string& err, uint64_t now_ms) {
    waiting_ = false;
    status_.in_progress = false;
    status_.last_error = roundError_.empty() ? err : roundError_;
    nextSyncAt_ = now_ms + kRetryMs;
    logf("ntp: %s (retry in %u s)", status_.last_error.c_str(), kRetryMs / 1000);
}

}  // namespace bms
