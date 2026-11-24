#!/usr/bin/env python3
"""Raw dump of the real TC and TM packets of cFS.

TC: receives Space Packets on UDP 1236, adds the TC frame (tc_frame_proxy.py), dumps it, sends it to CI_LAB.
TM: receives TO_LAB frames on UDP 1235 and dumps them. Run it instead of tc_frame_proxy.py.
"""
import argparse
import os
import re
import select
import socket
import sys
import time
from types import SimpleNamespace

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tc_test"))
import tc_frame_proxy  # noqa: E402


def macro(path, name):
    """Read a number from a C header, so the view follows the flight code."""
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        match = re.search(rf"^#define\s+{name}\s+(\d+)", f.read(), re.M)
    if match is None:
        sys.exit(f"{name} not found in {path}")
    return int(match.group(1))


TC_H = "apps/ci_lab/fsw/src/tc_sdlp_my.h"
TM_H = "apps/to_lab/fsw/src/tm_sdlp_my.h"
TC_SEC_HDR = macro(TC_H, "TC_SDLP_MY_SDLS_HDR_LENGTH")
TC_SEC_TRL = macro(TC_H, "TC_SDLP_MY_SDLS_TRL_LENGTH")
TM_SEC_HDR = macro(TM_H, "TM_SDLP_MY_SDLS_HDR_LENGTH")
TM_SEC_TRL = macro(TM_H, "TM_SDLP_MY_SDLS_TRL_LENGTH")


def spp_total(packet):
    return int.from_bytes(packet[4:6], "big") + 7


def dump(kind, frame, full):
    """Offset, hex and ASCII of every byte; repeated lines become one "*" like hexdump -C."""
    print(f"\n{time.strftime('%H:%M:%S')} {kind.upper()} {len(frame)} bytes")
    last, starred = None, False
    for pos in range(0, len(frame), 16):
        line = frame[pos:pos + 16]
        if line == last and not full:
            if not starred:
                print("*")
            starred = True
            continue
        last, starred = line, False
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in line)
        print(f"{pos:04x}  {line[:8].hex(' '):23}  {line[8:].hex(' '):23}  |{text}|")
    print(f"{len(frame):04x}")


def output_enable(ip):
    """TO_LAB OUTPUT_ENABLE (MID 0x1880, FC 6): start sending TM to this IP."""
    packet = bytearray.fromhex("1880C0000011") + bytes([6, 0]) + ip.encode().ljust(16, b"\0")
    checksum = 0xFF
    for byte in packet:
        checksum ^= byte
    packet[7] = checksum
    return bytes(packet)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cfs", default="127.0.0.1", help="IP of cFS (the board or this host)")
    parser.add_argument("--enable", metavar="MY_IP", help="send OUTPUT_ENABLE with this ground IP at the start")
    parser.add_argument("--only", choices=("tc", "tm"), help="dump only this direction")
    parser.add_argument("--mid", type=lambda v: int(v, 0), help="dump only frames that hold this MID, e.g. 0x08D0")
    parser.add_argument("--count", type=int, default=0, help="exit after dumping this many frames (0 = run until Ctrl+C)")
    parser.add_argument("--full", action="store_true", help="print repeated lines too")
    parser.add_argument("--tc-port", type=int, default=1236, help="UDP port for Space Packets from the ground")
    parser.add_argument("--ci-port", type=int, default=1234, help="UDP port of CI_LAB")
    parser.add_argument("--tm-port", type=int, default=1235, help="UDP port for TM frames")
    args = parser.parse_args()
    if (tc_frame_proxy.SDLS_HDR_LEN, tc_frame_proxy.SDLS_TRL_LEN) != (TC_SEC_HDR, TC_SEC_TRL):
        sys.exit("SDLS lengths differ between tc_frame_proxy.py and tc_sdlp_my.h")

    tc = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tc.bind(("127.0.0.1", args.tc_port))
    tm = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tm.bind(("0.0.0.0", args.tm_port))
    out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    frame_args = SimpleNamespace(scid=3, vcid=0, mode="good")
    seq = shown = 0

    def wanted(kind, frame):
        if args.only and args.only != kind:
            return False
        if args.mid is None:
            return True
        mids, pos = [], 5 + TC_SEC_HDR
        if kind == "tc":
            return int.from_bytes(frame[pos:pos + 2], "big") == args.mid
        data = frame[6 + TM_SEC_HDR:len(frame) - TM_SEC_TRL]
        pos = int.from_bytes(frame[4:6], "big") & 0x7FF
        while pos + 6 <= len(data):
            mids.append(int.from_bytes(data[pos:pos + 2], "big"))
            pos += spp_total(data[pos:])
        return args.mid in mids

    def uplink(packet):
        nonlocal seq
        try:
            frame = tc_frame_proxy.build_frame(packet, seq, frame_args)
        except ValueError as error:
            print(f"TC not sent: {error}")
            return None
        out.sendto(frame, (args.cfs, args.ci_port))
        seq += 1
        return frame

    print(f"TC: UDP {args.tc_port} -> {args.cfs}:{args.ci_port}   TM: UDP {args.tm_port}   (Ctrl+C to stop)", flush=True)
    try:
        pending = [("tc", uplink(output_enable(args.enable)))] if args.enable else []
        while args.count == 0 or shown < args.count:
            if not pending:
                for sock in select.select([tc, tm], [], [])[0]:
                    data = sock.recv(65535)
                    pending.append(("tc", uplink(data)) if sock is tc else ("tm", data))
            kind, frame = pending.pop(0)
            if frame is not None and wanted(kind, frame):
                dump(kind, frame, args.full)
                sys.stdout.flush()
                shown += 1
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
