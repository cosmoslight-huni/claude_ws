#pragma once

#include <cstdint>
#include <string>

namespace bms {

void logf(const char* fmt, ...);

bool parseIpv4(const std::string& s, uint32_t& out);
bool isValidHostname(const std::string& s);
// Hostname or dotted IPv4.
bool isValidHost(const std::string& s);

std::string toHex(const uint8_t* data, size_t len);
std::string sha256Hex(const std::string& data);

// epoch seconds -> "YYYY-MM-DD HH:MM:SS"
std::string formatTime(int64_t epoch_s);

}  // namespace bms
