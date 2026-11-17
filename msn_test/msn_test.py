#!/usr/bin/env python3
"""Send MSN_APP commands (through tc_frame_proxy.py) and check the MSN telemetry in the TO_LAB TM frames.
Scenario A: sensor read, device switch, ARM/FIRE thruster. Scenario C: analysis and log download.
"""
import argparse
import socket
import struct
import sys
import time

MSN_CMD_MID = 0x18D0
HK_MID, SENSOR_MID, ANALYSIS_MID, LOG_MID = 0x08D0, 0x08D1, 0x08D2, 0x08D3
NOOP, READ_SENSOR, SET_DEVICE, ARM, FIRE, GET_ANALYSIS, GET_LOG, RESET_ANALYSIS = 0, 2, 3, 4, 5, 6, 7, 8

FRAME_SIZE = 1786
DATA_OFFSET = 20  # TM Primary Header 6 + reserved SDLS Security Header 14
IDLE_MID = 0x07FF
TLM_HDR_LEN = 16  # CCSDS 6 + time 6 + spare 4


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmd-port", type=int, default=1236, help="tc_frame_proxy.py UDP port")
    parser.add_argument("--tlm-port", type=int, default=1235, help="TO_LAB telemetry UDP port")
    parser.add_argument("--sensor-file", required=True, help="msn_sensor.csv used by the target")
    parser.add_argument("--state-file", required=True, help="msn_state.txt written by the target")
    return parser.parse_args()


class Ground:
    def __init__(self, args):
        self.cmd_addr = ("127.0.0.1", args.cmd_port)
        self.tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx.bind(("0.0.0.0", args.tlm_port))
        self.seq = 0
        self.packets = {}

    def send(self, cc, payload=b""):
        """cFS v1 command: CCSDS header, function code, checksum (XOR of all bytes is 0xFF)."""
        length = 2 + len(payload) - 1
        pkt = bytearray(struct.pack(">HHHBB", MSN_CMD_MID, 0xC000 | self.seq, length, cc, 0) + payload)
        self.seq = (self.seq + 1) & 0x3FFF
        checksum = 0xFF
        for b in pkt:
            checksum ^= b
        pkt[7] = checksum
        self.tx.sendto(bytes(pkt), self.cmd_addr)
        time.sleep(0.3)

    def receive(self, timeout):
        """Read one TM frame and store its Space Packets by MID."""
        self.rx.settimeout(timeout)
        try:
            frame, _ = self.rx.recvfrom(4096)
        except socket.timeout:
            return
        offset = DATA_OFFSET
        while offset + 6 <= len(frame):
            mid = (frame[offset] << 8) | frame[offset + 1]
            size = ((frame[offset + 4] << 8) | frame[offset + 5]) + 7
            if mid == IDLE_MID or offset + size > len(frame):
                break
            self.packets.setdefault(mid, []).append(frame[offset + TLM_HDR_LEN:offset + size])
            offset += size

    def wait(self, mid, timeout=5.0):
        """Return the payload of the next packet with this MID, or None."""
        end = time.time() + timeout
        while not self.packets.get(mid) and time.time() < end:
            self.receive(end - time.time())
        return self.packets[mid].pop(0) if self.packets.get(mid) else None

    def drop(self, mid):
        self.packets.pop(mid, None)


class Result:
    def __init__(self):
        self.passed = 0
        self.failed = 0

    def check(self, name, ok, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}: {name}" + (f" ({detail})" if detail else ""))
        if ok:
            self.passed += 1
        else:
            self.failed += 1


def near(a, b):
    return abs(a - b) < 0.01


def decode_hk(p):
    v = struct.unpack("<BBBB4Bff", p[:16])
    return {"cmd": v[0], "err": v[1], "armed": v[2], "dev": list(v[4:8]), "alt": v[8], "fuel": v[9]}


def decode_analysis(p):
    v = struct.unpack("<I3f3f3fIfII", p[:56])
    return {"n": v[0], "min": v[1:4], "max": v[4:7], "avg": v[7:10], "burns": v[10], "dv": v[11],
            "switches": v[12], "uptime": v[13]}


def download_log(g):
    """Ask for the log chunk by chunk until the target returns Length 0."""
    text, offset = b"", 0
    while True:
        g.send(GET_LOG, struct.pack("<I", offset))
        p = g.wait(LOG_MID)
        if p is None:
            return None
        size, _, length = struct.unpack("<IIH", p[:10])
        if length == 0:
            return text.decode(errors="replace")
        text += p[12:12 + length]
        offset += length


def main():
    args = parse_args()
    g = Ground(args)
    r = Result()
    rows = [[float(x) for x in line.split(",")] for line in open(args.sensor_file) if line.strip()]

    print("=== Waiting for MSN HK telemetry ===")
    hk = g.wait(HK_MID, 15)
    if hk is None:
        print("FAIL: no MSN HK packet")
        return 1
    hk = decode_hk(hk)
    r.check("initial state is 400 km, 5 kg, all devices off",
            near(hk["alt"], 400) and near(hk["fuel"], 5) and hk["dev"] == [0, 0, 0, 0], str(hk))

    print("=== Scenario A: sensor read ===")
    for i in range(3):
        g.send(READ_SENSOR)
        p = g.wait(SENSOR_MID)
        if p is None:
            r.check(f"READ_SENSOR {i + 1}", False, "no SENSOR packet")
            continue
        n, *values = struct.unpack("<I3f", p[:16])
        r.check(f"READ_SENSOR {i + 1} matches line {i + 1}",
                n == i + 1 and all(near(a, b) for a, b in zip(values, rows[i])), f"n={n} values={values}")

    print("=== Scenario A: device and thruster control ===")
    g.send(SET_DEVICE, struct.pack("<BBxx", 1, 1))
    g.send(SET_DEVICE, struct.pack("<BBxx", 7, 1))  # rejected: no device 7
    g.send(FIRE, struct.pack("<hxx", 200))          # rejected: not armed
    g.send(ARM)
    g.send(FIRE, struct.pack("<hxx", 200))          # +2 m/s
    g.send(ARM)
    g.send(NOOP)
    g.send(FIRE, struct.pack("<hxx", 100))          # rejected: NOOP disarmed the thruster
    g.send(ARM)
    g.send(FIRE, struct.pack("<hxx", 5000))         # rejected: out of range

    state = open(args.state_file).read().split()
    r.check("state file is '0 1 0 0 403.520 4.900'", state == ["0", "1", "0", "0", "403.520", "4.900"], str(state))

    print("=== Scenario C: analysis ===")
    g.send(GET_ANALYSIS)
    p = g.wait(ANALYSIS_MID)
    if p is None:
        r.check("GET_ANALYSIS", False, "no ANALYSIS packet")
    else:
        a = decode_analysis(p)
        first = rows[:3]
        ok = (a["n"] == 3 and a["burns"] == 1 and near(a["dv"], 2.0) and a["switches"] == 1
              and all(near(a["min"][k], min(x[k] for x in first)) for k in range(3))
              and all(near(a["max"][k], max(x[k] for x in first)) for k in range(3))
              and all(near(a["avg"][k], sum(x[k] for x in first) / 3) for k in range(3)))
        r.check("analysis has 3 samples, 1 burn of 2 m/s, 1 device switch, correct min/max/avg", ok, str(a))

    print("=== Scenario C: log download ===")
    log = download_log(g)
    print("--- log ---\n" + (log or "") + "-----------")
    expected = ["MSN App started", "SET_DEVICE 1 ON", "FIRE rejected: thruster not armed",
                "FIRE dV 2.00 m/s, altitude 403.52 km, fuel 4.900 kg", "FIRE rejected: dV 5000 cm/s out of range"]
    r.check("log has the expected lines", log is not None and all(e in log for e in expected))

    print("=== HK counters ===")
    g.drop(HK_MID)
    hk = g.wait(HK_MID, 10)
    hk = decode_hk(hk) if hk else None
    r.check("HK shows 4 rejected commands, thruster disarmed, device 1 on, 403.52 km",
            hk is not None and hk["err"] == 4 and hk["armed"] == 0 and hk["dev"] == [0, 1, 0, 0]
            and near(hk["alt"], 403.52), str(hk))

    print("=== Scenario C: reset ===")
    g.send(RESET_ANALYSIS)
    g.send(GET_ANALYSIS)
    p = g.wait(ANALYSIS_MID)
    r.check("analysis is cleared", p is not None and decode_analysis(p)["n"] == 0)
    log = download_log(g)
    r.check("log holds only the reset line", log is not None and log.count("\n") == 1 and "reset" in log, repr(log))

    print(f"\n=== {r.passed} passed, {r.failed} failed ===")
    return 0 if r.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
