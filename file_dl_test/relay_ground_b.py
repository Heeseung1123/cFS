#!/usr/bin/env python3
"""Ground B: the downlink station of a relay. Everything goes through the satellite.

Adds itself as an extra TO_LAB destination (ADD_DEST, CC 7), keeps the 0x08C3 segments with
its DestTag, sends RESEND and DONE (MID 0x18C0) to CI_LAB itself and removes itself at the end.
Start it before relay_ground_a.py.
"""
import argparse
import os
import sys
import zlib

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)
from file_xfer_test import RELAY_SEG, TO_LAB_CMD, DownlinkRx, Link, finish_downlink  # noqa: E402

TO_ADD_DEST_CC, TO_REMOVE_DEST_CC = 7, 8  # to_lab_dest_my.h


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tag", type=int, default=2, help="DestTag this station keeps")
    parser.add_argument("--host", default="127.0.0.1", help="IP of the cFS host (CI_LAB)")
    parser.add_argument("--ci-port", type=int, default=1234)
    parser.add_argument("--my-ip", default="127.0.0.1", help="IP of this station, as the satellite sees it")
    parser.add_argument("--tm-port", type=int, default=1245, help="TM port of this station")
    parser.add_argument("--dl-loss", type=float, default=0.0, help="fraction of TM frames dropped")
    parser.add_argument("--quiet-time", type=float, default=3.0, help="seconds without segments before RESEND")
    parser.add_argument("--timeout", type=float, default=900.0)
    parser.add_argument("--out", default=os.path.join(SCRIPT_DIR, "received"), help="folder for the rebuilt file")
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()

    link = Link(argparse.Namespace(host=args.host, ci_port=args.ci_port, tlm_port=args.tm_port,
                                   seed=args.seed, dl_loss=args.dl_loss))
    link.dl = DownlinkRx(RELAY_SEG, args.tag)  # only 0x08C3 segments with this tag
    dest = args.my_ip.encode().ljust(16, b"\0") + args.tm_port.to_bytes(2, "big")
    print(f"[B] ADD_DEST {args.my_ip}:{args.tm_port}, waiting for 0x{RELAY_SEG:04X} tag {args.tag}")
    link.cmd(TO_LAB_CMD, TO_ADD_DEST_CC, dest)

    try:
        data = finish_downlink(link, args.timeout, args.quiet_time)  # RESEND gaps, then DONE
    finally:
        link.cmd(TO_LAB_CMD, TO_REMOVE_DEST_CC, dest)
    if data is None:
        print("[B] RESULT: FAIL (timeout)")
        return 1

    os.makedirs(args.out, exist_ok=True)
    out = os.path.join(args.out, os.path.basename(link.dl.name or "relay.bin"))
    with open(out, "wb") as f:
        f.write(data)
    ok = zlib.crc32(data) == link.dl.end_crc
    print(f"[B] {link.dl.name}: {len(data)} bytes saved to {out}, crc32 {'matches' if ok else 'DIFFERS from'} END")
    print(f"[B] TM frames {link.frames}, dropped on purpose {link.dropped}, RESEND rounds {link.dl.rounds}")
    print("[B] RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
