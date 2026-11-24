#!/usr/bin/env python3
"""Send MSN_APP commands (through tc_frame_proxy.py) and check the MSN telemetry in the TO_LAB TM frames.
--phase 1: parameters, temperature, diagnostics, app status, disable SAMPLE_APP in the startup script.
--phase 2 (after a cFS restart): SAMPLE_APP was not started, parameters were kept, enable it again.
"""
import argparse
import difflib
import os
import socket
import struct
import sys
import time

MSN_CMD_MID = 0x18D0
HK_MID, TEMP_MID, DIAG_MID, APP_STATUS_MID = 0x08D0, 0x08D1, 0x08D2, 0x08D3
NOOP, RESET, SET_PARAM, SET_STARTUP, RUN_DIAG, GET_APP_STATUS, READ_TEMP = range(7)
TEMP_HIGH, TEMP_LOW, TEMP_PERIOD = 0, 1, 2
NORMAL, HIGH, LOW, NO_DATA = 0, 1, 2, 3
NOT_FOUND, ENABLED, DISABLED = 0, 1, 2

FRAME_SIZE = 1786
DATA_OFFSET = 6  # TM Primary Header 6, no SDLS Security Header
IDLE_MID = 0x07FF
TLM_HDR_LEN = 16  # CCSDS 6 + time 6 + spare 4


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--phase", type=int, choices=(1, 2), required=True)
    parser.add_argument("--cmd-port", type=int, default=1236, help="tc_frame_proxy.py UDP port")
    parser.add_argument("--tlm-port", type=int, default=1235, help="TO_LAB telemetry UDP port")
    parser.add_argument("--cf-dir", required=True, help="the target's /cf directory on this host")
    parser.add_argument("--startup-backup", required=True, help="copy of the original startup script")
    parser.add_argument("--log", required=True, help="core-cpu1 log, to check events")
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
        pkt = bytearray(struct.pack(">HHHBB", MSN_CMD_MID, 0xC000 | self.seq, 1 + len(payload), cc, 0) + payload)
        self.seq = (self.seq + 1) & 0x3FFF
        checksum = 0xFF
        for b in pkt:
            checksum ^= b
        pkt[7] = checksum
        self.tx.sendto(bytes(pkt), self.cmd_addr)
        time.sleep(0.3)

    def receive(self, timeout):
        """Read one TM frame and store its Space Packets by MID."""
        self.rx.settimeout(max(timeout, 0.01))
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

    def request(self, cc, payload, mid):
        """Send a command and return the telemetry it produces."""
        self.packets.pop(mid, None)
        self.send(cc, payload)
        return self.wait(mid)

    def hk(self):
        self.packets.pop(HK_MID, None)
        p = self.wait(HK_MID, 10)
        if p is None:
            return None
        v = struct.unpack("<BBBxf3i", p[:20])
        return {"cmd": v[0], "err": v[1], "status": v[2], "temp": round(v[3], 2), "param": list(v[4:7])}


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


def param(pid, value):
    return struct.pack("<B3xi", pid, value)


def name20(name):
    return name.encode().ljust(20, b"\0")


def temp(p):
    if p is None:
        return None
    t, status, n = struct.unpack("<fB3xI", p[:12])
    return {"temp": round(t, 2), "status": status, "n": n}


def diag(p):
    return None if p is None else list(struct.unpack("<5B", p[:5]))


def app_status(p):
    if p is None:
        return None
    name, running, startup, prio, stack, runs, children = struct.unpack("<20sBB2x4I", p[:40])
    return {"name": name.rstrip(b"\0").decode(), "running": running, "startup": startup, "prio": prio}


def until(cond, timeout=3.0):
    """Commands reach the app a little later through the proxy and CI_LAB: poll the result."""
    end = time.time() + timeout
    while not cond() and time.time() < end:
        time.sleep(0.1)
    return cond()


def log_has(path, text):
    return text in open(path, errors="replace").read()


def startup_section(path, name):
    """'enabled' if the app line is above the first '!' line, 'disabled' if below, else None."""
    active = True
    for line in open(path):
        if line.startswith("!"):
            active = False
        elif len(line.split(",")) > 3 and line.split(",")[3].strip() == name:
            return "enabled" if active else "disabled"
    return None


def phase1(g, r, args):
    temp_file = os.path.join(args.cf_dir, "msn_temp.txt")
    script = os.path.join(args.cf_dir, "cfe_es_startup.scr")

    print("=== Parameters ===")
    hk = g.hk()
    r.check("default parameters 60 / -20 C, period 1", hk is not None and hk["param"] == [60, -20, 1], str(hk))
    g.send(SET_PARAM, param(TEMP_HIGH, 50))
    g.send(SET_PARAM, param(9, 1))           # rejected: no parameter 9
    g.send(SET_PARAM, param(TEMP_LOW, 50))   # rejected: low must stay below high
    hk = g.hk()
    r.check("SET_PARAM high=50 applied, 2 bad SET_PARAM rejected",
            hk is not None and hk["param"] == [50, -20, 1] and hk["err"] == 2, str(hk))
    saved = open(os.path.join(args.cf_dir, "msn_param.txt")).read().split()
    r.check("parameter file is '50 -20 1'", saved == ["50", "-20", "1"], str(saved))

    print("=== Temperature ===")
    g.packets.pop(TEMP_MID, None)
    t = temp(g.wait(TEMP_MID, 10))
    r.check("periodic TEMP tlm shows 21.5 C NORMAL", t is not None and t["temp"] == 21.5 and t["status"] == NORMAL,
            str(t))
    open(temp_file, "w").write("75.0\n")
    t = temp(g.request(READ_TEMP, b"", TEMP_MID))
    r.check("file edited to 75.0: READ_TEMP shows HIGH", t is not None and t["temp"] == 75.0 and t["status"] == HIGH,
            str(t))
    r.check("HIGH event sent", until(lambda: log_has(args.log, "Temperature 75.00 C is HIGH")))

    print("=== Diagnostics ===")
    d = diag(g.request(RUN_DIAG, b"", DIAG_MID))
    r.check("RUN_DIAG fails only the temperature limit check", d == [0, 1, 0, 0, 0], str(d))
    open(temp_file, "w").write("-30\n")
    t = temp(g.request(READ_TEMP, b"", TEMP_MID))
    r.check("file edited to -30: LOW", t is not None and t["status"] == LOW, str(t))
    os.remove(temp_file)
    d = diag(g.request(RUN_DIAG, b"", DIAG_MID))
    r.check("file removed: RUN_DIAG fails the file and limit checks", d == [1, 1, 0, 0, 0], str(d))
    open(temp_file, "w").write("21.5\n")
    d = diag(g.request(RUN_DIAG, b"", DIAG_MID))
    r.check("file restored: RUN_DIAG passes", d == [0, 0, 0, 0, 0], str(d))

    print("=== App status ===")
    s = app_status(g.request(GET_APP_STATUS, name20("SAMPLE_APP"), APP_STATUS_MID))
    r.check("SAMPLE_APP running, enabled, priority 50",
            s is not None and s["running"] == 1 and s["startup"] == ENABLED and s["prio"] == 50, str(s))
    s = app_status(g.request(GET_APP_STATUS, name20("NO_SUCH_APP"), APP_STATUS_MID))
    r.check("NO_SUCH_APP not running, not in the script",
            s is not None and s["running"] == 0 and s["startup"] == NOT_FOUND, str(s))

    print("=== Startup script ===")
    g.send(SET_STARTUP, name20("CI_LAB_APP") + struct.pack("<B3x", 0))
    r.check("disabling CI_LAB_APP is rejected", until(lambda: log_has(args.log, "CI_LAB_APP is needed for commanding"))
            and startup_section(script, "CI_LAB_APP") == "enabled")
    g.send(SET_STARTUP, name20("SAMPLE_APP") + struct.pack("<B3x", 0))
    r.check("SAMPLE_APP line moved below '!'", until(lambda: startup_section(script, "SAMPLE_APP") == "disabled"))
    s = app_status(g.request(GET_APP_STATUS, name20("SAMPLE_APP"), APP_STATUS_MID))
    r.check("SAMPLE_APP still running now, startup disabled",
            s is not None and s["running"] == 1 and s["startup"] == DISABLED, str(s))


def phase2(g, r, args):
    script = os.path.join(args.cf_dir, "cfe_es_startup.scr")

    print("=== After restart ===")
    r.check("SAMPLE_APP was not started", not log_has(args.log, "SAMPLE App Initialized"))
    s = app_status(g.request(GET_APP_STATUS, name20("SAMPLE_APP"), APP_STATUS_MID))
    r.check("SAMPLE_APP not running, startup disabled",
            s is not None and s["running"] == 0 and s["startup"] == DISABLED, str(s))
    hk = g.hk()
    r.check("parameters kept across the restart", hk is not None and hk["param"] == [50, -20, 1], str(hk))
    g.send(SET_STARTUP, name20("SAMPLE_APP") + struct.pack("<B3x", 1))
    r.check("SAMPLE_APP enabled again: above '!', same lines as the original (now last in start order)",
            until(lambda: startup_section(script, "SAMPLE_APP") == "enabled")
            and sorted(open(script).readlines()) == sorted(open(args.startup_backup).readlines()),
            "".join(difflib.unified_diff(open(args.startup_backup).readlines(), open(script).readlines(), n=0)))

def main():
    args = parse_args()
    g = Ground(args)
    r = Result()
    if g.wait(HK_MID, 15) is None:
        print("FAIL: no MSN HK packet")
        return 1
    (phase1 if args.phase == 1 else phase2)(g, r, args)
    print(f"\n=== phase {args.phase}: {r.passed} passed, {r.failed} failed ===")
    return 0 if r.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
