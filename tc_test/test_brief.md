# TC-SDLP Test (`run_tc_test.sh`)
Starts native `core-cpu1` and sends commands through `tc_frame_proxy.py` (UDP 1236 -> CI_LAB 1234).
- **Good frames:** NOOP and RESET_COUNTERS must run and be logged by CI_LAB.
- **Broken frames:** `length`, `packet`, `short`, `raw` must log a "TC frame dropped" event and must not run NOOP.
- **Result:** prints PASS/FAIL per check; exits 0 only if all 10 checks pass.
