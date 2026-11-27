# CircuitCheck Pro board communication

Connect the three BeagleY-AI boards and the dashboard computer to the same LAN. Keep the USB cable attached for SSH if you use it. On each board run:

```sh
ip -4 -brief address show scope global
```

Share the board number and the address on the common LAN with the group. The USB address works from the directly connected host. Use the shared LAN address when running the controller on another device.

| Endpoint | UDP port | Service |
| --- | ---: | --- |
| Board 1 | 35101 | The team's controller, or the bench tool during an isolated test |
| Board 2 | 35102 | `board2_measurement` |
| Board 3 | 35103 | The team's signal generator |
| Dashboard to Board 1 | 35100 | The team's dashboard interface |

## Check discovery

Start each board's service. Board 2 setup is in the [measurement guide](board2.md). On Board 1 or a bench laptop on the same LAN, use Python 3.8 or newer. Run these commands from the repository and replace the quoted values with the shared-network addresses:

```sh
BOARD1_IP='REPLACE_WITH_BOARD_1_IP'
BOARD2_IP='REPLACE_WITH_BOARD_2_IP'
BOARD3_IP='REPLACE_WITH_BOARD_3_IP'

python3 tools/board_udp.py ping --board 1 --host "$BOARD1_IP"
python3 tools/board_udp.py ping --board 2 --host "$BOARD2_IP"
python3 tools/board_udp.py ping --board 3 --host "$BOARD3_IP"
```

Each check prints an online message after it gets a matching `PONG`. These checks use an ephemeral source port, so the production controller can stay running for discovery. From a host connected directly to Board 2 over USB, you can use the address reported by the board's USB interface for a local check.

## Run the first wired test

Wire CH0 and the divider per the [Board 2 guide](board2.md). Confirm that Board 3 drives the divider low during PREPARE and can idle safely on ABORT. Stop the production controller before running the bench tool on the same host so UDP 35101 is available. Use one controller at a time.

```sh
python3 tools/board_udp.py run --board2 "$BOARD2_IP" --board3 "$BOARD3_IP" --test DIVIDER
```

The bench tool pings Boards 2 and 3, sends PREPARE to Board 3, ARMs Board 2, and sends START to Board 3. It keeps one source socket, retries incomplete commands with the same test ID, acknowledges received results, prints the JSON, and sends ABORT to both peers for cleanup. Ctrl+C requests the same cleanup. Missing ABORT acknowledgements are reported so you can check Board 3's output directly. The bench tool exits 0 for PASS, 1 for a measurement FAIL, and 2 for communication or cleanup errors.

With the nominal 10 kΩ / 10 kΩ divider at 3.3 V, expect about 1.65 V. For RC or PWM substitute `--test RC` or `--test PWM` after connecting the corresponding circuit. To ask Board 3 to inject an applicable fault, add `--fault WRONG_VALUE`, `--fault WRONG_FREQ`, or `--fault NO_SIGNAL`. Board 2 receives only the test type.

Use the [team UDP protocol](udp-protocol.md) for the production controller. The bench tool covers board discovery and test sequencing. Dashboard integration uses the reserved dashboard port and the team's controller and dashboard software.
