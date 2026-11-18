#!/usr/bin/env python3
"""Ground A: the uplink station of a relay. Everything goes through the satellite.

Points the main TO_LAB output (OUTPUT_ENABLE, UDP 1235) at itself, sends the file with the
relay commands (MID 0x18C3) to CI_LAB (UDP 1234) and reads the relay status (0x08C2).
Ground B gets the relayed file from the satellite on its own (relay_ground_b.py).
"""
import argparse
import os
import random
import sys
import time
import zlib

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)
from file_xfer_test import OS_MAX_FILE_NAME, RELAY_CMD, RELAY_STATUS_TLM, TO_LAB_CMD, Link, upload  # noqa: E402

TO_OUTPUT_ENABLE_CC = 6


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("file", help="ground file to relay (name at most 14 bytes, at most 4 MiB)")
    parser.add_argument("--tag", type=int, default=2, help="DestTag of the station that gets the file")
    parser.add_argument("--host", default="127.0.0.1", help="IP of the cFS host (CI_LAB)")
    parser.add_argument("--ci-port", type=int, default=1234)
    parser.add_argument("--my-ip", default="127.0.0.1", help="IP of this station, as the satellite sees it")
    parser.add_argument("--ul-loss", type=float, default=0.0, help="fraction of DATA commands not sent")
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()

    base = os.path.basename(args.file)
    if len((base + ".part").encode()) >= OS_MAX_FILE_NAME or not os.path.isfile(args.file):
        print(f"FAIL: {args.file} must exist and its name be at most {OS_MAX_FILE_NAME - 6} bytes")
        return 1
    data = open(args.file, "rb").read()
    name = "/cf/relay/" + base

    # TO_LAB always sends its main output to port 1235.
    link = Link(argparse.Namespace(host=args.host, ci_port=args.ci_port, tlm_port=1235,
                                   seed=args.seed, dl_loss=0.0, ul_loss=args.ul_loss))
    print(f"[A] OUTPUT_ENABLE: main TM output to {args.my_ip}:1235")
    link.cmd(TO_LAB_CMD, TO_OUTPUT_ENABLE_CC, args.my_ip.encode().ljust(16, b"\0"))
    link.poll(1.0)

    print(f"[A] relay START {name}, {len(data)} bytes, crc32 0x{zlib.crc32(data):08x}, tag {args.tag}")
    t0 = time.time()
    st, rounds, skipped = upload(link, RELAY_CMD, RELAY_STATUS_TLM, name, data, args.tag,
                                 rng=random.Random(args.seed + 1))
    print(f"[A] relay uplink: {skipped} chunks not sent on purpose, {rounds} gap rounds, "
          f"{time.time() - t0:.1f} s, status {st and st['state']}")
    ok = st is not None and st["state"] == "COMPLETE" and st["file_crc"] == zlib.crc32(data)
    print("[A] RESULT:", "PASS (the satellite now sends it to tag %d)" % args.tag if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
