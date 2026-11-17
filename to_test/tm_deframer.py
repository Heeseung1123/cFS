"""Joins Space Packets split over TO_LAB TM frames, using the First Header Pointer."""

FRAME_SIZE = 1786
DATA_OFFSET = 20  # Primary Header 6 + reserved SDLS Security Header 14
SPP_HDR_LEN = 6
SPP_MIN_SIZE = 7
NO_FHP = 0x7FF  # no packet starts in this frame


def packet_size(hdr):
    return ((hdr[4] << 8) | hdr[5]) + SPP_MIN_SIZE


class Deframer:
    def __init__(self):
        self.buf = b""
        self.synced = False
        self.last_mc = None
        self.segmented = 0  # packets that started in an earlier frame

    def feed(self, frame):
        """Returns (packets, errors) for one frame; packets are complete SPP byte strings."""
        errors = []
        fhp = ((frame[4] & 0x07) << 8) | frame[5]
        mc = frame[2]
        data = frame[DATA_OFFSET:FRAME_SIZE]

        if self.last_mc is not None and mc != (self.last_mc + 1) & 0xFF:
            self.buf, self.synced = b"", False  # a lost frame breaks the split packet
        self.last_mc = mc

        if not self.synced:
            if fhp == NO_FHP:
                return [], errors
            self.buf, self.synced, data = b"", True, data[fhp:]
        elif not self.buf and fhp != 0:
            errors.append(f"FHP is {fhp}, expected 0 (no packet was split)")
        elif len(self.buf) >= SPP_HDR_LEN:
            need = packet_size(self.buf) - len(self.buf)
            expected = need if need < len(data) else NO_FHP
            if fhp != expected:
                errors.append(f"FHP is {fhp}, expected {expected} ({need} octets left of the split packet)")

        carried = len(self.buf)
        self.buf += data
        packets = []
        while len(self.buf) >= SPP_HDR_LEN and len(self.buf) >= packet_size(self.buf):
            size = packet_size(self.buf)
            packets.append(self.buf[:size])
            self.buf = self.buf[size:]
            if carried > 0:
                self.segmented += 1
            carried = max(0, carried - size)
        return packets, errors
