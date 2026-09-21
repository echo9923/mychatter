"""Isolated fixtures and server processes for reproducible local validation.

The canonical schema is applied only to a new, empty *_validation database.
Passwords below are synthetic credentials for the disposable test containers.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import socket
import subprocess
import time
import winreg

import psutil
import pymysql
import redis

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "bench_run/results/20260921"
BASE_UID = 20_000_000
BASE_THREAD = 10_000_000
TEST_PASSWORD = "llfc-validation-only"
MYSQL_PORT = 13308
REDIS_PORT = 16380
SCHEMA = "llfc_validation"


def write_json(path: Path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False), encoding="utf-8")


def db():
    return pymysql.connect(host="127.0.0.1", port=MYSQL_PORT, user="root",
                           password=TEST_PASSWORD, database=SCHEMA,
                           charset="utf8mb4", autocommit=True,
                           connect_timeout=5, read_timeout=30, write_timeout=30)


def cache():
    return redis.Redis(host="127.0.0.1", port=REDIS_PORT, password=TEST_PASSWORD,
                       decode_responses=True, socket_timeout=5)


def fixture_environment():
    env = os.environ.copy()
    env.update(LLFC_TEST_MYSQL_PORT=str(MYSQL_PORT),
               LLFC_TEST_MYSQL_SCHEMA=SCHEMA,
               LLFC_TEST_MYSQL_PASSWORD=TEST_PASSWORD,
               LLFC_TEST_REDIS_PORT=str(REDIS_PORT),
               LLFC_TEST_REDIS_PASSWORD=TEST_PASSWORD)
    return env


def environment():
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                       r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
        cpu = winreg.QueryValueEx(key, "ProcessorNameString")[0]
    status = subprocess.check_output(["git", "status", "--short"], cwd=ROOT,
                                     text=True, encoding="utf-8")
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT,
                                       text=True).strip()
    return {"recorded_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "os": platform.platform(), "cpu": cpu,
            "logical_cpus": psutil.cpu_count(),
            "physical_cores": psutil.cpu_count(logical=False),
            "ram_bytes": psutil.virtual_memory().total,
            "python": platform.python_version(), "git_head": revision,
            "git_status": status.splitlines(),
            "topology": "Windows host: load generator + native Release servers; "
                        "same-host Docker Desktop Linux VM: dedicated MySQL + Redis",
            "mysql_port": MYSQL_PORT, "redis_port": REDIS_PORT, "schema": SCHEMA}


def seed(max_users=10000):
    assert SCHEMA.endswith("_validation") and MYSQL_PORT == 13308
    conn = db()
    with conn.cursor() as cur:
        cur.execute("SHOW TABLES")
        if cur.fetchall():
            raise RuntimeError("Refusing to apply destructive schema to a nonempty database")
        sql_path = ROOT / "sql备份/20260830_database_rebuild.sql"
        sql = re.sub(r"--[^\n]*", "", sql_path.read_text(encoding="utf-8"))
        for statement in sql.split(";"):
            if statement.strip():
                cur.execute(statement)
        salt = bytes.fromhex("7b17e89c4e104834999580368c24aa20")
        def password_hash(password):
            digest = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, 600000)
            return f"pbkdf2-sha256$i=600000${salt.hex()}${digest.hex()}"
        common = (ROOT / "tests/integration/im_common.h").read_text(encoding="utf-8")
        fixture_password = re.search(r'FIXTURE_PASSWD\s*=\s*"([^"]+)"', common)[1]
        email_a = re.search(r'FIXTURE_SENDER_EMAIL\s*=\s*"([^"]+)"', common)[1]
        email_b = re.search(r'FIXTURE_RECEIVER_EMAIL\s*=\s*"([^"]+)"', common)[1]
        fh = password_hash(fixture_password)
        cur.executemany("INSERT INTO users(user_id,username,email,password_hash) VALUES(%s,%s,%s,%s)",
                        [(1002, "fixture_sender", email_a, fh),
                         (1019, "fixture_receiver", email_b, fh)])
        cur.execute("INSERT INTO friendships VALUES(1002,1019)")
        cur.execute("INSERT INTO private_chats VALUES(35,1002,1019)")
        bh = password_hash(TEST_PASSWORD)
        rows = [(BASE_UID+i, f"validation_{i}", f"validation_{i}@example.invalid", bh)
                for i in range(max_users)]
        cur.executemany("INSERT INTO users(user_id,username,email,password_hash) VALUES(%s,%s,%s,%s)", rows)
        cur.executemany("INSERT INTO friendships VALUES(%s,%s)",
                        [(BASE_UID+i, BASE_UID+i+1) for i in range(0, max_users, 2)])
        cur.executemany("INSERT INTO private_chats VALUES(%s,%s,%s)",
                        [(BASE_THREAD+i//2, BASE_UID+i, BASE_UID+i+1)
                         for i in range(0, max_users, 2)])
        cur.execute("SELECT VERSION(), @@innodb_flush_log_at_trx_commit, @@sync_binlog, @@max_connections")
        version, flush, sync_binlog, max_conns = cur.fetchone()
    conn.close()
    info = environment()
    info.update(mysql_version=version, innodb_flush_log_at_trx_commit=flush,
                sync_binlog=sync_binlog, mysql_max_connections=max_conns,
                redis_version=cache().info("server")["redis_version"],
                seeded_users=max_users, schema_sha256=hashlib.sha256(sql_path.read_bytes()).hexdigest())
    write_json(RESULTS / "environment.json", info)
    print(json.dumps({"seeded_users": max_users, "mysql": version,
                      "redis": info["redis_version"], "schema": SCHEMA}), flush=True)


def mysql_block():
    return (f"[Mysql]\nHost=127.0.0.1\nPort={MYSQL_PORT}\nUser=root\n"
            f"Passwd={TEST_PASSWORD}\nSchema={SCHEMA}\n"
            f"[Redis]\nHost=127.0.0.1\nPort={REDIS_PORT}\nPasswd={TEST_PASSWORD}\n")


class Stack:
    """Only owns processes launched by this instance; never kills by image name."""
    def __init__(self, directory: Path):
        self.directory = directory
        self.processes = {}
        self.files = []

    def start(self, name, exe, config, port):
        with socket.socket() as probe:
            probe.settimeout(0.3)
            if probe.connect_ex(("127.0.0.1", port)) == 0:
                raise RuntimeError(f"Refusing to reuse occupied test port {port}")
        path = self.directory / name
        path.mkdir(parents=True, exist_ok=True)
        (path / "config.ini").write_text(config, encoding="utf-8")
        log = (path / "server.log").open("wb")
        self.files.append(log)
        p = subprocess.Popen([str(ROOT / "out/run/Release" / exe)], cwd=path,
                             stdout=log, stderr=subprocess.STDOUT,
                             creationflags=subprocess.CREATE_NO_WINDOW)
        self.processes[name] = p
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if p.poll() is not None:
                raise RuntimeError(f"{name} exited with {p.returncode}; see {path / 'server.log'}")
            with socket.socket() as probe:
                probe.settimeout(0.2)
                if probe.connect_ex(("127.0.0.1", port)) == 0:
                    print(f"started {name} pid={p.pid} port={port}", flush=True)
                    return
            time.sleep(0.1)
        raise TimeoutError(f"{name} readiness timeout")

    def start_single(self, logic_workers=4, delivery_workers=4, mysql_pool_size=5, deadlock_retries=2):
        shared = mysql_block()
        self.start("status", "StatusServer/StatusServer.exe",
                   "[StatusServer]\nHost=127.0.0.1\nPort=25052\n"+shared, 25052)
        self.start("chat", "chatserver1/ChatServer.exe",
                   "[SelfServer]\nName=validation_chat\nHost=127.0.0.1\nPort=28090\n"
                   "RegisterHost=127.0.0.1\nRegisterPort=28090\nRPCPort=25055\nRegisterRPCPort=25055\n"
                   "[StatusServer]\nHost=127.0.0.1\nPort=25052\n"+shared+
                   f"[Concurrency]\nLogicWorkers={logic_workers}\nDeliveryWorkers={delivery_workers}\nMysqlPoolSize={mysql_pool_size}\nMysqlDeadlockRetries={deadlock_retries}\n"
                   "[Delivery]\nRpcDeadlineMs=3000\nRpcMaxAttempts=3\nRpcBackoffMs=100\n"
                   "[Discovery]\nReportIntervalSeconds=2\nLeaseTtlSeconds=8\n", 28090)
        self.start("gate", "GateServer/GateServer.exe",
                   "[GateServer]\nPort=28080\n[StatusServer]\nHost=127.0.0.1\nPort=25052\n"
                   "[VarifyServer]\nHost=127.0.0.1\nPort=50051\n"
                   "[ResServer]\nName=validation_resource\nHost=127.0.0.1\nPort=28081\n"
                   "[Concurrency]\nHandlerWorkers=4\nHandlerQueueCapacity=1024\n"+shared, 28080)

    def close(self):
        for p in reversed(list(self.processes.values())):
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait(timeout=5)
        for f in self.files:
            f.close()


def manifest():
    files = list((ROOT / "out/run/Release").rglob("*.exe"))
    files += [p for directory in (ROOT / "server", ROOT / "client/llfcchat", ROOT / "tests", ROOT / "proto", ROOT / "bench_run")
              for p in directory.rglob("*")
              if p.is_file() and p.suffix in (".cpp", ".h", ".py", ".ps1")]
    write_json(RESULTS / "manifest.json", {
        "recorded_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "files": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in files if "results" not in p.parts}})


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=["seed", "manifest"])
    ap.add_argument("--max-users", type=int, default=10000)
    args = ap.parse_args()
    if args.action == "seed":
        seed(args.max_users)
    else:
        manifest()
