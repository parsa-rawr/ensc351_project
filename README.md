# ENSC 351 project: BeagleY-AI board diagnostics
 It implements a Linux C tool for
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
channel is **CH3**. This is a proposed new wiring plan, not a claim
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

