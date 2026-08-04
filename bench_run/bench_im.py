# bench_im.py — llfcchat IM benchmark.
# Phases: gate login -> chat login (hold conns) -> paired text messaging.
# Usage: python bench_im.py --users 100 --per-pair 100 [--window 64]
import argparse, asyncio, json, struct, time, statistics, sys
from concurrent.futures import ThreadPoolExecutor
import urllib.request

GATE = ("127.0.0.1", 18080)
BASE_UID = 90001

def gate_login(email, passwd, timeout=30):
    req = urllib.request.Request(
        f"http://{GATE[0]}:{GATE[1]}/user_login",
        data=json.dumps({"email": email, "passwd": passwd}).encode(),
        headers={"Content-Type": "text/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())

async def send_frame(writer, mid, obj):
    body = json.dumps(obj).encode()
    writer.write(struct.pack(">HH", mid, len(body)) + body)
    await writer.drain()

async def recv_frame(reader):
    hdr = await reader.readexactly(4)
    mid, ln = struct.unpack(">HH", hdr)
    body = await reader.readexactly(ln)
    return mid, json.loads(body)

def pct(lat, p):
    if not lat: return 0.0
    return lat[min(len(lat) - 1, int(len(lat) * p))]

async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--users", type=int, default=100)
    ap.add_argument("--per-pair", type=int, default=100)
    ap.add_argument("--window", type=int, default=64)
    ap.add_argument("--runtag", type=str, default=str(int(time.time())),
                    help="unique_id prefix tag so repeated runs don't conflict on the DB unique index")
    ap.add_argument("--conn-concurrency", type=int, default=256,
                    help="max simultaneous chat-login TCP connects (stagger the connect storm)")
    args = ap.parse_args()
    if args.users % 2: args.users -= 1
    uids = [BASE_UID + i for i in range(args.users)]

    # ---- Phase 1: gate logins ----
    t0 = time.perf_counter()
    def gl(uid):
        t = time.perf_counter()
        try:
            info = gate_login(f"bench{uid-BASE_UID+1}@bench.local", "123456")
            return uid, info, (time.perf_counter() - t) * 1000, None
        except Exception as e:
            return uid, None, (time.perf_counter() - t) * 1000, repr(e)
    gate_ok, gate_lat, gate_err = [], [], 0
    with ThreadPoolExecutor(max_workers=32) as ex:
        for uid, info, dt, err in ex.map(gl, uids):
            gate_lat.append(dt)
            if info: gate_ok.append((uid, info))
            else: gate_err += 1
    gd = time.perf_counter() - t0
    gate_lat.sort()
    print(f"[gate] {len(gate_ok)}/{len(uids)} ok in {gd:.1f}s "
          f"({len(gate_ok)/gd:.0f}/s) P50={pct(gate_lat,.5):.0f}ms P99={pct(gate_lat,.99):.0f}ms err={gate_err}", flush=True)

    # ---- Phase 2: chat logins ----
    sem = asyncio.Semaphore(args.conn_concurrency)
    async def cl(uid, info):
        t = time.perf_counter()
        try:
            async with sem:
                r, w = await asyncio.open_connection(info["chathost"], int(info["chatport"]))
            await send_frame(w, 1005, {"uid": uid, "chat_ticket": info["chat_ticket"]})
            mid, rsp = await asyncio.wait_for(recv_frame(r), 30)
            if mid == 1006 and rsp.get("error") == 0:
                return uid, r, w, (time.perf_counter() - t) * 1000
            w.close(); return uid, None, None, None
        except Exception:
            return uid, None, None, None
    t0 = time.perf_counter()
    conns, chat_lat = {}, []
    for uid, r, w, dt in await asyncio.gather(*[cl(u, i) for u, i in gate_ok]):
        if r: conns[uid] = (r, w); chat_lat.append(dt)
    cd = time.perf_counter() - t0
    chat_lat.sort()
    by_port = {}
    for uid, info in gate_ok:
        if uid in conns: by_port[info["chatport"]] = by_port.get(info["chatport"], 0) + 1
    print(f"[chat] {len(conns)}/{len(gate_ok)} concurrent authed conns in {cd:.1f}s "
          f"P50={pct(chat_lat,.5):.0f}ms P99={pct(chat_lat,.99):.0f}ms dist={by_port}", flush=True)

    # ---- Phase 3: paired messaging ----
    puids = sorted(conns)
    pairs = [(puids[i], puids[i + len(puids)//2]) for i in range(len(puids)//2)]
    K, CONTENT = args.per_pair, "benchmark-payload-" + "x" * 80
    send_t0 = {a: {} for a, _ in pairs}
    acked = {a: set() for a, _ in pairs}
    ack_err = {a: 0 for a, _ in pairs}
    recv_got = {b: set() for _, b in pairs}
    rtts = []

    async def reader_loop(uid):
        r, _ = conns[uid]
        try:
            while True:
                mid, body = await recv_frame(r)
                if mid == 1018:
                    if body.get("error", 0) != 0: ack_err[uid] = ack_err.get(uid, 0) + 1
                    for env in body.get("chat_datas", []):
                        uq = env.get("unique_id")
                        if uq in send_t0.get(uid, {}):
                            rtts.append((time.perf_counter() - send_t0[uid].pop(uq)) * 1000)
                            acked[uid].add(uq)
                elif mid == 1019:
                    for env in body.get("chat_datas", []):
                        recv_got.setdefault(uid, set()).add(env.get("unique_id"))
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            pass

    async def sender_loop(a, b):
        _, w = conns[a]
        tid = 900000 + a
        for k in range(K):
            uq = f"b-{args.runtag}-{a}-{k}"
            while len(send_t0[a]) >= args.window:
                await asyncio.sleep(0.0005)
            send_t0[a][uq] = time.perf_counter()
            await send_frame(w, 1017, {"fromuid": a, "touid": b, "thread_id": tid,
                "text_array": [{"content": CONTENT, "unique_id": uq}]})

    readers = [asyncio.create_task(reader_loop(u)) for u in conns]
    t0 = time.perf_counter()
    total = len(pairs) * K
    senders = [asyncio.create_task(sender_loop(a, b)) for a, b in pairs]
    while not all(s.done() for s in senders):
        await asyncio.sleep(5)
        got = sum(len(acked[a]) for a, _ in pairs)
        el = time.perf_counter() - t0
        print(f"[msg] progress acked={got}/{total} {got/el:.0f} msg/s", flush=True)
    sd = time.perf_counter() - t0
    sent = sum(len(acked[a]) + len(send_t0[a]) for a, _ in pairs)
    print(f"[msg] pumped {sent} in {sd:.1f}s", flush=True)

    deadline = time.perf_counter() + 180
    while time.perf_counter() < deadline and sum(len(send_t0[a]) for a, _ in pairs):
        await asyncio.sleep(0.5)
    dur = time.perf_counter() - t0
    total_acked = sum(len(acked[a]) for a, _ in pairs)
    rtts.sort()
    print(f"[msg] ACK {total_acked}/{total} in {dur:.1f}s = {total_acked/dur:.0f} msg/s "
          f"RTT P50={pct(rtts,.5):.1f}ms P95={pct(rtts,.95):.1f}ms P99={pct(rtts,.99):.1f}ms "
          f"outstanding={sum(len(send_t0[a]) for a,_ in pairs)} ack_err={sum(ack_err.values())}", flush=True)

    # let notifies settle
    await asyncio.sleep(5)
    exp = {b: {f"b-{args.runtag}-{a}-{k}" for k in range(K)} for a, b in pairs}
    lost = sum(len(exp[b] - recv_got.get(b, set())) for _, b in pairs)
    got_n = sum(len(recv_got.get(b, set()) & exp[b]) for _, b in pairs)
    print(f"[recv] notify delivered {got_n}/{total}, lost={lost}", flush=True)

    for t in readers: t.cancel()
    for r, w in conns.values(): w.close()

asyncio.run(main())
