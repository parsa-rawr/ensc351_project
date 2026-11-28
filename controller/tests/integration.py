"""UDP integration tests for the controller and its two local mock stations."""
import json
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time


CONTROLLER = str(Path(sys.argv[1]).resolve())
MOCK = str(Path(sys.argv[2]).resolve())
CLI = str(Path(MOCK).with_name("controller_cli.py"))


class Fixture:
    def __init__(self, board2=True, board3=True, board2_args=(), board3_args=(),
                 command_timeout=120, measurement_timeout=1200, cleanup_timeout=200,
                 retries=2):
        self.processes = {}
        self.controller_args = [CONTROLLER, "--command-timeout-ms", str(command_timeout),
                                "--cleanup-timeout-ms", str(cleanup_timeout),
                                "--measurement-timeout-ms", str(measurement_timeout),
                                "--retries", str(retries)]
        self.client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.client.bind(("127.0.0.1", 0))
        self.client.settimeout(0.35)
        if board2:
            self.start_board(2, board2_args)
        if board3:
            self.start_board(3, board3_args)
        self.controller = subprocess.Popen(self.controller_args, stdout=subprocess.PIPE,
                                           stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.processes["controller"] = self.controller

    def start_board(self, number, extra=()):
        process = subprocess.Popen([sys.executable, MOCK, "--board", str(number), *extra],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, bufsize=1)
        self.processes[f"board{number}"] = process
        return process

    def request(self, message, timeout=2.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.client.sendto(message.encode(), ("127.0.0.1", 35100))
            try:
                packet, _ = self.client.recvfrom(16384)
                return json.loads(packet.decode())
            except socket.timeout:
                continue
        raise AssertionError(f"no JSON response to {message!r}")

    def wait_online(self, both=True, timeout=4.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            status = self.request("STATUS")
            if ((status["board2_connected"] and status["board3_connected"])
                    if both else status["board3_connected"]):
                return status
            time.sleep(0.05)
        raise AssertionError(f"boards did not come online: {status}")

    def wait_state(self, test_id, expected, timeout=4.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            status = self.request(f"GET_RESULT,{test_id}")
            if status["state"] == expected:
                return status
            time.sleep(0.04)
        raise AssertionError(f"test {test_id} did not reach {expected}: {status}")

    def run(self, request_id="case", test_type="RC", fault="NONE"):
        response = self.request(f"RUN,{request_id},{test_type},{fault}")
        assert response.get("test_id"), response
        return response

    @staticmethod
    def finish(process, signal_number=signal.SIGTERM):
        if process.poll() is None:
            process.send_signal(signal_number)
        try:
            output, _ = process.communicate(timeout=4)
        except subprocess.TimeoutExpired:
            process.kill()
            output, _ = process.communicate(timeout=2)
            raise AssertionError(f"process did not stop cleanly: {output}")
        return output

    def close(self):
        for process in reversed(list(self.processes.values())):
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
        for process in reversed(list(self.processes.values())):
            try:
                process.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate(timeout=2)
        self.client.close()


def successful_sequence():
    f = Fixture(board2_args=("--duplicate-acks", "--inject-late-acks"),
                board3_args=("--duplicate-acks", "--inject-late-acks"))
    try:
        f.wait_online()
        response = f.run()
        rejected = f.request("RUN,parallel,RC,NONE")
        assert "another test is active" in rejected["error_message"], rejected
        result = f.wait_state(response["test_id"], "COMPLETED")
        assert result["measurement_result"]["status"] == "PASS", result
        assert result["measurement_result"]["simulated"] is True, result
        assert result["test_progress"] == "complete", result
        assert result["network_latency_ms"]["board2"]["samples"] >= 1, result
        assert result["cleanup_acknowledgements"] == {
            "board2": "confirmed", "board3": "confirmed"}, result
        board2_output = Fixture.finish(f.processes["board2"])
        board3_output = Fixture.finish(f.processes["board3"])
        assert board2_output.count("armed ") == 1, board2_output
        assert board3_output.count("prepared ") == 1, board3_output
        assert board3_output.count("started ") == 1, board3_output
        assert board3_output.count("stopped stimulus") == 1, board3_output
    finally:
        f.close()


def sequential_runs():
    f = Fixture()
    try:
        f.wait_online()
        ids = []
        for index in range(1, 4):
            response = f.run(f"sequential-{index}", "DIVIDER", "NONE")
            ids.append(response["test_id"])
            result = f.wait_state(response["test_id"], "COMPLETED")
            assert result["measurement_result"]["status"] == "PASS", result
        assert len(set(ids)) == 3, ids
        board2_output = Fixture.finish(f.processes["board2"])
        board3_output = Fixture.finish(f.processes["board3"])
        assert board2_output.count("armed ") == 3, board2_output
        assert board3_output.count("started ") == 3, board3_output
    finally:
        f.close()


def seeded_delay_and_loss_stress():
    f = Fixture(
        board2_args=("--seed", "4102", "--delay-min-ms", "250", "--delay-max-ms", "1500",
                     "--loss-rate", "0.12"),
        board3_args=("--seed", "4103", "--delay-min-ms", "250", "--delay-max-ms", "1500",
                     "--loss-rate", "0.12", "--drop", "PREPARE=1"),
        command_timeout=2000, measurement_timeout=12000, cleanup_timeout=2000, retries=3)
    try:
        started = time.monotonic()
        f.wait_online(timeout=7)
        response = f.run("seeded-stress", "RC", "NONE")
        result = f.wait_state(response["test_id"], "COMPLETED", timeout=20)
        elapsed_ms = (time.monotonic() - started) * 1000
        assert result["measurement_result"]["status"] == "PASS", result
        assert result["cleanup_acknowledgements"] == {
            "board2": "confirmed", "board3": "confirmed"}, result
        latency = result["network_latency_ms"]
        for board in ("board2", "board3"):
            assert latency[board]["minimum"] >= 250, latency
            assert latency[board]["maximum"] <= 1700, latency
        board2_output = Fixture.finish(f.processes["board2"])
        board3_output = Fixture.finish(f.processes["board3"])
        controller_output = Fixture.finish(f.controller)
        loss_count = board2_output.count("intermittent loss dropped")
        loss_count += board3_output.count("intermittent loss dropped")
        retry_count = controller_output.count("RETRY ")
        assert loss_count > 0, (board2_output, board3_output)
        assert retry_count >= 1, controller_output
        print("STRESS_MEASURED "
              f"elapsed_ms={elapsed_ms:.1f} "
              f"pong_min_ms={min(latency['board2']['minimum'], latency['board3']['minimum']):.1f} "
              f"pong_max_ms={max(latency['board2']['maximum'], latency['board3']['maximum']):.1f} "
              f"seeded_loss_drops={loss_count} command_retries={retry_count} "
              f"result={result['measurement_result']['status']}")
    finally:
        f.close()


def dashboard_cli_commands():
    f = Fixture(board2_args=("--result-delay-ms", "2500"), measurement_timeout=6000)
    try:
        f.wait_online()
        status = subprocess.run([sys.executable, CLI, "status"], capture_output=True,
                                text=True, timeout=4)
        assert status.returncode == 0, (status.stdout, status.stderr)
        assert json.loads(status.stdout)["board2_connected"] is True

        started = subprocess.run([sys.executable, CLI, "--timeout", "2", "run", "DIVIDER",
                                  "--request-id", "cli-run-1", "--wait", "--wait-timeout", "8"],
                                 capture_output=True, text=True, timeout=12)
        assert started.returncode == 0, (started.stdout, started.stderr)
        completed = json.loads(started.stdout)
        assert completed["state"] == "COMPLETED", completed
        test_id = completed["current_test_id"]

        fetched = subprocess.run([sys.executable, CLI, "result", test_id],
                                 capture_output=True, text=True, timeout=4)
        assert fetched.returncode == 0, (fetched.stdout, fetched.stderr)
        assert json.loads(fetched.stdout)["measurement_result"]["status"] == "PASS"

        active = f.run("cli-abort", "RC", "NONE")
        aborted = subprocess.run([sys.executable, CLI, "abort", active["test_id"],
                                  "--wait", "--wait-timeout", "5"],
                                 capture_output=True, text=True, timeout=8)
        assert aborted.returncode == 0, (aborted.stdout, aborted.stderr)
        assert json.loads(aborted.stdout)["state"] == "ABORTED"
    finally:
        f.close()


def faulty_result_and_duplicate_request():
    f = Fixture(board2_args=("--result-status", "FAIL", "--failure-reason",
                             "signal_missing", "--duplicate-result"))
    try:
        f.wait_online()
        first = f.run("same-request", "DIVIDER", "WRONG_VALUE")
        second = f.run("same-request", "DIVIDER", "WRONG_VALUE")
        assert first["test_id"] == second["test_id"], (first, second)
        assert second["duplicate_request"] is True, second
        result = f.wait_state(first["test_id"], "COMPLETED")
        assert result["measurement_result"]["status"] == "FAIL", result
        assert result["measurement_result"]["failure_reason"] == "signal_missing", result
        again = f.run("same-request", "DIVIDER", "WRONG_VALUE")
        assert again["test_id"] == first["test_id"] and again["duplicate_request"], again
        board2_output = Fixture.finish(f.processes["board2"])
        assert "sent RESULT" in board2_output, board2_output
        assert board2_output.count("received result ACK") >= 2, board2_output
    finally:
        f.close()


def command_retry_and_measurement_timeout():
    f = Fixture(board3_args=("--drop", "PREPARE=1"), command_timeout=100,
                measurement_timeout=500, retries=2)
    try:
        f.wait_online()
        response = f.run("retry")
        result = f.wait_state(response["test_id"], "COMPLETED")
        assert result["measurement_result"]["status"] == "PASS", result
        output = Fixture.finish(f.processes["board3"])
        assert "deliberately dropped PREPARE" in output, output
        assert output.count("prepared ") == 1, output
        assert output.count(",PREPARE") == 1, output
    finally:
        f.close()

    f = Fixture(board2_args=("--no-result",), command_timeout=100,
                measurement_timeout=450, retries=1)
    try:
        f.wait_online()
        response = f.run("measurement-timeout")
        result = f.wait_state(response["test_id"], "FAILED", timeout=3)
        assert "measurement timeout" in result["error_message"], result
        assert result["test_progress"] == "failed", result
        assert result["cleanup_acknowledgements"] == {
            "board2": "confirmed", "board3": "confirmed"}, result
    finally:
        f.close()


def requested_abort():
    f = Fixture(board2_args=("--result-delay-ms", "2500"))
    try:
        f.wait_online()
        response = f.run("cancel")
        reply = f.request(f"ABORT,{response['test_id']}")
        assert reply["current_test_id"] == response["test_id"], reply
        result = f.wait_state(response["test_id"], "ABORTED")
        assert result["test_progress"] == "aborted", result
        assert result["cleanup_acknowledgements"] == {
            "board2": "confirmed", "board3": "confirmed"}, result
    finally:
        f.close()


def validation_and_source_endpoint():
    f = Fixture(board2=False, board3=True)
    try:
        f.wait_online(both=False)
        malformed = f.request("RUN,request-only")
        assert malformed["error_message"] == "RUN requires request_id,test_type,fault", malformed
        invalid = f.request("RUN,bad,PWM,WRONG_VALUE")
        assert "unsupported" in invalid["error_message"], invalid
        attacker = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        attacker.sendto(b"PONG,2,123", ("127.0.0.1", 35101))
        attacker.sendto(b"ACK,fake,ARM", ("127.0.0.1", 35101))
        attacker.close()
        time.sleep(0.15)
        status = f.request("STATUS")
        assert status["board2_connected"] is False, status
        refused = f.request("RUN,offline,RC,NONE")
        assert refused.get("test_id") is None and "online" in refused["error_message"], refused
    finally:
        f.close()


def heartbeat_disconnect_and_reconnect():
    f = Fixture()
    try:
        f.wait_online()
        board3 = f.processes["board3"]
        board3_output = Fixture.finish(board3)
        deadline = time.monotonic() + 4.0
        offline = None
        while time.monotonic() < deadline:
            offline = f.request("STATUS")
            if not offline["board3_connected"]:
                break
            time.sleep(0.1)
        assert offline and not offline["board3_connected"], offline
        f.start_board(3)
        status = f.wait_online(timeout=3.0)
        assert status["board3_connected"], status
        assert "listening" in board3_output, board3_output
    finally:
        f.close()


def graceful_shutdown_sends_abort():
    f = Fixture(board2_args=("--no-result",), measurement_timeout=5000)
    try:
        f.wait_online()
        f.run("shutdown")
        time.sleep(0.1)
        output = Fixture.finish(f.controller, signal.SIGTERM)
        f.processes.pop("controller", None)
        assert f.controller.returncode == 0, output
        board2_output = Fixture.finish(f.processes["board2"])
        board3_output = Fixture.finish(f.processes["board3"])
        assert "stopped acquisition" in board2_output, board2_output
        assert "stopped stimulus" in board3_output, board3_output
    finally:
        f.close()


def heartbeat_during_measurement_and_cleanup():
    f = Fixture(board2_args=("--result-delay-ms", "1300", "--drop", "ABORT=all"),
                cleanup_timeout=300, measurement_timeout=3000, retries=2)
    try:
        f.wait_online()
        response = f.run("heartbeat-active")
        time.sleep(1.15)
        measuring = f.request("STATUS")
        assert measuring["board2_connected"] and measuring["board3_connected"], measuring
        assert measuring["network_latency_ms"]["board2"]["samples"] >= 2, measuring
        result = f.wait_state(response["test_id"], "FAILED", timeout=4)
        assert result["measurement_result"]["status"] == "PASS", result
        assert result["cleanup_acknowledgements"] == {
            "board2": "unconfirmed", "board3": "confirmed"}, result
        assert "Board 2" in result["error_message"], result
        assert result["board3_connected"] is True, result
        board2_output = Fixture.finish(f.processes["board2"])
        board3_output = Fixture.finish(f.processes["board3"])
        assert board2_output.count("deliberately dropped ABORT") >= 1, board2_output
        assert board3_output.count("sent PONG,3,") >= 2, board3_output
    finally:
        f.close()


def board3_safe_stop_when_board2_is_unavailable():
    f = Fixture(board2_args=("--no-result",), cleanup_timeout=100,
                measurement_timeout=10000, retries=1)
    try:
        f.wait_online()
        response = f.run("board2-loss")
        deadline = time.monotonic() + 3.0
        progress = None
        while time.monotonic() < deadline:
            progress = f.request(f"GET_RESULT,{response['test_id']}")
            if progress["test_progress"] == "measuring":
                break
            time.sleep(0.04)
        assert progress and progress["test_progress"] == "measuring", progress
        Fixture.finish(f.processes["board2"])
        result = f.wait_state(response["test_id"], "FAILED", timeout=6)
        assert result["cleanup_acknowledgements"] == {
            "board2": "unconfirmed", "board3": "confirmed"}, result
        assert "Board 2 heartbeat lost" in result["error_message"], result
        f.start_board(2)
        f.wait_online(timeout=4)
        recovery = f.run("board2-reconnected")
        recovered = f.wait_state(recovery["test_id"], "COMPLETED")
        assert recovered["measurement_result"]["status"] == "PASS", recovered
        followup = f.run("after-recovery")
        followed = f.wait_state(followup["test_id"], "COMPLETED")
        assert followed["measurement_result"]["status"] == "PASS", followed
        board3_output = Fixture.finish(f.processes["board3"])
        assert board3_output.count("stopped stimulus") == 3, board3_output
        assert board3_output.count("sent PONG,3,") >= 2, board3_output
    finally:
        f.close()


def mock_board3_local_stimulus_timeout():
    f = Fixture(board2_args=("--no-result",),
                board3_args=("--max-stimulus-duration-ms", "100"),
                measurement_timeout=700)
    try:
        f.wait_online()
        response = f.run("local-watchdog")
        result = f.wait_state(response["test_id"], "FAILED", timeout=3)
        assert result["error_message"] == "measurement timeout", result
        board3_output = Fixture.finish(f.processes["board3"])
        assert "maximum stimulus duration expired; outputs forced safe" in board3_output, board3_output
    finally:
        f.close()


successful_sequence()
sequential_runs()
seeded_delay_and_loss_stress()
dashboard_cli_commands()
faulty_result_and_duplicate_request()
command_retry_and_measurement_timeout()
requested_abort()
validation_and_source_endpoint()
heartbeat_disconnect_and_reconnect()
graceful_shutdown_sends_abort()
heartbeat_during_measurement_and_cleanup()
board3_safe_stop_when_board2_is_unavailable()
mock_board3_local_stimulus_timeout()
print("PASS: controller sequencing, duplicate/late packets, bilateral cleanup, heartbeat, retries, timeouts, abort, validation, recovery, shutdown, and mock local watchdog.")
