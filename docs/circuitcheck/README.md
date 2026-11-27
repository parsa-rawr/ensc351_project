# CircuitCheck Pro dashboard

Person 4’s Node.js bridge serves the dashboard and talks only to Board 1. Board
1 coordinates Boards 2 and 3. The browser displays readings and classifications
delivered by the controller. The source has no demo readings or simulated PASS
fallback.

## Run it

Use Node.js 18 or newer. No npm dependencies or build step are required. From
the repo root on your laptop or Board 1:

```bash
CONTROLLER_IP=192.168.1.50 node docs/circuitcheck/server.js
```

Replace the example IPv4 address with Board 1's address. When the dashboard
bridge runs on Board 1 and the controller listens on loopback, use
`CONTROLLER_IP=127.0.0.1`. Open **http://127.0.0.1:8080** from the machine
running Node. If Node runs on Board 1, open an SSH tunnel from your laptop:

```bash
ssh -L 8080:127.0.0.1:8080 debian@192.168.1.50
```

The browser continues to use **http://127.0.0.1:8080**. Run one dashboard instance
per controller. Override `HTTP_PORT`, `CONTROLLER_PORT`, or `DATA_DIR` when
needed. The default data directory is `docs/circuitcheck/data/`. The bridge
keeps the latest 100 completed runs. Reports include verdict, detected fault,
measurements, units in the headers, and any received voltage trace.

## Controller handoff: proposed dashboard protocol

**Person 1 needs to confirm and implement this interface.** The shared board
protocol is unchanged. Board 1 uses UDP 35101 to coordinate Board 2 on 35102
and Board 3 on 35103. Extend Board 1 to bind UDP **35100** for the browser
bridge. Send replies from 35100 to the dashboard datagram's source IP and port.
The dashboard uses an ephemeral UDP port and only accepts packets from its
configured controller address and port.

UDP packets are UTF-8, one message per datagram. IDs and nonces are positive
32-bit signed integers. For a JSON field, split only at the first two commas.
Use the same dashboard test ID when addressing Boards 2 and 3.

| Direction | Message | Meaning |
| --- | --- | --- |
| Dashboard → 1 | `STATUS,<nonce>` | Request Board 1’s latest station states once per second. |
| 1 → dashboard | `STATUS,<nonce>,{"measurement":true,"signal":true}` | Echo nonce and return Boolean station states derived from Board 1’s heartbeats. The reply proves Board 1 is reachable. |
| Dashboard → 1 | `RUN,<id>,<type>,<fault>` | Start a coordinated test. Repeat requests use the same ID. |
| 1 → dashboard | `ACK,<id>,RUN` | Controller accepted the request. Duplicate RUN requests must receive the same ACK without reissuing hardware commands. |
| 1 → dashboard | `SAMPLE,<id>,<JSON>` | Optional live voltage and metric batch. Time is seconds from the start of the capture. |
| 1 → dashboard | `RESULT,<id>,<JSON>` | Final Board 2 verdict and measurements with hardware activity finished. |
| Dashboard → 1 | `ACK,<id>,RESULT` | Result persisted. Board 1 should retry delivery until ACK or a bounded deadline. |
| Dashboard → 1 | `ABORT,<id>` | The controller cancels the run and commands safe shutdown. |
| 1 → dashboard | `ACK,<id>,ABORT` | Cancellation completed and hardware is safe. Acknowledge repeated ABORT for an already stopped test. |
| 1 → dashboard | `ERROR,<id>,<reason>` | Terminal error. Send after safe shutdown, or when the controller refused to start. |

Allow one active test at a time. On RUN, the controller performs the agreed
PREPARE → ARM → START sequence as applicable, waits for the board acknowledgements,
and handles boards timing out or disconnecting. Board 2 should receive the
test type and test ID without the requested fault, infer the fault from its
measurements, and retain its RESULT until Board 1 sends the board-level ACK.
Board 1 separately delivers the outcome to the dashboard. Board 3 owns its
communication-loss watchdog. Browser or bridge shutdown cannot guarantee that a
UDP stop packet arrives.

Supported choices in the dashboard:

- DIVIDER or RC: NONE, WRONG_VALUE.
- PWM: NONE, WRONG_FREQ, NO_SIGNAL.

Confirm which fault combinations Board 3 can physically produce before using
them in the demo. The bridge retries RUN and ABORT with the same ID and waits
up to 60 seconds for a running test before requesting ABORT. A missed ACK stays
visible as an unconfirmed start or stop. If Board 1 stops replying for 3.5
seconds, the dashboard marks it offline and the other stations unknown. Boards
2 and 3 each show online or offline when a fresh status reply is available.

### Measurement payloads

Example messages below specify packet format. Values are illustrative.

```text
SAMPLE,101,{"metrics":{"voltage_v":2.07},"samples":[{"t_s":0.5,"v":2.07,"phase":"charge"}]}
RESULT,101,{"status":"PASS","fault_class":"NONE","metrics":{"expected_tau_s":0.5,"charge_tau_s":0.502,"discharge_tau_s":0.497}}
RESULT,102,{"status":"FAIL","fault_class":"WRONG_FREQ","metrics":{"expected_frequency_hz":20,"frequency_hz":11.9,"jitter_ms":0.21}}
```

`status` is required on RESULT. It must be PASS or FAIL. `fault_class` comes from
Board 2 and is distinct from the requested fault. Optional fields include
`message`, `metrics`, and `samples` on RESULT, and `metrics` and `samples` on
SAMPLE. Supported keys:

| Test | Metrics |
| --- | --- |
| Divider | voltage_v, expected_voltage_v |
| RC | voltage_v, expected_tau_s, charge_tau_s, discharge_tau_s |
| PWM and optical sampling | frequency_hz, expected_frequency_hz, jitter_ms, duty_cycle_pct, light_v, voltage_v |

Use one increasing elapsed time axis across RC phases. If the Milestone 2 CSV
time resets in each phase, add the phase start offset to make times increasing
across the full run. Optional `phase` is `prepare`, `charge`, or `discharge`.
Use small sample batches (ideally below 1200 bytes) and keep RESULT compact.
The bridge sorts early or out of order packets by timestamp, ignores duplicate
timestamps, and accepts up to the latest 1800 points for the dashboard and CSV.
UDP samples can be dropped. Retain the Board 2 capture for a complete trace.

## Start automatically with systemd

Run on the board hosting the dashboard bridge. Use the path to your dashboard
checkout and a Node binary at `/usr/bin/node`:

```bash
sudo install -d /opt/circuitcheck-dashboard
sudo install -m 644 docs/circuitcheck/{server.js,index.html,dashboard.js,style.css} /opt/circuitcheck-dashboard/
sudo install -m 644 docs/circuitcheck/circuitcheck-dashboard.service /etc/systemd/system/
sudoedit /etc/circuitcheck-dashboard.env   # Set CONTROLLER_IP if needed.
sudo systemctl daemon-reload
sudo systemctl enable --now circuitcheck-dashboard
journalctl -u circuitcheck-dashboard -f
```

## Integration order

Run the HTTP/UDP integration check locally before using the boards:

```bash
node --test docs/circuitcheck/integration.test.js
```

The check uses a loopback mock controller and temporary history.

1. Confirm 35100 messages with Person 1 before combining branches. Start
   the controller, both stations, and the dashboard. Confirm all three states
   turn online before running the divider.
2. Run DIVIDER/NONE. Check that the browser result matches the measurement
   station and that a CSV downloads from run history.
3. Run RC/NONE. Check that samples plot during capture, then compare both
   time constants with Board 2 output. Run PWM/NONE next, then PWM/WRONG_FREQ
   or PWM/NO_SIGNAL. Verify the two fault cases using real measurement data.
4. Stop an active run. Disconnect a station during a separate run, observe
   the board status and terminal error, and verify that Board 3 reaches its
   safe state. Reconnect and repeat a normal run.
5. Restart the service and confirm completed results remain in history.
