# Host verification: divider and RC diagnostics

Verified on a Linux x86-64 host with GCC 14.2.0 and Python 3.12.
These are host simulation results, not physical board measurements.

- CMake simulation-only build: passed with strict warnings treated as errors.
- CTest diagnostic_scenarios: passed, covering 51 CLI scenarios.
- Independent GCC build with -fanalyzer: passed without diagnostics.
- Hardware-enabled native builds against real libgpiod 1.6.2 and 2.1.3 headers
  and libraries: compiled with strict warnings/static analysis; all 51 simulation
  scenarios passed for each binary. This verifies both API build paths, not
  physical GPIO/SPI access or an ARM cross-build.
- AddressSanitizer build and the same 51 scenarios: passed with
  ASAN_OPTIONS=detect_leaks=0. LeakSanitizer cannot run under this managed
  environment's process tracing, so leak detection was not verified.
- Tests check charging/discharging tau against the independent 0.5 s model;
  wrong R/C expectations; fast, slow and stuck-voltage circuits; ADC/GPIO
  errors; two loopback levels; CSV numeric/phase/timestamp integrity;
  refusal to overwrite files; invalid options; and SIGINT/SIGTERM.

Reproduce normal verification:

```sh
cmake -S . -B build-sim -DENABLE_HARDWARE=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build-sim
ctest --test-dir build-sim --output-on-failure
```

Captured combined run:

```text
SIMULATION: synthetic circuits and virtual time; not hardware measurements or timing.
ADC device=/dev/spidev0.0 divider_channel=3 rc_channel=2 reference=3.3000V
GPIO drive=GPIO22
ADC SUMMARY samples=32 min=1.6484V max=1.6508V mean=1.6496V
DIVIDER expected=1.6500V measured=1.6496V error=-0.0004V tolerance=+/-0.1000V
RESULT PASS: voltage divider
RC expected_tau=0.500000s R=50000ohm C=10.000uF tolerance=+/-25.0% interval=0.0050s window=3.500s
RC endpoints initial=0.0000V charged=3.2968V discharged=0.0024V
RC charge_tau=0.500055s
RC discharge_tau=0.499879s
RESULT PASS: RC charge/discharge
OVERALL PASS
```

Before collecting physical evidence, verify SPI availability, GPIO mapping,
actual supply/reference voltages, wiring and component values on the BeagleY-AI.
Use a NEW --csv filename and retain terminal results and exit status. Photograph
the normal circuit, then power off and change only the RC resistance from 50k
to 10k, keeping software expecting 50k, to demonstrate fault detection.
