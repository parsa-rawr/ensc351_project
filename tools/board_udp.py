#!/usr/bin/env python3
"""Discover CircuitCheck boards and run a bench test over the team UDP protocol."""

import argparse
import json
import math
import signal
import socket
import sys
import time
import uuid

PORTS = {1: 35101, 2: 35102, 3: 35103}
RETRY_SECONDS = 0.25
FAULTS = {
    "DIVIDER": ("NONE", "WRONG_VALUE", "NO_SIGNAL"),
    "RC": ("NONE", "WRONG_VALUE", "NO_SIGNAL"),
    "PWM": ("NONE", "WRONG_FREQ", "NO_SIGNAL"),
}


class ProtocolError(Exception):
    pass


def address(host, board):
    return socket.gethostbyname(host), PORTS[board]


def send(sock, peer, message):
    sock.sendto(message.encode("ascii"), peer)


def receive(sock, deadline):
    """Return one decoded datagram and its sender, or None at the deadline."""
    while (remaining := deadline - time.monotonic()) > 0:
        sock.settimeout(remaining)
        try:
            data, peer = sock.recvfrom(65535)
        except socket.timeout:
            return None
        try:
            return data.decode("utf-8").rstrip("\r\n"), peer
        except UnicodeDecodeError:
            continue
    return None


def ping(sock, peer, board):
    nonce = uuid.uuid4().hex[:16]
    for _ in range(3):
        send(sock, peer, f"PING,{nonce}")
        deadline = time.monotonic() + RETRY_SECONDS
        while (packet := receive(sock, deadline)) is not None:
            if packet == (f"PONG,{board},{nonce}", peer):
                print(f"Board {board} online at {peer[0]}:{peer[1]}")
                return
    raise ProtocolError(f"No PONG from Board {board} at {peer[0]}:{peer[1]}")


class BenchRun:
    def __init__(self, sock, board2, board3, kind):
        self.sock = sock
        self.board2 = board2
        self.board3 = board3
        self.kind = kind
        self.test_id = uuid.uuid4().hex
        self.result = None

    def handle(self, packet, *, cleanup=False):
        message, peer = packet
        fields = message.split(",", 2)
        if peer not in (self.board2, self.board3) or len(fields) != 3:
            return
        verb, test_id, value = fields
        if test_id != self.test_id:
            return
        if verb == "ERROR" and not cleanup:
            board = 2 if peer == self.board2 else 3
            raise ProtocolError(f"Board {board}: {value}")
        if verb != "RESULT" or peer != self.board2:
            return
        if self.result is None:
            try:
                result = json.loads(value)
                if not isinstance(result, dict) or result.get("test_type") != self.kind:
                    raise ValueError("RESULT test type does not match the request")
                if result.get("status") not in ("PASS", "FAIL"):
                    raise ValueError("RESULT status must be PASS or FAIL")
            except ValueError as exc:
                if cleanup:
                    return
                raise ProtocolError(f"Invalid RESULT from Board 2: {exc}") from exc
            self.result = result
        send(self.sock, self.board2, f"ACK,{self.test_id},RESULT")

    def command(self, peer, verb, arguments, timeout):
        deadline = time.monotonic() + timeout
        message = f"{verb},{self.test_id},{arguments}"
        expected = f"ACK,{self.test_id},{verb}", peer
        while time.monotonic() < deadline:
            send(self.sock, peer, message)
            retry_at = min(deadline, time.monotonic() + RETRY_SECONDS)
            while (packet := receive(self.sock, retry_at)) is not None:
                if packet == expected:
                    return
                self.handle(packet)
        raise ProtocolError(f"No ACK for {verb} from {peer[0]}:{peer[1]}")

    def stop(self):
        pending = {self.board2, self.board3}
        # Wait through the measurement station's 500 ms result retry interval.
        deadline = time.monotonic() + 0.80
        while time.monotonic() < deadline:
            for peer in pending:
                try:
                    send(self.sock, peer, f"ABORT,{self.test_id}")
                except OSError as exc:
                    print(f"ABORT send failed for {peer[0]}:{peer[1]}: {exc}", file=sys.stderr)
            retry_at = min(deadline, time.monotonic() + RETRY_SECONDS)
            while (packet := receive(self.sock, retry_at)) is not None:
                message, peer = packet
                if message == f"ACK,{self.test_id},ABORT":
                    pending.discard(peer)
                else:
                    self.handle(packet, cleanup=True)
        return pending

    def execute(self, fault, result_timeout):
        try:
            self.command(self.board3, "PREPARE", f"{self.kind},{fault}", 8)
            self.command(self.board2, "ARM", self.kind, 2)
            self.command(self.board3, "START", self.kind, 2)
            deadline = time.monotonic() + result_timeout
            while self.result is None:
                packet = receive(self.sock, deadline)
                if packet is None:
                    raise ProtocolError("Timed out waiting for Board 2 RESULT")
                self.handle(packet)
        finally:
            pending = self.stop()
            if pending:
                missing = ", ".join(f"{host}:{port}" for host, port in sorted(pending))
                print(f"ABORT not confirmed by {missing}. Check stimulus shutdown.", file=sys.stderr)
        print(json.dumps(self.result, indent=2))
        if pending:
            return 2
        return 0 if self.result["status"] == "PASS" else 1


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    probe = commands.add_parser("ping", help="discover a single board")
    probe.add_argument("--board", type=int, choices=PORTS, required=True)
    probe.add_argument("--host", required=True, help="board IPv4 address or hostname")
    run = commands.add_parser("run", help="coordinate one test from UDP port 35101")
    run.add_argument("--board2", required=True, help="measurement board IPv4 address or hostname")
    run.add_argument("--board3", required=True, help="signal board IPv4 address or hostname")
    run.add_argument("--test", choices=FAULTS, required=True)
    run.add_argument("--fault", choices=("NONE", "WRONG_VALUE", "WRONG_FREQ", "NO_SIGNAL"),
                     default="NONE")
    run.add_argument("--bind-ip", default="0.0.0.0", help="controller IPv4 address")
    run.add_argument("--result-timeout", type=float, default=12, help="seconds to wait after START")
    options = parser.parse_args()
    if options.command == "run":
        if options.fault not in FAULTS[options.test]:
            parser.error(f"{options.fault} is not applicable to {options.test}")
        if not math.isfinite(options.result_timeout) or options.result_timeout <= 0:
            parser.error("--result-timeout must be a finite positive number")
    return options


def interrupted(_signum, _frame):
    raise KeyboardInterrupt


def main():
    options = parse_args()
    signal.signal(signal.SIGTERM, interrupted)
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            if options.command == "ping":
                ping(sock, address(options.host, options.board), options.board)
                return 0
            board2 = address(options.board2, 2)
            board3 = address(options.board3, 3)
            sock.bind((options.bind_ip, PORTS[1]))
            ping(sock, board2, 2)
            ping(sock, board3, 3)
            return BenchRun(sock, board2, board3, options.test).execute(
                options.fault, options.result_timeout)
    except KeyboardInterrupt:
        print("Cancelled", file=sys.stderr)
        return 130
    except (OSError, ProtocolError) as exc:
        print(exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
