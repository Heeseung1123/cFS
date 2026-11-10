#!/usr/bin/env python3
"""Receive Space Packets (for example from cmdUtil) over UDP, wrap each one in a
TC Transfer Frame and forward it to CI_LAB.

Frame: [Primary Header 5][Space Packet]
No Segment Header and no FECF. SDLS is disabled, so the Security Header and Trailer are 0 bytes.
"""
import argparse
import socket
import sys

PRIHDR_LEN = 5
SDLS_HDR_LEN = 0  # SDLS disabled (SPI 2 + SN 4 when enabled), must match TC_SDLP_MY_SDLS_HDR_LENGTH
SDLS_TRL_LEN = 0  # SDLS disabled (MAC 16 when enabled), must match TC_SDLP_MY_SDLS_TRL_LENGTH
MAX_FRAME_LEN = 1024

MODES = ("good", "length", "packet", "short", "raw")


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--listen-port", type=int, default=1236, help="UDP port for incoming Space Packets")
    parser.add_argument("--dest-host", default="127.0.0.1", help="CI_LAB address")
    parser.add_argument("--dest-port", type=int, default=1234, help="CI_LAB UDP port")
    parser.add_argument("--count", type=int, default=0, help="exit after this many packets (0 = run forever)")
    parser.add_argument("--timeout", type=float, default=0, help="exit if no packet arrives for this long (0 = wait forever)")
    parser.add_argument("--scid", type=int, default=3, help="Spacecraft ID")
    parser.add_argument("--vcid", type=int, default=0, help="Virtual Channel ID")
    parser.add_argument("--spi", type=int, default=1, help="Security Parameter Index")
    parser.add_argument("--mode", choices=MODES, default="good",
                        help="good, or a broken frame: length (wrong Frame Length field), "
                             "packet (extra data byte), short (header only), raw (no framing)")
    parser.add_argument("--quiet", action="store_true", help="do not print each frame")
    return parser.parse_args()


def build_frame(packet, seq, args):
    data = bytearray(packet)
    if args.mode == "packet":
        data.append(0)  # Space Packet length no longer matches the data field

    # The Security Header is SPI 2 + Sequence Number 4 only while SDLS is enabled.
    sec_hdr = (args.spi.to_bytes(2, "big") + seq.to_bytes(4, "big")) if SDLS_HDR_LEN else b""
    sec_trl = bytes(SDLS_TRL_LEN)
    total = PRIHDR_LEN + len(sec_hdr) + len(data) + len(sec_trl)
    if total > MAX_FRAME_LEN:
        raise ValueError(f"frame of {total} bytes exceeds {MAX_FRAME_LEN}")

    length_field = total - 1
    if args.mode == "length":
        length_field += 1

    hdr = bytes([
        (0 << 6) | (1 << 5) | (0 << 4) | ((args.scid >> 8) & 0x03),  # TFVN 0, Bypass 1, Control 0
        args.scid & 0xFF,
        ((args.vcid & 0x3F) << 2) | ((length_field >> 8) & 0x03),
        length_field & 0xFF,
        seq & 0xFF,
    ])
    frame = hdr + sec_hdr + bytes(data) + sec_trl
    if args.mode == "short":
        frame = frame[:PRIHDR_LEN + SDLS_HDR_LEN]
    return frame


def main():
    args = parse_args()
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("127.0.0.1", args.listen_port))
    if args.timeout > 0:
        rx.settimeout(args.timeout)
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (args.dest_host, args.dest_port)
    print(f"tc_frame_proxy: UDP {args.listen_port} -> {dest[0]}:{dest[1]} mode={args.mode}", flush=True)

    seq = 0
    try:
        while args.count == 0 or seq < args.count:
            try:
                packet, _ = rx.recvfrom(4096)
            except socket.timeout:
                print("tc_frame_proxy: timed out waiting for a packet", flush=True)
                return 1
            out = packet if args.mode == "raw" else build_frame(packet, seq, args)
            tx.sendto(out, dest)
            if not args.quiet:
                print(f"  sent {len(out)} bytes ({args.mode}): {out[:PRIHDR_LEN + SDLS_HDR_LEN].hex(' ')} | "
                      f"SPP {packet[:8].hex(' ')}", flush=True)
            seq += 1
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
