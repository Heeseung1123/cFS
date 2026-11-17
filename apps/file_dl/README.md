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
- Section 5 explains how this loop works together with the CI_LAB and TO_LAB loops.

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

**Sequence diagram of the downlink**

```mermaid
sequenceDiagram
    autonumber
    participant G as Ground
    participant CI as CI_LAB
    participant DL as FILE_DL downlink engine<br/>(file_dl_my.c)
    participant FS as /cf
    participant TO as TO_LAB

    Note over G,TO: Start (MID 0x18C0)
    G->>CI: TC 0x18C0 CC1 START(/cf/x.bin)
    CI->>DL: SB 0x18C0
    DL->>DL: FILE_DL_Prepare: refuse while a first pass runs, close the previous finished file
    DL->>FS: OS_stat + OS_OpenCreate(read only)
    DL->>DL: Offset = 0, CRC = 0, Active = Open = true
    DL->>TO: segment 0x08C0 START (name, size, DestTag 0)
    TO->>G: TM frame

    Note over G,TO: First pass (paced by the 100 ms tick)
    loop every 100 ms tick (FILE_DL_Wakeup), until every byte is sent
        DL->>FS: OS_lseek(Offset) + OS_read up to 1024 B
        DL->>DL: update CRC-32, Offset += bytes
        DL->>TO: segment 0x08C0 DATA (offset, bytes)
        TO->>G: TM frame (full frames at once, partly filled every 500 ms)
        G->>G: store DATA at its offset
    end
    DL->>TO: segment 0x08C0 END (size, CRC of the whole file)
    TO->>G: TM frame
    DL->>DL: Active = false (the file stays open for RESEND)

    Note over G,TO: Repair (after END and a few quiet seconds)
    loop while the ground finds missing ranges
        G->>CI: TC 0x18C0 CC2 RESEND(offset, length), 20 ms apart, up to 64
        CI->>DL: SB 0x18C0
        DL->>DL: FILE_DL_Resend: queue the range
        loop every 100 ms tick, queued ranges first
            DL->>FS: read 1024 B of the range
            DL->>TO: segment 0x08C0 DATA
            TO->>G: TM frame
        end
    end

    Note over G,TO: Close
    G->>G: whole file rebuilt, CRC equals END CRC
    G->>CI: TC 0x18C0 CC3 DONE(crc)
    CI->>DL: SB 0x18C0
    alt CRC matches
        DL->>DL: event "confirmed by ground"
    else CRC differs
        DL->>DL: event "crc32 mismatch"
    end
    DL->>FS: OS_close (Open = false)
    Note over DL: the next START (or a queued relay) can now begin
```

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

**Sequence diagram of the upload**

```mermaid
sequenceDiagram
    autonumber
    participant G as Ground
    participant CI as CI_LAB
    participant UL as FILE_DL upload engine<br/>(file_ul_my.c)
    participant FS as /cf/inbox
    participant TO as TO_LAB

    Note over G,TO: Start (MID 0x18C2)
    G->>CI: TC 0x18C2 CC1 START(/cf/inbox/x.bin, size, crc)
    CI->>UL: SB 0x18C2
    UL->>UL: FILE_UL_Start: check name (under /cf/inbox/) and size (1 B to 4 MiB)
    UL->>FS: OS_OpenCreate x.bin.part (CREATE, TRUNCATE)
    UL->>UL: clear bitmap, ChunkCount = ceil(size / 960), RECEIVING

    Note over G,TO: Data (one chunk per command)
    loop every 20 ms, offset = 0, 960, 1920, ...
        G->>CI: TC 0x18C2 CC2 DATA(offset, 960 B)
        CI->>UL: SB 0x18C2 (about 5 per 100 ms CI poll)
        alt bit already set
            UL->>UL: ignore the repeat
        else new chunk
            UL->>FS: OS_lseek(offset) + OS_write
            UL->>UL: set bit offset/960, ReceivedChunks++
        end
    end

    Note over G,TO: End and gap rounds
    G->>CI: TC 0x18C2 CC3 END
    CI->>UL: SB 0x18C2
    loop while chunks are missing
        UL->>TO: status 0x08C1 RECEIVING + Gaps[] (up to 32 ranges)
        TO->>G: TM frame
        G->>CI: TC 0x18C2 DATA for each gap
        CI->>UL: SB 0x18C2
        UL->>FS: write the missing chunks
        G->>CI: TC 0x18C2 CC3 END
        CI->>UL: SB 0x18C2
    end
    UL->>UL: ReceivedChunks == ChunkCount, VERIFYING
    UL->>TO: status 0x08C1 VERIFYING
    TO->>G: TM frame

    Note over G,TO: Check (paced by the 100 ms tick)
    loop every 100 ms tick (FILE_UL_Tick, FILE_UL_Verify)
        UL->>FS: read back 16 chunks
        UL->>UL: update CRC-32
    end
    alt CRC matches START crc
        UL->>FS: OS_remove(x.bin), OS_rename(x.bin.part to x.bin)
        UL->>TO: status 0x08C1 COMPLETE
    else CRC differs or file error
        UL->>TO: status 0x08C1 FAILED (.part kept)
    end
    TO->>G: TM frame

    opt status lost
        G->>CI: TC 0x18C2 CC4 STATUS
        CI->>UL: SB 0x18C2
        UL->>TO: status 0x08C1 (current state)
        TO->>G: TM frame
    end
    Note over G,UL: Nothing is sent down for an upload. CC5 CANCEL drops it at any time.
```

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

**Sequence diagram of the relay**

```mermaid
sequenceDiagram
    autonumber
    participant A as Ground A (uplink)
    participant CI as CI_LAB
    participant RL as FILE_DL relay engine<br/>(file_ul_my.c)
    participant DL as FILE_DL downlink engine<br/>(file_dl_my.c)
    participant TO as TO_LAB
    participant B as Ground B (DestTag 2)

    Note over A,B: Phase 1 - relay uplink (MID 0x18C3)
    A->>CI: TC 0x18C3 CC1 START(/cf/relay/x.bin, size, crc, DestTag=2)
    CI->>RL: SB 0x18C3
    RL->>RL: FILE_UL_Start: check name/size, create x.bin.part, clear bitmap, RECEIVING

    loop every 20 ms, one chunk
        A->>CI: TC 0x18C3 CC2 DATA(offset, 960 B)
        CI->>RL: SB 0x18C3 (about 5 per 100 ms CI poll)
        RL->>RL: FILE_UL_WriteChunk: lseek+write, set bit
    end

    A->>CI: TC 0x18C3 CC3 END
    CI->>RL: SB 0x18C3
    alt chunks missing
        RL->>TO: status 0x08C2 RECEIVING + Gaps[]
        TO->>A: TM frame
        A->>CI: DATA for the gaps, then END again
        CI->>RL: SB 0x18C3
    end
    RL->>TO: status 0x08C2 VERIFYING
    TO->>A: TM frame

    loop every 100 ms tick (FILE_UL_Tick)
        RL->>RL: FILE_UL_Verify: read back 16 chunks, update CRC
    end
    RL->>RL: FILE_UL_Finish: CRC ok, rename .part to x.bin, queue (x.bin, tag 2)
    RL->>TO: status 0x08C2 COMPLETE
    TO->>A: TM frame

    Note over A,B: Phase 2 - relay downlink (segments on 0x08C3)
    RL->>DL: FILE_UL_RelayNext (same tick, if FILE_DL_IsBusy() is false)<br/>FILE_DL_StartFile(x.bin, tag 2, 0x08C3)
    DL->>TO: segment 0x08C3 START (name, size, tag 2)
    TO->>B: TM frame

    loop every 100 ms tick (FILE_DL_Wakeup)
        DL->>TO: segment 0x08C3 DATA (offset, 1024 B, tag 2)
        TO->>B: TM frame (B keeps 0x08C3 with tag 2)
    end
    DL->>TO: segment 0x08C3 END (CRC of the whole file)
    TO->>B: TM frame
    Note over DL: file stays open for RESEND

    Note over A,B: Phase 3 - repair and close (downlink MID 0x18C0)
    B-->>A: missing ranges (ground network)
    opt ranges missing at B
        A->>CI: TC 0x18C0 CC2 RESEND(offset, length)
        CI->>DL: SB 0x18C0
        DL->>TO: segment 0x08C3 DATA (sent before anything else)
        TO->>B: TM frame
    end
    B-->>A: file complete, CRC
    A->>CI: TC 0x18C0 CC3 DONE(crc)
    CI->>DL: SB 0x18C0
    DL->>DL: FILE_DL_Done: compare CRC, close file (Open=false)
    Note over RL,DL: next tick: FILE_UL_RelayNext may start the next queued relay
```

### 4.7 What can run at the same time

| Combination | Possible? |
|---|---|
| Upload and relay uplink | Yes. Each has its own instance and bitmap. |
| Uplink (upload or relay) while a downlink is running | Yes. The downlink keeps its 10 segments/s. |
| Two downlinks (downlink START and a relay downlink) | No, one at a time. A relay waits in the queue. A downlink START during a first pass is refused. |

---

## 5. Tick and loops: how timing links uplink and downlink

### 5.1 What a tick is, and what waiting in ReceiveBuffer means

**A tick is the name for "the fixed work done once every 100 ms".** It is not an OS feature, just one variable and one comparison:

```c
if (now - lastTick >= 100 ms)   /* 100 ms since the fixed work was last done? */
{
    FILE_DL_Wakeup();           /* send one segment */
    FILE_UL_Tick();             /* a bit of CRC checking, start a relay */
    lastTick = now;             /* remember that it was just done */
}
```

FILE_DL has two kinds of work:

| Kind | When it comes | Example |
|---|---|---|
| **Periodic work** | Must go on although nobody asks | Send a file down, 1 KiB per 100 ms |
| **Commands** | Whenever the ground sends one, at any time | START, DATA, RESEND, DONE |

The tick is when the periodic work runs.

**The problem:** FILE_DL is one task, so it does one thing at a time. When it has nothing to do, it must **sleep**, so that CI, TO and the other apps get the CPU. So the task needs this:

> "Wake me as soon as a command arrives. If none arrives, wake me when the next periodic work is due."

**`CFE_SB_ReceiveBuffer(pipe, wait)` is a sleep with two wake-up conditions:**

```
CFE_SB_ReceiveBuffer(&pBuf, pipe, 70)
   ├─ a command enters the pipe within 70 ms → wakes at that moment (CFE_SUCCESS, command returned)
   └─ nothing arrives for 70 ms              → wakes after 70 ms   (CFE_SB_TIME_OUT, nothing returned)
```

Passing **the time left until the next tick** as `wait` makes the task wake at whichever comes first: a command, or the next tick. It wakes only when needed and sleeps the rest of the time.

**Example:** commands arrive at 30 ms and 60 ms.

```
time   what happens                                         next wait
  0    tick (one segment sent), lastTick = 0                100 - 0  = 100 ms
       → sleep (up to 100 ms)
 30    command arrives → wakes at once, handles it
       clock: 30 ms since the tick                          100 - 30 = 70 ms
       → sleep (up to 70 ms)
 60    command arrives → wakes at once, handles it
       clock: 60 ms since the tick                          100 - 60 = 40 ms
       → sleep (up to 40 ms)
100    40 ms pass with no command → wakes on the timeout
       clock: 100 ms → tick, lastTick = 100                 100 ms
```

- Commands are handled **the moment they arrive** (30, 60).
- The tick runs **every 100 ms exactly** (0, 100, 200 …), however many commands come.
- In between, the task sleeps and uses no CPU.

**Why other ways do not work**

| Way | Result |
|---|---|
| `ReceiveBuffer(pipe, PEND_FOREVER)` | With no command the task sleeps forever, so **the file is never sent**. |
| `OS_TaskDelay(100)`, then poll the pipe (as TO_LAB does) | A command waits up to 100 ms, and commands pile up in the pipe and may overflow it. |
| `ReceiveBuffer(pipe, 100)` always, with the tick only on a timeout (**earlier version**) | Each command restarts the 100 ms. With a command every 20 ms the wait never times out, so **the tick never runs and the downlink stops**. |
| Never sleep, keep reading the clock (busy loop) | Uses 100% CPU, so lower-priority apps never run. |
| **`ReceiveBuffer(pipe, time left)` (now)** | Commands at once, the tick every 100 ms, sleep the rest of the time. |

The earlier and current versions differ only in the wait they pass:

```
earlier: always 100 ms          → each command sets the alarm 100 ms later again
now:     100 ms - time passed   → the alarm stays fixed at the next tick
```

**Analogy:** someone must post a letter at fixed times (the tick) and open the door when the bell rings (a command).
- **Now:** they set the alarm for the next posting time and sleep. When the bell wakes them, they answer the door, leave the alarm as it is, and go back to sleep.
- **Earlier:** each time the bell woke them, they set the alarm to "100 ms from now". With visitors coming all the time, the alarm never went off and no letter was posted.

### 5.2 Three loops and two queues

Between the ground and FILE_DL there are **three loops**, each a separate task with its own clock. **SB pipes** (queues) connect them.

```
Ground ─UDP─▶ [CI_LAB loop] ─SB─▶ FILE_DL_PIPE ─▶ [FILE_DL loop] ─SB─▶ TO_LAB_TLM_PIPE ─▶ [TO_LAB loop] ─UDP─▶ Ground
              socket every 100 ms     (queue)     100 ms tick + commands     (queue)       empty pipe every 100 ms
```

| Loop | Wakes up when | Does | Setting |
|---|---|---|---|
| CI_LAB | A command arrives for CI, or 100 ms pass | Reads up to 11 TC frames from the UDP socket and sends each packet to the SB | `CI_LAB_UPLINK_POLL_MSEC` 100 |
| **FILE_DL** | A command arrives, or the **next tick is due** | Every tick: one downlink segment, CRC check steps, start of a relay. Commands are handled as they arrive. | `FILE_DL_SEND_PERIOD_MSEC` 100 |
| TO_LAB | Every 100 ms (`OS_TaskDelay`) | Empties its telemetry pipe into TM frames and sends them | `TO_TASK_MSEC` 100 |

- The loops never wait for each other.
- What one loop hands to the next waits in an SB pipe.
- The OS scheduler decides which task runs when. On native Linux, task priorities are ignored unless the app runs as root.

### 5.3 One pass of the FILE_DL loop

```c
while (CFE_ES_RunLoop(&runStatus))
{
    (1) now = CFE_PSP_GetTime();
        waitMsec = 100 - (now - lastTick);          /* time left until the next tick */

    (2) if (waitMsec <= 0)                          /* 100 ms have passed: tick */
        {
            FILE_DL_Wakeup();   /* downlink: one segment */
            FILE_UL_Tick();     /* upload + relay: CRC check steps, start a queued relay */
            lastTick = now;
            waitMsec = 100;
        }

    (3) CFE_SB_ReceiveBuffer(&pBuf, pipe, waitMsec); /* wakes at once on a command, else after waitMsec */

    (4) dispatch by MID: 0x18C0 downlink, 0x18C2 upload, 0x18C3 relay, 0x18C1 one extra tick
}
```

The loop does two kinds of work:

| Kind | When it runs | Examples |
|---|---|---|
| **Tick work** (2) | Once per 100 ms, by the clock | Send a segment; read a stored file back for its CRC; start a relay |
| **Command work** (4) | As soon as a command arrives | Open a file, write a chunk, queue a RESEND, send a status |

**Why commands never push the tick back**
- After each command, the loop returns to (1) and reads the clock again. If 100 ms have passed, the tick runs before the next command.
- The wait in (3) is always the time *left*, not a fresh 100 ms.

```
time (ms)   0      20     40     60     80     100    120
command     ▼      ▼      ▼      ▼      ▼      ▼      ▼        (upload DATA every 20 ms)
wait        100    80     60     40     20     –      80
tick        ●                                  ●               (every 100 ms exactly)
```

- An earlier version ran the tick only when `ReceiveBuffer` timed out, that is, after 100 ms with *no* command at all. A stream of upload DATA then never let the wait time out, and the downlink stopped.
- Now a tick can be late only by the time one command takes to handle, which is a few milliseconds.

### 5.4 What runs in a tick

**`FILE_DL_Wakeup` (downlink)** sends `FILE_DL_SEGMENTS_PER_TICK` (1) segment per tick. It picks the segment in this order:

1. A queued RESEND range: the next 1024 bytes of it.
2. Otherwise, if the first pass is already done: nothing.
3. Otherwise, the next 1024 bytes of the file, and the CRC is updated.
4. When every byte has been sent: the END segment, and the first pass ends.

So **the tick sets the downlink rate**: 1024 B per 100 ms, about 10 KiB/s.

**`FILE_UL_Tick` (upload and relay)**
1. `FILE_UL_Verify(upload)`: if the upload is VERIFYING, it reads 16 chunks (about 15 KB) back from storage and adds them to the CRC.
2. `FILE_UL_Verify(relay)`: the same for the relay file.
3. `FILE_UL_RelayNext`: if a verified relay file is waiting and no downlink file is open, the relay downlink starts.

- Chunks are **written when DATA arrives** (command work), not in the tick. The tick only handles the check after reception ends.
- The CRC check is split across ticks. Reading 4 MiB at once would block the loop: the pipe could overflow and the downlink tick would slip.

### 5.5 Link to the downlink

```
FILE_DL tick ──1 segment──▶ TO_LAB_TLM_PIPE ──(TO empties it every 100 ms)──▶ TM frame 1786 B ──UDP──▶ Ground
```

1. Each tick, `FILE_DL_Send` puts one segment (up to 1056 B) into TO_LAB's telemetry pipe.
2. Every 100 ms TO_LAB takes packets until the pipe is empty and calls `TM_SDLP_myAddPacketToFrame` for each one.
   - A frame whose data field (1766 B) is full is sent **at once**.
   - The rest of the packet goes to the overflow queue and starts the next frame. This is packet segmentation, marked by the First Header Pointer.
3. A partly filled frame is padded with an idle packet and sent every `TO_LAB_FLUSH_CYCLES` (5) cycles, which is every 500 ms.

**Why both loops use 100 ms**
- FILE_DL adds one segment per 100 ms, and TO_LAB empties its pipe once per 100 ms. So usually at most one file segment waits in TO's pipe.
- Native Linux trims an SB pipe to 10 messages. An earlier version sent 8 segments at once on a 1 Hz wakeup; together with HK packets, that made `Pipe Overflow`.

**Delay**
- The two loops have the same period but are not in phase.
- A segment waits up to 100 ms for TO_LAB to wake up. In a partly filled frame it may wait up to 500 ms more.
- Two segments (2112 B) are more than one frame, so most frames go out within one or two ticks:

```
FILE_DL tick:  ●seg0           ●seg1           ●seg2 ...
TO_LAB wake:       ◆               ◆               ◆
                   seg0 into frame  seg1 added → over 1766 B
                   (not full yet)   → frame 1 sent, the rest starts frame 2
```

### 5.6 Link to the uplink

```
Ground ─TC every 20 ms─▶ UDP socket ─(CI: up to 11 frames every 100 ms)─▶ FILE_DL_PIPE ─(at once)─▶ FILE_DL command work
```

1. The ground sends a DATA command every 20 ms. The frames wait in the OS socket buffer.
2. CI_LAB wakes every 100 ms, reads up to 11 frames, and sends them to the SB. So about **5 commands arrive at FILE_DL together** every 100 ms.
3. FILE_DL wakes as soon as they are in its pipe and handles them one after another. If the tick becomes due in between, the tick runs first.

**What sets the upload rate**
- The **ground's spacing** sets it, not the tick, because writing chunks is command work.
- With 960 B every 20 ms, the limit is about 47 KiB/s. About 31 KiB/s was measured, because of per-command overhead and the waits for status after END.
- Sending faster than every 20 ms puts more commands into each CI batch. That can overflow FILE_DL's pipe (10 messages on native) or the limit of 32 messages per MID, which shows up as `Msg Limit Err`.
- A command reaches FILE_DL within about 100 ms (the CI poll) plus its handling time.

**The check after reception is paced by the tick**

```
END (command) → VERIFYING → 16 chunks of CRC per tick → … → last tick: COMPLETE, status sent
```

For 1 MiB (1,093 chunks), that is about 69 ticks, or about 7 s.

### 5.7 Upload and downlink at the same time

```
time (ms)   0     20    40    60    80    100   120   140   160   180   200
CI wake     ◇                             ◇                             ◇
pipe gets   ▮▮▮▮▮                         ▮▮▮▮▮                         ▮▮▮▮▮
FILE_DL     [5 commands]                  [5 commands]                  [5 commands]
tick        ●                             ●                             ●
              └ 1 DL segment ─▶ TO pipe     └ 1 segment                   └ 1 segment
TO wake        ◆                             ◆                             ◆
```

- Upload commands arrive in batches at CI's pace and are handled at once.
- Downlink segments leave once per tick, by the clock.
- Measured (`--mode both`, 256 KiB, 5% loss each way): 65 downlink segments came down during an 8.0 s upload. The ideal number is 80.

### 5.8 Summary and settings

| Question | Answer |
|---|---|
| What is a tick? | A fixed batch of work the FILE_DL loop runs every 100 ms by the clock: one downlink segment, CRC check steps, starting a relay. |
| What sets the downlink rate? | The tick: 1024 B per 100 ms. |
| What sets the upload rate? | The ground's command spacing and CI's poll period. The tick does not. |
| What sets the upload check time? | The tick: 16 chunks per 100 ms. |
| Is sch_lab involved? | No. FILE_DL keeps its own clock. `0x18C1` WAKEUP still works, but nothing sends it. |
| Are FILE_DL and TO_LAB in step? | Same period (100 ms), independent phase. The pipe absorbs the difference. |

| Setting | Where | Raising it |
|---|---|---|
| `FILE_DL_SEGMENTS_PER_TICK` (1) | `file_dl_my.h` | Faster downlink, but TO's pipe may overflow |
| `FILE_DL_SEND_PERIOD_MSEC` (100) | `file_dl_my.h` | Slower ticks, so slower downlink and slower checks |
| `FILE_UL_VERIFY_CHUNKS` (16) | `file_ul_my.h` | Faster check, but each tick takes longer |
| Ground command spacing (20 ms) | `CMD_GAP` in `file_xfer_test.py` | Lowering it makes uploads faster, but risks pipe overflow and the MID message limit |

---

## 6. Timing and limits

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

## 7. Files

| File | Content |
|---|---|
| `fsw/platform_inc/file_dl_msgids.h` | Message IDs |
| `fsw/src/file_dl_my.c/.h` | `FILE_DL_AppMain` (main loop, dispatch by MID), downlink engine, downlink commands, shared helpers (`FILE_DL_Crc32`, big-endian get/put, `FILE_DL_StartFile`, `FILE_DL_IsBusy`) |
| `fsw/src/file_ul_my.c/.h` | Upload engine with two instances (upload, relay): commands, chunk bitmap, `.part`, CRC read-back, rename, relay queue |
| `*.bak` | Earlier versions |

## 8. Tests (native)

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
