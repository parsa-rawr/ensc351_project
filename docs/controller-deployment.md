# Controller build, deployment, and operations

The controller uses the board addresses supplied at launch. Do not edit the C
defaults when a network address changes. The current lab addresses supplied for
this checkout are Board 1 `10.11.28.51`, Board 2 `10.11.28.152`, and Board 3
`10.11.28.93`; treat these as replaceable configuration values.

The board protocol and UDP ports remain documented in
[`network-protocol.md`](network-protocol.md): controller 35101, measurement
station 35102, stimulus station 35103, and the localhost dashboard interface
35100.

## Native build and interactive launch

Build from the repository root:

```sh
cmake -S . -B build-controller -DENABLE_HARDWARE=OFF -DBUILD_TESTING=ON
cmake --build build-controller -j4
ctest --test-dir build-controller --output-on-failure
```

Set Board 2 and Board 3 addresses when launching. The Board 1 listener defaults
to `0.0.0.0`; set `CONTROLLER_BIND_IP` to a specific interface address if
needed:

```sh
export BOARD2_IP=10.11.28.152
export BOARD3_IP=10.11.28.93
export CONTROLLER_BIND_IP=0.0.0.0
./controller/scripts/run_controller.sh
```

The values above are examples for the currently reported lab network. Update
them whenever the boards receive different addresses. The startup script also
accepts `COMMAND_TIMEOUT_MS`, `MEASUREMENT_TIMEOUT_MS`, `CLEANUP_TIMEOUT_MS`,
`RETRIES`, and `CONTROLLER_BIN`. Defaults are 500 ms, 15 s, 1 s, and 2 retries.
Use measurement timeouts long enough for the selected physical acquisition.

The Python dashboard client uses the existing localhost API and fields; it does
not alter the wire protocol:

```sh
python3 controller/tools/controller_cli.py status
python3 controller/tools/controller_cli.py run DIVIDER --fault NONE --wait
python3 controller/tools/controller_cli.py result TEST_ID
python3 controller/tools/controller_cli.py abort TEST_ID --wait
```

`run` accepts `DIVIDER`, `RC`, or `PWM`. It generates a request ID unless
`--request-id` is supplied. `--wait` polls GET_RESULT; omitting it returns the
initial RUN response. The client retries each dashboard request with the same
message so repeating RUN remains idempotent.

## ARM64 static build and guarded SCP deployment

Install a native `aarch64-linux-gnu-gcc` toolchain with static libc development
files, plus CMake and OpenSSH client tools. Build and inspect without using the
network:

```sh
./controller/scripts/build_deploy_arm64.sh --build-only
file build-arm64-controller/controller/circuit_controller
```

The script configures `ENABLE_HARDWARE=OFF`, targets only
`circuit_controller`, links statically, and verifies that `file` identifies an
AArch64 static executable. A transfer requires configurable SSH settings and
passes an SSH reachability check before it invokes SCP:

```sh
TARGET_SSH_USER=debian \
TARGET_SSH_ADDRESS=10.11.28.51 \
./controller/scripts/build_deploy_arm64.sh
```

`REMOTE_BINARY_PATH` can override the destination. The script uses existing SSH
key/agent authentication; it contains no credentials. If SSH cannot reach the
target, the script exits without running SCP. Do not run the transfer while the
board is disconnected.

## Install the systemd service

On Board 1, install the ARM64 executable at `/usr/local/bin/circuit_controller`
and the service unit. Create the unprivileged service account once, then copy
and edit the environment file with the current board addresses:

```sh
sudo id -u circuitcheck >/dev/null 2>&1 || \
  sudo useradd --system --no-create-home --shell /usr/sbin/nologin circuitcheck
sudo install -D -o root -g root -m 0755 \
  /home/debian/circuit_controller /usr/local/bin/circuit_controller
sudo install -D -o root -g root -m 0644 \
  deploy/systemd/circuit_controller.service \
  /etc/systemd/system/circuit_controller.service
sudo install -D -o root -g root -m 0640 \
  deploy/systemd/controller.env.example /etc/circuitcheck/controller.env
sudoedit /etc/circuitcheck/controller.env
```

Replace `CHANGE_ME` with current Board 2 and Board 3 IPv4 addresses before
starting the service. Set `CONTROLLER_BIND_IP` and the four timeout/retry values
there if defaults are unsuitable. The service waits for
`network-online.target`, restarts after nonzero exits, sends SIGTERM on stop, and
allows up to 15 seconds for the controller's bilateral ABORT cleanup. Logs go
to journald.

Enable and inspect it:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now circuit_controller.service
sudo systemctl status circuit_controller.service
sudo journalctl -u circuit_controller.service -f
```

`network-online.target` only waits for link configuration when the matching
wait-online service is enabled. For systemd-networkd use
`systemd-networkd-wait-online.service`; for NetworkManager use
`NetworkManager-wait-online.service`. Enable the one matching the board's
network manager. Check `systemctl list-dependencies network-online.target` if
the controller starts before its route is ready.

## Network configuration and troubleshooting

All three stations must use the same reachable IPv4 network, and the remote
services must listen on UDP 35102 and 35103. Board 1 listens on UDP 35101 and
the dashboard API binds only to localhost UDP 35100. Allow the needed UDP
traffic in host and network firewalls. Replies from Boards 2 and 3 must come
from the configured station IP and source port; packets from another endpoint
are ignored.

Useful checks on Board 1:

```sh
ip -4 -brief address show scope global
ip route get 10.11.28.152
ss -lunp | grep -E ':(35100|35101)\b'
python3 controller/tools/controller_cli.py status
sudo journalctl -u circuit_controller.service --since '-5 minutes'
```

If a board shows offline, confirm its current IP, route, service state, UDP
listener, firewall rules, and that `controller.env` contains the current
address. Board 2/3 may reply to discovery but remain marked offline if their
configured endpoint or source port does not match. Increase
`COMMAND_TIMEOUT_MS` for a slow LAN; increase `MEASUREMENT_TIMEOUT_MS` for a
long acquisition. Cleanup timeout and retries control how long the controller
waits for each station's independent ABORT ACK. The final JSON includes
`cleanup_acknowledgements.board2` and `.board3` as `confirmed` or
`unconfirmed`.

## Reproducible recovery demonstration

The host integration suite exercises disconnection, recovery, delayed responses,
loss, duplicate packets, repeated requests, sequential tests, and shutdown:

```sh
cmake --build build-controller -j4
ctest --test-dir build-controller --output-on-failure
```

For a controlled physical demonstration, first use an approved safe test setup
and confirm Board 3's local watchdog is active. Check both boards with
`controller_cli.py status`, start a test with `--wait`, then interrupt one
station service during a separate run. Confirm the controller reports the
offline board and each cleanup ACK independently. Restore the station and wait
for its heartbeat to mark it online; then run a new test ID. Do not use a
physical disconnection test until the stimulus safe-stop watchdog has been
verified on Board 3. Simulation results are not physical hardware validation.

## Known teammate branch compatibility issues

The fetched `origin/person-2-measurement` branch uses the same UDP ports and
PREPARE/ARM/START/ABORT command flow. Its Board 2 documentation currently
specifies a 3.5-second RC charge, at least 3.5 seconds of discharge, and ±25%
time-constant tolerance. This controller's shared specification is 3 seconds
per phase and ±20%. Agree on one RC profile before combining these branches;
otherwise Board 2 and Board 3 may evaluate different acquisition windows and
acceptance limits. The teammate branch was not changed.

The fetched `origin/circuitcheck-pro-dashboard` branch currently proposes a
different Board 1 dashboard datagram API (`STATUS,<nonce>`, numeric RUN IDs,
ACK/RESULT push messages, and `measurement`/`signal` state keys). This
controller's published localhost API uses `STATUS`, textual request IDs,
GET_RESULT, and the existing JSON fields such as `board2_connected` and
`board3_connected`. The board protocol and ports align, but the dashboard
bridge needs an agreed adapter or protocol revision before those branches are
combined. No teammate branch was modified.

The older `origin/parts1-6` branch contains hardware diagnostics work but no
matching distributed-controller protocol; it also changes overlapping root
CMake, app, and HAL files. Integrate those hardware changes selectively rather
than merging that branch wholesale into the controller branch.
