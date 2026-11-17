# MSN_APP

Satellite control app. Files on `/cf` stand in for hardware and non-volatile memory.

| Function | Command (Function Code) | Result |
|---|---|---|
| Change a parameter | SET_PARAM (2) | value checked, saved to `/cf/msn_param.txt`, shown in HK |
| Choose the apps cFS starts | SET_STARTUP (3) | app line moved in `/cf/cfe_es_startup.scr`, used at the next cFS start |
| Diagnostics | RUN_DIAG (4) | 5 checks, DIAG tlm |
| App status | GET_APP_STATUS (5) | ES info plus startup script state, APP_STATUS tlm |
| Temperature to the ground | READ_TEMP (6), or every N HK requests | value read from `/cf/msn_temp.txt`, TEMP tlm |

## 1. Structure

```
ground --UDP 1236--> tc_frame_proxy.py --TC frame, UDP 1234--> CI_LAB
CI_LAB --SB MID 0x18D0 (command)--> MSN_APP
SCH_LAB --SB MID 0x18D1 (every 4 s)--> MSN_APP: HK tlm, and TEMP tlm every TEMP_PERIOD requests
MSN_APP --SB MID 0x08D0..0x08D3 (telemetry)--> TO_LAB --TM frame, UDP 1235--> ground

MSN_APP reads/writes:  /cf/msn_temp.txt        one number in deg C, edited by hand (the "sensor")
                       /cf/msn_param.txt       "high low period", loaded at start, rewritten by SET_PARAM
                       /cf/cfe_es_startup.scr  read by SET_STARTUP, GET_APP_STATUS and RUN_DIAG
MSN_APP asks ES:       CFE_ES_GetAppIDByName / CFE_ES_GetAppInfo for app status
```

Every packet on the command pipe goes through the same path, as in `sample_app`:

1. `MSN_APP_Main()` waits on the pipe with `CFE_SB_ReceiveBuffer()`.
2. `MSN_APP_ProcessCommandPacket()` looks at the **Message ID (MID)**:
   0x18D0 goes to `ProcessGroundCommand()`, 0x18D1 goes to `ReportHousekeeping()`, anything else
   gives an "invalid command packet" event.
3. `MSN_APP_ProcessGroundCommand()` looks at the **Function Code (FC, also called command code)**
   and calls `MSN_APP_VerifyCmdLength()` with the size that function code must have.
4. A wrong length gives a length event and an error count; the right length calls the function.
5. Each function adds 1 to the command counter on success or to the error counter on failure,
   and reports with an event and, for most commands, a telemetry packet.

## 2. Message IDs

| MID | Direction | Sender | Content |
|---|---|---|---|
| 0x18D0 | command | ground (through CI_LAB) | all MSN_APP commands, told apart by the Function Code |
| 0x18D1 | command | SCH_LAB, every 4 s | HK request, no Function Code used |
| 0x08D0 | telemetry | MSN_APP | HK |
| 0x08D1 | telemetry | MSN_APP | TEMP |
| 0x08D2 | telemetry | MSN_APP | DIAG |
| 0x08D3 | telemetry | MSN_APP | APP_STATUS |

## 3. Command packet format

A command is an 8-byte header (big endian, CCSDS + cFS v1 secondary header) and a payload
(little endian on BBB and x86).

| Byte | Field | MSN_APP value |
|---|---|---|
| 0-1 | Stream ID = MID | `18 D0` |
| 2-3 | sequence flags + count | `C0 00` (count may be anything) |
| 4-5 | length = total bytes - 7 | depends on the command, see below |
| 6 | Function Code | 0 to 6 |
| 7 | checksum: XOR of all bytes is 0xFF | MSN_APP does not check it |
| 8- | payload | see below |

| FC | Name | Payload | Total bytes | Length field | Example packet (hex) |
|---|---|---|---|---|---|
| 0 | NOOP | - | 8 | 1 | `18 D0 C0 00 00 01 00 F6` |
| 1 | RESET_COUNTERS | - | 8 | 1 | `18 D0 C0 00 00 01 01 F7` |
| 2 | SET_PARAM | `uint8 ParamId, 3 spare, int32 Value` | 16 | 9 | high limit 50: `18 D0 C0 00 00 09 02 CE` `00 00 00 00 32 00 00 00` |
| 3 | SET_STARTUP | `char AppName[20], uint8 Enable, 3 spare` | 32 | 25 | disable SAMPLE_APP: `18 D0 C0 00 00 19 03 F5` `53 41 4D 50 4C 45 5F 41 50 50 00 ...` (name padded with 00 to 20 bytes, then Enable `00` and 3 spare) |
| 4 | RUN_DIAG | - | 8 | 1 | `18 D0 C0 00 00 01 04 F2` |
| 5 | GET_APP_STATUS | `char AppName[20]` | 28 | 21 | SAMPLE_APP: `18 D0 C0 00 00 15 05 FF` `53 41 4D 50 4C 45 5F 41 50 50 00 ...` |
| 6 | READ_TEMP | - | 8 | 1 | `18 D0 C0 00 00 01 06 F0` |

A packet with the right MID and Function Code but another total size is rejected before the
function runs. `AppName` is the cFE name, the 4th field of a startup script line (`SAMPLE_APP`,
`FILE_DL`, ...).

With cmdUtil (`--endian=LE` makes the payload little endian):

```
cmdUtil --port=1236 --endian=LE --pktid=0x18D0 --cmdcode=2 --uint8=0 --uint8=0 --uint16=0 --int32=50
cmdUtil --port=1236 --endian=LE --pktid=0x18D0 --cmdcode=3 --string=20:SAMPLE_APP --uint8=0 --uint8=0 --uint16=0
cmdUtil --port=1236 --endian=LE --pktid=0x18D0 --cmdcode=4
cmdUtil --port=1236 --endian=LE --pktid=0x18D0 --cmdcode=5 --string=20:SAMPLE_APP
cmdUtil --port=1236 --endian=LE --pktid=0x18D0 --cmdcode=6
```

## 4. Telemetry packet format

Every packet starts with a 16-byte header: CCSDS primary header (6), time (6) and spare (4).
The offsets below count from the start of the payload, after those 16 bytes.

| MID | Packet | Payload bytes |
|---|---|---|
| 0x08D0 | HK (20 B) | 0 `CommandCounter`, 1 `CommandErrorCounter`, 2 `TempStatus`, 3 spare, 4-7 `float Temperature`, 8-11 `int32 Param[0]` (high), 12-15 `Param[1]` (low), 16-19 `Param[2]` (period) |
| 0x08D1 | TEMP (12 B) | 0-3 `float Temperature`, 4 `Status`, 5-7 spare, 8-11 `uint32 SampleCount` |
| 0x08D2 | DIAG (12 B) | 0-4 `uint8 Result[5]` (0 pass, 1 fail), 5 `FailCount`, 6-7 spare, 8-11 `uint32 RunCount` |
| 0x08D3 | APP_STATUS (40 B) | 0-19 `AppName`, 20 `Running`, 21 `Startup`, 22-23 spare, 24-27 `Priority`, 28-31 `StackSize`, 32-35 `ExecutionCounter`, 36-39 `ChildTasks` |

- Temperature status: 0 NORMAL, 1 HIGH, 2 LOW, 3 NO_DATA.
- Startup: 0 not in the script, 1 enabled, 2 disabled.

## 5. Events

| EID | Type | When |
|---|---|---|
| 1 | info | app initialized, with the parameters in use |
| 2 | error | unknown Function Code |
| 3 / 4 | info | NOOP / RESET_COUNTERS |
| 5 | error | unknown MID on the pipe |
| 6 | error | wrong command length |
| 7 | error | pipe read error, the app exits |
| 8 | error | a file cannot be read or written |
| 9 / 10 | info / error | SET_PARAM accepted / rejected |
| 11 / 12 | info / error | SET_STARTUP done (or nothing to do) / rejected |
| 13 / 14 | info / error | RUN_DIAG with no failed check / with failed checks |
| 15 | info | GET_APP_STATUS result |
| 16 / 17 | info / error | temperature status changed to NORMAL / to HIGH, LOW or NO_DATA |

## 6. How each command works

### NOOP (FC 0) and RESET_COUNTERS (FC 1)

NOOP adds 1 to the command counter and sends an event with the app version. RESET_COUNTERS sets
both counters to 0. Both are handled inside `ProcessGroundCommand()`.

### SET_PARAM (FC 2)

SET_PARAM changes how the temperature is watched and reported without loading new software.

| ParamId | Name | Default | Valid range | Used by |
|---|---|---|---|---|
| 0 | TEMP_HIGH (deg C) | 60 | above TEMP_LOW, up to 200 | above it: HIGH |
| 1 | TEMP_LOW (deg C) | -20 | below TEMP_HIGH, down to -100 | below it: LOW |
| 2 | TEMP_PERIOD (HK requests) | 1 (every 4 s) | 0-100, 0 = only on READ_TEMP | automatic TEMP tlm |

Steps in `MSN_APP_SetParam()`:

1. Take `ParamId` and `Value` from the payload.
2. A `switch` on `ParamId` checks the range. The two limits are checked against each other, so
   TEMP_LOW always stays below TEMP_HIGH. An unknown `ParamId` is invalid.
3. Invalid: error counter + 1, event 10, `Param` unchanged.
4. Valid: keep the old value for the event, store the new one in `Param[ParamId]`, then
   `MSN_APP_SaveParams()` rewrites `/cf/msn_param.txt` as `"high low period\n"`.
5. Command counter + 1, event 9 `Param <id> changed <old> -> <new>`.

Example, starting from `Param = [60, -20, 1]`:

| Command (Id, Value) | Check | Result |
|---|---|---|
| (0, 50) | `50 > Param[1] (-20)` and `50 <= 200` | `Param = [50, -20, 1]`, file `50 -20 1` |
| (1, 50) | `50 < Param[0] (50)` is false | rejected |
| (9, 1) | `default` | rejected, no parameter 9 |
| (2, 0) | `0 <= 0 <= 100` | `Param = [50, -20, 0]`, TEMP tlm only on READ_TEMP |

To move both limits above the old high limit (for example to 80 / 70), send the high limit first.
At the next start, `MSN_APP_Init()` reads the file with `MSN_APP_ReadParamFile()`; if it is missing
or does not hold 3 numbers, the defaults are used and written to the file.

The new values take effect at once: the next temperature read uses the new limits (55 C is NORMAL
with a high limit of 60 but HIGH with 50), and the next HK request uses the new period.

### SET_STARTUP (FC 3)

cFE ES reads `cfe_es_startup.scr` from the top and stops at the first `!`. SET_STARTUP moves the
app's line below or above the first line that starts with `!`. No line is added or deleted.

Steps in `MSN_APP_SetStartup()`:

1. Copy `AppName` with `CFE_SB_MessageStringGet()`, which ends it with `'\0'` even if all 20 bytes
   of the command are used.
2. `Enable` must be 0 or 1, otherwise rejected.
3. If `Enable` is 0, the name must not be `CI_LAB_APP`, `TO_LAB_APP`, `SCH_LAB_APP` or `MSN_APP`.
   Without them the ground could not send the command to enable them again.
4. `MSN_APP_FindStartupEntry()` reads the script into `StartupBuf` (4 KB). It finds:
   - `Entry`: the line whose 4th comma-separated field is `AppName`;
   - `Marker`: the first line that starts with `!` (`"!\n"` is added at the end if there is none).
   A read error or a name that is not in the script is rejected.
5. If the app is already in the asked state, the command succeeds and the file is not written.
6. The new file is made from 4 pieces of `StartupBuf` (see the example below).
7. The pieces are written to `/cf/cfe_es_startup.tmp`. Only if every write succeeds is the file
   renamed over `cfe_es_startup.scr`; otherwise it is removed and the old script stays.
8. Command counter + 1, event 11 `<app> enabled|disabled in /cf/cfe_es_startup.scr from the next cFS start`.

Nothing changes in the running system: a disabled app keeps running until cFS restarts (for
example with ES RESTART, MID 0x1806 FC 2). On the RTEMS image `/cf` is unpacked into RAM at boot,
so the change is lost when the board reboots.

**Example.** Every pointer is a byte number in `StartupBuf`; `[From, To)` means "from `From` up
to, but not including, `To`". One letter per line, B is `SAMPLE_APP`:

```
byte:   0    1    2    3    4    5    6    7    8    9
char:   A   \n    B   \n    C   \n    !   \n    E   \n
```

`EntryEnd = strchr(Entry, '\n') + 1` is the first byte after the entry's newline, so
`[Entry, EntryEnd)` is exactly one whole line. `MarkerEnd` is found the same way for the `!` line.

Disable B (Enable 0): `Entry` 2, `EntryEnd` 4, `Marker` 6, `MarkerEnd` 8, `End` 10.

| i | From[i] | To[i] | Bytes | Text written |
|---|---|---|---|---|
| 0 | Buf 0 | Entry 2 | 0-1 | `A\n` |
| 1 | EntryEnd 4 | MarkerEnd 8 | 4-7 | `C\n!\n` |
| 2 | Entry 2 | EntryEnd 4 | 2-3 | `B\n` |
| 3 | MarkerEnd 8 | End 10 | 8-9 | `E\n` |

New file `A C ! B E`: ES stops at `!`, so B is not started. `To[1]` must be `MarkerEnd`, not
`Marker`: with `Marker` the bytes 6-7 (`!\n`) would be in no piece and the marker would be lost.

Enable B again (Enable 1). The file is now `A C ! B E`:

```
byte:   0    1    2    3    4    5    6    7    8    9
char:   A   \n    C   \n    !   \n    B   \n    E   \n
```

`Marker` 4, `Entry` 6, `EntryEnd` 8, `End` 10. `Dest`, where B is inserted, is `Marker` (4).

| i | From[i] | To[i] | Bytes | Text written |
|---|---|---|---|---|
| 0 | Buf 0 | Dest 4 | 0-3 | `A\nC\n` |
| 1 | Entry 6 | EntryEnd 8 | 6-7 | `B\n` |
| 2 | Dest 4 | Entry 6 | 4-5 | `!\n` |
| 3 | EntryEnd 8 | End 10 | 8-9 | `E\n` |

New file `A C B ! E`: B starts again, now as the last app.

Enable a library L (file `B C ! L E`, L is a `CFE_LIB` line and B uses it). `Dest` is `Buf` (0),
so the library goes to the top and loads before the apps that use it:

```
byte:   0    1    2    3    4    5    6    7    8    9
char:   B   \n    C   \n    !   \n    L   \n    E   \n
```

| i | From[i] | To[i] | Bytes | Text written |
|---|---|---|---|---|
| 0 | Buf 0 | Dest 0 | none | skipped: `To > From` is false (OS_write rejects length 0) |
| 1 | Entry 6 | EntryEnd 8 | 6-7 | `L\n` |
| 2 | Dest 0 | Entry 6 | 0-5 | `B\nC\n!\n` |
| 3 | EntryEnd 8 | End 10 | 8-9 | `E\n` |

New file `L B C ! E`. With `Dest = Marker` it would be `B C L ! E`, and B would fail to load.
In every case the 4 pieces hold each byte of the old file exactly once.

### RUN_DIAG (FC 4)

Steps in `MSN_APP_RunDiag()`:

1. Clear the DIAG payload.
2. Run 5 checks; each writes 0 (pass) or 1 (fail) into `Result[]`:

   | Index | Check | How |
   |---|---|---|
   | 0 | temperature file holds a number | `MSN_APP_ReadTemperature()` returns success |
   | 1 | temperature within the limits | the status it set is NORMAL |
   | 2 | parameter file holds the parameters in use | `MSN_APP_ReadParamFile()` and `memcmp` with `Param` |
   | 3 | the 4 essential apps are enabled in the startup script | `FindStartupEntry()` returns ENABLED for each |
   | 4 | the 4 essential apps are running | `CFE_ES_GetAppIDByName()` succeeds for each |

3. `FailCount` = sum of `Result[]`; `RunCount` + 1 so the ground can tell a new packet from an old one.
4. Send DIAG tlm, command counter + 1, event 13 (all passed) or 14, with the results as digits.

| Situation | Result | FailCount | Event text |
|---|---|---|---|
| everything normal, 21.5 C | `0 0 0 0 0` | 0 | `Diagnostics 0 of 5 failed (00000)` |
| file says 75.0, high limit 50 | `0 1 0 0 0` | 1 | `(01000)` |
| temperature file removed | `1 1 0 0 0` | 2 | NO_DATA is also not NORMAL, so check 1 fails too |
| `msn_param.txt` edited by hand to `70 -20 1` while `Param = [50, -20, 1]` | `0 0 1 0 0` | 1 | `(00100)` |
| `TO_LAB_APP` line moved below `!` by hand | `0 0 0 1 0` | 1 | `(00010)` |

RUN_DIAG itself always succeeds; failed checks are results, not command errors.

### GET_APP_STATUS (FC 5)

Steps in `MSN_APP_GetAppStatus()`:

1. Clear the APP_STATUS payload, so every field not filled below is 0.
2. Copy `AppName` with `CFE_SB_MessageStringGet()` (always ends with `'\0'`).
3. Ask ES: `CFE_ES_GetAppIDByName()` then `CFE_ES_GetAppInfo()`. If both succeed, set
   `Running` 1 and copy `Priority`, `StackSize`, `ExecutionCounter` and `NumOfChildTasks`.
4. `FindStartupEntry()` gives `Startup` (0 not found, 1 enabled, 2 disabled; a read error gives 0).
5. Send APP_STATUS tlm, command counter + 1, event 15.

| AppName | ES lookup | Startup script | APP_STATUS |
|---|---|---|---|
| `SAMPLE_APP` | found | line above `!` | Running 1, Startup 1, Priority 50, StackSize 16384, ExecutionCounter n |
| `SAMPLE_APP` after SET_STARTUP disable | found | line below `!` | Running 1, Startup 2 (until the restart) |
| same, after a restart | not found | line below `!` | Running 0, Startup 2, other fields 0 |
| `SAMPLE_LIB` | not found (ES looks up apps only) | line above `!` | Running 0, Startup 1 |
| `NO_SUCH_APP` | not found | no line | Running 0, Startup 0 |

The command always succeeds; an unknown name is reported in the telemetry, not as an error.

### READ_TEMP (FC 6) and periodic TEMP

Steps in `MSN_APP_SendTemperature()` and `MSN_APP_ReadTemperature()`:

1. Open `/cf/msn_temp.txt`, read at most 31 bytes and add `'\0'`, so `sscanf` stays in the buffer.
2. `sscanf("%f")` reads one number. If it does, store it in `Temperature`, add 1 to `SampleCount`
   and compare it with the limits: above `Param[0]` is HIGH, below `Param[1]` is LOW, else NORMAL.
   If not (no file, empty, text), the status is NO_DATA and the last good value is kept.
3. Only if the status differs from the last one: event 16 (NORMAL) or 17 (HIGH, LOW, NO_DATA).
4. Fill TEMP tlm with temperature, status and sample count and send it.
5. For READ_TEMP, `ProcessGroundCommand()` adds 1 to the command counter, or to the error counter
   for NO_DATA. Telemetry is sent in both cases.

With limits 50 / -20:

| `/cf/msn_temp.txt` | Temperature | Status | SampleCount | Event |
|---|---|---|---|---|
| `21.5` | 21.5 | 0 NORMAL | +1 | only if the status was not NORMAL |
| `75.0` | 75.0 | 1 HIGH (`75 > 50`) | +1 | `Temperature 75.00 C is HIGH (limits 50 / -20)` |
| `75.0` again | 75.0 | 1 HIGH | +1 | none, status unchanged |
| `-30` | -30.0 | 2 LOW (`-30 < -20`) | +1 | `... is LOW ...` |
| `abc` or no file | last value | 3 NO_DATA | unchanged | `No temperature in /cf/msn_temp.txt` |

Periodic sending: on every SEND_HK (0x18D1, every 4 s), `ReportHousekeeping()` adds 1 to `HkCount`
and calls `SendTemperature()` when `Param[2] > 0` and `HkCount % Param[2] == 0`, then sends HK.

| Param[2] | HkCount values that send TEMP | Interval |
|---|---|---|
| 1 | 1, 2, 3, ... | 4 s |
| 3 | 3, 6, 9, ... | 12 s |
| 0 | none | only READ_TEMP |

## 7. Files

| File | Written by | Read by | Content |
|---|---|---|---|
| `/cf/msn_temp.txt` | the user (by hand) | READ_TEMP, periodic TEMP, RUN_DIAG | one number in deg C; installed from `bbb_defs/cpu1_msn_temp.txt` (21.5) |
| `/cf/msn_param.txt` | SET_PARAM, Init (defaults) | Init, RUN_DIAG | `high low period` |
| `/cf/cfe_es_startup.scr` | SET_STARTUP | ES at start, SET_STARTUP, GET_APP_STATUS, RUN_DIAG | ES startup script |
| `/cf/cfe_es_startup.tmp` | SET_STARTUP | - | new script before the rename; removed on failure |

## 8. Test

`msn_test/run_msn_test.sh -b <build_dir>` runs the native build, checks every command with the
telemetry and events, restarts cFS to confirm the startup script change and that the parameters
were kept, and then restores the original startup script.
