"""Sustained one-ChatServer benchmark with heartbeats and durable reconciliation.

Tokens are seeded in the isolated Redis before timing. TCP login/authentication
is real; this measures authenticated chat capacity, not password-login QPS.
"""
from __future__ import annotations

import argparse
import asyncio
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import statistics
import struct
import time
import urllib.request
import uuid

import psutil

from validation_support import (ROOT, RESULTS, BASE_UID, BASE_THREAD, TEST_PASSWORD,
                                Stack, cache, db, write_json)


def percentile(values, q):
    if not values:
        return None
    ordered = sorted(values)
    return round(ordered[max(0, math.ceil(len(ordered)*q)-1)], 3)


def distribution(values):
    return {"count": len(values), "p50": percentile(values, .50),
            "p95": percentile(values, .95), "p99": percentile(values, .99),
            "max": round(max(values), 3) if values else None}


async def frame(reader):
    mid, size = struct.unpack(">HH", await reader.readexactly(4))
    return mid, json.loads(await reader.readexactly(size))


async def send(writer, mid, data):
    body = json.dumps(data, separators=(",", ":")).encode()
    writer.write(struct.pack(">HH", mid, len(body))+body)
    await writer.drain()


def gate_smoke():
    request = urllib.request.Request("http://127.0.0.1:28080/user_login",
        data=json.dumps({"email": "validation_0@example.invalid", "passwd": TEST_PASSWORD}).encode(),
        headers={"Content-Type": "application/json"}, method="POST")
    started = time.perf_counter()
    with urllib.request.urlopen(request, timeout=30) as response:
        result = json.load(response)
    assert result.get("error") == 0 and result.get("token"), {k: v for k, v in result.items() if k != "token"}
    return {"success": True, "latency_ms": round((time.perf_counter()-started)*1000, 3)}


async def run(args, stack, directory):
    wall_started = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    driver_hash = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    tag = "val-"+uuid.uuid4().hex[:12]
    active = int(args.users * args.active_fraction)
    assert 0 < active <= (args.users if args.bidirectional else args.users//2)
    rate = active / args.interval
    planned = int(args.seconds * rate)
    payload = "x" * args.payload_bytes
    tokens = {BASE_UID+i: uuid.uuid4().hex for i in range(args.users)}
    pipe = cache().pipeline(transaction=False)
    for uid, token in tokens.items():
        # The previous stage terminates its server. Remove only these fixture
        # users' transient routing/session locks before opening a new stage.
        pipe.delete(f"uip_{uid}", f"usession_{uid}", f"lock:lock_{uid}")
        pipe.set(f"utoken_{uid}", token, ex=3600)
    pipe.execute()

    connections, tasks, login_ms, login_errors = {}, [], [], []
    records, unexpected, samples = {}, [], []
    ack_errors, disconnects, heartbeat_responses = Counter(), [], 0
    acked, received, live = 0, 0, set()
    closing, phase = False, "login"
    begin = time.perf_counter()
    admission = asyncio.Semaphore(args.login_concurrency)

    async def reader_loop(uid, reader):
        nonlocal heartbeat_responses, acked, received
        try:
            while True:
                mid, body = await frame(reader)
                now = time.perf_counter()
                if mid == 1104:
                    heartbeat_responses += 1
                    continue
                cid = body.get("client_message_id")
                if mid == 1302:
                    if body.get("error", -1) != 0:
                        ack_errors[str(body.get("error"))] += 1
                    row = records.get(cid)
                    if row is not None:
                        row["ack_count"] += 1
                        if body.get("error") == 0:
                            if "ack_at" not in row:
                                acked += 1
                            row.setdefault("ack_at", now)
                            row["message_id"] = str(body["message_id"])
                        else:
                            row["error"] = body.get("error")
                    else:
                        unexpected.append({"kind": "unknown_ack", "uid": uid, "id": cid})
                elif mid == 1701:
                    row = records.get(cid)
                    if row is None:
                        unexpected.append({"kind": "unknown_notify", "uid": uid, "id": cid})
                    else:
                        row["notify_count"] += 1
                        if "notify_at" not in row:
                            received += 1
                        row.setdefault("notify_at", now)
                        row["notify_message_id"] = str(body.get("message_id"))
                        if uid != row["receiver"] or body.get("text_content") != payload:
                            unexpected.append({"kind": "content_or_recipient_mismatch", "id": cid})
                else:
                    unexpected.append({"kind": "other_frame", "uid": uid, "type": mid})
        except asyncio.CancelledError:
            pass
        except Exception as error:
            if not closing:
                disconnects.append({"uid": uid, "phase": phase, "error": repr(error),
                                    "after_start_s": round(time.perf_counter()-begin, 3)})
        finally:
            live.discard(uid)

    async def heartbeat(uid, writer):
        await asyncio.sleep((uid % 100)*0.05)
        try:
            while True:
                await send(writer, 1103, {"uid": uid})
                await asyncio.sleep(5)
        except (ConnectionError, OSError):
            pass

    async def login(uid, token):
        writer = None
        async with admission:
            started = time.perf_counter()
            try:
                reader, writer = await asyncio.wait_for(asyncio.open_connection("127.0.0.1", 28090), 15)
                await send(writer, 1101, {"uid": uid, "token": token})
                mid, body = await asyncio.wait_for(frame(reader), 30)
                if mid != 1102 or body.get("error") != 0:
                    raise RuntimeError(f"login type={mid} error={body.get('error')}")
                connections[uid] = writer
                live.add(uid)
                login_ms.append((time.perf_counter()-started)*1000)
                tasks.extend([asyncio.create_task(reader_loop(uid, reader)),
                              asyncio.create_task(heartbeat(uid, writer))])
            except Exception as error:
                login_errors.append({"uid": uid, "error": repr(error)})
                if writer:
                    writer.close()

    chat_process = psutil.Process(stack.processes["chat"].pid)
    driver_process = psutil.Process()
    chat_process.cpu_percent()
    driver_process.cpu_percent()
    psutil.cpu_percent()

    sample_log = (directory / "samples.jsonl").open("w", encoding="utf-8")

    async def monitor():
        while True:
            sample = {"elapsed_s": round(time.perf_counter()-begin, 3), "phase": phase,
                "chat_rss_mib": round(chat_process.memory_info().rss/2**20, 3),
                "chat_cpu_pct_one_core": chat_process.cpu_percent(),
                "driver_cpu_pct_one_core": driver_process.cpu_percent(),
                "host_cpu_pct": psutil.cpu_percent(),
                "host_available_mib": round(psutil.virtual_memory().available/2**20, 3),
                "connections": len(live), "sent": len(records),
                "acked": acked, "received": received,
                "pending_ack": len(records)-acked-sum(ack_errors.values()),
                "pending_receive": len(records)-received,
                "ack_error_count": sum(ack_errors.values()),
                "heartbeat_responses": heartbeat_responses}
            samples.append(sample)
            sample_log.write(json.dumps(sample)+"\n")
            sample_log.flush()
            await asyncio.sleep(1)

    monitor_task = asyncio.create_task(monitor())
    container_samples = []

    async def monitor_containers():
        process = None
        try:
            while True:
                process = await asyncio.create_subprocess_exec(
                    "docker", "stats", "--no-stream", "--format", "{{json .}}",
                    "llfc-validation-20260921-mysql", "llfc-validation-20260921-redis",
                    stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
                stdout, stderr = await process.communicate()
                sample = {"elapsed_s": round(time.perf_counter()-begin, 3), "phase": phase,
                          "exit_code": process.returncode,
                          "containers": [json.loads(line) for line in stdout.decode().splitlines()],
                          "error": stderr.decode(errors="replace")}
                container_samples.append(sample)
                write_json(directory / "container-samples.json", container_samples)
                await asyncio.sleep(5)
        finally:
            if process is not None and process.returncode is None:
                process.terminate()
                await process.wait()

    container_task = asyncio.create_task(monitor_containers())
    try:
        await asyncio.gather(*(login(uid, token) for uid, token in tokens.items()))
        login_seconds = time.perf_counter()-begin
        print(f"login {len(connections)}/{args.users} in {login_seconds:.2f}s errors={len(login_errors)}", flush=True)
        write_json(directory / "admission.json", {"authenticated": len(connections),
            "expected": args.users, "errors": login_errors,
            "server_exit_codes": {name: process.poll() for name, process in stack.processes.items()}})
        if len(connections) != args.users:
            raise RuntimeError("Admission failed; see admission.json before interpreting capacity")
        phase = "warmup"
        await asyncio.sleep(3)
        phase = "measurement"
        database_before = database_snapshot()
        write_json(directory / "database-before.json", database_before)
        start = time.perf_counter()
        schedule_lateness = []
        send_errors = []
        for i in range(planned):
            due = start + i/rate
            await asyncio.sleep(max(0, due-time.perf_counter()))
            uid = BASE_UID + (i % active)*(1 if args.bidirectional else 2)
            receiver = BASE_UID + ((uid-BASE_UID) ^ 1)
            cid = f"{tag}-{i}"
            if uid not in connections:
                send_errors.append({"id": cid, "error": "sender_login_failed"})
                continue
            sent_at = time.perf_counter()
            schedule_lateness.append((sent_at-due)*1000)
            records[cid] = {"sender": uid, "receiver": receiver, "sent_at": sent_at,
                            "ack_count": 0, "notify_count": 0}
            try:
                await send(connections[uid], 1301,
                    {"client_message_id": cid, "target_user_id": receiver,
                     "thread_id": str(BASE_THREAD+(uid-BASE_UID)//2), "text_content": payload})
            except Exception as error:
                send_errors.append({"id": cid, "error": repr(error)})
            if i and i % max(1, int(rate*15)) == 0:
                print(f"progress sent={len(records)}/{planned} ack={acked} "
                      f"notify={received} errors={sum(ack_errors.values())} live={len(live)}", flush=True)
        await asyncio.sleep(max(0, start+args.seconds-time.perf_counter()))
        measurement_end = time.perf_counter()
        phase = "drain"
        deadline = time.perf_counter()+args.drain_seconds
        while time.perf_counter() < deadline:
            if all("ack_at" in r and "notify_at" in r for r in records.values()):
                break
            await asyncio.sleep(.1)
        await asyncio.sleep(1)
        ack_times = [(r["ack_at"]-r["sent_at"])*1000 for r in records.values() if "ack_at" in r]
        notify_times = [(r["notify_at"]-r["sent_at"])*1000 for r in records.values() if "notify_at" in r]
        phase = "reconcile"
        database_after = database_snapshot()
        write_json(directory / "database-after.json", database_after)
        with db() as conn, conn.cursor() as cur:
            cur.execute("SELECT client_message_id,message_id,status,text_content FROM chat_messages WHERE client_message_id LIKE %s", (tag+"-%",))
            durable_rows = cur.fetchall()
            cur.execute("SELECT m.client_message_id,e.recipient_user_id,e.event_seq FROM user_events e JOIN chat_messages m ON m.message_id=e.message_id WHERE m.client_message_id LIKE %s", (tag+"-%",))
            event_rows = cur.fetchall()
        durable_counts = Counter(row[0] for row in durable_rows)
        event_counts = Counter(row[0] for row in event_rows)
        expected_ids = set(records)
        durable_ok = (set(durable_counts) == expected_ids and all(v == 1 for v in durable_counts.values())
                      and all(status == 1 and text == payload and str(mid) == records[cid].get("message_id")
                              for cid, mid, status, text in durable_rows))
        event_ok = (set(event_counts) == expected_ids and all(v == 1 for v in event_counts.values())
                    and all(recipient == records[cid]["receiver"] for cid, recipient, seq in event_rows))
        recipient_sequences = defaultdict(list)
        for cid, recipient, seq in event_rows:
            recipient_sequences[recipient].append(int(seq))
        sequences_contiguous = all(max(values)-min(values)+1 == len(set(values))
                                   for values in recipient_sequences.values())
        notification_ids_match = all(r.get("message_id") == r.get("notify_message_id")
                                    for r in records.values() if "ack_at" in r and "notify_at" in r)
        acknowledged_ids = {cid for cid, row in records.items() if "ack_at" in row}
        received_ids = {cid for cid, row in records.items() if "notify_at" in row}
        successful_sets_equal = acknowledged_ids == received_ids == set(durable_counts) == set(event_counts)
        duplicate_acks = sum(max(0, r["ack_count"]-1) for r in records.values())
        duplicate_notifications = sum(max(0, r["notify_count"]-1) for r in records.values())
        ack_stats, notify_stats = distribution(ack_times), distribution(notify_times)
        windows = []
        for offset in range(0, args.seconds, 60):
            rows = [r for r in records.values() if offset <= r["sent_at"]-start < offset+60]
            windows.append({"from_second": offset, "sent": len(rows),
                "acked": sum("ack_at" in r for r in rows),
                "received": sum("notify_at" in r for r in rows),
                "ack_ms": distribution([(r["ack_at"]-r["sent_at"])*1000 for r in rows if "ack_at" in r]),
                "receive_ms": distribution([(r["notify_at"]-r["sent_at"])*1000 for r in rows if "notify_at" in r])})
        result = {"stage": directory.name, "tag": tag, "started_at": wall_started,
            "binary_sha256": hashlib.sha256((ROOT / "out/run/Release/chatserver1/ChatServer.exe").read_bytes()).hexdigest(),
            "driver_sha256": driver_hash,
            "users": args.users, "authenticated": len(connections), "login_seconds": round(login_seconds, 3),
            "live_at_end": len(live), "bidirectional": args.bidirectional,
            "server_parameters": {"logic_workers": args.logic_workers,
                                  "delivery_workers": args.delivery_workers,
                                  "mysql_pool_size_requested": args.mysql_pool_size,
                                  "deadlock_retries_requested": args.deadlock_retries},
            "database_status_delta": {k: v-database_before["status"].get(k, v)
                                      for k, v in database_after["status"].items()},
            "login_latency_ms": distribution(login_ms), "login_errors": login_errors,
            "duration_seconds": args.seconds, "active_senders": active,
            "active_fraction": args.active_fraction, "send_interval_seconds": args.interval,
            "payload_bytes": args.payload_bytes, "target_messages_per_second": rate,
            "planned_messages": planned, "attempted_messages": len(records),
            "send_count_per_user": distribution(list(Counter(r["sender"] for r in records.values()).values())),
            "successful_acks": len(ack_times), "received_unique": len(notify_times),
            "acked_during_measurement": sum(r.get("ack_at", math.inf) <= measurement_end for r in records.values()),
            "received_during_measurement": sum(r.get("notify_at", math.inf) <= measurement_end for r in records.values()),
            "actual_measurement_seconds": round(measurement_end-start, 3),
            "ack_latency_ms": ack_stats, "receiver_latency_ms": notify_stats,
            "minute_windows": windows,
            "scheduler_lateness_ms": distribution(schedule_lateness),
            "ack_errors": dict(ack_errors), "send_errors": send_errors, "disconnects": disconnects,
            "duplicate_acks": duplicate_acks, "duplicate_notifications": duplicate_notifications,
            "unexpected": unexpected, "heartbeat_responses": heartbeat_responses,
            "durable_message_rows": len(durable_rows), "durable_event_rows": len(event_rows),
            "database_reconciled": durable_ok and event_ok,
            "successful_ack_receive_database_sets_equal": successful_sets_equal,
            "notification_message_ids_match": notification_ids_match,
            "event_sequences_contiguous": sequences_contiguous,
            "missing_ack_ids": [cid for cid, r in records.items() if "ack_at" not in r],
            "missing_notify_ids": [cid for cid, r in records.items() if "notify_at" not in r],
            "slo": {"required_success_fraction": 1.0, "p99_ack_and_receive_ms_max": args.p99_ms,
                    "max_disconnects": 0, "max_scheduler_p99_ms": 100},
            "resources": {"chat_peak_rss_mib": max(s["chat_rss_mib"] for s in samples),
                "chat_cpu_mean_pct_one_core": round(statistics.mean(s["chat_cpu_pct_one_core"] for s in samples if s["phase"] == "measurement"), 2),
                "host_cpu_mean_pct": round(statistics.mean(s["host_cpu_pct"] for s in samples if s["phase"] == "measurement"), 2),
                "driver_cpu_mean_pct_one_core": round(statistics.mean(s["driver_cpu_pct_one_core"] for s in samples if s["phase"] == "measurement"), 2)}}
        result["passed"] = (len(connections) == args.users and len(records) == planned
            and len(ack_times) == planned and len(notify_times) == planned
            and not ack_errors and not send_errors and not disconnects and not unexpected
            and duplicate_notifications == 0 and duplicate_acks == 0 and durable_ok and event_ok
            and notification_ids_match and sequences_contiguous
            and ack_stats["p99"] is not None and ack_stats["p99"] <= args.p99_ms
            and notify_stats["p99"] is not None and notify_stats["p99"] <= args.p99_ms
            and all(w["ack_ms"]["p99"] is not None and w["ack_ms"]["p99"] <= args.p99_ms
                    and w["receive_ms"]["p99"] is not None and w["receive_ms"]["p99"] <= args.p99_ms
                    for w in windows)
            and percentile(schedule_lateness, .99) <= 100)
        write_json(directory / "result.json", result)
        write_json(directory / "samples.json", samples)
        with (directory / "messages.jsonl").open("w", encoding="utf-8") as output:
            for cid, row in records.items():
                output.write(json.dumps({"client_message_id": cid, **row})+"\n")
        print(json.dumps({k: result[k] for k in ("users", "duration_seconds", "successful_acks", "received_unique", "ack_latency_ms", "receiver_latency_ms", "database_reconciled", "passed")}), flush=True)
        return result["passed"]
    finally:
        closing = True
        monitor_task.cancel()
        container_task.cancel()
        for task in tasks:
            task.cancel()
        await asyncio.gather(monitor_task, container_task, *tasks, return_exceptions=True)
        sample_log.close()
        for writer in connections.values():
            writer.close()
        await asyncio.gather(*(writer.wait_closed() for writer in connections.values()), return_exceptions=True)


def database_snapshot():
    with db() as conn, conn.cursor() as cur:
        cur.execute("SHOW GLOBAL STATUS WHERE Variable_name IN "
                    "('Innodb_deadlocks','Innodb_row_lock_waits','Innodb_row_lock_time',"
                    "'Threads_running','Threads_connected','Com_commit','Com_rollback',"
                    "'Innodb_log_waits','Innodb_buffer_pool_reads','Innodb_buffer_pool_read_requests',"
                    "'Bytes_received','Bytes_sent','Questions')")
        status = {key: int(value) for key, value in cur.fetchall()}
        cur.execute("SELECT NAME,COUNT FROM information_schema.INNODB_METRICS "
                    "WHERE NAME IN ('lock_deadlocks','lock_timeouts')")
        status.update({key: int(value) for key, value in cur.fetchall()})
        cur.execute("SELECT DIGEST_TEXT,COUNT_STAR,SUM_TIMER_WAIT,SUM_LOCK_TIME,SUM_ERRORS "
                    "FROM performance_schema.events_statements_summary_by_digest "
                    "WHERE SCHEMA_NAME=DATABASE() ORDER BY SUM_TIMER_WAIT DESC LIMIT 40")
        digests = [dict(zip(("sql", "count", "time_ps", "lock_time_ps", "errors"), row))
                   for row in cur.fetchall()]
        cur.execute("SELECT EVENT_NAME,COUNT_STAR,SUM_TIMER_WAIT FROM "
                    "performance_schema.file_summary_by_event_name ORDER BY SUM_TIMER_WAIT DESC LIMIT 10")
        file_io = [dict(zip(("event", "count", "time_ps"), row)) for row in cur.fetchall()]
        cur.execute("SHOW ENGINE INNODB STATUS")
        innodb = cur.fetchone()[2]
        return {"status": status, "digests": digests, "file_io": file_io, "innodb": innodb}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--users", type=int, required=True)
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--active-fraction", type=float, default=.2)
    ap.add_argument("--interval", type=float, default=10)
    ap.add_argument("--payload-bytes", type=int, default=256)
    ap.add_argument("--login-concurrency", type=int, default=64)
    ap.add_argument("--drain-seconds", type=int, default=30)
    ap.add_argument("--p99-ms", type=float, default=1000)
    ap.add_argument("--label", required=True)
    ap.add_argument("--output", type=Path, default=RESULTS)
    ap.add_argument("--bidirectional", action="store_true")
    ap.add_argument("--logic-workers", type=int, default=4)
    ap.add_argument("--delivery-workers", type=int, default=4)
    ap.add_argument("--mysql-pool-size", type=int, default=5)
    ap.add_argument("--deadlock-retries", type=int, default=2)
    args = ap.parse_args()
    assert args.users % 2 == 0 and args.seconds > 0
    directory = args.output.resolve() / args.label
    directory.relative_to(ROOT / "bench_run/results")
    if directory.exists():
        raise RuntimeError("Use a new label to preserve previous raw evidence")
    directory.mkdir(parents=True)
    stack = Stack(directory)
    try:
        stack.start_single(args.logic_workers, args.delivery_workers, args.mysql_pool_size, args.deadlock_retries)
        time.sleep(2)
        write_json(directory / "gate_smoke.json", gate_smoke())
        return 0 if asyncio.run(run(args, stack, directory)) else 1
    except Exception as error:
        write_json(directory / "fatal.json", {"error": repr(error)})
        raise
    finally:
        stack.close()


if __name__ == "__main__":
    raise SystemExit(main())
