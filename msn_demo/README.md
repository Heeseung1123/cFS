# MSN_APP demo

`msn_demo.py` is a small ground console for MSN_APP on the native build. It shows what each MSN_APP
function does by sending the command, decoding the telemetry and printing the events, and lets you
change the files MSN_APP reads, so you can see the satellite react.

For the commands and packets themselves see `apps/mission_app/README.md`.

## Run

```
cd cFS
make SIMULATION=native O=build-native prep      # once
make O=build-native && make O=build-native install
python3 msn_demo/msn_demo.py --tour             # guided tour, then the menu
python3 msn_demo/msn_demo.py                    # menu only
```

The script starts `core-cpu1` (log in `msn_demo/core-cpu1.log`) and `tc_test/tc_frame_proxy.py`,
and enables TO_LAB telemetry. Stop any other `core-cpu1` first: the ports 1234-1236 are shared.

On exit (`q` or Ctrl+C) it stops cFS and puts back `cfe_es_startup.scr`, `msn_temp.txt` and
`msn_param.txt` as they were before the start. Use `--keep-files` to keep your changes, and
`-b <dir>` for another build directory.

## Menu

| Key | Action | What to look at |
|---|---|---|
| `h` | latest HK | HK arrives every 4 s without any command; it only reports values in memory |
| `f` | write the temperature file | the "sensor": nothing happens until MSN_APP reads it |
| `1` | READ_TEMP | reads the file now; TEMP tlm with NORMAL/HIGH/LOW/NO_DATA |
| `2` | SET_PARAM | new limit or period; the next TEMP uses it; `msn_param.txt` is rewritten |
| `3` | RUN_DIAG | 5 checks with pass/fail |
| `4` | GET_APP_STATUS | running or not (from ES) and enabled or not (from the startup script) |
| `5` | SET_STARTUP | moves the app line in the startup script; nothing changes until `r` |
| `s` | show the startup script | which apps ES will start, which are below `!` |
| `w` | watch telemetry | every HK and TEMP packet as it arrives, and the events |
| `r` | restart cFS | ES reads the startup script again; the started apps are listed |
| `t` | guided tour | the steps below |

## Guided tour

| Step | Action | What you see |
|---|---|---|
| 1 | watch 9 s | HK and TEMP every 4 s, 21.5 C NORMAL |
| 2 | write 75.0, watch | the next periodic TEMP says HIGH, with an event |
| 3 | READ_TEMP | the same value at once, without waiting for the period |
| 4 | SET_PARAM high = 80, READ_TEMP | the same 75.0 is now NORMAL |
| 5 | RUN_DIAG | 0 of 5 failed |
| 6 | write `broken`, RUN_DIAG | checks 0 and 1 fail, NO_DATA event |
| 7 | GET_APP_STATUS SAMPLE_APP | running, enabled, priority 50 |
| 8 | SET_STARTUP disable SAMPLE_APP | moved below `!`, but still running |
| 9 | restart | SAMPLE_APP is not started, status: not running, disabled |
| 10 | SET_STARTUP enable, restart | SAMPLE_APP is back, started last |
| 11 | watch | HK still shows high limit 80: the parameter file survived the restart |

## Making and editing the files by hand

All three files are in the target's `/cf`, which on the native build is
`cFS/build-native/exe/cpu1/cf/`. Edit them while cFS runs or while it is stopped.

### `msn_temp.txt` - the temperature sensor

One number in deg C. MSN_APP reads it on READ_TEMP, on every periodic TEMP and in RUN_DIAG.

```
echo 75.0 > build-native/exe/cpu1/cf/msn_temp.txt     # HIGH with the default limit 60
echo -30  > build-native/exe/cpu1/cf/msn_temp.txt     # LOW with the default limit -20
echo oops > build-native/exe/cpu1/cf/msn_temp.txt     # NO_DATA, a broken sensor
rm build-native/exe/cpu1/cf/msn_temp.txt               # NO_DATA as well
```

Only the first 31 bytes are read and only the first number counts (`75.0 C` reads as 75.0).
The default file comes from `bbb_defs/cpu1_msn_temp.txt` (21.5); `make install` copies it again
and overwrites your value. To change the value built into the RTEMS image, edit that file.

### `msn_param.txt` - the saved parameters

Three integers: `high low period`, for example `60 -20 1`. SET_PARAM rewrites it; MSN_APP reads it
only at start.

```
rm build-native/exe/cpu1/cf/msn_param.txt              # next start uses the defaults 60 -20 1
echo "50 -20 3" > build-native/exe/cpu1/cf/msn_param.txt   # used from the next start
```

Edited while cFS runs, the file no longer matches the values in memory: RUN_DIAG check 2 fails
until SET_PARAM writes it again or cFS restarts. A file that does not hold 3 numbers is replaced
with the defaults at the next start. `make install` does not touch this file.

### `cfe_es_startup.scr` - the apps cFS starts

One line per app or library, read by ES at start up to the first `!`:

```
CFE_APP, sample_app,  SAMPLE_APP_Main, SAMPLE_APP,   50,   16384, 0x0, 0;
         file         entry point      cFE name      priority stack  -   exception action
```

To disable an app by hand, move its line below the `!` line (what SET_STARTUP does); to enable it,
move it above. MSN_APP only looks at lines that start with `!` in the first column, and the cFE name
(4th field) is what SET_STARTUP and GET_APP_STATUS use. Keep a `CFE_LIB` line above the apps that
use it. Do not move `CI_LAB_APP` or `TO_LAB_APP` below `!`: the ground would lose the link after
the restart (RUN_DIAG check 3 reports it).

The installed file comes from `bbb_defs/cpu1_cfe_es_startup.scr`; `make install` overwrites the one
in `exe/cpu1/cf`. Change the `bbb_defs` file to change the default for every build.

## Without the demo script

The same commands can be sent with cmdUtil while cFS, the proxy and TO_LAB output are running
(see `msn_test/run_msn_test.sh` for the start sequence):

```
build-native/exe/host/cmdUtil --port=1236 --endian=LE --pktid=0x18D0 --cmdcode=6
build-native/exe/host/cmdUtil --port=1236 --endian=LE --pktid=0x18D0 --cmdcode=2 --uint8=0 --uint8=0 --uint16=0 --int32=80
```

The results are then only in the events (`grep MSN core-cpu1.log`) unless a ground system decodes
the telemetry; that decoding is what `msn_demo.py` adds.
