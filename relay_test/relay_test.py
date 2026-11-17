#!/usr/bin/env python3
"""Acts as the second ground station (TV_STATION, UDP 1240): sends RELAY_APP commands through
tc_frame_proxy.py, receives the FILE_DL segments RELAY_APP forwards, rebuilds the file and checks it.
"""
import argparse
import socket
import struct
import sys
import time
import zlib

RELAY_CMD_MID = 0x18E0
FILE_DL_WAKEUP_MID = 0x18C1  # sent here at 10 Hz in place of sch_lab, see receive_file()
NOOP, FORWARD, STOP = 0, 2, 3
FILE_DL_SEG_MID = 0x08C0
TYPE_START, TYPE_DATA, TYPE_END = 0, 1, 2
TLM_HDR_LEN = 16  # CCSDS 6 + time 6 + spare 4
SEG_HDR = struct.Struct(">BBHIII")  # Type, Spare, DataLength, Offset, FileSize, Crc32


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmd-port", type=int, default=1236, help="tc_frame_proxy.py UDP port")
    parser.add_argument("--station-port", type=int, default=1240, help="TV_STATION UDP port")
    parser.add_argument("--file", required=True, help="on-board path of the file to relay, e.g. /cf/inbox/x.bin")
    parser.add_argument("--local-file", required=True, help="the same file on this host, to compare")
    parser.add_argument("--log", required=True, help="core-cpu1 log, to check events")
    return parser.parse_args()


def send(tx, port, cc, payload=b"", mid=RELAY_CMD_MID, wait=0.5):
    """cFS v1 command: CCSDS header, function code, checksum (XOR of all bytes is 0xFF)."""
    pkt = bytearray(struct.pack(">HHHBB", mid, 0xC000, 1 + len(payload), cc, 0) + payload)
    checksum = 0xFF
    for b in pkt:
        checksum ^= b
    pkt[7] = checksum
    tx.sendto(bytes(pkt), ("127.0.0.1", port))
    time.sleep(wait)


def forward_payload(name, station):
    return name.encode().ljust(64, b"\0") + struct.pack("<B3x", station)


def receive_file(rx, tx, cmd_port, timeout):
    """Collect segments until END; returns (start name, file bytes, END crc, segment count) or None.
    FILE_DL sends one segment per WAKEUP here, because its 100 ms receive timeout does not fire
    on the native build; on the target the 1 Hz WAKEUP comes from sch_lab."""
    name, data, size, end_crc, count = None, {}, None, None, 0
    end = time.time() + timeout
    while time.time() < end:
        rx.settimeout(0.1)
        try:
            pkt, _ = rx.recvfrom(4096)
        except socket.timeout:
            send(tx, cmd_port, 0, mid=FILE_DL_WAKEUP_MID, wait=0)
            continue
        mid = (pkt[0] << 8) | pkt[1]
        if mid != FILE_DL_SEG_MID:
            continue
        count += 1
        seg_type, _, length, offset, file_size, crc = SEG_HDR.unpack_from(pkt, TLM_HDR_LEN)
        body = pkt[TLM_HDR_LEN + SEG_HDR.size:TLM_HDR_LEN + SEG_HDR.size + length]
        if seg_type == TYPE_START:
            name, size = body.decode(), file_size
        elif seg_type == TYPE_DATA:
            data[offset] = body
        elif seg_type == TYPE_END:
            end_crc = crc
            break
    if name is None or end_crc is None:
        return None
    rebuilt = b"".join(data[k] for k in sorted(data))
    return name, rebuilt[:size], end_crc, count


def log_has(path, text):
    return text in open(path, errors="replace").read()


def main():
    args = parse_args()
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("0.0.0.0", args.station_port))
    expected = open(args.local_file, "rb").read()
    passed = failed = 0

    def check(name, ok, detail=""):
        nonlocal passed, failed
        print(f"  {'PASS' if ok else 'FAIL'}: {name}" + (f" ({detail})" if detail else ""))
        passed, failed = passed + ok, failed + (not ok)

    print("=== Rejected FORWARD commands ===")
    send(tx, args.cmd_port, FORWARD, forward_payload(args.file, 7))
    check("unknown station is rejected", log_has(args.log, "RELAY: Unknown station 7"))
    send(tx, args.cmd_port, FORWARD, forward_payload("/cf/inbox/missing.bin", 0))
    check("missing file is rejected", log_has(args.log, "RELAY: Cannot find /cf/inbox/missing.bin"))

    print(f"=== FORWARD {args.file} to TV_STATION ===")
    send(tx, args.cmd_port, FORWARD, forward_payload(args.file, 0))
    send(tx, args.cmd_port, FORWARD, forward_payload(args.file, 0))  # rejected: relay is running
    result = receive_file(rx, tx, args.cmd_port, 30)
    if result is None:
        check("file received at TV_STATION", False, "no START/END segment")
    else:
        name, rebuilt, end_crc, count = result
        check("START segment names the file", name == args.file, name)
        check("rebuilt file matches the on-board file", rebuilt == expected,
              f"{len(rebuilt)} of {len(expected)} bytes, {count} segments")
        check("END CRC32 matches", end_crc == zlib.crc32(expected), f"0x{end_crc:08x}")
    check("second FORWARD while relaying is rejected", log_has(args.log, "is still being relayed"))

    send(tx, args.cmd_port, STOP)
    check("STOP reports the forwarded segments", log_has(args.log, "RELAY: Stopped,"))

    print(f"\n=== {passed} passed, {failed} failed ===")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
