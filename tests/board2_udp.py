"""Exercise Board 2 as a controller using real UDP sockets and simulated inputs."""

import contextlib
import json
import signal
import socket
import subprocess
import sys

BOARD = ("127.0.0.1", 35102)


def send(client, message):
    client.sendto(message.encode("ascii"), BOARD)


def receive(client):
    payload, sender = client.recvfrom(1024)
    assert sender == BOARD, sender
    return payload.decode("ascii")


def expect(client, message):
    actual = receive(client)
    assert actual == message, (message, actual)


def decode_result(client, test_id, kind, fault):
    raw = receive(client)
    verb, actual_id, payload = raw.split(",", 2)
    assert (verb, actual_id) == ("RESULT", test_id), raw
    result = json.loads(payload)
    assert result["simulated"] and result["test_type"] == kind, result
    assert result["fault"] == fault, result
    assert result["status"] == ("PASS" if fault == "NONE" else "FAIL"), result
    timing = result["sample_interval_ms"]
    assert 0 < timing["min"] <= timing["mean"] <= timing["max"], timing
    assert timing["count"] > 0
    assert abs(timing["jitter_pp"] - (timing["max"] - timing["min"])) < 0.000002
    return raw, result


@contextlib.contextmanager
def station(scenario):
    with subprocess.Popen(
        [sys.argv[1], "--simulate", "--scenario", scenario],
        stdout=subprocess.PIPE, text=True,
    ) as process, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
        assert "listening" in process.stdout.readline(), "Failed to start Board 2"
        client.settimeout(10)
        try:
            yield client
        finally:
            process.send_signal(signal.SIGTERM)
            assert process.wait(timeout=2) == 0


def arm(client, test_id, kind):
    send(client, f"ARM,{test_id},{kind}")
    expect(client, f"ACK,{test_id},ARM")


def expect_silence(client, seconds=0.7):
    timeout = client.gettimeout()
    client.settimeout(seconds)
    try:
        unexpected = receive(client)
    except socket.timeout:
        return
    finally:
        client.settimeout(timeout)
    raise AssertionError(f"Unexpected datagram {unexpected!r}")


with station("normal") as client, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as stranger:
    stranger.settimeout(1)
    send(client, "PING,discovery-42")
    expect(client, "PONG,2,discovery-42")
    send(client, "ARM,malformed,DIVIDER,WRONG_VALUE")
    expect(client, "ERROR,malformed,BAD_COMMAND")
    arm(client, "divider-1", "DIVIDER")
    arm(client, "divider-1", "DIVIDER")
    send(client, "ARM,divider-1,RC")
    expect(client, "ERROR,divider-1,TEST_TYPE_MISMATCH")
    send(client, "ARM,different,RC")
    expect(client, "ERROR,different,BUSY")
    raw, data = decode_result(client, "divider-1", "DIVIDER", "NONE")
    assert abs(data["measured_v"] - 1.65) < 0.005, data

    send(stranger, "ACK,divider-1,RESULT")
    expect(stranger, "ERROR,divider-1,CONTROLLER_MISMATCH")
    arm(client, "divider-1", "DIVIDER")
    expect(client, raw)
    send(client, "ABORT,divider-1")
    expect(client, "ACK,divider-1,ABORT")
    expect(client, raw)
    send(client, "ACK,divider-1,RESULT")
    arm(client, "divider-1", "DIVIDER")
    expect_silence(client)

    arm(client, "rc-cancelled", "RC")
    send(client, "ABORT,rc-cancelled")
    expect(client, "ACK,rc-cancelled,ABORT")
    send(client, "ABORT,rc-cancelled")
    expect(client, "ACK,rc-cancelled,ABORT")
    send(client, "ARM,rc-cancelled,RC")
    expect(client, "ERROR,rc-cancelled,ABORTED")

    send(client, "ABORT,early")
    expect(client, "ACK,early,ABORT")
    send(client, "ARM,early,PWM")
    expect(client, "ERROR,early,ABORTED")

    arm(client, "rc-2", "RC")
    _, data = decode_result(client, "rc-2", "RC", "NONE")
    for key in ("charge_tau_s", "discharge_tau_s"):
        assert abs(data[key] - 0.5) < 0.02, data
    send(client, "ACK,rc-2,RESULT")

    arm(client, "pwm-3", "PWM")
    _, data = decode_result(client, "pwm-3", "PWM", "NONE")
    assert abs(data["frequency_hz"] - 20) < 0.5 and data["cycles"] >= 18, data
    send(client, "ACK,pwm-3,RESULT")

for scenario, kind, expected_fault in (
    ("divider-wrong", "DIVIDER", "WRONG_VALUE"),
    ("rc-fast", "RC", "WRONG_VALUE"),
    ("pwm-wrong", "PWM", "WRONG_FREQ"),
    ("no-signal", "RC", "NO_SIGNAL"),
):
    with station(scenario) as client:
        arm(client, "fault-test", kind)
        _, data = decode_result(client, "fault-test", kind, expected_fault)
        if scenario == "no-signal":
            assert data["charge_tau_s"] is None and data["discharge_tau_s"] is None

with station("adc-error") as client:
    for _ in range(2):
        send(client, "ARM,broken,RC")
        expect(client, "ERROR,broken,ADC_READ")

print("PASS: Board 2 UDP workflow, idempotency, retention, abort, and fault classification")
