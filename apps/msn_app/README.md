# MSN_APP

Mission app for two demo scenarios. Files on `/cf` stand in for real hardware.

- **Scenario A (control):** read sensor values, switch devices, fire the thruster to change the orbit.
- **Scenario C (log/analysis):** send sensor and maneuver statistics, download the app log.

## Files

| File | Use |
|---|---|
| `/cf/msn_sensor.csv` | Sensor input, one `temperature,voltage,current` line per read. Installed from `bbb_defs/cpu1_msn_sensor.csv`. |
| `/cf/msn_state.txt` | `dev0 dev1 dev2 dev3 altitude_km fuel_kg`. Read at startup, rewritten on every change. Defaults: all off, 400 km, 5 kg. |
| `/cf/msn_log.txt` | `<MET seconds> <text>` lines for startup, device switches and thruster burns. |

## Commands (MID 0x18D0)

| CC | Name | Payload (little endian on BBB/x86) | Result |
|---|---|---|---|
| 0 | NOOP | - | event |
| 1 | RESET_COUNTERS | - | HK counters cleared |
| 2 | READ_SENSOR | - | next CSV line (wraps at the end) as SENSOR tlm |
| 3 | SET_DEVICE | `uint8 DeviceId (0-3), uint8 State (0/1), 2 spare` | state file updated |
| 4 | ARM_THRUSTER | - | arms FIRE for the next command only |
| 5 | FIRE_THRUSTER | `int16 DeltaV (cm/s, max ±1000), 2 spare` | altitude += 1.76 km per m/s, fuel -= 0.05 kg per m/s |
| 6 | GET_ANALYSIS | - | ANALYSIS tlm |
| 7 | GET_LOG | `uint32 Offset` | up to 200 log bytes as LOG tlm; repeat with Offset += Length until Length is 0 |
| 8 | RESET_ANALYSIS | - | statistics cleared, log emptied |

FIRE is rejected (error counter, event and log line) if ARM was not the previous command,
if dV is out of range, or if there is not enough fuel.

## Telemetry

| MID | Packet | Payload |
|---|---|---|
| 0x08D0 | HK (every 4 s from SCH_LAB) | counters, armed flag, 4 device states, altitude, fuel |
| 0x08D1 | SENSOR | sample count, 3 values |
| 0x08D2 | ANALYSIS | sample count, min/max/avg of the 3 values, burn count, total dV, device switch count, uptime |
| 0x08D3 | LOG | file size, offset, length, 200 text bytes |

## Example (native, commands through `tc_test/tc_frame_proxy.py` on port 1236)

```
cmdUtil --port=1236 --endian=LE --pktid=0x18D0 --cmdcode=2
cmdUtil --port=1236 --endian=LE --pktid=0x18D0 --cmdcode=3 --uint8=1 --uint8=1 --uint16=0
cmdUtil --port=1236 --endian=LE --pktid=0x18D0 --cmdcode=4
cmdUtil --port=1236 --endian=LE --pktid=0x18D0 --cmdcode=5 --int16=200 --uint16=0
cmdUtil --port=1236 --endian=LE --pktid=0x18D0 --cmdcode=7 --uint32=0
```

`msn_test/run_msn_test.sh -b <build_dir>` runs both scenarios and checks the telemetry.
