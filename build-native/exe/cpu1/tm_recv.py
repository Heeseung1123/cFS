import socket

PORT, FRAME_SIZE, DATA_OFFSET = 1235, 1786, 20

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("", PORT))
print(f"listening on UDP {PORT}")

while True:
    d, addr = sock.recvfrom(4096)
    if len(d) != FRAME_SIZE:
        print(f"{addr[0]} not a frame: {len(d)} bytes, starts {d[:6].hex(' ')}")
        continue

    tfvn = d[0] >> 6
    scid = ((d[0] & 0x3F) << 4) | (d[1] >> 4)
    vcid = (d[1] >> 1) & 0x07
    fhp = ((d[4] & 0x07) << 8) | d[5]
    print(f"TF hdr={d[:6].hex(' ')} TFVN={tfvn} SCID={scid} VCID={vcid} MC={d[2]} FHP={fhp}")

    off = DATA_OFFSET
    while off + 6 <= FRAME_SIZE:
        mid = (d[off] << 8) | d[off + 1]
        size = ((d[off + 4] << 8) | d[off + 5]) + 7
        print(f"  {'idle' if mid == 0x07FF else 'SPP '} MID=0x{mid:04X} size={size}")
        off += size
    if off != FRAME_SIZE:
        print(f"  BAD boundary: ended at {off}")
