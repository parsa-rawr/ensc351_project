#!/usr/bin/env python3
"""Local UDP mock for CircuitCheck Pro Board 2 or Board 3."""
import argparse
import heapq
import itertools
import json
import random
import signal
import socket
import time


def parse_drops(values):
    drops = {}
    for value in values:
        if "=" not in value:
            raise argparse.ArgumentTypeError("drop rules use MESSAGE=count or MESSAGE=all")
        name, count = value.split("=", 1)
        if not name or (count != "all" and (not count.isdigit() or int(count) < 1)):
            raise argparse.ArgumentTypeError("invalid drop rule")
        drops[name] = None if count == "all" else int(count)
    return drops


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", type=int, choices=(2, 3), required=True)
    parser.add_argument("--bind-ip", default="127.0.0.1")
    parser.add_argument("--controller-ip", default="127.0.0.1")
    parser.add_argument("--result-status", choices=("PASS", "FAIL"), default="PASS")
    parser.add_argument("--failure-reason", default="simulated_out_of_tolerance")
    parser.add_argument("--result-delay-ms", type=int, default=350)
    parser.add_argument("--max-stimulus-duration-ms", type=int, default=8000)
    parser.add_argument("--no-result", action="store_true")
    parser.add_argument("--duplicate-result", action="store_true",
                        help="send each simulated RESULT datagram twice")
    parser.add_argument("--drop", action="append", default=[], metavar="MESSAGE=count|all",
                        help="drop selected replies, e.g. --drop PREPARE=1")
    parser.add_argument("--drop-heartbeats", action="store_true")
    parser.add_argument("--duplicate-acks", action="store_true",
                        help="send each ACK twice to exercise duplicate handling")
    parser.add_argument("--inject-late-acks", action="store_true",
                        help="send stale test-ID ACKs along with heartbeats")
    parser.add_argument("--delay-min-ms", type=int, default=0,
                        help="minimum seeded response delay")
    parser.add_argument("--delay-max-ms", type=int, default=0,
                        help="maximum seeded response delay")
    parser.add_argument("--loss-rate", type=float, default=0.0,
                        help="seeded per-response packet loss probability (0..1)")
    parser.add_argument("--seed", type=int, default=1,
                        help="reproducible impairment seed")
    args = parser.parse_args()
    try:
        drop_rules = parse_drops(args.drop)
    except argparse.ArgumentTypeError as error:
        parser.error(str(error))
    if args.delay_min_ms < 0 or args.delay_max_ms < args.delay_min_ms:
        parser.error("response delay must satisfy 0 <= min <= max")
    if not 0 <= args.loss_rate <= 1:
        parser.error("--loss-rate must be between 0 and 1")

    number = args.board
    port = 35102 if number == 2 else 35103
    controller = (args.controller_ip, 35101)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind_ip, port))
    sock.settimeout(0.05)
    rng = random.Random(args.seed + number)
    scheduled = []
    sequence = itertools.count()
    counts = {"sent": 0, "loss_dropped": 0, "rule_dropped": 0}
    running = True

    def stop(_signum, _frame):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    state = {"id": None, "type": None, "prepared": False, "started": False,
             "start_seen": False,
             "fault": "NONE", "started_at": None, "due": None,
             "pending_result": None, "last_result_send": 0.0}
    known_tests = set()
    print(f"SIMULATED Board {number} listening on {args.bind_ip}:{port}", flush=True)

    def send(message, kind):
        remaining = drop_rules.get(kind, 0)
        if remaining is None:
            counts["rule_dropped"] += 1
            print(f"SIMULATED Board {number} deliberately dropped {kind}", flush=True)
            return
        if remaining:
            drop_rules[kind] = remaining - 1
            counts["rule_dropped"] += 1
            print(f"SIMULATED Board {number} deliberately dropped {kind}", flush=True)
            return
        copies = 2 if ((kind == "RESULT" and args.duplicate_result)
                       or (kind in ("PREPARE", "ARM", "START", "ABORT")
                           and args.duplicate_acks)) else 1
        for _ in range(copies):
            if args.loss_rate and rng.random() < args.loss_rate:
                counts["loss_dropped"] += 1
                print(f"SIMULATED Board {number} intermittent loss dropped {kind}", flush=True)
                continue
            delay_ms = rng.randint(args.delay_min_ms, args.delay_max_ms)
            due = time.monotonic() + delay_ms / 1000.0
            heapq.heappush(scheduled, (due, next(sequence), message.encode("utf-8"),
                                       message, delay_ms))

    def flush_scheduled(now):
        while scheduled and scheduled[0][0] <= now:
            _, _, payload, message, delay_ms = heapq.heappop(scheduled)
            try:
                sock.sendto(payload, controller)
            except OSError as error:
                print(f"SIMULATED Board {number} send error: {error}", flush=True)
                continue
            counts["sent"] += 1
            print(f"SIMULATED Board {number} sent {message} delay_ms={delay_ms}", flush=True)

    try:
        while running:
            now = time.monotonic()
            flush_scheduled(now)
            if (number == 3 and state["started"] and state["started_at"] is not None
                    and (now - state["started_at"]) * 1000 >= args.max_stimulus_duration_ms):
                state["started"] = False
                print(f"SIMULATED Board 3 maximum stimulus duration expired; outputs forced safe for {state['id']}",
                      flush=True)
            if (number == 2 and state["due"] is not None and now >= state["due"]
                    and state["pending_result"] is None and not args.no_result):
                measurement = {
                    "status": args.result_status,
                    "simulated": True,
                    "test_type": state["type"],
                    "summary": "mock healthy measurement" if args.result_status == "PASS"
                               else "mock injected circuit failure",
                }
                if args.result_status == "FAIL":
                    measurement["failure_reason"] = args.failure_reason
                state["pending_result"] = f"RESULT,{state['id']},{json.dumps(measurement, separators=(',', ':'))}"
            if (number == 2 and state["pending_result"] is not None
                    and now - state["last_result_send"] >= 0.3):
                send(state["pending_result"], "RESULT")
                state["last_result_send"] = now
            try:
                packet, peer = sock.recvfrom(8192)
            except socket.timeout:
                continue
            except OSError:
                break
            if peer != controller:
                print(f"SIMULATED Board {number} ignored incorrect controller endpoint {peer}", flush=True)
                continue
            try:
                message = packet.decode("utf-8")
            except UnicodeDecodeError:
                continue
            fields = message.split(",", 3)
            if args.loss_rate and rng.random() < args.loss_rate:
                counts["loss_dropped"] += 1
                print(f"SIMULATED Board {number} intermittent loss dropped inbound {fields[0]}",
                      flush=True)
                continue
            if fields[0] == "PING" and len(fields) == 2 and fields[1].isdigit():
                if not args.drop_heartbeats:
                    send(f"PONG,{number},{fields[1]}", "PONG")
                if args.inject_late_acks:
                    sock.sendto(b"ACK,stale-test-id,PREPARE", controller)
                    sock.sendto(b"ACK,stale-test-id,ARM", controller)
            elif number == 3 and fields[0] == "PREPARE" and len(fields) == 4:
                _, test_id, test_type, fault = fields
                if state["id"] in (None, test_id):
                    if state["id"] is None:
                        state.update(id=test_id, type=test_type, fault=fault,
                                     prepared=True, started=False, start_seen=False,
                                     started_at=None)
                        print(f"SIMULATED Board 3 prepared {test_id} {test_type} fault={fault}", flush=True)
                    if state["type"] == test_type and state["fault"] == fault:
                        send(f"ACK,{test_id},PREPARE", "PREPARE")
            elif number == 3 and fields[0] == "START" and len(fields) == 3:
                _, test_id, test_type = fields
                if state["id"] == test_id and state["prepared"] and state["type"] == test_type:
                    if not state["start_seen"]:
                        state["started"] = True
                        state["start_seen"] = True
                        state["started_at"] = now
                        print(f"SIMULATED Board 3 started {test_id}", flush=True)
                        if state["fault"] == "NO_SIGNAL":
                            print("SIMULATED Board 3 emitter disabled by NO_SIGNAL", flush=True)
                        elif state["fault"] == "WRONG_FREQ":
                            print("SIMULATED Board 3 PWM configured to 10 Hz", flush=True)
                    else:
                        print(f"SIMULATED Board 3 ignored duplicate START for {test_id}", flush=True)
                    send(f"ACK,{test_id},START", "START")
            elif number == 3 and fields[0] == "ABORT" and len(fields) == 2:
                test_id = fields[1]
                if state["id"] in (None, test_id):
                    state.update(id=None, type=None, fault="NONE", prepared=False,
                                 started=False, start_seen=False, started_at=None)
                    print(f"SIMULATED Board 3 stopped stimulus for {test_id}", flush=True)
                    send(f"ACK,{test_id},ABORT", "ABORT")
            elif number == 2 and fields[0] == "ARM" and len(fields) == 3:
                _, test_id, test_type = fields
                if state["id"] in (None, test_id) and (state["id"] is None or state["type"] == test_type):
                    if state["id"] is None:
                        state.update(id=test_id, type=test_type, due=now + args.result_delay_ms / 1000.0,
                                     pending_result=None, last_result_send=0.0)
                        known_tests.add(test_id)
                    print(f"SIMULATED Board 2 armed {test_id} {test_type}", flush=True)
                    send(f"ACK,{test_id},ARM", "ARM")
            elif number == 2 and fields[0] == "ABORT" and len(fields) == 2:
                test_id = fields[1]
                if state["id"] in (None, test_id):
                    if state["id"] == test_id:
                        state.update(id=None, type=None, due=None, pending_result=None,
                                     last_result_send=0.0)
                    print(f"SIMULATED Board 2 stopped acquisition for {test_id}", flush=True)
                    send(f"ACK,{test_id},ABORT", "ABORT")
            elif number == 2 and fields[0] == "ACK" and len(fields) == 3 and fields[2] == "RESULT":
                if fields[1] in known_tests:
                    print(f"SIMULATED Board 2 received result ACK for {fields[1]}", flush=True)
                    if state["id"] == fields[1]:
                        state.update(id=None, type=None, due=None, pending_result=None,
                                     last_result_send=0.0)
            # All malformed or unsupported datagrams are intentionally ignored.
    finally:
        print(f"STRESS_SUMMARY board={number} seed={args.seed + number} "
              f"sent={counts['sent']} loss_dropped={counts['loss_dropped']} "
              f"rule_dropped={counts['rule_dropped']} "
              f"delay_ms={args.delay_min_ms}..{args.delay_max_ms}", flush=True)
        sock.close()


if __name__ == "__main__":
    main()
