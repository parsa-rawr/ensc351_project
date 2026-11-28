# CircuitCheck Pro dashboard

Person 4’s Node.js bridge serves the dashboard and talks only to Board 1. Board
1 coordinates Boards 2 and 3. The browser displays readings and classifications
delivered by the controller. The source has no demo readings or simulated PASS
fallback.

## Run it

Use Node.js 18 or newer. Run this from the dashboard branch checkout **on
Board 1**, alongside the controller:

```bash
CONTROLLER_IP=127.0.0.1 node docs/circuitcheck/server.js
```

The controller accepts dashboard traffic at `127.0.0.1:35100`. The HTTP
server listens at `127.0.0.1:8080`. To use a browser on a separate laptop or VM,
open an SSH tunnel (replace the example IP with Board 1's Wi-Fi IP):

```bash
ssh -N -L 8080:127.0.0.1:8080 mosman@10.11.28.51
```

Leave the tunnel open and browse to **http://127.0.0.1:8080** on the machine
running SSH. When testing through Board 1's USB link, use its USB IP from the
computer physically connected to it. You can inspect the bridge locally on
Board 1 with `curl http://127.0.0.1:8080/api/state`. No npm packages are needed.
`HTTP_PORT`, `CONTROLLER_PORT`, and `DATA_DIR` are optional overrides. The
default data directory is `docs/circuitcheck/data/`. The bridge keeps the
latest 100 saved records with CSV exports.

## Board 1 protocol

This dashboard uses the implemented
[controller protocol](https://github.com/parsa-rawr/ensc351_project/blob/mohamad/controller/docs/network-protocol.md).
Each UDP request receives a JSON reply.

| Request | Dashboard behavior |
| --- | --- |
| `STATUS` | Polls Board 1 for controller state and Board 2/3 connectivity. |
| `RUN,<request_id>,<test_type>,<fault>` | Retries reuse the same request ID. The controller's returned string test ID identifies the run. |
| `GET_RESULT,<test_id>` | Polls the controller's retained record, including progress, measurements, and cleanup acknowledgements. |
| `ABORT,<test_id>` | Requests stop. Polling continues until the controller reports the final state. |

A `COMPLETED` run uses the measurement's PASS or FAIL. A controller `FAILED`
run is an execution error even if Board 2 previously reported a PASS. The
dashboard preserves that measurement and the two cleanup acknowledgements.
A controller `ABORTED` run confirms safe cleanup. When the bridge sees a run
created elsewhere, it can display or save it with the requested fault left
blank, because the controller's status does not include that field.

The bridge reads the
[Board 2 result fields](https://github.com/parsa-rawr/ensc351_project/blob/person-2-measurement/docs/board2.md),
including divider voltage, RC time constants, and PWM frequency and sampling
jitter. Board 2's reported fault remains distinct from the requested fault.
It shows a simulation label when the payload says `simulated: true`.

The controller returns measurements as a result object. There is no sample
stream in this protocol. The existing chart and CSV include a trace when Board
2 supplies a `samples` array in its result. A sample uses `t_s` (elapsed
seconds), `v` (volts), and optionally `phase`. The current Board 2 branch reports
summary measurements, so the chart stays empty when only those are available.

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

1. Start
   the controller, both stations, and the dashboard. Confirm all three states
   turn online before running the divider.
2. Run DIVIDER/NONE. Check that the browser result matches the measurement
   station and that a CSV downloads from run history.
3. Run RC/NONE. Compare both time constants with Board 2 output and check the trace if the result includes samples. Run PWM/NONE next, then PWM/WRONG_FREQ
   or PWM/NO_SIGNAL. Verify the two fault cases using real measurement data.
4. Stop an active run. Disconnect a station during a separate run, observe
   the board status and terminal error, and verify that Board 3 reaches its
   safe state. Reconnect and repeat a normal run.
5. Restart the service and confirm completed results remain in history.
