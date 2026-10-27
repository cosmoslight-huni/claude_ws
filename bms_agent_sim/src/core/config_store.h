// Configuration: JSON (de)serialization, validation and persistence.
#pragma once

#include <string>

#include "../../third_party/ArduinoJson.h"
#include "hal.h"
#include "types.h"

namespace bms {

// Section helpers. fromJson() merges the given fields onto `cfg`
// (partial updates are allowed) and type/range-checks them;
// validate() checks cross-field rules. Both return "" on success.
void toJson(const NetworkConfig& c, JsonObject o);
void toJson(const NtpConfig& c, JsonObject o);
void toJson(const BmsConfig& c, JsonObject o);
void toJson(const AlarmConfig& c, JsonObject o);
void toJson(const SnmpConfig& c, JsonObject o);
std::string fromJson(JsonVariantConst v, NetworkConfig& c);
std::string fromJson(JsonVariantConst v, NtpConfig& c);
std::string fromJson(JsonVariantConst v, BmsConfig& c);
std::string fromJson(JsonVariantConst v, AlarmConfig& c);
std::string fromJson(JsonVariantConst v, SnmpConfig& c);
std::string validate(const NetworkConfig& c);
std::string validate(const NtpConfig& c);
std::string validate(const AlarmConfig& c);
std::string validate(const SnmpConfig& c);

class ConfigStore {
public:
    explicit ConfigStore(IStorage& storage) : storage_(storage) {}

    // Loads config.json, falls back to config.bak, then to defaults.
    // Returns false when defaults had to be used.
    bool load(Config& cfg);
    // Keeps the previous file as config.bak, then writes atomically.
    bool save(const Config& cfg);
    void erase();

    static std::string serialize(const Config& cfg);
    static std::string deserialize(const std::string& json, Config& cfg);

private:
    IStorage& storage_;
};

}  // namespace bms
