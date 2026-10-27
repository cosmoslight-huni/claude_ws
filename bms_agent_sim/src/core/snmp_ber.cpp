#include "snmp_ber.h"

namespace bms {
namespace snmp {

Oid parseOid(const std::string& dotted) {
    Oid o;
    uint32_t v = 0;
    bool have = false;
    for (char c : dotted) {
        if (c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
            have = true;
        } else if (c == '.') {
            if (have) o.push_back(v);
            v = 0;
            have = false;
        }
    }
    if (have) o.push_back(v);
    return o;
}

std::string oidToString(const Oid& o) {
    std::string s;
    for (size_t i = 0; i < o.size(); i++) {
        if (i) s += '.';
        s += std::to_string(o[i]);
    }
    return s;
}

bool oidStartsWith(const Oid& o, const Oid& prefix) {
    if (o.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); i++)
        if (o[i] != prefix[i]) return false;
    return true;
}

// ---------------------------------------------------------------- encoding

void append(Bytes& out, const Bytes& part) { out.insert(out.end(), part.begin(), part.end()); }

Bytes tlv(uint8_t tag, const Bytes& content) {
    Bytes out;
    out.push_back(tag);
    size_t len = content.size();
    if (len < 0x80) {
        out.push_back((uint8_t)len);
    } else if (len <= 0xff) {
        out.push_back(0x81);
        out.push_back((uint8_t)len);
    } else {
        out.push_back(0x82);
        out.push_back((uint8_t)(len >> 8));
        out.push_back((uint8_t)len);
    }
    append(out, content);
    return out;
}

Bytes encodeInt(int64_t v, uint8_t tag) {
    Bytes c;
    if (tag != INTEGER) {
        // Unsigned application types: 32-bit, add a leading 0 if the top bit is set.
        uint32_t u = (uint32_t)v;
        for (int i = 3; i >= 0; i--) c.push_back((uint8_t)(u >> (i * 8)));
        while (c.size() > 1 && c[0] == 0 && !(c[1] & 0x80)) c.erase(c.begin());
        if (c[0] & 0x80) c.insert(c.begin(), 0);
        return tlv(tag, c);
    }
    for (int i = 7; i >= 0; i--) c.push_back((uint8_t)((uint64_t)v >> (i * 8)));
    while (c.size() > 1 && ((c[0] == 0x00 && !(c[1] & 0x80)) || (c[0] == 0xff && (c[1] & 0x80))))
        c.erase(c.begin());
    return tlv(tag, c);
}

Bytes encodeOctets(const std::string& s, uint8_t tag) { return tlv(tag, Bytes(s.begin(), s.end())); }

Bytes encodeOid(const Oid& o) {
    Bytes c;
    if (o.size() < 2) {
        c.push_back(0);
        return tlv(OBJECT_ID, c);
    }
    auto arc = [&c](uint32_t v) {
        uint8_t tmp[5];
        int n = 0;
        do {
            tmp[n++] = v & 0x7f;
            v >>= 7;
        } while (v);
        while (n--) c.push_back(tmp[n] | (n ? 0x80 : 0));
    };
    arc(o[0] * 40 + o[1]);
    for (size_t i = 2; i < o.size(); i++) arc(o[i]);
    return tlv(OBJECT_ID, c);
}

Bytes encodeValue(const Value& v) {
    switch (v.type) {
        case INTEGER: return encodeInt(v.num);
        case COUNTER32:
        case GAUGE32:
        case TIMETICKS: return encodeInt(v.num, v.type);
        case OCTET_STRING:
        case IP_ADDRESS: return encodeOctets(v.str, v.type);
        case OBJECT_ID: return encodeOid(v.oid);
        default: return tlv(v.type, {});  // NULL and exception values
    }
}

Bytes encodeVarBind(const Oid& o, const Value& v) {
    Bytes c = encodeOid(o);
    append(c, encodeValue(v));
    return tlv(SEQUENCE, c);
}

// ---------------------------------------------------------------- decoding

bool Reader::header(uint8_t& tag, size_t& len) {
    if (pos_ + 2 > n_) return false;
    tag = p_[pos_++];
    uint8_t b = p_[pos_++];
    if (b & 0x80) {
        int k = b & 0x7f;
        if (k == 0 || k > 3) return false;
        len = 0;
        while (k--) {
            if (pos_ >= n_) return false;
            len = (len << 8) | p_[pos_++];
        }
    } else {
        len = b;
    }
    return len <= n_ - pos_;
}

bool Reader::enter(uint8_t& tag, Reader& content) {
    size_t len;
    if (!header(tag, len)) return false;
    content = Reader(p_ + pos_, len);
    pos_ += len;
    return true;
}

bool Reader::expect(uint8_t tag, Reader& content) {
    uint8_t t;
    return enter(t, content) && t == tag;
}

static bool decodeInt(const uint8_t* p, size_t n, bool isSigned, int64_t& v) {
    if (n == 0 || n > 9) return false;
    v = (isSigned && (p[0] & 0x80)) ? -1 : 0;
    for (size_t i = 0; i < n; i++) v = (int64_t)(((uint64_t)v << 8) | p[i]);
    return true;
}

bool Reader::readInt(int64_t& v) {
    uint8_t tag;
    size_t len;
    if (!header(tag, len) || tag != INTEGER) return false;
    bool ok = decodeInt(p_ + pos_, len, true, v);
    pos_ += len;
    return ok;
}

bool Reader::readOctets(std::string& s) {
    uint8_t tag;
    size_t len;
    if (!header(tag, len) || tag != OCTET_STRING) return false;
    s.assign((const char*)p_ + pos_, len);
    pos_ += len;
    return true;
}

static bool decodeOid(const uint8_t* p, size_t n, Oid& o) {
    o.clear();
    uint64_t v = 0;
    bool first = true;
    for (size_t i = 0; i < n; i++) {
        v = (v << 7) | (p[i] & 0x7f);
        if (v > 0xffffffffULL) return false;
        if (!(p[i] & 0x80)) {
            if (first) {
                uint32_t a = v < 80 ? (uint32_t)(v / 40) : 2;
                o.push_back(a);
                o.push_back((uint32_t)(v - a * 40));
                first = false;
            } else {
                o.push_back((uint32_t)v);
            }
            v = 0;
        }
    }
    return !first && o.size() <= 128;
}

bool Reader::readOid(Oid& o) {
    uint8_t tag;
    size_t len;
    if (!header(tag, len) || tag != OBJECT_ID) return false;
    bool ok = decodeOid(p_ + pos_, len, o);
    pos_ += len;
    return ok;
}

bool Reader::readValue(Value& v) {
    uint8_t tag;
    size_t len;
    if (!header(tag, len)) return false;
    const uint8_t* c = p_ + pos_;
    pos_ += len;
    v = Value();
    v.type = tag;
    switch (tag) {
        case INTEGER: return decodeInt(c, len, true, v.num);
        case COUNTER32:
        case GAUGE32:
        case TIMETICKS: return decodeInt(c, len, false, v.num);
        case OCTET_STRING:
        case IP_ADDRESS: v.str.assign((const char*)c, len); return true;
        case OBJECT_ID: return decodeOid(c, len, v.oid);
        default: return true;  // NULL etc.
    }
}

}  // namespace snmp
}  // namespace bms
