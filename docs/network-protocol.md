# CircuitCheck Pro distributed protocol

This document defines the UDP interface for the Board 1 controller and the
Board 2/Board 3 stations. Messages are UTF-8, comma-delimited text datagrams
with no trailing newline. The controller accepts IPv4 board addresses through
`--board2-ip` and `--board3-ip`; the board UDP ports are fixed by this version
of the protocol.

## Ports and message routing

| Role | Address | UDP port |
| --- | --- | ---: |
| Dashboard client | localhost only | 35100 |
| Board 1 controller | configurable bind address, default `0.0.0.0` | 35101 |
| Board 2 measurement station | configured IP | 35102 |
| Board 3 stimulus station | configured IP | 35103 |

Board commands and replies use the source IP and port configured for that
station. The controller rejects board traffic from any other endpoint. Dashboard
datagrams must originate from loopback and their replies go to the request's
source endpoint. The controller handles one dashboard request per datagram and
returns one JSON datagram.

## Board protocol

Fields in the command forms below are literal comma-separated fields. `test_id`
and `nonce` contain no commas. Board numbers are decimal `2` or `3`.

| Direction | Message | Purpose |
| --- | --- | --- |
| Either | `PING,nonce` | Heartbeat probe |
| Reply to PING | `PONG,board_number,nonce` | Heartbeat response; echo nonce |
| Board 1 → Board 3 | `PREPARE,test_id,test_type,fault` | Configure stimulus and fault |
| Board 3 → Board 1 | `ACK,test_id,PREPARE` | Stimulus is configured |
| Board 1 → Board 2 | `ARM,test_id,test_type` | Prepare measurement |
| Board 2 → Board 1 | `ACK,test_id,ARM` | Measurement is ready |
| Board 1 → Board 3 | `START,test_id,test_type` | Begin stimulus |
| Board 3 → Board 1 | `ACK,test_id,START` | Stimulus has started |
| Board 2 → Board 1 | `RESULT,test_id,JSON_payload` | Measurement result |
| Board 1 → Board 2 | `ACK,test_id,RESULT` | Result was accepted |
| Board 1 → Board 2 and Board 3 | `ABORT,test_id` | Stop acquisition and stimulus; return to safe idle |
| Board 2 → Board 1 | `ACK,test_id,ABORT` | Acquisition is stopped |
| Board 3 → Board 1 | `ACK,test_id,ABORT` | Stimulus is stopped |
| Either station → Board 1 | `ERROR,test_id,reason` | Station reports execution error |

The controller also answers station-originated `PING,nonce` with
`PONG,1,nonce`. Board stations should answer duplicate commands idempotently:
repeating a command with the same test ID must not start another physical
operation. They should repeat a pending RESULT until `ACK,test_id,RESULT` is
received. Both stations must treat ABORT as idempotent and ACK it even when the
test is already safely idle. The controller acknowledges duplicate valid
results and ignores stale or duplicate command ACKs unless the matching command
and test ID are currently awaited.

`RESULT` is split only at its first two commas. The complete remainder is the
JSON payload, so JSON strings, objects, and arrays may contain commas. It must
be a valid JSON object with a top-level `status` string equal to `PASS` or
`FAIL`. A representative payload is:

```json
{"status":"PASS","simulated":false,"test_type":"RC","measured_tau_s":0.51}
```

`status: FAIL` is a completed diagnostic that found a circuit fault. It is not
a transport or controller execution failure. The controller reports it as
state `COMPLETED` and preserves the payload. Invalid JSON, missing/invalid
status, incorrect source endpoints, malformed fields, or unknown test IDs are
discarded rather than used to advance the test.

Supported test types are `DIVIDER`, `RC`, and `PWM`. Faults are `NONE`,
`WRONG_VALUE`, `WRONG_FREQ`, and `NO_SIGNAL`. `WRONG_VALUE` applies to DIVIDER
and RC; `WRONG_FREQ` applies to PWM; `NO_SIGNAL` applies to any type. Invalid
type/fault combinations are rejected before a test ID is created.

## Test coordination and timing

After both stations respond to heartbeats, the controller starts the following
sequence:

1. Assign a unique test ID and send PREPARE to Board 3.
2. On its ACK, send ARM to Board 2.
3. On its ACK, send START to Board 3.
4. Accept Board 2's RESULT, validate its JSON, and send its RESULT ACK.
5. Send ABORT to Board 3 first and then Board 2; wait for each station's ACK,
   including after a normal result.
6. Publish a final state and retain the result for GET_RESULT. Cleanup status is
   reported independently for each station.

The test states are `IDLE`, `PREPARING`, `ARMING`, `RUNNING`, `COMPLETED`,
`FAILED`, and `ABORTED`. `FAILED` means command/measurement/communication or
cleanup failure. `ABORTED` means a requested cancellation completed its safety
cleanup. If a diagnostic returns `status: FAIL`, the final state remains
`COMPLETED` and the measurement JSON carries the diagnostic failure.

Heartbeats are sent every 1 second. A board becomes offline after 3 seconds
without a matching PONG, and becomes online again on a valid response. The
controller tracks last, average, minimum, and maximum PING/PONG round-trip
latencies. Command retries reuse the same command and test ID; the default is
two retries after the first send. Default timeouts are 500 ms per command, 1 s
for ABORT confirmation, and 15 s for measurement. Override them with
`--command-timeout-ms`, `--cleanup-timeout-ms`, and
`--measurement-timeout-ms`. RC measurements that need longer than 15 seconds
must use a larger measurement timeout. Only one test can be active at a time.

After a communication failure during a test, the controller sends ABORT to
both stations even if either send fails, tracks their ACKs independently, and
reports each unconfirmed cleanup after its bounded retries expire. If either
station does not confirm, the final state is `FAILED`; a diagnostic result, if
already received, is still retained. SIGINT/SIGTERM also attempt bilateral
ABORT for an active test before the controller exits. Heartbeats continue while
preparing, arming, running, waiting for a result, and waiting for cleanup ACKs.

## Shared hardware test specifications

These are the values and acquisition behavior Board 2 and Board 3 must share.
The controller passes the test type and fault to Board 3; Board 2 reports its
measurement through RESULT.

### DIVIDER

- Nominal supply: 3.3 V.
- Expected resistors: 10 kΩ top and 10 kΩ bottom.
- Expected midpoint voltage: 1.65 V.
- Voltage tolerance: ±0.10 V.
- Board 2 should start buffering before ACKing ARM, then evaluate settled
  measurements acquired after Board 3 begins stimulus.
- A settled voltage outside tolerance is a diagnostic result with `status: FAIL`.

### RC charge/discharge

- Expected series resistance: 50 kΩ.
- Expected capacitance: 10 µF.
- Expected time constant: 0.5 seconds.
- Proposed time-constant tolerance: ±20% (0.40–0.60 seconds).
- Before ACKing PREPARE, Board 3 must precondition the capacitor to a discharged
  state and confirm its baseline is within the safe threshold.
- Before ACKing ARM, Board 2 must begin sampling and buffering.
- On START, Board 3 charges for 3 seconds and then discharges for 3 seconds.
- Board 2 must detect actual voltage transitions and use local monotonic sample
  timestamps to estimate both time constants. It must not assume a UDP command
  arrived at an exact physical time.
- Missing threshold crossings, incorrect endpoints, or out-of-tolerance time
  constants are diagnostic failures and should produce a timely RESULT with
  `status: FAIL`. ADC, sensor, or internal acquisition failures use ERROR.

### PWM

- Expected frequency: 20 Hz.
- `WRONG_FREQ` generates 10 Hz.
- `NO_SIGNAL` disables the emitter.
- Proposed frequency tolerance: ±2 Hz.
- Board 2 must collect enough samples over enough cycles to estimate frequency
  reliably. Missing or out-of-range signals are diagnostic failures and should
  produce RESULT with `status: FAIL`.

For every test type, a measured circuit fault is distinct from a communication
failure. If a station detects a missing physical signal, it must return a
diagnostic FAIL instead of waiting indefinitely. The controller's measurement
timeout is a final guard for a missing RESULT and is reported as an execution
or communication failure, not as a measured circuit FAIL.

### Board 3 local safety timeout

Board 3 must enforce a local maximum stimulus duration with its own monotonic
timer or hardware watchdog. It must work if Board 1 becomes unreachable or its
process exits. On expiry, Board 3 must disable driven outputs, return stimulus
lines to their safe idle state, and release them without waiting for another
UDP command. Since the specified RC sequence lasts 6 seconds, an 8-second
maximum is a reasonable initial value, subject to hardware review and any longer
test profiles. A valid ABORT must trigger the same safe cleanup immediately.

## Dashboard requests

Requests are sent to `127.0.0.1:35100`:

| Request | Meaning |
| --- | --- |
| `STATUS` | Current/latest state and connectivity |
| `RUN,request_id,test_type,fault` | Start a test |
| `ABORT,test_id` | Cancel the matching active test |
| `GET_RESULT,test_id` | Read a retained test record |

`request_id` and `test_id` are 1–79 characters containing only ASCII letters,
digits, `_`, `-`, and `.`. A repeated RUN with an existing request ID returns
the existing test ID and sets `duplicate_request: true`; it never starts a
second operation. A different request ID while a test is active is rejected
with an `error_message`.

Every dashboard reply is a JSON object with these fields:

```json
{
  "state": "RUNNING",
  "board2_connected": true,
  "board3_connected": true,
  "current_test_id": "1234-abc-1",
  "test_type": "RC",
  "test_progress": "measuring",
  "measurement_result": null,
  "error_message": null,
  "cleanup_acknowledgements": {"board2": "not_started", "board3": "not_started"},
  "network_latency_ms": {
    "board2": {"last": 0.12, "average": 0.14, "minimum": 0.10, "maximum": 0.20, "samples": 8},
    "board3": {"last": 0.15, "average": 0.13, "minimum": 0.09, "maximum": 0.22, "samples": 8}
  }
}
```

Latency values are milliseconds and are `null` until a matching PONG has been
received. `measurement_result` is null until a valid RESULT arrives, then it is
the original JSON payload (not a quoted string). Cleanup acknowledgement values
are `not_started`, `pending`, `confirmed`, or `unconfirmed` for each station.
For RUN replies, the object also includes `request_id` and `test_id`; duplicate
RUN replies include `duplicate_request`. Invalid requests and unknown IDs
return the same status shape with a non-null `error_message`.

## Local mock stations

From the repository root, start the two simulated stations in separate
terminals:

```sh
python3 controller/tools/mock_board.py --board 2
python3 controller/tools/mock_board.py --board 3
```

They label all output and result data as simulated. Use
`--result-status FAIL --failure-reason signal_missing` to generate a diagnostic
missing-signal FAIL, `--no-result` to exercise measurement timeout,
`--result-delay-ms 800` to slow the response, `--drop PREPARE=1` to drop one
reply, or `--drop-heartbeats` to simulate a disconnected station. Use
`--drop ABORT=all` to test unconfirmed cleanup, `--duplicate-acks` and
`--duplicate-result` to test duplicate replies, or `--inject-late-acks` to send
stale test-ID ACKs. The Board 3 mock defaults to an 8-second local maximum
stimulus duration and forces its outputs safe on expiry. Multiple `--drop`
options are allowed; use `NAME=all` to drop every reply of that kind.
