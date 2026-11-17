# FILE_DL — downlink, upload and relay of files

`file_dl` is one cFS app with three jobs. Each job has its own **command MID** and its own **function codes**.

| Job | Command MID | Telemetry MID | What happens |
|---|---|---|---|
| **Downlink** | `0x18C0` | `0x08C0` segments | A file on board is sent to the ground. |
| **Upload** | `0x18C2` | `0x08C1` status | The ground writes a file into `/cf/inbox/`. Nothing is sent down. |
| **Relay** | `0x18C3` | `0x08C2` status, `0x08C3` segments | The ground writes a file into `/cf/relay/`. After the CRC check, the satellite sends it down to the station named by `DestTag`. |

All three move data by **byte offset**. Every piece carries its own offset, so pieces may arrive in any order and only the missing ranges are sent again. A standard CRC-32 (IEEE, the one used by zip and PNG) checks the whole file at the end.

Commands come in through CI_LAB, and all telemetry goes out through TO_LAB.

```
                      0x18C0 DL    ─┐                                   ┌─ 0x08C0 DL segments
Ground ─TC─▶ CI_LAB ─SB─ 0x18C2 UL ─┼─▶ FILE_DL app ─────────────────────┼─ 0x08C1 UL status    ─SB─▶ TO_LAB ─TM─▶ Ground
                      0x18C3 RELAY ─┘   │ file_dl_my.c  downlink engine  ├─ 0x08C2 RELAY status             (A or B)
                                        │ file_ul_my.c  upload engine ×2 └─ 0x08C3 RELAY segments
                                        │               (upload, relay)
                                        ▼
                     /cf/...    /cf/inbox/<name>    /cf/relay/<name>
```

---

## 1. Message IDs

| MID | Dir | Macro (`file_dl_msgids.h`) | Used for |
|---|---|---|---|
| `0x18C0` | TC | `FILE_DL_CMD_MID` | Downlink commands (CC 0–3) |
| `0x18C1` | TC | `FILE_DL_WAKEUP_MID` | Optional: runs one downlink tick. The app has its own 100 ms clock, so nothing sends this now. |
| `0x18C2` | TC | `FILE_UL_CMD_MID` | Upload commands (CC 0–5) |
| `0x18C3` | TC | `FILE_RELAY_CMD_MID` | Relay commands (CC 0–5, same codes as upload) |
| `0x08C0` | TM | `FILE_DL_SEG_TLM_MID` | Downlink segments: START, DATA, END |
| `0x08C1` | TM | `FILE_UL_STATUS_TLM_MID` | Upload status: state, missing ranges, CRC |
| `0x08C2` | TM | `FILE_RELAY_STATUS_TLM_MID` | Relay upload status (same format as `0x08C1`) |
| `0x08C3` | TM | `FILE_RELAY_SEG_TLM_MID` | Relay downlink segments (same format as `0x08C0`) |

---

## 2. Command packets

### 2.1 Common header (8 bytes)

Every command starts with the cFS v1 command header. All multi-byte fields are **big-endian**.

| Byte | Field | Value |
|---|---|---|
| 0–1 | Stream ID | The command MID, e.g. `0x18 0xC0` |
| 2–3 | Sequence | `0xC0 0x00` (unsegmented) |
| 4–5 | Length | Total packet length − 7 |
| 6 | **Function code (CC)** | See the tables below |
| 7 | Checksum | XOR of all bytes, starting from `0xFF`. Neither CI_LAB nor FILE_DL checks it. |

The payload starts at **byte 8**. FILE_DL checks the total length of each command exactly. A wrong length gives the event `bad command cc N, size M`.

### 2.2 Downlink — MID `0x18C0`

| CC | Macro | Name | Payload (bytes from 8) | Total | Action | Reply |
|---|---|---|---|---|---|---|
| **0** | `FILE_DL_NOOP_CC` | NOOP | – | 8 | – | event `FILE_DL: NOOP` |
| **1** | `FILE_DL_START_CC` | START | `8–71 Filename[64]` (NUL-padded) | 72 | Opens the file and sends the START segment. Then one DATA every 100 ms, then END. | segments on `0x08C0` |
| **2** | `FILE_DL_RESEND_CC` | RESEND | `8–11 Offset[4]`, `12–15 Length[4]` | 16 | Queues the byte range (up to 64 ranges). It is sent before any new data. | DATA segments |
| **3** | `FILE_DL_DONE_CC` | DONE | `8–11 Crc32[4]` (of the rebuilt file) | 12 | Compares the CRC with the file's, then closes the file. | event `confirmed by ground` or `crc32 mismatch` |

RESEND and DONE also work for a relayed file, because the downlink engine sends relays too.

### 2.3 Upload — MID `0x18C2` / Relay — MID `0x18C3`

Both MIDs use the same function codes. Only START differs: relay START has a `DestTag`.

| CC | Macro | Name | Payload (bytes from 8) | Total | Action | Reply |
|---|---|---|---|---|---|---|
| **0** | `FILE_UL_NOOP_CC` | NOOP | – | 8 | – | event `FILE_UL: NOOP` / `FILE_RELAY: NOOP` |
| **1** | `FILE_UL_START_CC` | START (upload) | `8–71 Filename[64]`, `72–75 FileSize[4]`, `76–79 Crc32[4]` | **80** | Checks the name and size, creates `<name>.part`, clears the bitmap. State becomes RECEIVING. | event `receiving ...` |
| **1** | `FILE_UL_START_CC` | START (relay) | as upload, plus `80 DestTag`, `81–83 Spare[3]` | **84** | The same, and remembers `DestTag` for the relay downlink. | event `receiving ..., tag N` |
| **2** | `FILE_UL_DATA_CC` | DATA | `8–11 Offset[4]`, `12–13 DataLength[2]`, `14–15 Spare[2]`, `16– Data[DataLength]` | 16 + n (n ≤ 960) | Writes the chunk at its offset and sets its bit. A chunk that is already there is ignored. | – (an event only if rejected) |
| **3** | `FILE_UL_END_CC` | END | – | 8 | If chunks are missing: reports them. If not: starts the CRC check (VERIFYING). | status |
| **4** | `FILE_UL_STATUS_CC` | STATUS | – | 8 | Sends the status again, e.g. when the last one was lost. | status |
| **5** | `FILE_UL_CANCEL_CC` | CANCEL | – | 8 | Closes and removes `<name>.part`. State becomes IDLE. | event `cancelled`, then status |

**Rules checked by the satellite**
- The file name must be directly under the MID's own directory: `/cf/inbox/` for upload, `/cf/relay/` for relay. No `..` and no subdirectories.
- The size must be between 1 byte and 4 MiB (`FILE_UL_MAX_FILE_SIZE`).
- DATA:
  - `Offset` must be a multiple of 960 (`FILE_UL_CHUNK_SIZE`).
  - `DataLength` must be 960, except for the last chunk, which holds `FileSize − Offset` bytes.
  - The packet length must be `16 + DataLength`.
- A new START is refused while a transfer on the same MID is RECEIVING or VERIFYING.

---

## 3. Telemetry packets

Each telemetry packet starts with a 16-byte header: 6 bytes CCSDS primary header, 6 bytes time, 4 bytes spare. The payload starts at **byte 16**.

### 3.1 Segment — `0x08C0` (downlink) and `0x08C3` (relay)

| Byte | Field | Meaning |
|---|---|---|
| 16 | `Type` | 0 START, 1 DATA, 2 END |
| 17 | `DestTag` | 0 for a downlink. For a relay, the tag from relay START. |
| 18–19 | `DataLength` | Bytes in `Data` |
| 20–23 | `Offset` | DATA: byte offset in the file. Otherwise 0, or the file size for END. |
| 24–27 | `FileSize` | File size. Every segment has it. |
| 28–31 | `Crc32` | END: CRC-32 of the whole file. Otherwise: CRC so far. |
| 32– | `Data` | START: the file name. DATA: up to 1024 file bytes. END: empty. |

A full DATA segment is 16 + 16 + 1024 = 1056 bytes, which fits one TM data field of 1766 bytes.

### 3.2 Status — `0x08C1` (upload) and `0x08C2` (relay), 360 bytes

| Byte | Field | Meaning |
|---|---|---|
| 16 | `State` | 0 IDLE, 1 RECEIVING, 2 VERIFYING, 3 COMPLETE, 4 FAILED |
| 17 | `Relay` | 0 on `0x08C1`, 1 on `0x08C2` |
| 18 | `DestTag` | Tag from relay START (0 for upload) |
| 19 | `GapCount` | Gaps listed in this packet (≤ 32) |
| 20–23 | `FileSize` | From START |
| 24–27 | `ReceivedBytes` | Bytes received so far |
| 28–31 | `ExpectedCrc` | CRC-32 from START |
| 32–35 | `FileCrc` | CRC-32 of the stored file, read back (valid after VERIFYING) |
| 36–39 | `TotalGaps` | All missing ranges. This may be more than `GapCount`. |
| 40–103 | `Filename[64]` | File name |
| 104–359 | `Gaps[32]` | `Offset[4] Length[4]` for each missing byte range |

**When it is sent:** after END, after STATUS, after CANCEL, when the CRC check ends (COMPLETE or FAILED), and after a write error.

| State | Meaning | What the ground does |
|---|---|---|
| 0 IDLE | Nothing in progress | Sends START |
| 1 RECEIVING | Chunks are missing | Sends the chunks in `Gaps`, then END again |
| 2 VERIFYING | All chunks are here; the CRC check is running | Waits. The next status comes by itself. |
| 3 COMPLETE | The CRC matched and `.part` was renamed to `<name>` | Upload: done. Relay: waits for segments on `0x08C3`. |
| 4 FAILED | The CRC did not match, or a file error happened. `.part` is kept. | Sends CANCEL, or sends START again |

### 3.3 Event IDs

| EID | Text starts with | When |
|---|---|---|
| 1 | `FILE_DL Initialized` | App start |
| 2 | `FILE_DL: ... rejected / bad command / still being sent` | Wrong downlink command |
| 3 | `FILE_DL: sending <name>, N bytes` | Downlink starts |
| 4 | `FILE_DL: cannot open / read error` | File error |
| 5 | `FILE_DL: <name> sent, crc32 ...` | First pass done (END sent) |
| 6 | `FILE_DL: NOOP` | NOOP |
| 7 / 8 | `confirmed by ground` / `crc32 mismatch` | DONE |
| 9 | `FILE_UL/FILE_RELAY: receiving ...`, `NOOP` | START, NOOP |
| 10 | `... rejected`, `bad command`, `queue full` | Wrong upload or relay command |
| 11 | `cannot create / write error / read error / cannot rename` | File error |
| 12 | `<name> received and verified` | COMPLETE |
| 13 | `crc32 mismatch` | FAILED |
| 14 | `FILE_RELAY: sending <name> to tag N` | Relay downlink starts |
| 15 | `<name> cancelled` | CANCEL |

---

## 4. How it works, end to end

### 4.1 Start-up

At start-up, `FILE_DL_AppMain` does the following once:
1. Creates the pipe `FILE_DL_PIPE` (depth 32).
2. Subscribes to `0x18C0`, `0x18C2` and `0x18C3`, each with a message limit of 32, and to `0x18C1`.
3. Calls `FILE_UL_Init`, which prepares two instances of the upload engine (`FILE_UL_Ctx_t`):
   - upload: `/cf/inbox`, status on `0x08C1`
   - relay: `/cf/relay`, status on `0x08C2`, relay flag on

   It also creates both directories.
4. Sends the event `FILE_DL Initialized`.

### 4.2 Main loop: one task, a 100 ms tick and commands in between

```
loop:
  now = CFE_PSP_GetTime()
  if now - lastTick >= 100 ms:            ── tick ──
      FILE_DL_Wakeup()                    downlink: one segment
      FILE_UL_Tick()                      upload + relay: CRC check steps, start a queued relay
      lastTick = now
  CFE_SB_ReceiveBuffer(pipe, time left until the next tick)
  dispatch by MID:
      0x18C0 → FILE_DL_ProcessCommand     (CC 0–3)
      0x18C2 → FILE_UL_ProcessCommand     → FILE_UL_Dispatch(upload instance)
      0x18C3 → FILE_RELAY_ProcessCommand  → FILE_UL_Dispatch(relay instance)
      0x18C1 → FILE_DL_Wakeup
```

- The tick follows the clock, not a receive timeout. So a stream of upload DATA, one command every 20 ms, does **not** stop the downlink.
- Commands are handled at once, between ticks. Work that takes time runs a little per tick: sending, and reading the file back for the CRC.

### 4.3 Path of a command and of a segment

```
Ground ──UDP TC frame──▶ CI_LAB: OS_SocketRecvFrom → TC_SDLP_myExtractPacket → CFE_SB_TransmitBuffer
                                                                                     │ SB (MID 0x18Cx)
                                                                                     ▼
                                                                            FILE_DL_PIPE → FILE_DL
FILE_DL: FILE_DL_NewSegment / FILE_UL_SendStatus → CFE_SB_TransmitBuffer / TransmitMsg │ SB (MID 0x08Cx)
                                                                                     ▼
TO_LAB (every 100 ms): ReceiveBuffer(POLL) → TM_SDLP_myAddPacketToFrame → TM_SDLP_myFillAndSendFrame
                                                                                     │ UDP TM frame (1786 B)
                                                                                     ▼
Ground: deframe (First Header Pointer) → packets by MID
```

### 4.4 Downlink (`0x18C0`)

| Step | Ground sends | Satellite (function) | Comes down |
|---|---|---|---|
| 1 | `0x18C0` CC 1 START(`/cf/x.bin`) | `FILE_DL_Start` → `FILE_DL_StartFile(name, tag 0, 0x08C0)`<br>→ `FILE_DL_Prepare`: refuses while a first pass is running; closes the previous finished file<br>→ `FILE_DL_OpenAndAnnounce`: `OS_stat`, `OS_OpenCreate` read-only, Active = Open = true | START segment (name, size) |
| 2 | – | Each tick, `FILE_DL_Wakeup` → `FILE_DL_SendData(Offset, …, firstPass)`<br>`OS_lseek` + `OS_read` up to 1024 B → `FILE_DL_NewSegment` (DestTag, SegMid) → `FILE_DL_Send`. The CRC is updated as it goes. | DATA segment, one per 100 ms |
| 3 | – | After the last byte: an END segment with the full CRC. Active = false. **The file stays open** for RESEND. | END segment |
| 4 | Ground rebuilds by `Offset` and finds the gaps.<br>`0x18C0` CC 2 RESEND(off, len) for each gap | `FILE_DL_Resend` queues the range (up to 64). Each tick, `FILE_DL_Wakeup` sends the queued ranges first. | DATA segments again |
| 5 | When nothing is missing: `0x18C0` CC 3 DONE(crc) | `FILE_DL_Done` compares the CRC, sends an event, then `FILE_DL_Close` (Open = false) | – |

### 4.5 Upload (`0x18C2`)

| Step | Ground sends | Satellite (function) | Comes down |
|---|---|---|---|
| 1 | `0x18C2` CC 1 START(`/cf/inbox/x.bin`, size, crc) | `FILE_UL_Dispatch` → `FILE_UL_Start`<br>`FILE_UL_IsValidName`, size check → `OS_OpenCreate(x.bin.part, CREATE\|TRUNCATE)`<br>Clears the bitmap, ChunkCount = ⌈size / 960⌉, State = RECEIVING | event |
| 2 | `0x18C2` CC 2 DATA(off, bytes), one every 20 ms | `FILE_UL_WriteChunk`: checks the chunk, ignores a repeat, then `OS_lseek` + `OS_write`, sets bit `off/960`, ReceivedChunks++ | – |
| 3 | `0x18C2` CC 3 END | `FILE_UL_End`: if ReceivedChunks == ChunkCount, State = VERIFYING. Then `FILE_UL_SendStatus` turns each run of 0 bits into one entry in `Gaps`. | status `0x08C1` |
| 4 | If RECEIVING: the DATA chunks listed in `Gaps`, then END again | Same as steps 2–3 | status |
| 5 | – | Each tick, `FILE_UL_Tick` → `FILE_UL_Verify` reads back 16 chunks and updates the CRC | – |
| 6 | – | After the last chunk, `FILE_UL_Finish`:<br>if the CRC matches: `OS_remove(x.bin)`, `OS_rename(x.bin.part → x.bin)`, COMPLETE<br>if not: FAILED | status COMPLETE or FAILED |

Nothing is sent down for an upload. To get the file back, send a downlink START.

### 4.6 Relay (`0x18C3`): uplink, then downlink

The relay START puts the relay instance in RECEIVING, so every following `0x18C3` DATA is treated as uplink. When the file is complete and checked, the satellite sends it down without another command.

| Step | Ground sends | Satellite (function) | Comes down |
|---|---|---|---|
| 1 | `0x18C3` CC 1 START(`/cf/relay/x.bin`, size, crc, **DestTag=2**) | `FILE_UL_Dispatch(relay instance)` → `FILE_UL_Start` (as for upload, plus DestTag) | event |
| 2–6 | `0x18C3` DATA ×N, END, and gap rounds | Same as upload steps 2–6, but status on `0x08C2` and files in `/cf/relay/` | status `0x08C2` |
| 7 | – | `FILE_UL_Finish` on COMPLETE: puts (name, tag) into the relay queue (4 entries) | status COMPLETE |
| 8 | – | In the same tick, `FILE_UL_RelayNext`: if `FILE_DL_IsBusy()` is false (no downlink file open),<br>`FILE_DL_StartFile(name, tag 2, 0x08C3)` | START segment on **`0x08C3`**, DestTag=2 |
| 9 | – | Downlink steps 2–3, with every segment on `0x08C3` carrying DestTag=2 | DATA ×N, END on `0x08C3` |
| 10 | Station 2 keeps only `0x08C3` segments with tag 2 and finds its gaps. Any station with uplink sends them on **`0x18C0`**: CC 2 RESEND, then CC 3 DONE. | Downlink steps 4–5 | DATA again |
| 11 | – | `FILE_DL_Close` makes Open false, so on the next tick `FILE_UL_RelayNext` can start the next queued relay | – |

If a downlink is still open (no DONE yet), the relay waits in the queue and starts on the first tick after it closes.

### 4.7 What can run at the same time

| Combination | Possible? |
|---|---|
| Upload and relay uplink | Yes. Each has its own instance and bitmap. |
| Uplink (upload or relay) while a downlink is running | Yes. The downlink keeps its 10 segments/s. |
| Two downlinks (downlink START and a relay downlink) | No, one at a time. A relay waits in the queue. A downlink START during a first pass is refused. |

---

## 5. Timing and limits

| Item | Value |
|---|---|
| Tick | 100 ms (`FILE_DL_SEND_PERIOD_MSEC`) |
| Downlink rate | 1 segment of 1024 B per tick, about 10 KiB/s |
| Upload chunk | 960 B (`FILE_UL_CHUNK_SIZE`). One DATA command (976 B) fits one 1024-byte TC frame. |
| Upload rate (measured) | About 31 KiB/s, with one DATA every 20 ms |
| CRC check | 16 chunks per tick, about 150 KiB/s |
| File size | 4 MiB (upload and relay) |
| RESEND queue | 64 ranges |
| Relay queue | 4 files |
| Status gap list | 32 ranges per packet |

- On native Linux, an SB pipe holds 10 messages at most, so the ground must leave 20 ms between commands.
- Nothing is authenticated: SDLS is off, so anyone with uplink access can write files.
- On RTEMS, it has not been checked whether `/cf` (`/nonvol`) allows writing and renaming.

## 6. Files

| File | Content |
|---|---|
| `fsw/platform_inc/file_dl_msgids.h` | Message IDs |
| `fsw/src/file_dl_my.c/.h` | `FILE_DL_AppMain` (main loop, dispatch by MID), downlink engine, downlink commands, shared helpers (`FILE_DL_Crc32`, big-endian get/put, `FILE_DL_StartFile`, `FILE_DL_IsBusy`) |
| `fsw/src/file_ul_my.c/.h` | Upload engine with two instances (upload, relay): commands, chunk bitmap, `.part`, CRC read-back, rename, relay queue |
| `*.bak` | Earlier versions |

## 7. Tests (native)

All tests live in `file_dl_test/`.

```bash
python3 file_dl_test/file_xfer_test.py --mode relay --size 1048576 --ul-loss 0.05 --dl-loss 0.05
```

| Mode | Checks |
|---|---|
| `dl` | Downlink START, RESEND and DONE |
| `ul` | Upload, gap rounds, the CRC read back, the rename, and that nothing is sent down |
| `relay` | Relay uplink, then station 2 rebuilds the file from the `0x08C3` segments |
| `both` | Upload while another file is downlinked. The downlink must keep going. |
| `reject` | DATA without START, wrong directories, a file over 4 MiB, the wrong START size, a wrong CRC (FAILED, no relay), and CANCEL |

`--ul-loss` drops DATA commands on purpose, and `--dl-loss` drops TM frames. `file_dl_ground_test.py` is the original downlink-only test.
