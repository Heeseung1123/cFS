#!/usr/bin/env python3
"""Plays the Ground System side of the PQC handshake against native core-cpu1.

Sends the user-defined SDLS EP packets (APID 0x180) in TC frames to CI_LAB and
checks the replies (MID 0x0980) in the TM frames from TO_LAB, plus the event log.
PQC is not linked yet, so ek, Sig_G and ct are placeholders.
"""
import argparse
import os
import select
import socket
import subprocess
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CFS_ROOT = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, os.path.join(CFS_ROOT, "to_test"))
from tm_deframer import Deframer  # noqa: E402

# TC frame and Space Packet
TC_PRIHDR_LEN = 5
TC_MAX_FRAME = 1024
SPP_HDR_LEN = 6
EP_HDR = (0x19, 0x80)  # APID 0x180
MAX_SEGMENT = TC_MAX_FRAME - TC_PRIHDR_LEN - SPP_HDR_LEN

# TM frame from TO_LAB
TM_FRAME_SIZE = 1786
TM_DATA_OFFSET = 6  # Primary Header 6, no Security Header
EP_TLM_MID = 0x0980
TLM_HDR_LEN = 16  # CFE_MSG_TelemetryHeader_t

# EP Tag: Type, User Flag 1, Service Group 0, PID
PID_HS_REQ, PID_HS_KEX, PID_ACTIVATE, PID_PING = 1, 2, 3, 4
EK_LEN, SIG_LEN, CT_LEN = 1184, 3309, 1088
SESSION_ID = bytes.fromhex("a1b2c3d4")


def tag(pid, reply=False):
    return (0x80 if reply else 0) | 0x40 | pid


def tlv(pid, value=b""):
    bits = len(value) * 8
    return bytes([tag(pid), bits >> 8, bits & 0xFF]) + value


class Ground:
    def __init__(self, args):
        self.args = args
        self.tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx.bind(("0.0.0.0", args.tlm_port))
        self.frame_seq = 0
        self.pkt_seq = 0
        self.deframer = Deframer()

    def send_frame(self, packet):
        total = TC_PRIHDR_LEN + len(packet)
        length = total - 1
        hdr = bytes([0x20, 0x03, (0 << 2) | (length >> 8), length & 0xFF, self.frame_seq & 0xFF])
        self.frame_seq += 1
        self.tx.sendto(hdr + packet, (self.args.host, self.args.ci_port))
        time.sleep(0.05)

    def send_ep(self, pdu, seq_jump=0):
        """Splits one EP PDU over Space Packets with the Sequence Flags."""
        chunks = [pdu[i:i + MAX_SEGMENT] for i in range(0, len(pdu), MAX_SEGMENT)] or [b""]
        for i, chunk in enumerate(chunks):
            if len(chunks) == 1:
                flags = 3
            elif i == 0:
                flags = 1
            elif i == len(chunks) - 1:
                flags = 2
            else:
                flags = 0
            if i == 2:
                self.pkt_seq += seq_jump  # used to test a lost segment
            word = (flags << 14) | (self.pkt_seq & 0x3FFF)
            length = len(chunk) - 1
            spp = bytes([EP_HDR[0], EP_HDR[1], word >> 8, word & 0xFF, length >> 8, length & 0xFF]) + chunk
            self.pkt_seq += 1
            self.send_frame(spp)
        return len(chunks)

    def send_cmd(self, mid, cc, payload=b""):
        """Builds a cFS command with its checksum and sends it in one TC frame."""
        length = 2 + len(payload) - 1
        pkt = bytearray([mid >> 8, mid & 0xFF, 0xC0, 0x00, length >> 8, length & 0xFF, cc, 0]) + payload
        csum = 0xFF
        for b in pkt:
            csum ^= b
        pkt[7] = csum
        self.send_frame(bytes(pkt))

    def drain(self):
        self.rx.setblocking(False)
        try:
            while True:
                self.rx.recv(4096)
        except BlockingIOError:
            pass

    def wait_reply(self, timeout=3.0):
        """Returns (tag, value) of the next EP reply found in a TM frame, or None."""
        end = time.time() + timeout
        while time.time() < end:
            ready, _, _ = select.select([self.rx], [], [], end - time.time())
            if not ready:
                break
            frame = self.rx.recv(4096)
            if len(frame) != TM_FRAME_SIZE:
                continue
            # A reply may now be split over frames, so packets are joined first.
            packets, _ = self.deframer.feed(frame)
            for pkt in packets:
                if ((pkt[0] << 8) | pkt[1]) == EP_TLM_MID:
                    t = TLM_HDR_LEN
                    bits = (pkt[t + 1] << 8) | pkt[t + 2]
                    return pkt[t], bytes(pkt[t + 3:t + 3 + bits // 8])
        return None


class Log:
    def __init__(self, path):
        self.path = path

    def count(self, text):
        with open(self.path, errors="replace") as f:
            return f.read().count(text)

    def wait(self, text, before, timeout=3.0):
        end = time.time() + timeout
        while time.time() < end:
            if self.count(text) > before:
                return True
            time.sleep(0.1)
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-b", "--build-dir", default="build-native")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--ci-port", type=int, default=1234)
    parser.add_argument("--tlm-port", type=int, default=1235)
    args = parser.parse_args()

    exe_dir = os.path.join(CFS_ROOT, args.build_dir, "exe", "cpu1")
    log_path = os.path.join(SCRIPT_DIR, "core-cpu1.log")
    if subprocess.run(["pgrep", "-x", "core-cpu1"], stdout=subprocess.DEVNULL).returncode == 0:
        print("FAIL: core-cpu1 is already running. Stop it first.")
        return 1

    results = []

    def check(ok, text):
        results.append(ok)
        print(f"  {'PASS' if ok else 'FAIL'}: {text}")

    ground = Ground(args)
    log = Log(log_path)
    with open(log_path, "w") as logf:
        core = subprocess.Popen(["./core-cpu1", "-R", "PO"], cwd=exe_dir, stdout=logf, stderr=subprocess.STDOUT)
    try:
        print("=== Starting core-cpu1 ===")
        if not log.wait("TO Lab Initialized", 0, 30) or not log.wait("CI Lab Initialized", 0, 30):
            print("FAIL: apps did not start")
            return 1
        time.sleep(2)

        print("=== Enabling TO_LAB output ===")
        for _ in range(10):
            ground.send_cmd(0x1880, 6, args.host.encode().ljust(16, b"\0"))
            if log.wait("telemetry output enabled", 0, 1.5):
                break
        check(log.count("telemetry output enabled") > 0, "TO_LAB output enabled")
        time.sleep(1)

        print("=== Broken order: Ping and HS_KEX before the handshake ===")
        n = log.count("Ping without active session")
        ground.send_ep(tlv(PID_PING))
        check(log.wait("Ping without active session", n), "Ping rejected before Activation")
        n = log.count("HS_KEX without HS_REQ")
        ground.send_ep(tlv(PID_HS_KEX, bytes(EK_LEN + SIG_LEN)))
        check(log.wait("HS_KEX without HS_REQ", n), "HS_KEX rejected before HS_REQ")

        print("=== 1-2. HS_REQ -> HS_REQ_R ===")
        ground.drain()
        ground.send_ep(tlv(PID_HS_REQ, SESSION_ID))
        reply = ground.wait_reply()
        check(reply == (tag(PID_HS_REQ, True), SESSION_ID), f"HS_REQ_R with Session ID {SESSION_ID.hex()} ({reply and (hex(reply[0]), reply[1].hex())})")

        print("=== 3. HS_KEX with a lost segment ===")
        n = log.count("segment out of order")
        ground.send_ep(tlv(PID_HS_KEX, bytes(range(256)) * 17 + bytes(EK_LEN + SIG_LEN - 256 * 17)), seq_jump=1)
        check(log.wait("segment out of order", n), "gap in Sequence Count detected")

        print("=== 3-4. HS_KEX (ek + Sig_G, split) -> HS_KEX_R (ct) ===")
        ground.drain()
        value = os.urandom(EK_LEN) + os.urandom(SIG_LEN)
        segments = ground.send_ep(tlv(PID_HS_KEX, value))
        reply = ground.wait_reply()
        check(reply is not None and reply[0] == tag(PID_HS_KEX, True) and len(reply[1]) == CT_LEN,
              f"HS_KEX_R with {CT_LEN} byte ct after {segments} segments ({reply and (hex(reply[0]), len(reply[1]))})")

        print("=== 5. Activation, Rekey, Start ===")
        n = log.count("session active")
        ground.send_ep(tlv(PID_ACTIVATE))
        check(log.wait("session active", n), "SPI 1 session active")

        print("=== 6-7. Ping -> Ping reply ===")
        ground.drain()
        ground.send_ep(tlv(PID_PING))
        reply = ground.wait_reply()
        check(reply == (tag(PID_PING, True), b""), f"Ping reply ({reply and hex(reply[0])})")

        print("=== Normal commands still reach the Software Bus ===")
        n = log.count("CI: NOOP command")
        ground.send_cmd(0x1884, 0)
        check(log.wait("CI: NOOP command", n), "CI_LAB NOOP executed")
    finally:
        core.terminate()
        try:
            core.wait(5)
        except subprocess.TimeoutExpired:
            core.kill()

    print("\n=== PQC events in the log ===")
    with open(log_path, errors="replace") as f:
        for line in f:
            if "PQC" in line:
                print(line.rstrip())
    print(f"\npassed: {results.count(True)}, failed: {results.count(False)}")
    print("RESULT:", "PASS" if all(results) else "FAIL")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
