#!/usr/bin/env python3
"""Feeds packet sequences to seg_harness and checks the TM frames it sends.

Each case checks the First Header Pointer of every frame, that every packet is
joined again byte for byte, and which packets were dropped.
"""
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from tm_deframer import FRAME_SIZE, NO_FHP, Deframer  # noqa: E402

DATA_FIELD = FRAME_SIZE - 6  # 1780
IDLE_MID = 0x07FF


def make_packet(index, size):
    body = bytes(((index * 31 + k) & 0xFF) for k in range(6, size))
    return bytes([0x08, index & 0xFF, 0xC0, 0x00, (size - 7) >> 8, (size - 7) & 0xFF]) + body


def run(harness, script):
    proc = subprocess.run([harness], input=script.encode(), capture_output=True, check=True)
    out = proc.stdout
    if len(out) % FRAME_SIZE:
        raise RuntimeError(f"output is {len(out)} bytes, not whole frames")
    frames = [out[i:i + FRAME_SIZE] for i in range(0, len(out), FRAME_SIZE)]
    return frames, proc.stderr.decode().splitlines()


def check(harness, name, ops, fhps=None, drops=0):
    """ops: list of packet sizes and 'F' (end of cycle). fhps: expected FHP per frame."""
    script = "\n".join("F" if op == "F" else f"P {op}" for op in ops) + "\n"
    frames, events = run(harness, script)
    sizes = [op for op in ops if op != "F"]

    errors = []
    deframer = Deframer()
    got = []
    for n, frame in enumerate(frames):
        if frame[2] != (n + 1) & 0xFF:
            errors.append(f"frame {n + 1}: MC count {frame[2]}")
        packets, errs = deframer.feed(frame)
        errors += [f"frame {n + 1}: {e}" for e in errs]
        got += [p for p in packets if ((p[0] << 8) | p[1]) != IDLE_MID]

    # Packets arrive in order; the ones that do not match were dropped.
    dropped, i = 0, 0
    for index, size in enumerate(sizes):
        if i < len(got) and got[i] == make_packet(index, size):
            i += 1
        else:
            dropped += 1
    if i != len(got):
        errors.append(f"{len(got) - i} received packets do not match any sent packet")
    drop_events = sum("overflow queue full" in e for e in events)
    if dropped != drops or drop_events != drops:
        errors.append(f"dropped {dropped} packets with {drop_events} events, expected {drops}")

    got_fhps = [((f[4] & 0x07) << 8) | f[5] for f in frames]
    if fhps is not None and got_fhps != fhps:
        errors.append(f"FHPs {got_fhps}, expected {fhps}")
    if deframer.buf:
        errors.append(f"{len(deframer.buf)} octets left after the last frame")

    shown = got_fhps if len(got_fhps) <= 6 else f"{got_fhps[:6]}..."
    print(f"  {'PASS' if not errors else 'FAIL'}: {name}: {len(frames)} frames, FHP {shown}, "
          f"{len(got)} packets joined, {deframer.segmented} split, {dropped} dropped")
    for e in errors[:5]:
        print(f"    {e}")
    return not errors


def main():
    harness = sys.argv[1]
    results = []
    print("=== TM segmentation with the overflow queue ===")
    # Packets that fit: no segmentation, FHP 0.
    results.append(check(harness, "packets fit in one frame", [100] * 5 + ["F"], fhps=[0]))
    # Exactly fills the data field: sent at once without idle data.
    results.append(check(harness, "packet fills the data field", [DATA_FIELD, 100, "F"], fhps=[0, 0]))
    # Slide example: 220 octets of the second packet go to the queue and open the next frame.
    results.append(check(harness, "packet split, rest opens the next frame", [1000, 1000, "F"], fhps=[0, 220]))
    # One packet spans three frames: the middle frame has no packet start.
    results.append(check(harness, "packet over three frames (FHP 0x7FF)", [2 * DATA_FIELD + 6, "F"],
                         fhps=[0, NO_FHP, 6]))
    # More than frame + queue: the whole packet is dropped and nothing is sent for it.
    results.append(check(harness, "packet larger than frame + queue is dropped", [DATA_FIELD + FRAME_SIZE + 1, 100, "F"],
                         fhps=[0], drops=1))
    # 4 octets left at the end of a cycle: the 7 octet idle packet is split as in NOS3.
    results.append(check(harness, "short idle packet split into the next frame", [DATA_FIELD - 4, "F", 50, "F"],
                         fhps=[0, 3]))
    # Random sizes and cycles: ring buffer and header wrap-around.
    rng = random.Random(1)
    ops = []
    for _ in range(3000):
        ops.append(rng.choice([rng.randint(7, 300), rng.randint(7, 1800), rng.randint(1500, 3500)]))
        if rng.random() < 0.3:
            ops.append("F")
    ops.append("F")
    results.append(check_random(harness, ops))

    print(f"\npassed: {results.count(True)}, failed: {results.count(False)}")
    print("RESULT:", "PASS" if all(results) else "FAIL")
    return 0 if all(results) else 1


def check_random(harness, ops):
    """Same checks; the drop count comes from the harness events."""
    script = "\n".join("F" if op == "F" else f"P {op}" for op in ops) + "\n"
    _, events = run(harness, script)
    drops = sum("overflow queue full" in e for e in events)
    return check(harness, "3000 random packets", ops, drops=drops)


if __name__ == "__main__":
    sys.exit(main())
