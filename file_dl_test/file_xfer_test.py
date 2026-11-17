#!/usr/bin/env python3
"""Downlink, upload and relay test of the file_dl app. All telemetry comes through TO_LAB.

  --mode dl      DL_START (MID 0x18C0) a file on board, rebuild it, RESEND gaps, DONE
  --mode ul      upload (MID 0x18C2) into /cf/inbox, gaps from UL status (0x08C1)
  --mode relay   relay (MID 0x18C3) into /cf/relay; after the CRC check the satellite sends
                 it down on 0x08C3 with DestTag, and that ground station rebuilds it
  --mode both    upload one file while another one is downlinked; the downlink must not stop
  --mode reject  DATA without START, bad names, too large, wrong CRC, CANCEL
--ul-loss drops DATA commands and --dl-loss drops TM frames on purpose.
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
sys.path.insert(0, SCRIPT_DIR)
import file_dl_ground_test as dlt  # noqa: E402  (send_cmd, parse_segment, missing_ranges)
from tm_deframer import FRAME_SIZE, Deframer  # noqa: E402

# Message IDs (file_dl_msgids.h)
DL_CMD, UL_CMD, RELAY_CMD, TO_LAB_CMD = 0x18C0, 0x18C2, 0x18C3, 0x1880
DL_SEG, UL_STATUS_TLM, RELAY_STATUS_TLM, RELAY_SEG = 0x08C0, 0x08C1, 0x08C2, 0x08C3
# Function codes
DL_START, DL_RESEND, DL_DONE = 1, 2, 3
NOOP, START, DATA, END, STATUS, CANCEL = 0, 1, 2, 3, 4, 5  # same on UL_CMD and RELAY_CMD
STATES = ["IDLE", "RECEIVING", "VERIFYING", "COMPLETE", "FAILED"]
CHUNK, NAME_LEN, MAX_RESENDS, TLM_HDR_LEN = 960, 64, 64, 16
CMD_GAP = 0.02  # native SB pipes hold 10 messages; one command per 20 ms keeps FILE_DL ahead
STATION_B = 2   # DestTag of the relay ground station


class DownlinkRx:
    """One ground station: keeps the segments of one MID and DestTag, by byte offset."""

    def __init__(self, mid=DL_SEG, tag=0):
        self.mid, self.tag, self.chunks, self.size, self.end_crc, self.name = mid, tag, {}, None, None, None
        self.segments = self.after_end = self.rounds = 0
        self.last_data = time.time()

    def feed(self, mid, pkt):
        if mid != self.mid or pkt[TLM_HDR_LEN + 1] != self.tag:
            return
        seg_type, offset, size, crc, data = dlt.parse_segment(pkt)
        self.size = size  # every segment carries the file size
        self.last_data = time.time()
        if seg_type == dlt.TYPE_START:
            self.name = bytes(data).decode(errors="replace")
        elif seg_type == dlt.TYPE_DATA:
            self.after_end += self.end_crc is not None
            self.chunks[offset] = bytes(data)
            self.segments += 1
        elif seg_type == dlt.TYPE_END:
            self.end_crc = crc

    def rebuild(self):
        out = bytearray(self.size)
        for off, data in self.chunks.items():
            out[off:off + len(data)] = data
        return bytes(out)


class Link:
    """Ground side of CI_LAB (commands) and TO_LAB (TM frames)."""

    def __init__(self, args):
        self.args, self.seq = args, 0
        self.tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx.bind(("0.0.0.0", args.tlm_port))
        self.deframer, self.loss_rng = Deframer(), random.Random(args.seed)
        self.frames = self.dropped = 0
        self.status, self.status_count = {}, {UL_STATUS_TLM: 0, RELAY_STATUS_TLM: 0}
        self.dl = DownlinkRx()
        self.relay_segments = 0  # segments seen on 0x08C3, whatever the tag

    def cmd(self, mid, cc, payload=b""):
        dlt.send_cmd(self.tx, self.args, self.seq, mid, cc, payload)
        self.seq += 1
        time.sleep(CMD_GAP)

    def poll(self, timeout=0.0):
        """Reads TM frames for timeout seconds (0: only what is waiting) and dispatches packets."""
        end = time.time() + timeout
        while True:
            ready, _, _ = select.select([self.rx], [], [], max(0.0, end - time.time()))
            if not ready:
                if time.time() >= end:
                    return
                continue
            frame = self.rx.recv(4096)
            if len(frame) != FRAME_SIZE:
                continue
            self.frames += 1
            if self.loss_rng.random() < self.args.dl_loss:
                self.dropped += 1
                continue
            packets, _ = self.deframer.feed(frame)
            for pkt in packets:
                mid = (pkt[0] << 8) | pkt[1]
                if mid in (DL_SEG, RELAY_SEG):
                    self.relay_segments += mid == RELAY_SEG
                    self.dl.feed(mid, pkt)
                elif mid in self.status_count:
                    self.status[mid] = parse_status(pkt)
                    self.status_count[mid] += 1

    def wait_status(self, cmd_mid, status_mid, cc=None, timeout=3.0, tries=3):
        """Sends cc (if any) and waits for a new status; asks again with STATUS if it was lost."""
        for _ in range(tries):
            before = self.status_count[status_mid]
            if cc is not None:
                self.cmd(cmd_mid, cc)
            t_end = time.time() + timeout
            while time.time() < t_end and self.status_count[status_mid] == before:
                self.poll(0.2)
            if self.status_count[status_mid] > before:
                return self.status[status_mid]
            cc = STATUS
        return None


def parse_status(pkt):
    p = pkt[TLM_HDR_LEN:]
    u32 = lambda i: int.from_bytes(p[i:i + 4], "big")  # noqa: E731
    gaps = [(u32(88 + 8 * k), u32(92 + 8 * k)) for k in range(p[3])]
    return {"state": STATES[p[0]] if p[0] < len(STATES) else p[0], "relay": p[1], "tag": p[2],
            "size": u32(4), "received": u32(8), "expected_crc": u32(12), "file_crc": u32(16),
            "total_gaps": u32(20), "name": p[24:88].split(b"\0")[0].decode(errors="replace"), "gaps": gaps}


def start_payload(name, size, crc, tag=None):
    """Upload START (no tag) or relay START (with DestTag)."""
    p = name.encode().ljust(NAME_LEN, b"\0") + size.to_bytes(4, "big") + crc.to_bytes(4, "big")
    return p if tag is None else p + bytes([tag, 0, 0, 0])


def upload(link, cmd_mid, status_mid, name, data, tag=None, crc=None, rng=None):
    """START, every chunk, END; sends the gaps the status lists again until the CRC check ends."""
    crc = zlib.crc32(data) if crc is None else crc
    link.cmd(cmd_mid, START, start_payload(name, len(data), crc, tag))
    todo, rounds, skipped = list(range(0, len(data), CHUNK)), 0, 0
    while True:
        for off in todo:
            if rng is not None and rng.random() < link.args.ul_loss:
                skipped += 1
                continue
            part = data[off:off + CHUNK]
            link.cmd(cmd_mid, DATA, off.to_bytes(4, "big") + len(part).to_bytes(2, "big") + b"\0\0" + part)
            link.poll()
        st = link.wait_status(cmd_mid, status_mid, END)
        if st is None:
            return None, rounds, skipped
        if st["state"] != "RECEIVING":
            break
        rounds += 1
        todo = [o for gs, gl in st["gaps"] for o in range(gs, gs + gl, CHUNK)]
        print(f"  round {rounds}: {st['total_gaps']} gaps, sending {len(todo)} chunks again")
    # VERIFYING: the satellite sends the status again when the CRC check is done.
    t_end = time.time() + 30
    while st is not None and st["state"] == "VERIFYING" and time.time() < t_end:
        st = link.wait_status(cmd_mid, status_mid, None, timeout=2.0, tries=1) or \
            link.wait_status(cmd_mid, status_mid, STATUS)
    return st, rounds, skipped


def finish_downlink(link, timeout, quiet):
    """After the first pass: RESEND the gaps until the file is whole, then DONE with its CRC32.
    RESEND and DONE always go to the downlink MID, also for a relayed file."""
    dl, t_end = link.dl, time.time() + timeout
    while time.time() < t_end:
        link.poll(0.5)
        if dl.size is None or time.time() - dl.last_data < quiet:
            continue
        gaps = dlt.missing_ranges(dl.chunks, dl.size)
        if not gaps:
            link.cmd(DL_CMD, DL_DONE, zlib.crc32(dl.rebuild()).to_bytes(4, "big"))
            link.poll(1.5)
            return dl.rebuild()
        dl.rounds += 1
        print(f"  DL RESEND round {dl.rounds}: {len(gaps)} ranges, {sum(e - s for s, e in gaps)} bytes")
        for gs, ge in gaps[:MAX_RESENDS]:
            link.cmd(DL_CMD, DL_RESEND, gs.to_bytes(4, "big") + (ge - gs).to_bytes(4, "big"))
        dl.last_data = time.time()
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--mode", choices=["dl", "ul", "relay", "both", "reject"], default="relay")
    parser.add_argument("-b", "--build-dir", default="build-native")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--ci-port", type=int, default=1234)
    parser.add_argument("--tlm-port", type=int, default=1235)
    parser.add_argument("--size", type=int, default=64 * 1024)
    parser.add_argument("--ul-loss", type=float, default=0.0, help="fraction of DATA commands not sent")
    parser.add_argument("--dl-loss", type=float, default=0.0, help="fraction of TM frames dropped")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--quiet-time", type=float, default=3.0, help="seconds without segments before RESEND")
    args = parser.parse_args()

    exe_dir = os.path.join(CFS_ROOT, args.build_dir, "exe", "cpu1")
    log_path = os.path.join(SCRIPT_DIR, "core-cpu1.log")
    if subprocess.run(["pgrep", "-x", "core-cpu1"], stdout=subprocess.DEVNULL).returncode == 0:
        print("FAIL: core-cpu1 is already running. Stop it first.")
        return 1

    data_rng = random.Random(args.seed)
    ul_data, dl_data = data_rng.randbytes(args.size), data_rng.randbytes(args.size)
    ul_name, relay_name, dl_name = "/cf/inbox/ul_test.bin", "/cf/relay/relay_test.bin", "/cf/dl_test.bin"
    board = lambda name: os.path.join(exe_dir, name.lstrip("/"))  # noqa: E731
    scratch = [board(ul_name), board(relay_name), board(dl_name)]
    for path in scratch:
        for p in (path, path + ".part"):
            if os.path.exists(p):
                os.remove(p)
    if args.mode in ("dl", "both"):
        with open(board(dl_name), "wb") as f:
            f.write(dl_data)

    results = []

    def check(ok, text):
        results.append(bool(ok))
        print(f"  {'PASS' if ok else 'FAIL'}: {text}")

    def log_text():
        return open(log_path, errors="replace").read()

    def check_file(name, data, what):
        path = board(name)
        got = open(path, "rb").read() if os.path.exists(path) else None
        check(got == data, f"{name} equals the {what} file")
        check(not os.path.exists(path + ".part"), ".part file renamed")

    link = Link(args)
    ul_rng = random.Random(args.seed + 1)
    with open(log_path, "w") as logf:
        core = subprocess.Popen(["./core-cpu1", "-R", "PO"], cwd=exe_dir, stdout=logf, stderr=subprocess.STDOUT)
    try:
        for _ in range(300):
            if all(s in log_text() for s in ("FILE_DL Initialized", "TO Lab Initialized", "CI Lab Initialized")):
                break
            time.sleep(0.1)
        time.sleep(2)
        for _ in range(10):
            link.cmd(TO_LAB_CMD, 6, args.host.encode().ljust(16, b"\0"))
            time.sleep(1.0)
            if "telemetry output enabled" in log_text():
                break
        link.poll(0.5)
        print(f"=== mode {args.mode}, {args.size} bytes, UL loss {args.ul_loss:.0%}, "
              f"TM frame loss {args.dl_loss:.0%} ===")
        t0 = time.time()

        if args.mode == "dl":
            link.cmd(DL_CMD, DL_START, dl_name.encode().ljust(NAME_LEN, b"\0"))
            got = finish_downlink(link, args.timeout, args.quiet_time)
            t_dl = time.time() - t0
            check(got == dl_data, f"file equals the board file ({t_dl:.1f} s, {args.size / t_dl / 1024:.1f} KiB/s)")
            check(link.dl.end_crc == zlib.crc32(dl_data), "END CRC32 matches")
            check("confirmed by ground" in log_text(), "FILE_DL got DONE and closed the file")

        if args.mode in ("ul", "both"):
            if args.mode == "both":
                link.cmd(DL_CMD, DL_START, dl_name.encode().ljust(NAME_LEN, b"\0"))
                link.poll(1.0)
                seg_before = link.dl.segments
            t_ul0 = time.time()
            st, rounds, skipped = upload(link, UL_CMD, UL_STATUS_TLM, ul_name, ul_data, rng=ul_rng)
            t_ul = time.time() - t_ul0
            print(f"  UL: {skipped} chunks not sent on purpose, {rounds} gap rounds, {t_ul:.1f} s "
                  f"({args.size / t_ul / 1024:.1f} KiB/s)")
            check(st is not None and st["state"] == "COMPLETE", f"UL status is COMPLETE ({st and st['state']})")
            check(st is not None and st["file_crc"] == zlib.crc32(ul_data), "CRC32 read back on board matches")
            check_file(ul_name, ul_data, "uploaded")
            if args.mode == "ul":
                link.poll(2.0)
                check(link.dl.segments == 0 and link.relay_segments == 0, "upload alone sends nothing down")
            else:
                during = link.dl.segments - seg_before
                expected = t_ul * 10 * (1 - args.dl_loss)
                check(during >= 0.5 * expected,
                      f"downlink kept going during the upload ({during} segments in {t_ul:.1f} s)")
                got = finish_downlink(link, args.timeout, args.quiet_time)
                check(got == dl_data, "downlinked file equals the board file")
                check("confirmed by ground" in log_text(), "FILE_DL got DONE and closed the file")

        if args.mode == "relay":
            link.dl = DownlinkRx(RELAY_SEG, STATION_B)  # this ground station keeps relay segments for tag B
            st, rounds, skipped = upload(link, RELAY_CMD, RELAY_STATUS_TLM, relay_name, ul_data, STATION_B, rng=ul_rng)
            t_ul = time.time() - t0
            print(f"  relay uplink: {skipped} chunks not sent on purpose, {rounds} gap rounds, {t_ul:.1f} s "
                  f"({args.size / t_ul / 1024:.1f} KiB/s)")
            check(st is not None and st["state"] == "COMPLETE", f"relay status is COMPLETE ({st and st['state']})")
            check(st is not None and st["relay"] == 1 and st["tag"] == STATION_B, "status shows relay and tag")
            check_file(relay_name, ul_data, "relayed")
            t_dl0 = time.time()
            got = finish_downlink(link, args.timeout, args.quiet_time)
            check(got == ul_data, f"station tag {STATION_B} rebuilt the file from 0x08C3 segments "
                                  f"({time.time() - t_dl0:.1f} s after the uplink, {time.time() - t0:.1f} s total)")
            check(link.dl.name == relay_name, f"START segment names the relay file ({link.dl.name})")
            check(link.dl.end_crc == zlib.crc32(ul_data), "END CRC32 matches")
            check("FILE_RELAY: sending /cf/relay/relay_test.bin to tag 2" in log_text(), "relay downlink started")
            check("confirmed by ground" in log_text(), "FILE_DL got DONE and closed the file")

        if args.mode == "reject":
            part = ul_data[:CHUNK]
            data_cmd = (0).to_bytes(4, "big") + len(part).to_bytes(2, "big") + b"\0\0" + part
            link.cmd(UL_CMD, DATA, data_cmd)
            link.cmd(RELAY_CMD, DATA, data_cmd)
            link.cmd(UL_CMD, START, start_payload("/cf/evil.bin", 100, 0))
            link.cmd(UL_CMD, START, start_payload("/cf/inbox/../evil.bin", 100, 0))
            link.cmd(UL_CMD, START, start_payload("/cf/relay/x.bin", 100, 0))   # relay dir on the upload MID
            link.cmd(RELAY_CMD, START, start_payload("/cf/inbox/x.bin", 100, 0, 2))
            link.cmd(UL_CMD, START, start_payload("/cf/inbox/big.bin", 5 * 1024 * 1024, 0))
            link.cmd(RELAY_CMD, START, start_payload("/cf/relay/x.bin", 100, 0))  # upload START size on relay MID
            link.poll(1.0)
            text = log_text()
            check("FILE_UL: DATA 0+960 rejected" in text and "FILE_RELAY: DATA 0+960 rejected" in text,
                  "DATA without START is rejected on both MIDs")
            check("FILE_UL: START /cf/evil.bin" in text and "FILE_UL: START /cf/inbox/../evil.bin" in text,
                  "names outside /cf/inbox are rejected")
            check("FILE_UL: START /cf/relay/x.bin" in text and "FILE_RELAY: START /cf/inbox/x.bin" in text,
                  "each MID only writes its own directory")
            check("FILE_UL: START /cf/inbox/big.bin, 5242880 bytes rejected" in text, "file over 4 MiB is rejected")
            check("FILE_RELAY: bad command cc 1, size 80" in text, "relay START without DestTag is rejected")
            link.dl = DownlinkRx(RELAY_SEG, STATION_B)
            st, _, _ = upload(link, RELAY_CMD, RELAY_STATUS_TLM, relay_name, ul_data, STATION_B,
                              crc=zlib.crc32(ul_data) ^ 1)
            link.poll(3.0)
            check(st is not None and st["state"] == "FAILED", f"wrong CRC32 gives FAILED ({st and st['state']})")
            check("FILE_RELAY: /cf/relay/relay_test.bin crc32 mismatch" in log_text(), "mismatch event")
            check(link.relay_segments == 0 and not os.path.exists(board(relay_name)),
                  "no relay downlink and no final file after FAILED")
            check(os.path.exists(board(relay_name) + ".part"), ".part kept after FAILED")
            st = link.wait_status(RELAY_CMD, RELAY_STATUS_TLM, CANCEL)
            check(st is not None and st["state"] == "IDLE" and not os.path.exists(board(relay_name) + ".part"),
                  "CANCEL removes the .part file and returns to IDLE")
    finally:
        core.terminate()
        try:
            core.wait(5)
        except subprocess.TimeoutExpired:
            core.kill()
        for path in scratch:
            for p in (path, path + ".part"):
                if os.path.exists(p):
                    os.remove(p)

    text = log_text()
    check("Msg Limit Err" not in text, "no command dropped by the SB message limit")
    check("Pipe Overflow" not in text, "no SB pipe overflow")
    print(f"  TM frames {link.frames}, dropped on purpose {link.dropped}, DATA segments {link.dl.segments} "
          f"({link.dl.after_end} after END), DL RESEND rounds {link.dl.rounds}")
    for line in text.splitlines():
        if ("FILE_UL" in line or "FILE_DL" in line or "FILE_RELAY" in line or "Msg Limit" in line
                or "Overflow" in line) and "DATA 0+960" not in line:
            print("  " + line.rstrip())
    print("RESULT:", "PASS" if all(results) else "FAIL")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
