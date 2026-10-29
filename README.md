# ENSC 351 project: BeagleY-AI board diagnostics (steps 1--6)

This is a separate project from Assignment 2. It implements a Linux C tool for
external MCP3208 SPI measurements, GPIO bring-up, voltage-divider validation,
and a full truth-table test of **one gate of a 74HC00 NAND IC at 3.3V**.
It does not include persistent result logging, a web dashboard or RC testing.
Those are later milestones. Physical wiring and on-board results remain to be
verified; host simulation is not hardware validation.

## Starter structure

The build is adapted from your supplied `cmake_starter (1).zip`: root CMake
settings, an `app/` executable linked to a `hal/` static library, public
`hal/...` headers, C11, strict compiler warnings and pthread linkage. The
starter's demonstration bugs are not used. The executable/deployment path is
changed to this project and AddressSanitizer is an option rather than always on.

```text
CMakeLists.txt
app/CMakeLists.txt
app/include/diagnostics.h
app/src/{main.c,diagnostics.c}
hal/CMakeLists.txt
hal/include/hal/hardware.h
hal/src/hardware.c
tests/smoke.py
```

`hardware` hides SPI, GPIO version differences and the simulator. `diagnostics`
owns expected-versus-measured checks. Main parses configuration, handles
signals and guarantees cleanup. There are no background threads or external
GPIO subprocesses. Pin ownership and descriptors are released on errors too;
GPIO inputs to the IC are driven low before release. A cleanup error is an
ERROR exit, not a successful test run.

## 1. Build on Linux or in the VM

A host VM can run the simulator. To access physical hardware, deploy a matching
ARM executable to the BeagleY-AI or build natively there. The VM's `/dev` is
not automatically the board's `/dev`. Check `uname -m` and `file` before
deploying; do not use an old BeagleBone compiler kit without checking its ABI.

Simulation-only build (C compiler, CMake >=3.18, Python 3 for tests):

```sh
cmake -S . -B build-sim -DENABLE_HARDWARE=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build-sim
ctest --test-dir build-sim --output-on-failure
./build-sim/app/board_diagnostics --simulate
./build-sim/app/board_diagnostics --simulate --test all
```

Hardware build requires target `libgpiod-dev`, pkg-config and Linux SPI headers.
Both libgpiod 1.x (>=1.4) and 2.x APIs are supported; CMake selects the backend
using the installed library version. On Debian/Ubuntu install the applicable
native packages, for example:

```sh
sudo apt install build-essential cmake pkg-config libgpiod-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF
cmake --build build
```

For cross compilation use the board's correct toolchain and sysroot containing
target libgpiod. Keep host and target build directories separate. There is no
hard-coded NFS destination; optional `-DDEPLOY_DIR=/your/destination` copies
the executable after a build. `-DENABLE_ASAN=ON` adds memory instrumentation
when its matching runtime is available. Do performance checks without it.

## Hardware choices

The ADC is the external **MCP3208**, not the Zen Hat's I2C ADC. Divider default
channel is **CH3**, leaving the Assignment 2 photocell's CH2 and the earlier
joystick's CH0/CH1 unmodified. This is a proposed new wiring plan, not a claim
that hardware is already connected.

Configure the board's spidev0 overlay according to the supplied course SPI
guide. Check installed filenames first, then preserve existing boot settings
while adding the matching overlay to the active entry of
`/boot/firmware/extlinux/extlinux.conf`:

```text
fdtoverlays /overlays/k3-am67a-beagley-ai-spidev0.dtbo
```

Reboot after enabling it, then confirm `ls -l /dev/spidev0.0`. This project does
not need a PWM overlay; do not remove overlays used by Assignment 2. It does not
edit boot files or reboot automatically. Use SPI mode 0, 8 bits per word,
250kHz. Each ADC conversion is one 3-byte `SPI_IOC_MESSAGE(1)` transaction.
Decoded code is `((rx[1] & 0x0F) << 8) | rx[2]`; voltage is
`raw / 4095.0 * reference_voltage`. There is no sysfs ADC or I2C path.

Power off before changing breadboard wiring. Use 3.3V throughout and a common
ground; do not connect a 5V-powered logic output to board inputs. The pin
tables below assume **bare MCP3208 DIP-16 and 74HC00 DIP-14 packages**, counted
using the package's notch/dot. They are not breakout connector positions.

### SPI ADC connections

| BeagleY-AI physical HAT pin | MCP3208 pin |
| --- | --- |
| 1 or 17: 3.3V | 16 VDD and 15 VREF |
| 6: ground | 9 DGND and 14 AGND |
| 19: SPI0 MOSI | 11 DIN |
| 21: SPI0 MISO | 12 DOUT |
| 23: SPI0 SCLK | 13 CLK |
| 24: SPI0 CE0 | 10 CS/SHDN |
| Divider midpoint | 4 CH3 |

Fit a 100nF bypass capacitor near ADC VDD/GND. The ADC and pin mappings follow
the [Microchip datasheet](https://ww1.microchip.com/downloads/en/DeviceDoc/21298E.pdf)
and [BeagleY-AI SPI pinout](https://pinout.beagleboard.io/pinout/spi).

## 2. Verify GPIO first with a loopback

Proposed lines are A=`GPIO22` (physical HAT15), B=`GPIO23` (HAT16),
Y=`GPIO24` (HAT18). These are **line names**, not gpiochip offsets. Check with
`gpioinfo` that they exist, are configured for GPIO, and are unused by the
attached HAT or other software before using them. If they conflict, choose
free lines and specify `--a NAME --b NAME --y NAME`; update the wiring to match.
The library requests directions itself. Do not reserve these lines through
sysfs or a separate GPIO process at the same time.

For this bring-up check, **disconnect the logic IC and wire A directly to Y**.
B is requested as an output held low; leave it unconnected. Never run this
loopback wiring against a connected IC output. Then run on the board:

```sh
sudo ./build/app/board_diagnostics --test loopback
```

The tool drives 0 then 1, waits 5ms after each stimulus, and requires five
matching observations per state. After checking, power off and remove this
jumper before wiring the logic IC.

## 3--5. Divider wiring, manual check and automatic validation

Use two 10k ohm resistors:

```text
3.3V --- Rtop 10k ---+--- Rbottom 10k --- GND
                    |
              MCP3208 CH3 (chip pin 4)
```

Expected voltage is `Vsupply * Rbottom / (Rtop + Rbottom)` = 1.65V nominally.
First measure the midpoint, supply and ADC reference with a multimeter. Confirm
the node stays between ground and VREF. Read and compare ADC values manually:

```sh
sudo ./build/app/board_diagnostics --test adc --channel 3 --samples 10
```

ADC mode prints each raw/voltage reading and a summary. It reports MEASURED,
not a circuit PASS: a plausible code alone does not establish correct wiring.
For the automated divider check:

```sh
sudo ./build/app/board_diagnostics --test divider
```

Defaults are 32 samples with a 1ms delay between them, 3.3V supply/reference,
10k/10k resistance and **+/-0.10V absolute tolerance**. This is an engineering
acceptance window for the initial circuit, not an automatic resistor error
budget or ADC accuracy specification. It requires every collected sample and
their average to lie in the expected window, so unstable excursions cannot
cancel out in the mean. It prints expected/mean/error, min/max and PASS/FAIL.
Set actual known component values and measured supply/reference as needed:

```sh
sudo ./build/app/board_diagnostics --test divider --rtop 10000 --rbottom 10000 \
  --supply 3.30 --vref 3.30 --tolerance 0.10 --samples 32
```

These options describe the circuit; they do not control the physical supply.
To demonstrate a real failure, keep the expected values at 10k/10k but replace
Rtop with 22k with power off. After powering back on, expect about 1.03V and
FAIL. If you change `--rtop` to 22000, that instead declares the new circuit
intentional and should pass when the hardware matches it.

## 6. Test a 74HC00 logic IC

Use a **74HC00 powered at 3.3V**, not a 74HCT00 or a 5V-only substitution.
This milestone tests the complete truth table of **gate 1**, not all four gates
or the chip's analog thresholds, propagation delay or output current limits.
The chosen family and pinout follow the [TI SN74HC00 datasheet](https://www.ti.com/lit/ds/symlink/sn74hc00.pdf).

| Connection | 74HC00 DIP-14 pin |
| --- | --- |
| 3.3V | 14 VCC |
| Common GND | 7 GND |
| GPIO22 / HAT15 (A output from board) | 1 gate-1 A input |
| GPIO23 / HAT16 (B output from board) | 2 gate-1 B input |
| GPIO24 / HAT18 (Y input to board) | 3 gate-1 Y output |
| GND: unused gate inputs | 4, 5, 9, 10, 12, 13 |
| Leave unused outputs unconnected | 6, 8, 11 |

Fit a 100nF bypass capacitor across pins 14/7. Do not leave unused CMOS inputs
floating. Remove the A-to-Y loopback jumper before connecting pin 3.

```sh
sudo ./build/app/board_diagnostics --test logic
```

| A | B | Expected Y |
| --- | --- | --- |
| 0 | 0 | 1 |
| 0 | 1 | 1 |
| 1 | 0 | 1 |
| 1 | 1 | 0 |

Each row waits 5ms for settling, then takes five readings 1ms apart. Every
reading must match; output shows high/low counts and row PASS/FAIL. Both board
outputs are restored low before release. Input changes are sequential and
may briefly pass through another combination; only settled values are tested.

`--test logic` does not open SPI; `--test divider` does not request GPIO. When
both circuits are ready, `--test all` runs both and reports the combined result.

## Results, simulation and checks

Exit codes: **0** = checks pass (or ADC measurement command completed),
**1** = measured circuit fails, **2** = invalid settings or I/O/setup/cleanup
error, **130** = interrupted. Missing hardware produces ERROR, never PASS.
Output goes only to the terminal. Persistent logging belongs to milestone 8.

The simulator models an independent fixed 3.3V 10k/10k divider and NAND gate;
it does not manufacture a voltage from your expected resistor arguments.
It supports deliberate faults for testing the diagnostic decisions:

```sh
./build-sim/app/board_diagnostics --simulate --fault divider-low
./build-sim/app/board_diagnostics --simulate --test logic --fault logic-stuck-low
./build-sim/app/board_diagnostics --simulate --fault adc-error
```

`--fault` requires simulation. All simulated runs print a SIMULATION banner.
It cannot establish real voltage accuracy, pinmux correctness or board behavior.
Use `--help` for the full option list. Stop with Ctrl+C; cleanup runs on SIGINT
or SIGTERM. SIGKILL/power loss cannot run userspace cleanup; released GPIO
directions/levels should not be relied upon as permanent stimulus.

Before claiming milestones complete on hardware, record actual checks for:
board build/run; both loopback levels; ADC readings versus multimeter; correct
divider PASS and wrong-resistor FAIL; all four NAND rows; and graceful shutdown.
No physical-board measurements have been performed during source preparation.
The `tests/smoke.py` suite checks simulated pass/fail cases, bad configuration,
I/O errors and interruption without extra Python packages.

Source-preparation checks completed in WSL: simulation-only and libgpiod 1.6
hardware builds with warnings treated as errors; CTest end-to-end scenarios;
the same simulator scenarios under AddressSanitizer with no reported errors;
and a compile-only check of the libgpiod 2.x branch against the official 2.2
header. The 2.x branch has not been linked to a 2.x library or run on a board.
The supplied starter's app/HAL layout is used in both builds. These checks
establish software behavior, not completion of physical milestones 1--6.
