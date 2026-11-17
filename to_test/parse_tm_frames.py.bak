#!/usr/bin/env python3
"""Receive TO_LAB TM Transfer Frames over UDP, print them and check their format."""
import argparse
import select
import socket
import sys
import time

FRAME_SIZE = 1786
DATA_OFFSET = 20
IDLE_MID = 0x07FF
SPP_MIN_SIZE = 7


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=1235, help="UDP port to listen on")
    parser.add_argument("--duration", type=float, default=10, help="seconds to run after the first frame")
    parser.add_argument("--start-timeout", type=float, default=60, help="seconds to wait for the first frame")
    parser.add_argument("--tfvn", type=int, default=0, help="expected TFVN")
    parser.add_argument("--scid", type=int, default=3, help="expected SCID")
    parser.add_argument("--vcid", type=int, default=1, help="expected VCID")
    parser.add_argument("--quiet", action="store_true", help="print only errors and the summary")
    return parser.parse_args()


def parse_packets(frame):
    """Walk the data field. Returns (mid, size) pairs and the end offset."""
    packets = []
    offset = DATA_OFFSET
    while offset < FRAME_SIZE:
        if offset + 6 > FRAME_SIZE:
            return packets, offset
        mid = (frame[offset] << 8) | frame[offset + 1]
        size = ((frame[offset + 4] << 8) | frame[offset + 5]) + SPP_MIN_SIZE
        packets.append((mid, size))
        offset += size
    return packets, offset


class Checker:
    def __init__(self, args):
        self.args = args
        self.frames = 0
        self.errors = 0
        self.idle_octets = 0
        self.last_mc = None
        self.mids = {}

    def error(self, text):
        self.errors += 1
        print(f"  ERROR: {text}")

    def check_datagram(self, data, addr):
        if len(data) != FRAME_SIZE:
            print(f"datagram from {addr[0]}: {len(data)} bytes, starts {data[:6].hex(' ')}")
            self.error(f"expected a {FRAME_SIZE} byte frame")
            return
        self.check_frame(data, addr)

    def check_frame(self, frame, addr):
        args = self.args
        tfvn = frame[0] >> 6
        scid = ((frame[0] & 0x3F) << 4) | (frame[1] >> 4)
        vcid = (frame[1] >> 1) & 0x07
        ocf = frame[1] & 0x01
        mc_count = frame[2]
        vc_count = frame[3]
        status = frame[4]
        fhp = ((frame[4] & 0x07) << 8) | frame[5]
        self.frames += 1

        if not args.quiet:
            print(f"frame {self.frames} from {addr[0]}: hdr={frame[:6].hex(' ')} TFVN={tfvn} "
                  f"SCID={scid} VCID={vcid} MC={mc_count} FHP={fhp}")

        if (tfvn, scid, vcid) != (args.tfvn, args.scid, args.vcid):
            self.error(f"TFVN/SCID/VCID is {tfvn}/{scid}/{vcid}")
        if ocf != 0 or vc_count != 0:
            self.error(f"OCF flag {ocf} or VC count {vc_count} is not 0")
        if status & 0xF8 != 0x18:
            self.error(f"data field status flags are 0x{status & 0xF8:02X}, expected 0x18")
        if fhp != 0:
            self.error(f"FHP is {fhp}, expected 0")
        if any(frame[6:DATA_OFFSET]):
            self.error("reserved SDLS area is not zero")
        if self.last_mc is not None and mc_count != (self.last_mc + 1) & 0xFF:
            self.error(f"MC count jumped from {self.last_mc} to {mc_count}")
        self.last_mc = mc_count

        packets, end = parse_packets(frame)
        for mid, size in packets:
            if mid == IDLE_MID:
                self.idle_octets += size
            else:
                self.mids[mid] = self.mids.get(mid, 0) + 1
            if not args.quiet:
                kind = "idle" if mid == IDLE_MID else "SPP "
                print(f"  {kind} MID=0x{mid:04X} size={size}")
        if end != FRAME_SIZE:
            self.error(f"packets end at offset {end}, expected {FRAME_SIZE}")

    def summary(self):
        print("\n=== Summary ===")
        print(f"frames: {self.frames}, errors: {self.errors}, idle octets: {self.idle_octets}")
        for mid in sorted(self.mids):
            print(f"  MID 0x{mid:04X}: {self.mids[mid]} packets")
        passed = self.frames > 0 and self.errors == 0 and bool(self.mids)
        print("RESULT: PASS" if passed else "RESULT: FAIL")
        return passed


def main():
    args = parse_args()
    checker = Checker(args)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("", args.port))
    print(f"listening on UDP {args.port}")

    deadline = time.monotonic() + args.start_timeout
    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            ready, _, _ = select.select([sock], [], [], remaining)
            if not ready:
                break
            data, addr = sock.recvfrom(4096)
            if checker.frames == 0:
                deadline = time.monotonic() + args.duration
            checker.check_datagram(data, addr)
    except KeyboardInterrupt:
        pass

    if checker.frames == 0:
        print(f"no frame received within {args.start_timeout} seconds")
    return 0 if checker.summary() else 1


if __name__ == "__main__":
    sys.exit(main())
