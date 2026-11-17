# RELAY_APP

Forwards a file that is already on board (for example one uplinked from the main ground
station) to another ground station, using FILE_DL to cut the file into segments.

```
main ground --FORWARD(file, station)--> RELAY_APP --START(file)--> FILE_DL
                                                                     | START / DATA / END segments
                                                                     v  (FILE_DL_SEG_TLM_MID on the SB)
                         TV_STATION <--UDP-- RELAY_APP  <------------+--> TO_LAB --> main ground
```

- FILE_DL is not changed. RELAY_APP sends it the same START command the ground would send.
- RELAY_APP subscribes to FILE_DL's segments and copies each segment of the requested file,
  unchanged, to the selected station. The station rebuilds the file by byte offset and checks
  the CRC32 in the END segment, the same way as the main ground.
- Stations are a fixed list in `relay_app.c` (`TV_STATION` 127.0.0.1:1240, `BACKUP_STATION`
  127.0.0.1:1241). A command picks one by index and cannot give a free IP address.
- The second station only receives. Missing ranges are asked again by the main ground with
  FILE_DL RESEND; RELAY_APP forwards those segments too until STOP.

## Commands (MID 0x18E0)

| CC | Name | Payload | Result |
|---|---|---|---|
| 0 | NOOP | - | event |
| 1 | RESET_COUNTERS | - | HK counters cleared |
| 2 | FORWARD | `char Filename[64], uint8 StationId, 3 spare` | checks the file and station, sends FILE_DL START, state WAITING |
| 3 | STOP | - | state IDLE (FILE_DL is not stopped) |

FORWARD is rejected while a relay is running, for an unknown station, or if the file does not exist.

## State

| State | Meaning |
|---|---|
| 0 IDLE | segments are ignored |
| 1 WAITING | FORWARD sent START to FILE_DL; waiting for a START segment naming the file |
| 2 FORWARDING | every FILE_DL segment goes to the station; a START segment for another file ends the relay |

## Telemetry

| MID | Packet | Payload |
|---|---|---|
| 0x08E0 | HK (every 4 s from SCH_LAB) | counters, state, station, segments and bytes forwarded |

## Test

`relay_test/run_relay_test.sh -b <build_dir>` puts a random file in `/cf/inbox`, forwards it
to TV_STATION and checks the rebuilt file and its CRC32.

## Uplink (planned, not implemented)

The file is expected in `/cf/inbox/` before FORWARD. A FILE_UL app, the mirror of FILE_DL, would
put it there:

1. The ground sends START (name, size, CRC32), then DATA commands (offset, up to ~900 bytes, so one
   segment fits a 1024-byte TC frame) through CI_LAB.
2. FILE_UL writes each DATA at its offset into `/cf/inbox/<name>.part`, keeps a received-range
   map, and reports the missing ranges in its telemetry so the ground can send them again.
3. On END it checks the size and CRC32, renames the file to `/cf/inbox/<name>` and sends an event;
   an optional "forward to station N when complete" field would then send RELAY_APP FORWARD itself.
