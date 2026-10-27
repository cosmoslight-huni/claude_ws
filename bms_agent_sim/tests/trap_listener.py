#!/usr/bin/env python3
"""Prints SNMPv2c traps (no net-snmp needed).

usage: python3 trap_listener.py [--port 1162]
Then add 127.0.0.1:1162 as a trap target in the Web UI.
"""
import argparse
import socket

NAMES = {
    "1.3.6.1.2.1.1.3.0": "sysUpTime",
    "1.3.6.1.6.3.1.1.4.1.0": "snmpTrapOID",
    "1.3.6.1.4.1.99999.1.0.1": "bmsAlarmRaised",
    "1.3.6.1.4.1.99999.1.0.2": "bmsAlarmCleared",
}
COLUMNS = {"1.3.6.1.4.1.99999.1.4.1.1": "bmsAlarmIndex",
           "1.3.6.1.4.1.99999.1.4.1.2": "bmsAlarmSeverity",
           "1.3.6.1.4.1.99999.1.4.1.3": "bmsAlarmText"}


def tlv(b, i):
    tag = b[i]
    n = b[i + 1]
    i += 2
    if n & 0x80:
        k = n & 0x7F
        n = int.from_bytes(b[i:i + k], "big")
        i += k
    return tag, b[i:i + n], i + n


def items(b):
    i = 0
    while i < len(b):
        tag, c, i = tlv(b, i)
        yield tag, c


def oid(c):
    arcs, v = [], 0
    for x in c:
        v = (v << 7) | (x & 0x7F)
        if not x & 0x80:
            arcs.append(v)
            v = 0
    first = arcs.pop(0)
    return ".".join(map(str, [min(first // 40, 2), first - min(first // 40, 2) * 40] + arcs))


def value(tag, c):
    if tag == 0x02:
        return int.from_bytes(c, "big", signed=True)
    if tag in (0x41, 0x42, 0x43):
        return int.from_bytes(c, "big")
    if tag == 0x04:
        return c.decode(errors="replace")
    if tag == 0x06:
        o = oid(c)
        return NAMES.get(o, o)
    return c.hex()


def name(o):
    if o in NAMES:
        return NAMES[o]
    for prefix, n in COLUMNS.items():
        if o.startswith(prefix + "."):
            return n + o[len(prefix):]
    return o


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=1162)
    args = ap.parse_args()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", args.port))
    print(f"listening for traps on UDP {args.port}", flush=True)
    while True:
        data, addr = sock.recvfrom(4096)
        try:
            _, msg, _ = tlv(data, 0)
            parts = list(items(msg))
            community = parts[1][1].decode()
            pdu_tag, pdu = parts[2]
            vbl = list(items(pdu))[3][1]
            print(f"--- trap from {addr[0]} (community '{community}', pdu 0x{pdu_tag:02x})", flush=True)
            for _, vb in items(vbl):
                (_, o), (t, c) = list(items(vb))
                print(f"    {name(oid(o))} = {value(t, c)}", flush=True)
        except Exception as e:  # noqa: BLE001
            print(f"undecodable packet from {addr}: {e}", flush=True)


if __name__ == "__main__":
    main()
