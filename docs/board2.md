# Board 2 measurement station

Board 2 listens for UDP datagrams on port 35102. It reads the MCP3208 over SPI and classifies faults from measured voltages. Board 3 supplies the stimulus. See the [team UDP protocol](udp-protocol.md) for the shared commands.

## Build on the BeagleY-AI

```sh
git clone --branch person-2-measurement https://github.com/parsa-rawr/ensc351_project.git circuitcheck
cd circuitcheck
sudo apt install build-essential cmake pkg-config libgpiod-dev
cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build --target board2_measurement
ls -l /dev/spidev0.0
./build/app/board2_measurement
```

Use `--spi PATH` if the hardware has a different device node. Enable the SPI overlay using the established board setup. If the device exists and access is denied, run with the existing SPI device permissions or use `sudo` for the bench test. Run `ip -4 -brief address show scope global` on the board and share the IPv4 address for the interface connected to the test network.

## Wiring and timing for the first integration

Use the existing [MCP3208 wiring](../README.md#mcp3208-wiring-bare-dip-16). Power off while changing connections, connect the grounds of Boards 2 and 3, and keep the signal inputs between ground and the ADC reference. Channels below refer to the [MCP3208 DIP package](https://ww1.microchip.com/downloads/en/DeviceDoc/21298E.pdf).

| Test | Board 2 input | Board 3 setup |
| --- | --- | --- |
| DIVIDER | CH0, pin 1, at the divider midpoint | Drive the top of the 10 kΩ / 10 kΩ divider LOW during preparation and HIGH after START. Hold HIGH until Board 2 has returned a result. |
| RC | CH2, pin 3, at the capacitor node | Move the Milestone 2 drive connection to Board 3. Keep the 50 kΩ chain between its output and the node and the 10 µF capacitor between the node and common ground. Discharge before acknowledging PREPARE. On START, drive HIGH for 3.5 s, then LOW for at least 3.5 s. |
| PWM | CH1, pin 2, from the Assignment 2 light sensor's analog output | Start the optical PWM after START at 20 Hz and 50% duty, and keep it running until Board 2 returns a result. Adjust the threshold so the sensor's illuminated and dark levels straddle the two hysteresis limits. |

The controller sends `PREPARE` to Board 3, waits for its ACK, sends `ARM` to Board 2, then sends `START` to Board 3 immediately after Board 2 acknowledges ARM. Board 2 begins sampling at ARM and detects the stimulus from the input. Allow about 12 seconds for the default RC run, including UDP retries. Use the same controller socket for commands and acknowledgements. On cancellation send `ABORT` to both boards. Board 3 owns stimulus shutdown.

| Test | Default classification |
| --- | --- |
| DIVIDER | Waits up to 3 s for at least 0.08 V, then waits 100 ms and checks that all 32 samples fall within 1.65 V ± 0.10 V. A low input throughout the detection window gives `NO_SIGNAL`. A detected level outside tolerance gives `WRONG_VALUE`. |
| RC | Estimates charging and discharging tau from interpolated threshold crossings. Both must be within 25% of 0.5 s. Also checks the high and low endpoints. An input that remains low during the 3 s trigger window gives `NO_SIGNAL`. A detected waveform that fails one of the checks gives `WRONG_VALUE`. |
| PWM | Samples every 1 ms and measures rising-edge periods with a 0.10 V hysteresis band around the configured threshold. With the default 20 Hz target it observes at least 1 s of stimulus and checks that individual periods correspond to frequencies within 10% of target. No complete cycle gives `NO_SIGNAL`. A periodic signal outside limits gives `WRONG_FREQ`. |

Use `--help` for all flags. Match `--vref`, `--supply`, `--divider-v`, `--rc-tau`, `--pwm-hz`, and `--pwm-threshold` to the real circuit and nominal stimulus. Keep PWM near 20 Hz for the first test and check reported sample jitter before increasing it. If the sampling gap is excessive during hardware RC or PWM acquisition, Board 2 reports `ERROR,test_id,SAMPLE_GAP`.

## Results and retries

Split `RESULT,test_id,JSON_payload` at the first two commas. The full remainder is JSON. The shared fields are `test_type`, `status` (`PASS` or `FAIL`), `fault`, `simulated`, and `sample_interval_ms` with `min`, `mean`, `max`, `jitter_pp` (peak to peak), and interval `count`.

| Test | Result fields |
| --- | --- |
| DIVIDER | `measured_v`, `expected_v`, `min_v`, `max_v` |
| RC | `charge_tau_s`, `discharge_tau_s`, `expected_tau_s`, `peak_v`, `final_v` |
| PWM | `frequency_hz`, `expected_hz`, `cycles`, `min_v`, `max_v` |

Unmeasurable tau/frequency values appear as JSON `null`. Acknowledge every valid result, including FAIL or a retry, with `ACK,test_id,RESULT`. The station resends the result every 500 ms until that ACK arrives. Duplicate ARM requests replay the prior ACK and any unacknowledged result. An active ABORT closes the ADC acquisition and prevents a late ARM from reusing the ID. If a result already exists, ABORT leaves it available for acknowledgement. The station accepts one active acquisition and keeps up to 4096 distinct IDs in memory for the lifetime of the process. Use fresh IDs and keep the controller's IP and source port stable during each test.

## Verification

Host simulation uses real loopback UDP and simulated readings. Run both the existing diagnostic scenarios and the Board 2 integration tests with:

```sh
cmake -S . -B build-sim -DENABLE_HARDWARE=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build-sim
ctest --test-dir build-sim --output-on-failure
```

For an interactive check on the shared network, start Board 2 and use the controller to send `PING,bench-1` to its IPv4 address on port 35102. Expect `PONG,2,bench-1`. Check DIVIDER first against a multimeter, then record charged and discharged RC tau, then record PWM frequency and jitter. Run each nominal case before repeating with Board 3's fault injection. With simulation off, the service always reads the SPI device. Hardware measurements and integration require the boards.

OpenAI Codex assisted with the Board 2 C service, acquisition algorithms, tests, and documentation. The service reuses the project's Milestone 2 MCP3208 HAL and adapts the Assignment 2 light sampler's hysteresis and interval statistics.
