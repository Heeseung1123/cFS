# FILE_DL — file downlink, upload and relay

`file_dl` is one cFS app that moves files by **byte offset**. It does three jobs:

| Job | What happens |
|---|---|
| **Downlink** | A file on board is sent to the ground. |
| **Upload** | The ground writes a file into `/cf/inbox`. |
| **Relay** | Upload, then the satellite sends the checked file down to another ground station. |

Every piece of data carries its own offset, so pieces can arrive in any order and only the missing ranges are sent again. A CRC-32 (the zlib one) checks the whole file at the end.

Commands come in through CI_LAB. All telemetry goes out through TO_LAB. There is no side channel.

```
          TC frames                                            TM frames
Ground ───────────▶ CI_LAB ──SB──▶ ┌──────────── FILE_DL app ────────────┐ ──SB──▶ TO_LAB ───────────▶ Ground
                                   │ file_dl_my.c   downlink engine      │  SEG_TLM          (A or B)
                                   │ file_ul_my.c   upload engine + relay│  UL_STATUS
                                   └──────────────┬──────────────────────┘
                                                  │ OS_read / OS_write
                                                  ▼
                                       /cf/...            /cf/inbox/<name>(.part)
```

## Messages

All multi-byte fields are big-endian.

### Commands — `FILE_DL_CMD_MID 0x18C0`

| CC | Name | Payload | Role |
|---|---|---|---|
| 0 | NOOP | – | Sends an event. |
| 1 | **DL_START** | `Filename[64]` | Downlink this file. |
| 2 | **DL_RESEND** | `Offset[4] Length[4]` | Send this byte range again (up to 64 ranges queued). |
| 3 | **DL_DONE** | `Crc32[4]` | The ground has the whole file. FILE_DL compares the CRC and closes the file. |
| 4 | **UL_START** | `Filename[64] FileSize[4] Crc32[4] Relay DestTag Spare[2]` | Start an upload into `/cf/inbox/` (max 4 MiB). Set `Relay=1` to downlink the file after it is checked. |
| 5 | **UL_DATA** | `Offset[4] DataLength[2] Spare[2] Data[≤960]` | One chunk. The offset must be a multiple of 960. |
| 6 | **UL_END** | – | All chunks were sent. Reply: UL_STATUS. |
| 7 | **UL_STATUS** | – | Ask for UL_STATUS again (if the last one was lost). |
| 8 | **UL_CANCEL** | – | Drop the upload and its `.part` file. |

`FILE_DL_WAKEUP_MID 0x18C1` is still accepted (it sends one downlink segment), but it is not needed any more.

### Telemetry

#### `FILE_DL_SEG_TLM_MID 0x08C0` — downlink segment

Each segment is a 16-byte header followed by its data.

```
Type | DestTag | DataLength[2] | Offset[4] | FileSize[4] | Crc32[4] | Data[≤1024]
```

| Type | Data | Meaning |
|---|---|---|
| 0 START | file name | A new file begins. |
| 1 DATA | file bytes at `Offset` | One piece of the file. |
| 2 END | – | The first pass is done. `Crc32` covers the whole file. |

`DestTag` says which ground station the file is for. `0` means any station, and is what DL_START uses. A relay uses the tag from UL_START.

#### `FILE_DL_UL_STATUS_TLM_MID 0x08C1` — upload status

```
State | Relay | DestTag | GapCount | FileSize[4] | ReceivedBytes[4] | ExpectedCrc[4] | FileCrc[4]
      | TotalGaps[4] | Filename[64] | Gaps[32] x (Offset[4], Length[4])
```

- `State` is one of: 0 IDLE, 1 RECEIVING, 2 VERIFYING, 3 COMPLETE, 4 FAILED.
- `Gaps` lists the first 32 missing byte ranges. `TotalGaps` may be larger than that.

## Flows

### Downlink

```
Ground                         FILE_DL
DL_START(name) ──────────────▶ open file
               ◀──── START ─── (every 100 ms:)
               ◀──── DATA ×N ─ one segment, CRC updated
               ◀──── END ───── first pass done, file stays open
(missing ranges?)
DL_RESEND(off,len) ──────────▶ queued, sent before anything else
               ◀──── DATA ──── 
DL_DONE(crc) ────────────────▶ compare CRC, close the file
```

### Upload

```
Ground                         FILE_DL (upload engine)
UL_START(name,size,crc) ─────▶ create /cf/inbox/<name>.part, clear the bitmap
UL_DATA(off,bytes) ×N ───────▶ write at the offset, set the chunk's bit (repeats ignored)
UL_END ──────────────────────▶ gaps left?
       ◀──── UL_STATUS ─────── RECEIVING + gap list   → the ground resends those chunks, then UL_END
       ◀──── UL_STATUS ─────── VERIFYING              → no gaps; read the file back, 16 chunks per 100 ms
       ◀──── UL_STATUS ─────── COMPLETE / FAILED      → CRC ok: rename .part → <name>
```

### Relay (store and forward)

```
Ground A                     Satellite                                    Ground B (DestTag)
UL_START(...,Relay=1,Tag=B) ▶ upload as above ─ COMPLETE ─▶ relay queue (4)
                             queue waits until no downlink file is open ─▶ DL engine, DestTag=B
                                                   START / DATA / END ──TO──▶ keeps segments with tag B
                             ◀──────────── DL_RESEND / DL_DONE (sent by whichever station has uplink)
```

To relay later instead, upload with `Relay=0`. When station B is in view, send it `DL_START("/cf/inbox/<name>")`.

## Timing

- The app runs one task. Every 100 ms, measured with `CFE_PSP_GetTime`, it runs a **tick**:
  - The downlink engine sends one segment.
  - The upload engine reads back up to 16 chunks for the CRC, then starts a queued relay.
- Between ticks the task waits on its SB pipe for commands. The wait is never longer than the time left until the next tick.
- The tick follows the clock, not a receive timeout. Because of that, an upload that sends a command every 20 ms does not stop the downlink.
- Downlink speed is about 10 segments/s (≈10 KiB/s). Upload measured about 31 KiB/s, with the ground sending one command every 20 ms.
- Native Linux trims SB pipes to 10 messages, so the ground must keep that 20 ms spacing. The command MID is subscribed with a message limit of 32.

## Files

| File | Content |
|---|---|
| `fsw/src/file_dl_my.c/.h` | App main loop, command dispatch, downlink engine, shared helpers (CRC-32, big-endian get/put, `FILE_DL_StartFile`, `FILE_DL_IsBusy`) |
| `fsw/src/file_ul_my.c/.h` | Upload engine: name and size checks, chunk bitmap, `.part` file, CRC read-back, rename, relay queue |
| `fsw/platform_inc/file_dl_msgids.h` | Message IDs |
| `*.bak` | Earlier versions |

## Limits

| Item | Value |
|---|---|
| Downlink segment | 1024 B (fits one 1766-byte TM data field) |
| Upload chunk | 960 B (one TC frame ≤ 1024 B) |
| Upload size | 4 MiB |
| Upload directory | `/cf/inbox/` (no `..`, no subdirectories) |
| Transfers at once | One downlink and one upload, at the same time. Up to 4 relays wait. |

- Nothing is authenticated: SDLS is off, so anyone with uplink access can write to `/cf/inbox`.
- A relay waits until the current downlink file is closed by DL_DONE or by the next DL_START.

## Tests (native)

All tests live in `file_dl_test/`.

```bash
python3 file_dl_test/file_xfer_test.py --mode relay --size 1048576 --ul-loss 0.05 --dl-loss 0.05
```

| Mode | Checks |
|---|---|
| `dl` | Downlink, RESEND, and DONE. |
| `ul` | Upload, gap rounds, the CRC read back on board, and the rename. |
| `relay` | Upload with Relay=1. The station with tag 2 rebuilds the relayed file. |
| `both` | Upload while another file is downlinked; the downlink must keep going. |
| `reject` | DATA without START, bad names, a file over 4 MiB, a wrong CRC (FAILED, no relay), and CANCEL. |

`file_dl_ground_test.py` is the original downlink-only test. It still passes.
