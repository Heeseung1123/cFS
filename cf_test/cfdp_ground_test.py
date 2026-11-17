#!/usr/bin/env python3
"""Plays a minimal CFDP Class 2 receiver (entity 23) against the CF app in native core-cpu1.

1. Starts core-cpu1, enables TO_LAB and writes a random test file into /cf.
2. Sends CF TX_FILE (class 2) through CI_LAB in a TC frame.
3. Joins the CF PDUs (MID 0x0FFD) from the TM frames, optionally dropping frames.
4. Answers EOF with ACK, missing data with NAK and a complete file with FIN.
5. Waits for the ACK of the FIN and compares the file and its checksum.
"""
import argparse
import os
import random
import select
import socket
import subprocess
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CFS_ROOT = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, os.path.join(CFS_ROOT, "to_test"))
from tm_deframer import FRAME_SIZE, Deframer  # noqa: E402

# Software Bus
CF_CMD_MID = 0x18B3
CF_TX_FILE_CC = 2
CF_TX_FILE_CMD_SIZE = 144  # sizeof(CF_TxFileCmd_t) in this build
CF_PDU_UP_MID = 0x1FFD     # channel 0 input MID (cf_def_config.c)
CF_PDU_DOWN_MID = 0x0FFD   # channel 0 output MID
TLM_HDR_LEN = 16           # offsetof(CF_PduTlmMsg_t, ph)
CMD_HDR_LEN = 8            # offsetof(CF_PduCmdMsg_t, ph)
TO_LAB_CMD_MID = 0x1880

# CFDP (CCSDS 727.0-B-5), same values as cf_cfdp_pdu.h
DIR_EOF, DIR_FIN, DIR_ACK, DIR_MD, DIR_NAK = 4, 5, 6, 7, 8
ACK_ACTIVE = 1
FIN_COMPLETE, FIN_RETAINED = 0, 2
NAK_MAX_SEGMENTS = 58      # CF_NAK_MAX_SEGMENTS
GROUND_EID, FLIGHT_EID = 23, 24


def cfdp_checksum(data):
    """CFDP modular checksum: sum of big-endian 32 bit words, as cf_crc.c."""
    pad = (-len(data)) % 4
    data = data + bytes(pad)
    total = 0
    for i in range(0, len(data), 4):
        total = (total + int.from_bytes(data[i:i + 4], "big")) & 0xFFFFFFFF
    return total


class Pdu:
    """Fixed PDU header fields (cf_codec.c:70-81) and the data field."""

    def __init__(self, raw):
        flags = raw[0]
        self.version = flags >> 5
        self.is_data = (flags >> 4) & 1
        self.toward_sender = (flags >> 3) & 1
        self.unacked = (flags >> 2) & 1
        length = (raw[1] << 8) | raw[2]
        self.elen = ((raw[3] >> 4) & 7) + 1
        self.slen = (raw[3] & 7) + 1
        i = 4
        self.src = int.from_bytes(raw[i:i + self.elen], "big"); i += self.elen
        self.seq = int.from_bytes(raw[i:i + self.slen], "big"); i += self.slen
        self.dst = int.from_bytes(raw[i:i + self.elen], "big"); i += self.elen
        self.data = raw[i:i + length]
        self.directive = None if self.is_data else self.data[0]


def build_pdu(ref, directive, body):
    """A file directive toward the sender of transaction ref (same IDs and lengths)."""
    data = bytes([directive]) + body
    flags = (ref.version << 5) | (0 << 4) | (1 << 3) | (0 << 2)
    lens = ((ref.elen - 1) << 4) | (ref.slen - 1)
    return (bytes([flags, len(data) >> 8, len(data) & 0xFF, lens]) + ref.src.to_bytes(ref.elen, "big")
            + ref.seq.to_bytes(ref.slen, "big") + ref.dst.to_bytes(ref.elen, "big") + data)


class Link:
    """TC frames to CI_LAB and TM frames from TO_LAB, with optional frame loss."""

    def __init__(self, args):
        self.args = args
        self.tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx.bind(("0.0.0.0", args.tlm_port))
        self.frame_seq = 0
        self.deframer = Deframer()
        self.rng = random.Random(args.seed)
        self.frames_in = self.frames_dropped = 0

    def send_packet(self, pkt):
        length = 5 + len(pkt) - 1
        hdr = bytes([0x20, 0x03, length >> 8, length & 0xFF, self.frame_seq & 0xFF])  # SCID 3, VCID 0
        self.frame_seq += 1
        self.tx.sendto(hdr + pkt, (self.args.host, self.args.ci_port))

    def send_cmd(self, mid, cc, payload):
        length = 2 + len(payload) - 1
        pkt = bytearray([mid >> 8, mid & 0xFF, 0xC0, 0x00, length >> 8, length & 0xFF, cc, 0]) + payload
        csum = 0xFF
        for b in pkt:
            csum ^= b
        pkt[7] = csum
        self.send_packet(bytes(pkt))

    def send_pdu(self, pdu):
        """CF_PduCmdMsg_t: command header (8) then the PDU; CF ignores the function code."""
        length = CMD_HDR_LEN + len(pdu) - 7
        self.send_packet(bytes([CF_PDU_UP_MID >> 8, CF_PDU_UP_MID & 0xFF, 0xC0, 0x00,
                                length >> 8, length & 0xFF, 0, 0]) + pdu)

    def recv_packets(self, timeout):
        """Returns the Space Packets joined from the frames that arrive within timeout."""
        ready, _, _ = select.select([self.rx], [], [], timeout)
        if not ready:
            return []
        frame = self.rx.recv(4096)
        if len(frame) != FRAME_SIZE:
            return []
        self.frames_in += 1
        if self.rng.random() < self.args.loss:
            self.frames_dropped += 1  # the Deframer sees the MC gap and drops the split packet
            return []
        packets, _ = self.deframer.feed(frame)
        return packets


class Receiver:
    """Class 2 receiving entity: keeps the file data by offset and the gaps."""

    def __init__(self, link, args):
        self.link, self.args = link, args
        self.ref = None
        self.md = None
        self.fsize = None
        self.eof = None
        self.data = {}  # offset -> bytes
        self.fd_count = self.dup_count = self.nak_count = self.fin_count = 0
        self.last_fd = self.last_nak = self.fin_sent = 0.0
        self.done = False

    def gaps(self):
        """Missing [start, end) ranges; (0, 0) asks again for the Metadata PDU."""
        result = [] if self.md else [(0, 0)]
        pos = 0
        for off in sorted(self.data):
            if off > pos:
                result.append((pos, off))
            pos = max(pos, off + len(self.data[off]))
        if pos < self.fsize:
            result.append((pos, self.fsize))
        return result

    def file_bytes(self):
        out = bytearray(self.fsize)
        for off, chunk in self.data.items():
            out[off:off + len(chunk)] = chunk
        return bytes(out)

    def on_pdu(self, pdu):
        if pdu.dst != GROUND_EID or pdu.toward_sender:
            return
        if self.ref is None:
            self.ref = pdu
        if pdu.is_data:
            off = int.from_bytes(pdu.data[0:4], "big")
            self.dup_count += off in self.data
            self.data[off] = bytes(pdu.data[4:])
            self.fd_count += 1
            self.last_fd = time.time()
        elif pdu.directive == DIR_MD:
            body = pdu.data[1:]
            self.fsize = int.from_bytes(body[1:5], "big")
            n = body[5]
            src = body[6:6 + n].decode()
            m = body[6 + n]
            dst = body[7 + n:7 + n + m].decode()
            if not self.md:
                print(f"  MD : seq {pdu.seq}, {src} -> {dst}, {self.fsize} bytes, closure {body[0] >> 7}")
            self.md = (src, dst)
        elif pdu.directive == DIR_EOF:
            body = pdu.data[1:]
            cc = body[0] >> 4
            crc = int.from_bytes(body[1:5], "big")
            size = int.from_bytes(body[5:9], "big")
            if self.eof is None:
                print(f"  EOF: cc {cc}, checksum 0x{crc:08x}, size {size}, {self.fd_count} FD so far")
            self.eof = (cc, crc, size)
            self.fsize = size
            # ACK(EOF): directive EOF, subtype 0; condition code of the EOF, transaction active.
            self.link.send_pdu(build_pdu(self.ref, DIR_ACK, bytes([DIR_EOF << 4, (cc << 4) | ACK_ACTIVE])))
        elif pdu.directive == DIR_ACK and (pdu.data[1] >> 4) == DIR_FIN:
            print(f"  ACK(FIN) received: transaction {pdu.seq} closed by CF")
            self.done = True

    def step(self):
        """Called after every packet and timeout: NAK the gaps or FIN a complete file."""
        if self.eof is None or self.done:
            return
        now = time.time()
        missing = self.gaps()
        if missing:
            # Wait until data stops arriving, then NAK every args.nak_timer seconds.
            if now - self.last_fd > 1.0 and now - self.last_nak > self.args.nak_timer:
                for i in range(0, len(missing), NAK_MAX_SEGMENTS):
                    segs = missing[i:i + NAK_MAX_SEGMENTS]
                    body = (0).to_bytes(4, "big") + self.fsize.to_bytes(4, "big")
                    body += b"".join(s.to_bytes(4, "big") + e.to_bytes(4, "big") for s, e in segs)
                    self.link.send_pdu(build_pdu(self.ref, DIR_NAK, body))
                    self.nak_count += 1
                lost = sum(e - s for s, e in missing)
                print(f"  NAK: {len(missing)} gaps, {lost} bytes missing")
                self.last_nak = now
        elif now - self.fin_sent > self.args.nak_timer:
            # FIN: no error, data complete, file retained. Resent until CF acknowledges it.
            self.link.send_pdu(build_pdu(self.ref, DIR_FIN, bytes([(0 << 4) | (FIN_COMPLETE << 2) | FIN_RETAINED])))
            self.fin_count += 1
            self.fin_sent = now
            if self.fin_count == 1:
                print(f"  FIN: file complete after {self.fd_count} FD ({self.dup_count} resent)")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-b", "--build-dir", default="build-native")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--ci-port", type=int, default=1234)
    parser.add_argument("--tlm-port", type=int, default=1235)
    parser.add_argument("--size", type=int, default=1 << 20, help="test file size in bytes")
    parser.add_argument("--loss", type=float, default=0.0, help="fraction of TM frames dropped on purpose")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--nak-timer", type=float, default=4.0, help="seconds between NAKs or FINs")
    parser.add_argument("--timeout", type=float, default=600.0)
    args = parser.parse_args()

    exe_dir = os.path.join(CFS_ROOT, args.build_dir, "exe", "cpu1")
    log_path = os.path.join(SCRIPT_DIR, "core-cpu1.log")
    name = "cfdp_test.bin"
    src_path = os.path.join(exe_dir, "cf", name)
    if subprocess.run(["pgrep", "-x", "core-cpu1"], stdout=subprocess.DEVNULL).returncode == 0:
        print("FAIL: core-cpu1 is already running. Stop it first.")
        return 1

    original = random.Random(args.seed).randbytes(args.size)
    with open(src_path, "wb") as f:
        f.write(original)

    link = Link(args)
    rx = Receiver(link, args)
    with open(log_path, "w") as logf:
        core = subprocess.Popen(["./core-cpu1", "-R", "PO"], cwd=exe_dir, stdout=logf, stderr=subprocess.STDOUT)
    ok = False
    try:
        print("=== Starting core-cpu1 ===")
        for _ in range(300):
            text = open(log_path, errors="replace").read()
            if "CF Initialized" in text and "TO Lab Initialized" in text and "CI Lab Initialized" in text:
                break
            time.sleep(0.1)
        time.sleep(2)

        print("=== Enabling TO_LAB output ===")
        for _ in range(10):
            link.send_cmd(TO_LAB_CMD_MID, 6, args.host.encode().ljust(16, b"\0"))
            time.sleep(1.0)
            if "telemetry output enabled" in open(log_path, errors="replace").read():
                break

        print(f"=== CF TX_FILE /cf/{name} ({args.size} bytes), class 2, TM frame loss {args.loss:.0%} ===")
        payload = bytearray(CF_TX_FILE_CMD_SIZE - 8)
        payload[0:4] = bytes([1, 1, 0, 0])  # class 2, keep file, channel 0, priority 0
        payload[4:8] = GROUND_EID.to_bytes(4, "little")  # dest_id, host order (x86 and BBB are little endian)
        payload[8:72] = f"/cf/{name}".encode().ljust(64, b"\0")
        payload[72:136] = f"/ground/{name}".encode().ljust(64, b"\0")
        start = time.time()
        link.send_cmd(CF_CMD_MID, CF_TX_FILE_CC, bytes(payload))

        while not rx.done and time.time() - start < args.timeout:
            for pkt in link.recv_packets(0.2):
                if ((pkt[0] << 8) | pkt[1]) == CF_PDU_DOWN_MID:
                    rx.on_pdu(Pdu(pkt[TLM_HDR_LEN:]))
            rx.step()
        elapsed = time.time() - start
    finally:
        core.terminate()
        try:
            core.wait(5)
        except subprocess.TimeoutExpired:
            core.kill()
        os.remove(src_path)

    print("\n=== Result ===")
    checks = []

    def check(cond, text):
        checks.append(cond)
        print(f"  {'PASS' if cond else 'FAIL'}: {text}")

    check(rx.done, f"transaction closed by ACK(FIN) in {elapsed:.1f} s")
    if rx.eof and rx.fsize is not None and not rx.gaps():
        received = rx.file_bytes()
        check(received == original, f"received file equals the sent file ({len(received)} bytes)")
        check(cfdp_checksum(received) == rx.eof[1], f"checksum 0x{cfdp_checksum(received):08x} matches EOF")
    else:
        check(False, "file complete")
    print(f"  TM frames {link.frames_in}, dropped on purpose {link.frames_dropped}; FD {rx.fd_count} "
          f"({rx.dup_count} resent); NAK PDUs {rx.nak_count}; FIN sent {rx.fin_count}")
    if rx.done:
        print(f"  throughput {args.size / elapsed / 1024:.1f} KiB/s")

    print("\n=== CF events in the log ===")
    for line in open(log_path, errors="replace"):
        if "/CF " in line:
            print("  " + line.rstrip())
    print("RESULT:", "PASS" if all(checks) else "FAIL")
    return 0 if all(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
