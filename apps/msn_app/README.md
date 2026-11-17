# MSN_APP

Satellite control app. Files on `/cf` stand in for hardware and non-volatile memory.

| Function | Command | Result |
|---|---|---|
| Change a parameter | SET_PARAM | value checked, saved to `/cf/msn_param.txt`, shown in HK |
| Choose the apps cFS starts | SET_STARTUP | app line moved in `/cf/cfe_es_startup.scr`, used at the next cFS start |
| Diagnostics | RUN_DIAG | 5 checks, DIAG tlm |
| App status | GET_APP_STATUS | ES info plus startup script state, APP_STATUS tlm |
| Temperature to the ground | READ_TEMP, or every N HK requests | value read from `/cf/msn_temp.txt`, TEMP tlm |

## Structure

```
ground --TC--> CI_LAB --SB 0x18D0--> MSN_APP --SB 0x08D0..0x08D3--> TO_LAB --TM--> ground
SCH_LAB --SB 0x18D1 (every 4 s)--> MSN_APP: HK tlm, and TEMP tlm every TEMP_PERIOD requests

MSN_APP reads/writes:  /cf/msn_temp.txt        one number in deg C, edited by hand (the "sensor")
                       /cf/msn_param.txt       "high low period", loaded at start, rewritten by SET_PARAM
                       /cf/cfe_es_startup.scr  read by SET_STARTUP, GET_APP_STATUS and RUN_DIAG
MSN_APP asks ES:       CFE_ES_GetAppIDByName / CFE_ES_GetAppInfo for app status
```

Like `sample_app`, one pipe receives commands and HK requests; `ProcessGroundCommand()` checks the
length and calls one function per command code.

## Commands (MID 0x18D0)

| CC | Name | Payload (little endian on BBB/x86) | Notes |
|---|---|---|---|
| 0 | NOOP | - | event |
| 1 | RESET_COUNTERS | - | HK counters cleared |
| 2 | SET_PARAM | `uint8 ParamId, 3 spare, int32 Value` | see Parameters |
| 3 | SET_STARTUP | `char AppName[20], uint8 Enable (0/1), 3 spare` | see Startup script |
| 4 | RUN_DIAG | - | DIAG tlm |
| 5 | GET_APP_STATUS | `char AppName[20]` | APP_STATUS tlm, also for unknown names |
| 6 | READ_TEMP | - | TEMP tlm; error counter if the file has no number |

`AppName` is the cFE name, the 4th field of a startup script line (`SAMPLE_APP`, `FILE_DL`, ...).

### Parameters

| Id | Name | Default | Valid range |
|---|---|---|---|
| 0 | TEMP_HIGH (deg C) | 60 | above TEMP_LOW, up to 200 |
| 1 | TEMP_LOW (deg C) | -20 | below TEMP_HIGH, down to -100 |
| 2 | TEMP_PERIOD (HK requests) | 1 (every 4 s) | 0-100, 0 = only on READ_TEMP |

The defaults are used and saved when `/cf/msn_param.txt` is missing or bad.

### Startup script

ES stops reading the script at the first `!` line. SET_STARTUP moves the app's line:

- Enable 0: just below the `!` line, so ES skips it at the next start. The app keeps running now.
- Enable 1: just above the `!` line (the app starts last), or to the top for a `CFE_LIB` line so
  the library still loads before the apps that use it.

The new script is written to `/cf/cfe_es_startup.tmp` and renamed, so a failed write keeps the old
one. `CI_LAB_APP`, `TO_LAB_APP`, `SCH_LAB_APP` and `MSN_APP` cannot be disabled, because the
ground could not command the satellite to enable them again. Restart cFS (for example ES RESTART,
MID 0x1806 CC 2) to apply the change. On the RTEMS image `/cf` is unpacked into RAM at boot, so the
change is lost when the board reboots.

### Diagnostics

| Result index | Check (0 = pass, 1 = fail) |
|---|---|
| 0 | `/cf/msn_temp.txt` holds a number |
| 1 | temperature is within TEMP_LOW..TEMP_HIGH |
| 2 | `/cf/msn_param.txt` holds the parameters in use |
| 3 | the 4 essential apps above are enabled in the startup script |
| 4 | the 4 essential apps are running |

## Telemetry

| MID | Packet | Payload |
|---|---|---|
| 0x08D0 | HK (every 4 s) | counters, temperature status, last temperature, 3 parameters |
| 0x08D1 | TEMP | `float Temperature, uint8 Status, 3 spare, uint32 SampleCount` |
| 0x08D2 | DIAG | `uint8 Result[5], uint8 FailCount, 2 spare, uint32 RunCount` |
| 0x08D3 | APP_STATUS | `char AppName[20], uint8 Running, uint8 Startup, 2 spare, uint32 Priority, StackSize, ExecutionCounter, ChildTasks` |

Temperature status: 0 NORMAL, 1 HIGH, 2 LOW, 3 NO_DATA. An event is sent only when it changes.
Startup: 0 not in the script, 1 enabled, 2 disabled. Libraries are never `Running` (ES lists apps only).

## Example (native, commands through `tc_test/tc_frame_proxy.py` on port 1236)

```
echo 75.0 > build-native/exe/cpu1/cf/msn_temp.txt                                 # "sensor" value
cmdUtil --port=1236 --endian=LE --pktid=0x18D0 --cmdcode=6                       # READ_TEMP
cmdUtil --port=1236 --endian=LE --pktid=0x18D0 --cmdcode=2 --uint8=0 --uint8=0 --uint16=0 --int32=80
cmdUtil --port=1236 --endian=LE --pktid=0x18D0 --cmdcode=3 --string=20:SAMPLE_APP --uint8=0 --uint8=0 --uint16=0
cmdUtil --port=1236 --endian=LE --pktid=0x18D0 --cmdcode=5 --string=20:SAMPLE_APP
cmdUtil --port=1236 --endian=LE --pktid=0x18D0 --cmdcode=4                       # RUN_DIAG
```

`msn_test/run_msn_test.sh -b <build_dir>` checks every command, restarts cFS to confirm the startup
script change and restores the original script.

## How the command functions work, with numbers

### SetParam (CC 2)

The parameters are one array, `Param[3]`, indexed by `ParamId`. Start: `Param = [60, -20, 1]`.

| Command (Id, Value) | Check in the `switch` | Result |
|---|---|---|
| (0, 50) | high: `50 > Param[1] (-20)` and `50 <= 200` | `Param = [50, -20, 1]`, file `50 -20 1`, event `Param 0 changed 60 -> 50` |
| (1, 50) | low: `50 < Param[0] (50)` is false | rejected, error counter +1, `Param` unchanged |
| (9, 1) | `default` | rejected, no parameter 9 |
| (2, 0) | period: `0 <= 0 <= 100` | `Param = [50, -20, 0]`, TEMP tlm only on READ_TEMP |

The check uses the current value of the other limit, so to move both limits up past the old high
limit (for example to 80 / 70), send the high limit first. `SaveParams()` rewrites the whole file
after every accepted change; `Init()` reads it back on the next start.

### SendTemperature (CC 6, and from ReportHousekeeping)

`SendTemperature()` calls `ReadTemperature()` and sends TEMP tlm with the result. With limits 50 / -20:

| `/cf/msn_temp.txt` | `sscanf("%f")` | Temperature | Status | SampleCount |
|---|---|---|---|---|
| `21.5\n` | 1 value | 21.5 | 0 NORMAL | +1 |
| `75.0\n` | 1 value | 75.0 | 1 HIGH (`75 > 50`) | +1 |
| `-30\n` | 1 value | -30.0 | 2 LOW (`-30 < -20`) | +1 |
| `abc` or no file | 0 values | last good value kept | 3 NO_DATA | unchanged |

At most 31 bytes are read and a `'\0'` is put after them, so `sscanf` never runs past the buffer.
An event is sent only when the status changes: reading 75.0 twice gives one HIGH event. READ_TEMP
counts NO_DATA as a command error.

Periodic sending: SCH_LAB sends SEND_HK every 4 s, `ReportHousekeeping()` adds 1 to `HkCount` and
sends TEMP when `HkCount % Param[2] == 0`.

| Param[2] | HkCount values that send TEMP | Interval |
|---|---|---|
| 1 | 1, 2, 3, ... | 4 s |
| 3 | 3, 6, 9, ... | 12 s |
| 0 | none (the `> 0` check skips the division) | only READ_TEMP |

TEMP payload bytes: 0-3 `float Temperature`, 4 `Status`, 5-7 spare, 8-11 `SampleCount`.

### RunDiag (CC 4)

Each check writes 0 (pass) or 1 (fail) into `Result[5]`; `FailCount` is their sum.

| Situation | Result | FailCount | Event |
|---|---|---|---|
| everything normal, 21.5 C | `0 0 0 0 0` | 0 | `Diagnostics 0 of 5 failed (00000)` (info) |
| file says 75.0, high limit 50 | `0 1 0 0 0` | 1 | `(01000)` (error) |
| temperature file removed | `1 1 0 0 0` | 2 | NO_DATA is also not NORMAL, so check 1 fails too |
| `msn_param.txt` edited by hand to `70 -20 1` while `Param = [50, -20, 1]` | `0 0 1 0 0` | 1 | file and memory differ (`memcmp`) |
| `TO_LAB_APP` line moved below `!` by hand | `0 0 0 1 0` | 1 | it would not start after a restart |

- Checks 0 and 1 read the file again (they may also send a temperature status event).
- Check 3 calls `FindStartupEntry()` for each of the 4 essential apps and fails if any one is not ENABLED.
- Check 4 calls `CFE_ES_GetAppIDByName()` for the same 4 names and fails if any one is not running.

`RunCount` counts RUN_DIAG commands, so the ground can tell a new DIAG packet from an old one.

### GetAppStatus (CC 5)

The payload is cleared first, so every field the function does not fill is 0. The name is copied
with `CFE_SB_MessageStringGet()`, which ends it with `'\0'` even if all 20 command bytes are used.

| AppName | ES lookup | Startup script | APP_STATUS |
|---|---|---|---|
| `SAMPLE_APP` | found | line above `!` | Running 1, Startup 1, Priority 50, StackSize 16384, ExecutionCounter n, ChildTasks 0 |
| `SAMPLE_APP` after SET_STARTUP disable | found | line below `!` | Running 1, Startup 2 (still running until the restart) |
| same, after a restart | not found | line below `!` | Running 0, Startup 2, other fields 0 |
| `SAMPLE_LIB` | not found (ES looks up apps only) | line above `!` | Running 0, Startup 1 |
| `NO_SUCH_APP` | not found | no line | Running 0, Startup 0 |

The command always succeeds; an unknown name is reported in the telemetry, not as an error.

APP_STATUS payload bytes: 0-19 `AppName`, 20 `Running`, 21 `Startup`, 22-23 spare, 24-27 `Priority`,
28-31 `StackSize`, 32-35 `ExecutionCounter`, 36-39 `ChildTasks`.

### SetStartup (CC 3)

The checks run in this order; the first failure ends the command:

1. `Enable` must be 0 or 1.
2. If `Enable` is 0, the name must not be one of the 4 essential apps.
3. `FindStartupEntry()` loads the script into `StartupBuf` and finds the app's line (`Entry`) and
   the first line starting with `!` (`Marker`). A read error or a missing name is an error.
4. If the app is already in the asked state, the command succeeds and the file is not written.

Then the file is rewritten from 4 pieces of `StartupBuf`. Every pointer is a position (byte number)
in the buffer, and `[From, To)` means "from `From` up to, but not including, `To`".

**Example file** (one letter per line, B is `SAMPLE_APP`):

```
byte:   0    1    2    3    4    5    6    7    8    9
char:   A   \n    B   \n    C   \n    !   \n    E   \n
```

`EntryEnd = strchr(Entry, '\n') + 1` is the first byte after the entry's newline, so
`[Entry, EntryEnd)` is exactly one whole line. `MarkerEnd` is found the same way for the `!` line.

**Disable B** (Enable 0, B is above `!`): `Entry` 2, `EntryEnd` 4, `Marker` 6, `MarkerEnd` 8, `End` 10.

| i | From[i] | To[i] | Bytes | Text written |
|---|---|---|---|---|
| 0 | Buf 0 | Entry 2 | 0-1 | `A\n` |
| 1 | EntryEnd 4 | MarkerEnd 8 | 4-7 | `C\n!\n` |
| 2 | Entry 2 | EntryEnd 4 | 2-3 | `B\n` |
| 3 | MarkerEnd 8 | End 10 | 8-9 | `E\n` |

New file: `A C ! B E`. ES stops at `!`, so B is not started. `To[1]` must be `MarkerEnd`, not
`Marker`: with `Marker` the bytes 6-7 (`!\n`) would be in no piece and the marker would be lost.

**Enable B again** (Enable 1, file is now `A C ! B E`, B is below `!`):

```
byte:   0    1    2    3    4    5    6    7    8    9
char:   A   \n    C   \n    !   \n    B   \n    E   \n
```

`Marker` 4, `Entry` 6, `EntryEnd` 8, `End` 10. `Dest`, the place to insert B, is `Marker` (4) for an app.

| i | From[i] | To[i] | Bytes | Text written |
|---|---|---|---|---|
| 0 | Buf 0 | Dest 4 | 0-3 | `A\nC\n` |
| 1 | Entry 6 | EntryEnd 8 | 6-7 | `B\n` |
| 2 | Dest 4 | Entry 6 | 4-5 | `!\n` |
| 3 | EntryEnd 8 | End 10 | 8-9 | `E\n` |

New file: `A C B ! E`. B is started again, now as the last app.

**Enable a library L** (file `B C ! L E`, L is `CFE_LIB` and B uses it):

```
byte:   0    1    2    3    4    5    6    7    8    9
char:   B   \n    C   \n    !   \n    L   \n    E   \n
```

`Dest` is `Buf` (0) because the line starts with `CFE_LIB`.

| i | From[i] | To[i] | Bytes | Text written |
|---|---|---|---|---|
| 0 | Buf 0 | Dest 0 | none | skipped: `To > From` is false (OS_write rejects length 0) |
| 1 | Entry 6 | EntryEnd 8 | 6-7 | `L\n` |
| 2 | Dest 0 | Entry 6 | 0-5 | `B\nC\n!\n` |
| 3 | EntryEnd 8 | End 10 | 8-9 | `E\n` |

New file: `L B C ! E`. With `Dest = Marker` it would be `B C L ! E`, and B would fail to load
because L is not loaded yet.

In every case the 4 pieces together hold each byte of the old file exactly once. They are written to
`/cf/cfe_es_startup.tmp`, which is renamed over the script only if all writes succeed, so a failed
write leaves the old script in place.
