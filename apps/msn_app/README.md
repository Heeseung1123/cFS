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
