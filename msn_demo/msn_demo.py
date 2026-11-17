#!/usr/bin/env python3
"""Interactive ground console for MSN_APP on the native build.

Starts core-cpu1 and tc_frame_proxy.py, enables TO_LAB telemetry, then lets you send each MSN_APP
command, edit the files MSN_APP reads, watch the telemetry and events, and restart cFS.
The startup script, temperature file and parameter file are restored on exit (see --keep-files).
"""
import argparse
import os
import socket
import struct
import subprocess
import sys
import threading
import time

MSN_CMD_MID, TO_LAB_CMD_MID = 0x18D0, 0x1880
HK_MID, TEMP_MID, DIAG_MID, APP_STATUS_MID = 0x08D0, 0x08D1, 0x08D2, 0x08D3
SET_PARAM, SET_STARTUP, RUN_DIAG, GET_APP_STATUS, READ_TEMP = 2, 3, 4, 5, 6
TO_OUTPUT_ENABLE_CC = 6

PROXY_PORT, CI_PORT, TLM_PORT = 1236, 1234, 1235
DATA_OFFSET = 20  # TM frame: primary header 6 + reserved SDLS security header 14
IDLE_MID = 0x07FF
TLM_HDR_LEN = 16  # CCSDS 6 + time 6 + spare 4

TEMP_STATUS = ["NORMAL", "HIGH", "LOW", "NO_DATA"]
STARTUP_STATE = ["not in the script", "enabled", "disabled"]
PARAM_NAMES = ["TEMP_HIGH (C)", "TEMP_LOW (C)", "TEMP_PERIOD (HK requests)"]
DIAG_CHECKS = ["temperature file readable", "temperature within limits", "parameter file matches",
               "essential apps enabled in startup script", "essential apps running"]

DEMO_DIR = os.path.dirname(os.path.abspath(__file__))
CFS_ROOT = os.path.dirname(DEMO_DIR)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-b", "--build-dir", default="build-native", help="build directory under cFS")
    parser.add_argument("--keep-files", action="store_true", help="do not restore the /cf files on exit")
    parser.add_argument("--tour", action="store_true", help="run the guided tour right after the start")
    return parser.parse_args()


# ---------------------------------------------------------------- telemetry decoding

def decode_hk(p):
    cmd, err, status, temp, high, low, period = struct.unpack("<BBBxf3i", p[:20])
    return (f"HK   cmd={cmd} err={err} temp={temp:.2f} C ({TEMP_STATUS[status]}) "
            f"params high={high} low={low} period={period}")


def decode_temp(p):
    temp, status, count = struct.unpack("<fB3xI", p[:12])
    return f"TEMP {temp:.2f} C  status={TEMP_STATUS[status]}  samples={count}"


def decode_diag(p):
    result = struct.unpack("<5B", p[:5])
    fail, runs = struct.unpack("<B2xI", p[5:12])
    lines = [f"DIAG run #{runs}: {fail} of 5 checks failed"]
    lines += [f"       [{'FAIL' if r else 'pass'}] {i} {DIAG_CHECKS[i]}" for i, r in enumerate(result)]
    return "\n".join(lines)


def decode_app_status(p):
    name, running, startup, prio, stack, runs, children = struct.unpack("<20sBB2x4I", p[:40])
    name = name.rstrip(b"\0").decode()
    text = (f"APP_STATUS {name}: {'running' if running else 'NOT running'}, "
            f"startup script: {STARTUP_STATE[startup]}")
    if running:
        text += f"\n       priority={prio} stack={stack} main loop runs={runs} child tasks={children}"
    return text


DECODERS = {HK_MID: decode_hk, TEMP_MID: decode_temp, DIAG_MID: decode_diag, APP_STATUS_MID: decode_app_status}


# ---------------------------------------------------------------- ground link

class Ground:
    """Sends commands through tc_frame_proxy.py and keeps the last packet of each MSN telemetry MID."""

    def __init__(self):
        self.tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx.bind(("0.0.0.0", TLM_PORT))
        self.rx.settimeout(0.2)
        self.lock = threading.Condition()
        self.last = {}   # MID -> (receive time, payload)
        self.count = {}  # MID -> packets received
        self.watching = False
        threading.Thread(target=self._receive, daemon=True).start()

    def send(self, cc, payload=b"", mid=MSN_CMD_MID):
        """cFS v1 command: CCSDS header, function code, checksum (XOR of all bytes is 0xFF)."""
        pkt = bytearray(struct.pack(">HHHBB", mid, 0xC000, 1 + len(payload), cc, 0) + payload)
        checksum = 0xFF
        for b in pkt:
            checksum ^= b
        pkt[7] = checksum
        self.tx.sendto(bytes(pkt), ("127.0.0.1", PROXY_PORT))

    def _receive(self):
        """Split each TM frame into Space Packets and keep the MSN ones."""
        while True:
            try:
                frame, _ = self.rx.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                return
            offset = DATA_OFFSET
            while offset + 6 <= len(frame):
                mid = (frame[offset] << 8) | frame[offset + 1]
                size = ((frame[offset + 4] << 8) | frame[offset + 5]) + 7
                if mid == IDLE_MID or offset + size > len(frame):
                    break
                if mid in DECODERS:
                    payload = frame[offset + TLM_HDR_LEN:offset + size]
                    with self.lock:
                        self.last[mid] = (time.time(), payload)
                        self.count[mid] = self.count.get(mid, 0) + 1
                        self.lock.notify_all()
                    if self.watching:
                        print(f"  {time.strftime('%H:%M:%S')}  {DECODERS[mid](payload)}")
                offset += size

    def mark(self, mid):
        with self.lock:
            return self.count.get(mid, 0)

    def wait_new(self, mid, mark, timeout=5.0):
        """Payload of the first packet with this MID received after mark(), or None."""
        with self.lock:
            self.lock.wait_for(lambda: self.count.get(mid, 0) > mark, timeout)
            return self.last[mid][1] if self.count.get(mid, 0) > mark else None


# ---------------------------------------------------------------- satellite (native core-cpu1)

class Satellite:
    def __init__(self, build_dir):
        self.exe_dir = os.path.join(CFS_ROOT, build_dir, "exe", "cpu1")
        self.cf = os.path.join(self.exe_dir, "cf")
        self.log = os.path.join(DEMO_DIR, "core-cpu1.log")
        self.proc = None
        self.proxy = None
        self.log_pos = 0

    def path(self, name):
        return os.path.join(self.cf, name)

    def start(self, ground):
        print("Starting core-cpu1 (log: msn_demo/core-cpu1.log) ...")
        self.log_pos = 0
        with open(self.log, "w") as log:
            self.proc = subprocess.Popen(["./core-cpu1", "-R", "PO"], cwd=self.exe_dir, stdout=log,
                                         stderr=subprocess.STDOUT)
        if self.proxy is None:
            self.proxy = subprocess.Popen([sys.executable, os.path.join(CFS_ROOT, "tc_test", "tc_frame_proxy.py"),
                                           "--listen-port", str(PROXY_PORT), "--dest-port", str(CI_PORT),
                                           "--quiet"])
        if not self._wait_log(["MSN App Initialized", "TO Lab Initialized"], 30):
            raise RuntimeError("cFS did not start, see msn_demo/core-cpu1.log")
        for _ in range(10):  # TO_LAB sends nothing until it is told where to send
            ground.send(TO_OUTPUT_ENABLE_CC, b"127.0.0.1".ljust(16, b"\0"), mid=TO_LAB_CMD_MID)
            if self._wait_log(["telemetry output enabled"], 1):
                break
        loaded = [line.split("APP: ")[1].strip() for line in self.log_lines()
                  if "Loading file" in line and "APP: " in line]
        print("Apps started from the startup script: " + ", ".join(loaded))
        self.events()  # skip the startup events

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None

    def close(self):
        self.stop()
        if self.proxy:
            self.proxy.terminate()

    def log_lines(self):
        with open(self.log, errors="replace") as f:
            return f.read().splitlines()

    def _wait_log(self, texts, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if self.proc.poll() is not None:
                return False
            text = "\n".join(self.log_lines())
            if all(t in text for t in texts):
                return True
            time.sleep(0.2)
        return False

    def events(self):
        """MSN_APP events written to the log since the last call."""
        with open(self.log, errors="replace") as f:
            f.seek(self.log_pos)
            new = f.read()
            self.log_pos = f.tell()
        return [line.split(": ", 1)[1] for line in new.splitlines() if "/MSN_APP " in line and ": " in line]


# ---------------------------------------------------------------- console

class Console:
    def __init__(self, sat, ground):
        self.sat = sat
        self.g = ground

    def show_events(self, wait=0.7):
        time.sleep(wait)
        for e in self.sat.events():
            print(f"  EVENT  {e}")

    def command(self, cc, payload, reply_mid):
        """Send one MSN command, print its telemetry reply and events."""
        mark = self.g.mark(reply_mid) if reply_mid else 0
        self.g.send(cc, payload)
        if reply_mid:
            p = self.g.wait_new(reply_mid, mark)
            print("  " + (DECODERS[reply_mid](p) if p else "(no telemetry reply)"))
        self.show_events()

    # -- the mission functions

    def show_hk(self):
        print("HK is not commanded: SCH_LAB asks for it every 4 s. It only reports values in memory.")
        with self.g.lock:
            item = self.g.last.get(HK_MID)
        if item:
            print(f"  ({time.time() - item[0]:.1f} s old)  {decode_hk(item[1])}")
        else:
            print("  no HK yet")

    def write_temp(self, value=None):
        value = value if value is not None else ask("New content of msn_temp.txt (a number, or text)", "75.0")
        with open(self.sat.path("msn_temp.txt"), "w") as f:
            f.write(value + "\n")
        print(f"  wrote '{value}' to {self.sat.path('msn_temp.txt')}")
        print("  Nothing is sent yet: MSN_APP reads the file on READ_TEMP, on the periodic TEMP or in RUN_DIAG.")

    def read_temp(self):
        print("READ_TEMP (FC 6): read msn_temp.txt now, compare with the limits, send TEMP.")
        self.command(READ_TEMP, b"", TEMP_MID)

    def set_param(self, pid=None, value=None):
        if pid is None:
            for i, n in enumerate(PARAM_NAMES):
                print(f"  {i}: {n}")
            pid = int(ask("ParamId", "0"))
            value = int(ask("Value", "80"))
        print(f"SET_PARAM (FC 2): ParamId {pid}, Value {value}. Saved to msn_param.txt and used at once.")
        self.command(SET_PARAM, struct.pack("<B3xi", pid, value), None)
        print(f"  msn_param.txt now: {read_text(self.sat.path('msn_param.txt'))}")

    def run_diag(self):
        print("RUN_DIAG (FC 4): 5 checks, each pass or fail.")
        self.command(RUN_DIAG, b"", DIAG_MID)

    def app_status(self, name=None):
        name = name or ask("App name", "SAMPLE_APP")
        print(f"GET_APP_STATUS (FC 5): ask ES about {name} and look it up in the startup script.")
        self.command(GET_APP_STATUS, name.encode().ljust(20, b"\0"), APP_STATUS_MID)

    def set_startup(self, name=None, enable=None):
        name = name or ask("App name", "SAMPLE_APP")
        enable = enable if enable is not None else int(ask("Enable (1) or disable (0)", "0"))
        print(f"SET_STARTUP (FC 3): {'enable' if enable else 'disable'} {name} from the next cFS start.")
        self.command(SET_STARTUP, name.encode().ljust(20, b"\0") + struct.pack("<B3x", enable), None)
        self.show_startup()

    # -- helpers to see the effect

    def show_startup(self):
        enabled, disabled, active = [], [], True
        for line in open(self.sat.path("cfe_es_startup.scr")):
            if line.startswith("!"):
                active = False
            elif len(line.split(",")) > 3:
                (enabled if active else disabled).append(line.split(",")[3].strip())
        print(f"  startup script, started by ES : {', '.join(enabled)}")
        print(f"  startup script, below '!'     : {', '.join(disabled) or '-'}")

    def watch(self, seconds=None):
        seconds = seconds or int(ask("Seconds to watch", "10"))
        print(f"Live telemetry for {seconds} s (HK every 4 s; TEMP every TEMP_PERIOD HK requests):")
        self.g.watching = True
        time.sleep(seconds)
        self.g.watching = False
        self.show_events(0)

    def restart(self):
        print("Restarting cFS: ES reads the startup script again.")
        self.sat.stop()
        self.sat.start(self.g)

    def tour(self):
        steps = [
            ("Housekeeping arrives by itself every 4 s, together with the periodic TEMP.",
             lambda: self.watch(9)),
            ("The temperature file is the 'sensor'. Make it hot (75.0, high limit 60) and wait.",
             lambda: (self.write_temp("75.0"), self.watch(5))),
            ("READ_TEMP measures on request instead of waiting for the period.", self.read_temp),
            ("SET_PARAM raises the high limit to 80: the same 75.0 becomes NORMAL.",
             lambda: (self.set_param(0, 80), self.read_temp())),
            ("RUN_DIAG with everything normal.", self.run_diag),
            ("Break the sensor (text instead of a number) and run RUN_DIAG again.",
             lambda: (self.write_temp("broken"), self.run_diag(), self.write_temp("21.5"))),
            ("GET_APP_STATUS asks ES about SAMPLE_APP.", lambda: self.app_status("SAMPLE_APP")),
            ("SET_STARTUP disables SAMPLE_APP. It keeps running until the restart.",
             lambda: (self.set_startup("SAMPLE_APP", 0), self.app_status("SAMPLE_APP"))),
            ("Restart cFS: SAMPLE_APP is not started any more.",
             lambda: (self.restart(), self.app_status("SAMPLE_APP"))),
            ("Enable SAMPLE_APP again and restart: it is back (now started last).",
             lambda: (self.set_startup("SAMPLE_APP", 1), self.restart(), self.app_status("SAMPLE_APP"))),
            ("Parameters survive restarts: HK still shows the high limit 80.", lambda: self.watch(5)),
        ]
        for i, (text, action) in enumerate(steps, 1):
            print(f"\n--- Tour step {i}/{len(steps)}: {text}")
            input("    press Enter ...")
            action()

    def loop(self):
        menu = [("h", "latest HK", self.show_hk),
                ("f", "write temperature file", self.write_temp),
                ("1", "READ_TEMP", self.read_temp),
                ("2", "SET_PARAM", self.set_param),
                ("3", "RUN_DIAG", self.run_diag),
                ("4", "GET_APP_STATUS", self.app_status),
                ("5", "SET_STARTUP", self.set_startup),
                ("s", "show startup script", self.show_startup),
                ("w", "watch telemetry", self.watch),
                ("r", "restart cFS", self.restart),
                ("t", "guided tour", self.tour),
                ("q", "quit", None)]
        while True:
            print("\n" + "  ".join(f"[{k}] {t}" for k, t, _ in menu))
            choice = input("> ").strip().lower()
            if choice == "q":
                return
            for key, _, action in menu:
                if key == choice:
                    try:
                        action()
                    except ValueError as e:
                        print(f"  invalid input: {e}")


def ask(prompt, default):
    answer = input(f"  {prompt} [{default}]: ").strip()
    return answer or default


def read_text(path):
    try:
        return open(path).read().strip()
    except OSError:
        return "(missing)"


def read_bytes(path):
    return open(path, "rb").read() if os.path.exists(path) else None


def restore(path, data):
    if data is None:
        if os.path.exists(path):
            os.remove(path)
    else:
        with open(path, "wb") as f:
            f.write(data)


def main():
    args = parse_args()
    if subprocess.run(["pgrep", "-x", "core-cpu1"], stdout=subprocess.DEVNULL).returncode == 0:
        print("core-cpu1 is already running. Stop it first.")
        return 1
    sat = Satellite(args.build_dir)
    if not os.path.exists(sat.path("msn_app.so")):
        print(f"{sat.path('msn_app.so')} not found. Build and install the native target first.")
        return 1

    # Keep copies of the files the demo changes, to put them back on exit
    saved = {name: read_bytes(sat.path(name)) for name in ("cfe_es_startup.scr", "msn_temp.txt", "msn_param.txt")}
    ground = Ground()
    try:
        sat.start(ground)
        console = Console(sat, ground)
        if args.tour:
            console.tour()
        console.loop()
    except (KeyboardInterrupt, EOFError):
        print()
    finally:
        sat.close()
        if not args.keep_files:
            for name, data in saved.items():
                restore(sat.path(name), data)
            print("Restored cfe_es_startup.scr, msn_temp.txt and msn_param.txt.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
