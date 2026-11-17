# ENSC 351: BeagleY-AI hardware diagnostics

A Linux C11 application for **MCP3208 SPI measurements**, voltage-divider
validation, **RC charging/discharging and time-constant estimation**, and GPIO
loopback. The kit's resistors and capacitor provide the RC circuit; no digital
logic IC is needed. The divider and RC circuits can stay connected on separate
ADC channels. The web dashboard is a separate prototype and is not connected by
this repository. RC sample CSV capture is implemented; a persistent run-history
service is not.

## Build and run

Host simulation (GCC, CMake >= 3.18, Python 3):

```sh
cmake -S . -B build-sim -DENABLE_HARDWARE=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build-sim
ctest --test-dir build-sim --output-on-failure
./build-sim/app/board_diagnostics --simulate --test all
./build-sim/app/board_diagnostics --simulate --test rc --csv simulated-rc.csv
./build-sim/app/board_diagnostics --simulate --test rc --fault rc-fast
```

Simulation uses deterministic **virtual time** and an independent fixed physical
model: a 10k/10k divider at 3.3 V and a 50k/10uF RC network (tau = 0.5 s).
Changing expected component values does not change that model. Faults include
`divider-low`, `rc-fast` (0.1 s), `rc-slow` (2 s), `rc-stuck-low`,
`rc-stuck-high`, `adc-error`, and `gpio-error`. These are software tests,
not physical measurements or Linux scheduling/timing validation.

Hardware build on the BeagleY-AI (Debian packages):

```sh
sudo apt install build-essential cmake pkg-config libgpiod-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF
cmake --build build
./build/app/board_diagnostics --test divider
./build/app/board_diagnostics --test rc --csv hardware-rc-001.csv
./build/app/board_diagnostics --test all --csv hardware-all-001.csv
```

Run a matching ARM executable on the board, not a host/VM executable. If
cross-compiling, use the correct target toolchain and sysroot. Check `uname -m`
and `file` before deployment. CMake supports libgpiod 1.x (>=1.4) and 2.x.
`-DDEPLOY_DIR=/your/path` optionally copies the binary after building.
`-DENABLE_ASAN=ON` enables AddressSanitizer where its runtime is available.

`--test all` runs **divider then RC**; GPIO loopback is a separate wiring setup.
Default mode is divider. Exit codes: 0 = pass/measurement completed, 1 = circuit
failure, 2 = invalid configuration or acquisition/I/O error, 130 = interrupted.
SIGINT/SIGTERM stop acquisition and trigger resource cleanup. GPIO drive is
reset low before release. Check exit status as well as printed results.

## MCP3208 wiring (bare DIP-16)

Confirm the chip marking is **MCP3208** before using this table. The MCP3204
has a different package pinout; do not apply this table to it. Count pins using
the package notch/dot and the datasheet top view. Power off before rewiring.
Use 3.3 V throughout, a common ground, and a breadboard.

| BeagleY-AI physical HAT pin / signal | MCP3208 pin / signal |
| --- | --- |
| 1 or 17: 3.3 V | 16 VDD and 15 VREF |
| 6: GND | 9 DGND and 14 AGND |
| 19: MOSI | 11 DIN |
| 21: MISO | 12 DOUT |
| 23: SCLK | 13 CLK |
| 24: CE0 | 10 CS/SHDN |
| Divider midpoint | 4 CH3 |
| RC capacitor node | 3 CH2 |

Fit a 0.1 uF capacitor close to ADC VDD/GND. Keep analog leads short.
The SPI bus must already be enabled, and the default device `/dev/spidev0.0`
must exist and be accessible. Follow the course SPI/overlay guide for the board
image; preserve other boot settings. The application does not edit boot files
or enable pin multiplexing. `--spi PATH` selects a different SPI device.
SPI uses mode 0, 8 bits/word, 250 kHz, and a three-byte conversion transaction.
Voltage conversion is `raw * VREF / 4096.0` for this 12-bit ADC.

References: [Microchip MCP3204/3208 datasheet](https://ww1.microchip.com/downloads/en/DeviceDoc/21298E.pdf),
[BeagleY-AI pinout](https://pinout.beagleboard.io/), and
[GPIO setup](https://docs.beagleboard.org/boards/beagley/ai/demos/using-gpio.html).

## Circuit 1: voltage divider

Wire 3.3 V through a 10k resistor to the divider midpoint, then through a
second 10k resistor to ground. Connect the midpoint to MCP3208 CH3.
Expected voltage is `supply * Rbottom / (Rtop + Rbottom)` = 1.65 V at 3.3 V.
The default check takes 32 samples, and the mean AND every sample must be
within +/-0.10 V of expectation.

Use `--rtop`, `--rbottom`, `--supply`, `--vref`, `--tolerance`, `--samples`, and
`--channel` to configure the check. Measure supply/reference with a multimeter
for calibration; the software does not automatically calibrate the hardware.
`--test adc` prints raw samples and voltages without applying circuit limits.

For a deliberate fault, power off and add another 10k resistor above the node:
20k top / 10k bottom gives about 1.10 V. Keep software expecting the original
10k/10k divider to demonstrate FAIL.

## Circuit 2: RC charging and discharging

1. Physical HAT pin **15 (GPIO22)** drives five 10k resistors in series (50k).
2. The far end of that chain is the RC node. Connect it to MCP3208 **CH2**.
3. Connect a **10 uF capacitor** between that node and common ground.
4. For a polarized capacitor, its positive lead goes to the node and negative
   lead to ground. Keep the resistor chain between GPIO and capacitor.

This uses seven of the ten kit resistors including the divider. Do not connect
the RC node to the divider midpoint. Do not drive a board input above 3.3 V.

```sh
./build/app/board_diagnostics --test rc --resistance 50000 --capacitance-uf 10 \
  --tau-tolerance 0.25 --interval-ms 5 --rc-channel 2 --drive GPIO22 \
  --csv hardware-rc-002.csv
```

Expected tau is `R*C = 50000 * 10e-6 = 0.5 s`. The application:

- Drives LOW and samples an initial discharge phase to check baseline.
- Drives HIGH and estimates charging tau at 63.2% of the excursion from the
  initial voltage to configured GPIO HIGH voltage (`--supply`, default 3.3 V).
- Drives LOW and estimates discharge tau at 36.8% of the charged voltage.
- Checks charged/discharged endpoints against `--tolerance` and **both** tau
  estimates against `--tau-tolerance` (fraction, default 0.25 = +/-25%).

Each phase lasts 7 expected time constants by default (3.5 s; about 10.5 s total
on hardware). `--window` changes that per-phase duration, with a minimum of 5
expected time constants. Missing threshold crossings or bad endpoint voltages
produce FAIL, not a fabricated tau. ADC/GPIO/file failures produce ERROR.

Samples use CLOCK_MONOTONIC timestamps and threshold-bracket interpolation.
This is ordinary Linux acquisition, not hard real-time. An acquisition/sample
gap above 20% of expected tau produces ERROR. Requested intervals must be no
more than tau/10. Supported expected tau is 0.02..10 s; tighter tolerances must
account for capacitor/resistor tolerances, GPIO voltage, quantization, and
scheduling. Start with the supplied slow 0.5 s network.

`--csv` exclusively creates a new file with `phase,time_s,raw,volts` columns.
Time is elapsed within each phase. Files from aborted/error runs may contain
partial samples; retain terminal output/exit code alongside the CSV. A reused
filename is an ERROR to avoid overwriting earlier evidence.

For a deliberate RC fault, power off and replace the 50k chain with one 10k
resistor, keeping C = 10 uF. Actual tau should become about 0.1 s. Keep the
software expecting 50k/10uF so it detects the mismatch. The simulator equivalent
is `--simulate --test rc --fault rc-fast`.

## Optional GPIO loopback

Disconnect the RC drive circuit first. Wire physical pin 15 (GPIO22) to
physical pin 18 (GPIO24), then run:

```sh
./build/app/board_diagnostics --test loopback --drive GPIO22 --sense GPIO24
```

This checks LOW and HIGH, five observations each. It is an interface bring-up
check, not a digital-IC test. The old three-pin logic-test options are removed;
use `--drive` and `--sense` for this two-pin loopback.

## Structure and verification

`app/src/main.c` handles validated options/signals; `app/src/diagnostics.c`
implements divider/RC/loopback checks; `hal/src/hardware.c` owns SPI, GPIO,
timing, simulation, and cleanup. No background threads are used. `tests/smoke.py`
checks numeric RC estimates, independent wrong-component expectations, injected
faults, both loopback levels, invalid arguments, CSV contents/overwrite refusal,
and SIGINT/SIGTERM. CTest runs this suite. See `docs/verification.md` for the
recorded host results; physical results must be collected on the board.
