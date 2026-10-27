#!/usr/bin/env python3
"""Minimal SNTP server for testing the agent without Internet access.

usage: sudo python3 fake_ntp_server.py [--port 123] [--offset-s 0]
Then set NTP server 1 to 127.0.0.1 in the Web UI.
"""
import argparse
import socket
import struct
import time

NTP_DELTA = 2208988800


def ntp_stamp(t):
    secs = int(t)
    frac = int((t - secs) * (1 << 32))
    return struct.pack("!II", secs + NTP_DELTA, frac)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=123)
    ap.add_argument("--offset-s", type=float, default=0.0, help="serve a shifted time")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", args.port))
    print(f"fake NTP server on UDP {args.port}", flush=True)
    while True:
        data, addr = sock.recvfrom(512)
        if len(data) < 48:
            continue
        now = time.time() + args.offset_s
        resp = bytearray(48)
        resp[0] = (0 << 6) | (4 << 3) | 4  # LI=0, VN=4, Mode=4 (server)
        resp[1] = 2                        # stratum
        resp[24:32] = data[40:48]          # origin = client's transmit timestamp
        resp[32:40] = ntp_stamp(now)       # receive
        resp[40:48] = ntp_stamp(now)       # transmit
        sock.sendto(bytes(resp), addr)
        print(f"answered {addr[0]}:{addr[1]}", flush=True)


if __name__ == "__main__":
    main()
