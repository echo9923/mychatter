#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""bench_resource.py — P1c 资源传输基准工具（真实全链路，未运行验证）。

两种 wire 模式（--mode，默认 new）：
  new      v2 二进制协议：1505/1512 为 [u16 meta_len BE][meta JSON][raw]，
           256KiB 分片（proto/resource_wire.h 同一规格）。
  baseline v1 旧协议对照：1505/1512 为 JSON + Base64，32KiB 分片。
           **baseline 必须指向独立运行的旧版（P1c 之前）ResourceServer 二进制
           所监听的端口**——v2 服务器不兼容旧协议，同端口无法混跑。

两种模式共用同一全链路流程：
  1. ChatServer 4B 帧头 [u16 msg_id BE][u16 body_len BE] 连接 + 1101 鉴权
     （uid/token）→ 1102。
  2. 1503 创建 Pending 资源（client_message_id=uuid、thread_id、
     target_user_id、message_type、original_file_name、file_size_bytes、
     sha256、mime_type ——按实际待传 blob 计算）→ 1504 取 message_id。
  3. ResourceServer 6B 帧头 [u16 msg_id BE][u32 body_len BE] 连接 + 1501
     鉴权 → 1502。
  4. 1507 验证：error=0、status=Pending(0)、file_size_bytes/sha256 与本地
     一致、fresh 会话 server_offset=0（--message-id 复用路径同样校验）。
  5. 逐片上传（new=256KiB binary / baseline=32KiB JSON+Base64）：
     每片校验 1506 error=0、server_offset 前进；new 始终要求回显 offset
     （P1a 起字段必在），baseline 仅在字段存在时严格比对（P1a 前的旧
     1506 不回显）；末片要求 status=Published(1) 且携带 event_seq。
  6. 可选 --download：1509 → 1510 校验元数据 → 1511 循环拉取 1512
     （new=binary，baseline=JSON+Base64），逐片校验 offset/bytes/
     chunk_sha256/total_size、raw 长度等于预期片长且不越过总大小，拼装后
     整体 SHA-256 必须等于声明值；0 字节分片立即报错（不无限循环）。

指标口径：
  - 每阶段 elapsed 用该阶段墙钟起止（首帧 send 前到最后响应后）——
    包含分片 SHA 计算/编码成本；payload 生成、1503 创建、1101/1501 鉴权
    均不计入传输阶段。
  - mb_per_s 按 elapsed 墙钟折算；chunk_p99_ms 为逐片 RTT（t0 在 send
    之前）的 nearest-rank p99（ceil(0.99*N)-1）。
  - 本脚本是逐片停等（在途窗口=1）的协议基准，不能代表真实 Qt 客户端
    TransferEngine 的 4 片在途窗口吞吐。

用法示例：
  python3 bench_run/bench_resource.py --host 127.0.0.1 \\
      --chat-port 18096 --res-port 18099 --uid 2001 --token <token> \\
      --peer 2002 --thread-id 35 --size-mb 100 --download --out new.json
  # baseline：旧版 ResourceServer 另起端口（如 19099），v2 端口不可复用
  python3 bench_run/bench_resource.py --host 127.0.0.1 \\
      --chat-port 18096 --res-port 19099 --uid 2001 --token <token> \\
      --peer 2002 --thread-id 35 --size-mb 100 --mode baseline --out old.json
"""
import argparse
import base64
import hashlib
import json
import math
import socket
import struct
import sys
import time
import uuid

CHUNK_NEW = 256 * 1024       # v2：proto/resource_wire.h 同一规格
CHUNK_BASELINE = 32 * 1024   # v1：JSON+Base64
CHAT_HEAD = struct.Struct(">HH")   # u16 msg_id + u16 body_len
RES_HEAD = struct.Struct(">HI")    # u16 msg_id + u32 body_len
MSG_PENDING, MSG_PUBLISHED = 0, 1
ERR_SUCCESS = 0


class FramedConn(object):
    """4B（聊天）或 6B（资源）帧头连接；recv 带长度预算与 msg_id 校验。"""

    def __init__(self, host, port, head, name):
        self.head = head
        self.name = name
        self.sock = socket.create_connection((host, port), timeout=30)
        self.buf = b""

    def send(self, msg_id, body):
        self.sock.sendall(self.head.pack(msg_id, len(body)) + body)

    def _recv_exact(self, n):
        if n > 8 * 1024 * 1024:
            raise RuntimeError("%s: body length %d over budget" % (self.name, n))
        while len(self.buf) < n:
            data = self.sock.recv(65536)
            if not data:
                raise EOFError("%s: connection closed" % self.name)
            self.buf += data
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self, expect_id=None, timeout=30):
        self.sock.settimeout(timeout)
        head = self._recv_exact(self.head.size)
        msg_id, length = self.head.unpack(head)
        if msg_id == 0 or msg_id > 2048:
            raise RuntimeError("%s: bad msg_id %d" % (self.name, msg_id))
        body = self._recv_exact(length)
        if expect_id is not None and msg_id != expect_id:
            raise RuntimeError("%s: expected msg %d got %d" %
                               (self.name, expect_id, msg_id))
        return msg_id, body

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def chat_login(host, port, uid, token):
    conn = FramedConn(host, port, CHAT_HEAD, "chat")
    conn.send(1101, json.dumps({"uid": uid, "token": token}).encode())
    _, body = conn.recv(expect_id=1102)
    rsp = json.loads(body)
    if rsp.get("error") != ERR_SUCCESS:
        raise RuntimeError("1101 auth failed: %r" % rsp)
    return conn


def create_resource(chat, peer, thread_id, size, sha, name):
    req = {
        "client_message_id": uuid.uuid4().hex,
        "thread_id": str(thread_id),
        "target_user_id": peer,
        "message_type": 3,  # FILE
        "original_file_name": name,
        "file_size_bytes": str(size),
        "sha256": sha,
        "mime_type": "application/octet-stream",
    }
    chat.send(1503, json.dumps(req).encode())
    _, body = chat.recv(expect_id=1504)
    rsp = json.loads(body)
    if rsp.get("error") != ERR_SUCCESS:
        raise RuntimeError("1503 create failed: %r" % rsp)
    mid = int(str(rsp["message_id"]))
    if mid <= 0:
        raise RuntimeError("1503 returned bad message_id: %r" % rsp)
    return mid


def res_login(host, port, uid, token):
    conn = FramedConn(host, port, RES_HEAD, "resource")
    conn.send(1501, json.dumps({"uid": uid, "token": token}).encode())
    _, body = conn.recv(expect_id=1502)
    rsp = json.loads(body)
    if rsp.get("error") != ERR_SUCCESS:
        raise RuntimeError("1501 auth failed: %r" % rsp)
    return conn


def query_progress(res, mid):
    res.send(1507, json.dumps({"message_id": str(mid)}).encode())
    _, body = res.recv(expect_id=1508)
    rsp = json.loads(body)
    if rsp.get("error") != ERR_SUCCESS:
        raise RuntimeError("1507 failed: %r" % rsp)
    return rsp


# ---- 1505/1512 编解码（两种 wire 模式） ------------------------------------

def encode_1505_new(mid, offset, sha, raw):
    meta = json.dumps({
        "message_id": str(mid),
        "offset": str(offset),
        "chunk_sha256": sha,
    }, separators=(",", ":")).encode()
    return struct.pack(">H", len(meta)) + meta + raw


def encode_1505_baseline(mid, offset, sha, raw):
    return json.dumps({
        "message_id": str(mid),
        "offset": str(offset),
        "chunk_sha256": sha,
        "data": base64.b64encode(raw).decode(),
    }, separators=(",", ":")).encode()


def decode_1512_new(body):
    if len(body) < 3:
        raise RuntimeError("1512 frame too short: %d" % len(body))
    meta_len = struct.unpack(">H", body[:2])[0]
    if meta_len == 0 or meta_len > 8 * 1024 or 2 + meta_len > len(body):
        raise RuntimeError("1512 bad meta_len %d" % meta_len)
    meta = json.loads(body[2:2 + meta_len])
    return meta, body[2 + meta_len:]


def decode_1512_baseline(body):
    meta = json.loads(body)
    data = meta.get("data", "")
    return meta, base64.b64decode(data) if data else b""


# ---- 上传/下载（共用校验，模式差异仅编码与 offset 回显策略） ----------------

def upload(res, mid, blob, mode):
    chunk_size = CHUNK_NEW if mode == "new" else CHUNK_BASELINE
    encode = encode_1505_new if mode == "new" else encode_1505_baseline
    latencies = []
    off = 0
    event_seq = None
    wall_start = time.perf_counter()
    while off < len(blob):
        raw = blob[off:off + chunk_size]
        sha = hashlib.sha256(raw).hexdigest()
        t0 = time.perf_counter()  #RTT 计时含 send（防止低估往返）
        res.send(1505, encode(mid, off, sha, raw))
        _, body = res.recv(expect_id=1506)
        latencies.append(time.perf_counter() - t0)
        rsp = json.loads(body)
        if rsp.get("error") != ERR_SUCCESS:
            raise RuntimeError("1505 failed at %d: %r" % (off, rsp))
        if mode == "new":
            #P1a 起 1506 必回显 offset
            if int(str(rsp.get("offset", -1))) != off:
                raise RuntimeError("1506 echo offset mismatch: %r" % rsp)
        elif "offset" in rsp:
            #P1a 前的旧 1506 不回显：字段存在才严格比对
            if int(str(rsp["offset"])) != off:
                raise RuntimeError("legacy 1506 echo offset mismatch: %r" % rsp)
        server_off = int(str(rsp["server_offset"]))
        if server_off != off + len(raw):
            raise RuntimeError("1506 server_offset mismatch at %d: %r" % (off, rsp))
        if rsp.get("status") == MSG_PUBLISHED and "event_seq" in rsp:
            event_seq = str(rsp["event_seq"])
        off += len(raw)
    wall_end = time.perf_counter()
    if off != len(blob) or event_seq is None:
        raise RuntimeError("upload not published (off=%d event_seq=%r)" %
                           (off, event_seq))
    return latencies, wall_end - wall_start


def download(res, mid, declared_sha, total, mode):
    chunk_size = CHUNK_NEW if mode == "new" else CHUNK_BASELINE
    decode = decode_1512_new if mode == "new" else decode_1512_baseline
    latencies = []
    got = b""
    wall_start = time.perf_counter()
    while len(got) < total:
        expected = min(chunk_size, total - len(got))
        req = json.dumps({
            "message_id": str(mid),
            "offset": str(len(got)),
        }).encode()
        t0 = time.perf_counter()  #t0 在 send 之前（原稿放 send 后会低估 RTT）
        res.send(1511, req)
        _, body = res.recv(expect_id=1512)
        latencies.append(time.perf_counter() - t0)
        meta, raw = decode(body)
        if meta.get("error") != ERR_SUCCESS:
            raise RuntimeError("1512 failed at %d: %r" % (len(got), meta))
        if int(str(meta["offset"])) != len(got):
            raise RuntimeError("1512 offset mismatch: %r" % meta)
        if int(str(meta["bytes"])) != len(raw):
            raise RuntimeError("1512 bytes field mismatch: %r" % meta)
        if int(str(meta["total_size"])) != total:
            raise RuntimeError("1512 total_size mismatch: %r" % meta)
        #raw 边界：长度必须等于预期片长，且拼装不越过总大小
        if len(raw) != expected:
            raise RuntimeError("1512 raw length %d != expected %d at %d" %
                               (len(raw), expected, len(got)))
        if len(got) + len(raw) > total:
            raise RuntimeError("1512 raw overruns total at %d" % len(got))
        if hashlib.sha256(raw).hexdigest() != meta.get("chunk_sha256"):
            raise RuntimeError("1512 chunk hash mismatch at %d" % len(got))
        if not raw:
            raise RuntimeError("1512 returned 0-byte chunk at %d (abort)" % len(got))
        got += raw
    wall_end = time.perf_counter()
    if hashlib.sha256(got).hexdigest() != declared_sha:
        raise RuntimeError("download whole-file sha mismatch")
    return latencies, wall_end - wall_start


def p99_nearest_rank(values):
    """nearest-rank p99：index = ceil(0.99*N) - 1。"""
    if not values:
        return 0.0
    ordered = sorted(values)
    idx = int(math.ceil(0.99 * len(ordered))) - 1
    if idx < 0:
        idx = 0
    return ordered[idx]


def summarize(label, blob_size, latencies, elapsed, mode):
    return {
        "phase": label,
        "wire_mode": mode,
        "inflight_window": 1,  #逐片停等；不代表 Qt 引擎 4 片在途窗口
        "bytes": blob_size,
        "frames": len(latencies),
        "mb_per_s": round((blob_size / (1024.0 * 1024.0)) / elapsed, 2)
        if elapsed else 0.0,
        "chunk_p99_ms": round(p99_nearest_rank(latencies) * 1000.0, 2),
        "total_s": round(elapsed, 3),  #墙钟起止（含每片 hash/编码成本）
    }


def load_blob(args):
    if args.file:
        with open(args.file, "rb") as fp:
            blob = fp.read()
    else:
        blob = bytes((i * 131 + 7) % 256
                     for i in range(int(args.size_mb * 1024 * 1024)))
    if not blob:
        raise RuntimeError("payload must be non-empty")
    if len(blob) > 100 * 1024 * 1024:
        raise RuntimeError("payload over server FILE limit (100MB)")
    return blob


def main():
    ap = argparse.ArgumentParser(
        description="P1c resource transfer benchmark: chat 1101+1503 create, "
                    "resource 1501 auth + 1507 verify, per-chunk upload with "
                    "strict response checks, optional verified download. "
                    "--mode baseline targets a SEPARATE legacy (pre-P1c) "
                    "ResourceServer port (JSON+Base64 32KiB); the v2 server "
                    "does not speak the old protocol.")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--chat-port", type=int, required=True,
                    help="ChatServer TCP port (4B header, 1101 auth + 1503 create)")
    ap.add_argument("--res-port", type=int, required=True,
                    help="ResourceServer TCP port (6B header, 1501 auth); "
                         "baseline mode must point at a legacy server port")
    ap.add_argument("--mode", choices=("new", "baseline"), default="new",
                    help="new = v2 binary 256KiB wire; baseline = legacy "
                         "JSON+Base64 32KiB against a separate old server")
    ap.add_argument("--uid", type=int, required=True)
    ap.add_argument("--token", required=True, help="Chat/Gate token for 1101/1501")
    ap.add_argument("--peer", type=int,
                    help="receiver uid; required unless --message-id")
    ap.add_argument("--thread-id", help="existing private chat thread id; "
                    "required unless --message-id")
    ap.add_argument("--message-id", type=int, default=None,
                    help="reuse an existing Pending message (skips 1503; "
                         "still 1507-verified: metadata must match and "
                         "server_offset must be 0)")
    ap.add_argument("--file", help="upload this file's bytes (default: "
                    "deterministic generated payload)")
    ap.add_argument("--size-mb", type=float, default=10.0,
                    help="generated payload size in MB (1..100, FILE limit)")
    ap.add_argument("--download", action="store_true",
                    help="also benchmark 1509/1511 download with per-chunk "
                         "hash/bytes/offset/total and whole-file SHA verify")
    ap.add_argument("--out", help="write result JSON here")
    args = ap.parse_args()

    blob = load_blob(args)
    sha = hashlib.sha256(blob).hexdigest()

    chat = None
    if args.message_id is None:
        if not args.peer or not args.thread_id:
            ap.error("--peer and --thread-id are required without --message-id")
        chat = chat_login(args.host, args.chat_port, args.uid, args.token)
        mid = create_resource(chat, args.peer, args.thread_id,
                              len(blob), sha,
                              "bench_%s.bin" % uuid.uuid4().hex[:8])
    else:
        mid = args.message_id

    res = res_login(args.host, args.res_port, args.uid, args.token)
    result = {"message_id": mid, "bytes": len(blob), "wire_mode": args.mode}
    try:
        prog = query_progress(res, mid)
        if prog.get("status") != MSG_PENDING:
            raise RuntimeError("1507 expects fresh Pending, got %r" % prog)
        if int(str(prog["file_size_bytes"])) != len(blob):
            raise RuntimeError("1507 file_size mismatch: %r" % prog)
        if prog.get("sha256") != sha:
            raise RuntimeError("1507 sha256 mismatch: %r" % prog)
        if int(str(prog["server_offset"])) != 0:
            raise RuntimeError("1507 expects fresh offset 0, got %r" % prog)

        up_lat, up_wall = upload(res, mid, blob, args.mode)
        result["upload"] = summarize("upload", len(blob), up_lat, up_wall,
                                     args.mode)
        prog2 = query_progress(res, mid)
        if (prog2.get("status") != MSG_PUBLISHED
                or int(str(prog2["server_offset"])) != len(blob)):
            raise RuntimeError("post-publish 1507 mismatch: %r" % prog2)

        if args.download:
            res.send(1509, json.dumps({"message_id": str(mid)}).encode())
            _, body = res.recv(expect_id=1510)
            info = json.loads(body)
            if (info.get("error") != ERR_SUCCESS
                    or int(str(info["file_size_bytes"])) != len(blob)
                    or info.get("sha256") != sha):
                raise RuntimeError("1510 metadata mismatch: %r" % info)
            down_lat, down_wall = download(res, mid, sha, len(blob), args.mode)
            result["download"] = summarize("download", len(blob), down_lat,
                                           down_wall, args.mode)
    finally:
        res.close()
        if chat is not None:
            chat.close()

    text = json.dumps(result, indent=2)
    print(text)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as fp:
            fp.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
