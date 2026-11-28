#!/usr/bin/env python3
"""Query the CircuitCheck controller's existing localhost dashboard UDP API."""

import argparse
import json
import secrets
import socket
import sys
import time


DASHBOARD = ("127.0.0.1", 35100)
TERMINAL = {"COMPLETED", "FAILED", "ABORTED"}
FAULTS = {
    "DIVIDER": {"NONE", "WRONG_VALUE", "NO_SIGNAL"},
    "RC": {"NONE", "WRONG_VALUE", "NO_SIGNAL"},
    "PWM": {"NONE", "WRONG_FREQ", "NO_SIGNAL"},
}


def request(message, timeout):
    encoded = message.encode("ascii")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout)
        for _ in range(3):
            sock.sendto(encoded, DASHBOARD)
            try:
                packet, _ = sock.recvfrom(16384)
                value = json.loads(packet.decode("utf-8"))
                if not isinstance(value, dict):
                    raise ValueError("controller response must be a JSON object")
                return value
            except socket.timeout:
                continue
    raise TimeoutError(f"no response from 127.0.0.1:35100 for {message.split(',', 1)[0]}")


def print_response(value):
    print(json.dumps(value, indent=2, sort_keys=True))


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=float, default=2.0,
                        help="seconds to wait per UDP response (default: 2)")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status", help="query STATUS")

    run = commands.add_parser("run", help="send RUN and optionally wait for a final result")
    run.add_argument("test_type", choices=tuple(FAULTS))
    run.add_argument("--fault", choices=("NONE", "WRONG_VALUE", "WRONG_FREQ", "NO_SIGNAL"),
                     default="NONE")
    run.add_argument("--request-id", default=None,
                     help="idempotency key; a random key is generated if omitted")
    run.add_argument("--wait", action="store_true", help="poll GET_RESULT until terminal")
    run.add_argument("--wait-timeout", type=float, default=180.0,
                     help="maximum seconds to poll when --wait is used")

    result = commands.add_parser("result", help="send GET_RESULT for a test ID")
    result.add_argument("test_id")
    abort = commands.add_parser("abort", help="send ABORT for an active test ID")
    abort.add_argument("test_id")
    abort.add_argument("--wait", action="store_true", help="wait for terminal cleanup status")
    abort.add_argument("--wait-timeout", type=float, default=15.0)
    options = parser.parse_args()
    if options.timeout <= 0:
        parser.error("--timeout must be positive")
    if options.command == "run":
        if options.fault not in FAULTS[options.test_type]:
            parser.error(f"{options.fault} is not applicable to {options.test_type}")
        if options.wait_timeout <= 0:
            parser.error("--wait-timeout must be positive")
    if options.command == "abort" and options.wait_timeout <= 0:
        parser.error("--wait-timeout must be positive")
    return options


def wait_for(test_id, timeout, response_timeout):
    deadline = time.monotonic() + timeout
    latest = None
    while time.monotonic() < deadline:
        latest = request(f"GET_RESULT,{test_id}", response_timeout)
        if latest.get("state") in TERMINAL or latest.get("error_message"):
            return latest
        time.sleep(0.25)
    if latest is not None:
        latest["client_wait_timeout"] = True
        return latest
    raise TimeoutError("no result response received")


def main():
    options = parse_args()
    try:
        if options.command == "status":
            response = request("STATUS", options.timeout)
        elif options.command == "result":
            response = request(f"GET_RESULT,{options.test_id}", options.timeout)
        elif options.command == "abort":
            response = request(f"ABORT,{options.test_id}", options.timeout)
            if (options.wait and response.get("test_id") == options.test_id
                    and response.get("state") not in TERMINAL):
                response = wait_for(options.test_id, options.wait_timeout, options.timeout)
        else:
            request_id = options.request_id or "cli-" + secrets.token_hex(8)
            response = request(
                f"RUN,{request_id},{options.test_type},{options.fault}", options.timeout)
            if options.wait and response.get("test_id"):
                response = wait_for(response["test_id"], options.wait_timeout, options.timeout)
        print_response(response)
        if response.get("error_message") and not (
                options.command == "abort" and response.get("state") == "ABORTED"):
            return 2
        result = response.get("measurement_result")
        if response.get("state") == "COMPLETED" and isinstance(result, dict):
            return 0 if result.get("status") == "PASS" else 1
        if response.get("state") == "FAILED":
            return 2
        if response.get("state") == "ABORTED":
            cleanup = response.get("cleanup_acknowledgements", {})
            return 0 if (cleanup.get("board2") == "confirmed"
                         and cleanup.get("board3") == "confirmed") else 2
        if response.get("client_wait_timeout"):
            return 1
        return 0
    except (OSError, TimeoutError, UnicodeError, json.JSONDecodeError, ValueError) as error:
        print(f"controller_cli: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
