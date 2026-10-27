// Minimal ASN.1 BER encoder/decoder for SNMP v1/v2c.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bms {
namespace snmp {

using Bytes = std::vector<uint8_t>;
using Oid = std::vector<uint32_t>;

enum Tag : uint8_t {
    INTEGER = 0x02,
    OCTET_STRING = 0x04,
    NULL_ = 0x05,
    OBJECT_ID = 0x06,
    SEQUENCE = 0x30,
    IP_ADDRESS = 0x40,
    COUNTER32 = 0x41,
    GAUGE32 = 0x42,
    TIMETICKS = 0x43,
    NO_SUCH_OBJECT = 0x80,
    NO_SUCH_INSTANCE = 0x81,
    END_OF_MIB_VIEW = 0x82,
    PDU_GET = 0xA0,
    PDU_GETNEXT = 0xA1,
    PDU_RESPONSE = 0xA2,
    PDU_SET = 0xA3,
    PDU_GETBULK = 0xA5,
    PDU_TRAP_V2 = 0xA7,
};

struct Value {
    uint8_t type = NULL_;
    int64_t num = 0;    // INTEGER, Counter32, Gauge32, TimeTicks
    std::string str;    // OCTET STRING (also IpAddress as 4 raw bytes)
    Oid oid;            // OBJECT IDENTIFIER

    static Value Int(int64_t v) { Value x; x.type = INTEGER; x.num = v; return x; }
    static Value Str(const std::string& s) { Value x; x.type = OCTET_STRING; x.str = s; return x; }
    static Value ObjectId(const Oid& o) { Value x; x.type = OBJECT_ID; x.oid = o; return x; }
    static Value Gauge(uint32_t v) { Value x; x.type = GAUGE32; x.num = v; return x; }
    static Value Ticks(uint32_t v) { Value x; x.type = TIMETICKS; x.num = v; return x; }
    static Value Exception(uint8_t t) { Value x; x.type = t; return x; }
    static Value Null() { return Value(); }
};

Oid parseOid(const std::string& dotted);
std::string oidToString(const Oid& o);
bool oidStartsWith(const Oid& o, const Oid& prefix);

// ---------------------------------------------------------------- encoding

Bytes tlv(uint8_t tag, const Bytes& content);
Bytes encodeInt(int64_t v, uint8_t tag = INTEGER);
Bytes encodeOctets(const std::string& s, uint8_t tag = OCTET_STRING);
Bytes encodeOid(const Oid& o);
Bytes encodeValue(const Value& v);
Bytes encodeVarBind(const Oid& o, const Value& v);
void append(Bytes& out, const Bytes& part);

// ---------------------------------------------------------------- decoding

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool atEnd() const { return pos_ >= n_; }

    // Reads a TLV header and returns a reader over its content.
    bool enter(uint8_t& tag, Reader& content);
    bool expect(uint8_t tag, Reader& content);
    bool readInt(int64_t& v);
    bool readOctets(std::string& s);
    bool readOid(Oid& o);
    bool readValue(Value& v);

private:
    bool header(uint8_t& tag, size_t& len);
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
};

}  // namespace snmp
}  // namespace bms
