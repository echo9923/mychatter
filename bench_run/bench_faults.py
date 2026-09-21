"""Wire-level response/notification loss followed by retry and paged recovery."""
from __future__ import annotations

import asyncio
from collections import Counter
import json
import struct
import time
import uuid

from bench_capacity import frame, send
from validation_support import BASE_UID, BASE_THREAD, RESULTS, Stack, cache, db, write_json


class DropProxy:
    def __init__(self, mode):
        self.mode = mode
        self.dropped = Counter()
        self.tasks = set()

    async def connection(self, client_reader, client_writer):
        upstream_writer = None
        async def pump(reader, writer):
            while data := await reader.read(65536):
                writer.write(data)
                await writer.drain()

        async def filtered(reader, writer):
            while True:
                mid, body = await frame(reader)
                cid = body.get("client_message_id")
                should_drop = ((self.mode == "first_ack" and mid == 1302 and body.get("error") == 0
                                and self.dropped[cid] == 0)
                               or (self.mode == "all_live" and mid == 1701))
                if should_drop:
                    self.dropped[cid] += 1
                else:
                    await send(writer, mid, body)
        running = []
        try:
            upstream_reader, upstream_writer = await asyncio.open_connection("127.0.0.1", 28090)
            running = [asyncio.create_task(pump(client_reader, upstream_writer)),
                       asyncio.create_task(filtered(upstream_reader, client_writer))]
            self.tasks.update(running)
            done, pending = await asyncio.wait(running, return_when=asyncio.FIRST_COMPLETED)
        finally:
            for task in running:
                task.cancel()
            await asyncio.gather(*running, return_exceptions=True)
            self.tasks.difference_update(running)
            client_writer.close()
            if upstream_writer:
                upstream_writer.close()


async def expect(reader, mid, timeout=10):
    while True:
        got, body = await asyncio.wait_for(frame(reader), timeout)
        if got == mid:
            assert body.get("error", 0) == 0, body
            return body
        assert got == 1104, (got, body)


async def faults(directory):
    tag = "fault-"+uuid.uuid4().hex[:12]
    count = 100
    sender, receiver = BASE_UID+9998, BASE_UID+9999
    thread = BASE_THREAD+4999
    tokens = {uid: uuid.uuid4().hex for uid in (sender, receiver)}
    pipe = cache().pipeline(transaction=False)
    for uid, token in tokens.items():
        pipe.set(f"utoken_{uid}", token, ex=3600)
        pipe.delete(f"uip_{uid}", f"usession_{uid}", f"lock:lock_{uid}")
    pipe.execute()
    with db() as conn, conn.cursor() as cur:
        cur.execute("SELECT last_event_seq FROM users WHERE user_id=%s", (receiver,))
        checkpoint = cur.fetchone()[0]
    ack_proxy, live_proxy = DropProxy("first_ack"), DropProxy("all_live")
    server_a = await asyncio.start_server(ack_proxy.connection, "127.0.0.1", 28092)
    server_b = await asyncio.start_server(live_proxy.connection, "127.0.0.1", 28093)
    a_writer = b_writer = None
    try:
        a_reader, a_writer = await asyncio.open_connection("127.0.0.1", 28092)
        b_reader, b_writer = await asyncio.open_connection("127.0.0.1", 28093)
        await send(a_writer, 1101, {"uid": sender, "token": tokens[sender]})
        await expect(a_reader, 1102)
        await send(b_writer, 1101, {"uid": receiver, "token": tokens[receiver]})
        await expect(b_reader, 1102)
        requests = [{"client_message_id": f"{tag}-{i}", "thread_id": str(thread),
                     "target_user_id": receiver, "text_content": f"fault-payload-{i}"}
                    for i in range(count)]
        expected = {r["client_message_id"] for r in requests}
        for body in requests:
            await send(a_writer, 1301, body)
        # Confirm the proxy actually intercepted every original successful ACK.
        deadline = time.perf_counter()+15
        while time.perf_counter() < deadline and len(ack_proxy.dropped) < count:
            await asyncio.sleep(.05)
        assert set(ack_proxy.dropped) == expected, ("not all original ACKs dropped", len(ack_proxy.dropped))
        # The client has received no ACK. Retry the exact same business IDs.
        for body in requests:
            await send(a_writer, 1301, body)
        responses = [await expect(a_reader, 1302) for _ in requests]
        assert {b["client_message_id"] for b in responses} == expected
        # Every realtime notification is also dropped, including the final one.
        deadline = time.perf_counter()+10
        while time.perf_counter() < deadline and len(live_proxy.dropped) < count:
            await asyncio.sleep(.05)
        assert set(live_proxy.dropped) == expected
        pages, recovered, cursor = 0, [], checkpoint
        while True:
            await send(b_writer, 1405, {"after_event_seq": str(cursor), "limit": 30})
            body = await expect(b_reader, 1406)
            events = body.get("events", [])
            recovered.extend(events)
            pages += 1
            cursor = int(body["next_event_seq"])
            if not body.get("has_more"):
                break
            assert pages < 20
        recovered_counts = Counter(b["client_message_id"] for b in recovered)
        assert set(recovered_counts) == expected and all(n == 1 for n in recovered_counts.values())
        assert [int(b["event_seq"]) for b in recovered] == list(range(checkpoint+1, checkpoint+count+1))
        with db() as conn, conn.cursor() as cur:
            cur.execute("SELECT client_message_id,message_id,text_content FROM chat_messages WHERE client_message_id LIKE %s", (tag+"-%",))
            stored = cur.fetchall()
            cur.execute("SELECT e.event_seq,m.client_message_id FROM user_events e JOIN chat_messages m ON e.message_id=m.message_id WHERE e.recipient_user_id=%s AND m.client_message_id LIKE %s ORDER BY e.event_seq", (receiver,tag+"-%"))
            events = cur.fetchall()
            cur.execute("SELECT last_event_seq FROM users WHERE user_id=%s", (receiver,))
            head = cur.fetchone()[0]
        canonical = {cid: str(mid) for cid, mid, text in stored}
        contents = {r["client_message_id"]: r["text_content"] for r in requests}
        assert len(stored) == len(events) == count and set(canonical) == expected
        assert all(contents[cid] == text for cid, mid, text in stored)
        assert all(contents[b["client_message_id"]] == b["text_content"] for b in recovered)
        assert all(canonical[b["client_message_id"]] == b["message_id"] for b in responses)
        assert all(canonical[b["client_message_id"]] == b["message_id"] for b in recovered)
        assert head == checkpoint+count
        result = {"passed": True, "business_messages": count,
                  "wire_send_requests": 2*count, "dropped_success_responses": sum(ack_proxy.dropped.values()),
                  "retry_success_responses": len(responses),
                  "dropped_live_notifications": sum(live_proxy.dropped.values()),
                  "recovered_unique_messages": len(recovered_counts), "sync_pages": pages,
                  "database_message_rows": len(stored), "database_event_rows": len(events),
                  "event_seq_increase": head-checkpoint, "duplicate_database_rows": len(stored)-len(canonical),
                  "duplicate_sync_messages": len(recovered)-len(recovered_counts),
                  "retry_and_sync_return_canonical_ids": True,
                  "client": "headless protocol client; client SQLite behavior covered separately by C++ tests"}
        write_json(directory / "result.json", result)
        write_json(directory / "evidence.json", {"requests": requests, "retry_responses": responses,
            "sync_events": recovered, "stored_messages": stored, "stored_events": events,
            "dropped_acks": ack_proxy.dropped, "dropped_live": live_proxy.dropped})
        print(json.dumps(result), flush=True)
    finally:
        for writer in (a_writer, b_writer):
            if writer:
                writer.close()
                await writer.wait_closed()
        server_a.close()
        server_b.close()
        await server_a.wait_closed()
        await server_b.wait_closed()
        tasks = list(ack_proxy.tasks | live_proxy.tasks)
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)


def main():
    directory = RESULTS / "wire-faults"
    directory.mkdir(parents=True, exist_ok=False)
    stack = Stack(directory)
    try:
        stack.start_single()
        asyncio.run(faults(directory))
    except Exception as error:
        write_json(directory / "fatal.json", {"passed": False, "error": repr(error)})
        raise
    finally:
        stack.close()


if __name__ == "__main__":
    main()
