#!/usr/bin/env python3
"""Ground side of the byte-offset file downlink prototype (apps/file_dl).

Starts core-cpu1, writes a random file into /cf, sends FILE_DL START through CI_LAB,
puts every DATA segment at its byte offset, asks again for missing ranges with
RESEND (offset, length) after END, checks the file against the END CRC32 and
sends DONE (CRC32) so FILE_DL can compare it and close the file.
With --loss some TM frames are dropped on purpose.
"""
import argparse
import os
import random
import select
import socket
import subprocess
import sys
import time
import zlib

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CFS_ROOT = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, os.path.join(CFS_ROOT, "to_test"))
from tm_deframer import FRAME_SIZE, Deframer  # noqa: E402

FILE_DL_CMD_MID, FILE_DL_SEG_TLM_MID = 0x18C0, 0x08C0
START_CC, RESEND_CC, DONE_CC, NAME_LEN = 1, 2, 3, 64
MAX_RESENDS = 16  # FILE_DL_MAX_RESENDS
TYPE_START, TYPE_DATA, TYPE_END = 0, 1, 2
TLM_HDR_LEN, SEG_HDR_LEN = 16, 16
TO_LAB_CMD_MID = 0x1880


def send_cmd(sock, args, seq, mid, cc, payload):
    """cFS command with checksum, in one TC frame (SCID 3, VCID 0) to CI_LAB."""
    length = 2 + len(payload) - 1
    pkt = bytearray([mid >> 8, mid & 0xFF, 0xC0, 0x00, length >> 8, length & 0xFF, cc, 0]) + payload
    csum = 0xFF
    for b in pkt:
        csum ^= b
    pkt[7] = csum
    flen = 5 + len(pkt) - 1
    sock.sendto(bytes([0x20, 0x03, flen >> 8, flen & 0xFF, seq & 0xFF]) + pkt, (args.host, args.ci_port))


def parse_segment(pkt):
    h = pkt[TLM_HDR_LEN:TLM_HDR_LEN + SEG_HDR_LEN]
    seg_type = h[0]
    length = int.from_bytes(h[2:4], "big")
    offset = int.from_bytes(h[4:8], "big")
    size = int.from_bytes(h[8:12], "big")
    crc = int.from_bytes(h[12:16], "big")
    data = pkt[TLM_HDR_LEN + SEG_HDR_LEN:TLM_HDR_LEN + SEG_HDR_LEN + length]
    return seg_type, offset, size, crc, data


def missing_ranges(chunks, size):
    """[start, end) ranges not covered by any received segment."""
    gaps, pos = [], 0
    for off in sorted(chunks):
        if off > pos:
            gaps.append((pos, off))
        pos = max(pos, off + len(chunks[off]))
    if pos < size:
        gaps.append((pos, size))
    return gaps


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-b", "--build-dir", default="build-native")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--ci-port", type=int, default=1234)
    parser.add_argument("--tlm-port", type=int, default=1235)
    parser.add_argument("--size", type=int, default=1 << 20)
    parser.add_argument("--loss", type=float, default=0.0, help="fraction of TM frames dropped on purpose")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--quiet-time", type=float, default=3.0, help="seconds without data before RESEND")
    args = parser.parse_args()

    exe_dir = os.path.join(CFS_ROOT, args.build_dir, "exe", "cpu1")
    log_path = os.path.join(SCRIPT_DIR, "core-cpu1.log")
    name = "file_dl_test.bin"
    src_path = os.path.join(exe_dir, "cf", name)
    if subprocess.run(["pgrep", "-x", "core-cpu1"], stdout=subprocess.DEVNULL).returncode == 0:
        print("FAIL: core-cpu1 is already running. Stop it first.")
        return 1

    original = random.Random(args.seed).randbytes(args.size)
    with open(src_path, "wb") as f:
        f.write(original)

    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("0.0.0.0", args.tlm_port))
    deframer, rng = Deframer(), random.Random(args.seed)
    chunks, start_seg, end_seg = {}, None, None
    frames = dropped = segments = resent = rounds = 0
    first_gaps = None

    with open(log_path, "w") as logf:
        core = subprocess.Popen(["./core-cpu1", "-R", "PO"], cwd=exe_dir, stdout=logf, stderr=subprocess.STDOUT)
    try:
        for _ in range(300):
            text = open(log_path, errors="replace").read()
            if all(s in text for s in ("FILE_DL Initialized", "TO Lab Initialized", "CI Lab Initialized")):
                break
            time.sleep(0.1)
        time.sleep(2)
        for seq in range(10):
            send_cmd(tx, args, seq, TO_LAB_CMD_MID, 6, args.host.encode().ljust(16, b"\0"))
            time.sleep(1.0)
            if "telemetry output enabled" in open(log_path, errors="replace").read():
                break

        print(f"=== START /cf/{name} ({args.size} bytes), TM frame loss {args.loss:.0%} ===")
        t0 = time.time()
        send_cmd(tx, args, 20, FILE_DL_CMD_MID, START_CC, f"/cf/{name}".encode().ljust(NAME_LEN, b"\0"))
        last_data = time.time()
        while time.time() - t0 < args.timeout:
            ready, _, _ = select.select([rx], [], [], 0.5)
            if ready:
                frame = rx.recv(4096)
                if len(frame) != FRAME_SIZE:
                    continue
                frames += 1
                if rng.random() < args.loss:
                    dropped += 1
                    continue
                packets, _ = deframer.feed(frame)
                for pkt in packets:
                    if ((pkt[0] << 8) | pkt[1]) != FILE_DL_SEG_TLM_MID:
                        continue
                    seg_type, offset, size, crc, data = parse_segment(pkt)
                    if seg_type == TYPE_START:
                        start_seg = (size, data.decode())
                        print(f"  START: {data.decode()}, {size} bytes")
                    elif seg_type == TYPE_DATA:
                        resent += offset in chunks or end_seg is not None
                        chunks[offset] = bytes(data)
                        segments += 1
                        last_data = time.time()
                    elif seg_type == TYPE_END and end_seg is None:
                        end_seg = (size, crc)
                        print(f"  END  : {size} bytes, crc32 0x{crc:08x}, after {segments} DATA segments")
            # After END: when data has stopped, ask for the missing byte ranges again.
            if end_seg is not None and time.time() - last_data > args.quiet_time:
                gaps = missing_ranges(chunks, end_seg[0])
                if not gaps:
                    # Whole file here: DONE carries the ground CRC32, FILE_DL closes the file.
                    rebuilt = bytearray(end_seg[0])
                    for off, data in chunks.items():
                        rebuilt[off:off + len(data)] = data
                    send_cmd(tx, args, 60, FILE_DL_CMD_MID, DONE_CC, zlib.crc32(rebuilt).to_bytes(4, "big"))
                    print(f"  DONE : crc32 0x{zlib.crc32(rebuilt):08x}")
                    time.sleep(2)  # leave time for the DONE event before core-cpu1 stops
                    break
                if first_gaps is None:
                    first_gaps = gaps
                rounds += 1
                print(f"  RESEND round {rounds}: {len(gaps)} ranges, {sum(e - s for s, e in gaps)} bytes")
                for i, (gs, ge) in enumerate(gaps[:MAX_RESENDS]):
                    payload = gs.to_bytes(4, "big") + (ge - gs).to_bytes(4, "big")
                    send_cmd(tx, args, 30 + i, FILE_DL_CMD_MID, RESEND_CC, payload)
                    time.sleep(0.02)  # native SB pipes hold only 10 messages; let FILE_DL drain them
                last_data = time.time()
        elapsed = time.time() - t0
    finally:
        core.terminate()
        try:
            core.wait(5)
        except subprocess.TimeoutExpired:
            core.kill()
        os.remove(src_path)

    print("\n=== Result ===")
    results = []

    def check(ok, text):
        results.append(ok)
        print(f"  {'PASS' if ok else 'FAIL'}: {text}")

    size = end_seg[0] if end_seg else (start_seg[0] if start_seg else args.size)
    gaps = missing_ranges(chunks, size)
    check(start_seg is not None and end_seg is not None, f"START and END received ({elapsed:.1f} s)")
    if gaps:
        lost = sum(e - s for s, e in gaps)
        shown = ", ".join(f"[{s}, {e})" for s, e in gaps[:5]) + (" ..." if len(gaps) > 5 else "")
        check(False, f"{len(gaps)} missing ranges, {lost} bytes: {shown}")
    else:
        received = bytearray(size)
        for off, data in chunks.items():
            received[off:off + len(data)] = data
        check(bytes(received) == original, f"file rebuilt from byte offsets equals the sent file ({size} bytes)")
        check(end_seg is not None and zlib.crc32(received) == end_seg[1], "CRC32 matches the END segment")
        print(f"  throughput {size / elapsed / 1024:.1f} KiB/s")
    log_text = open(log_path, errors="replace").read()
    check("confirmed by ground" in log_text, "FILE_DL got DONE, CRC32 matched and the file was closed")
    check("Msg Limit Err" not in log_text, "no RESEND/DONE command dropped by the SB message limit")
    print(f"  TM frames {frames}, dropped on purpose {dropped}, DATA segments {segments} "
          f"({resent} after END), RESEND rounds {rounds}")
    for line in open(log_path, errors="replace"):
        if "FILE_DL" in line or "Msg Limit" in line or "Pipe Overflow" in line:
            print("  " + line.rstrip())
    print("RESULT:", "PASS" if all(results) else "FAIL")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
