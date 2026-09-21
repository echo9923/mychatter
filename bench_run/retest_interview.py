"""Run existing interview validation with fresh, independently named evidence."""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import time

import bench_capacity
import bench_faults
import validation_support as support


def snapshot(directory):
    info = support.environment()
    with support.db() as conn, conn.cursor() as cur:
        cur.execute("SELECT VERSION(), @@innodb_flush_log_at_trx_commit, @@sync_binlog, @@max_connections")
        version, flush, sync, connections = cur.fetchone()
        cur.execute("SELECT COUNT(*) FROM users WHERE user_id >= %s", (support.BASE_UID,))
        seeded = cur.fetchone()[0]
    info.update(mysql_version=version, innodb_flush_log_at_trx_commit=flush,
                sync_binlog=sync, mysql_max_connections=connections, seeded_users=seeded,
                redis_version=support.cache().info("server")["redis_version"])
    support.write_json(directory / "environment.json", info)
    paths = []
    for folder in ("server", "client/llfcchat", "tests", "proto", "bench_run"):
        paths.extend(p for p in (support.ROOT / folder).rglob("*")
                     if p.is_file() and "results" not in p.parts
                     and p.suffix in (".cpp", ".h", ".py", ".ps1", ".txt"))
    paths.extend((support.ROOT / "out/run/Release").rglob("*.exe"))
    paths.extend((support.ROOT / "out/build/interview-validation/tests/Release").glob("*.exe"))
    support.write_json(directory / "manifest.json", {
        "recorded_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "files": {str(p.relative_to(support.ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in paths}})
    print(f"Recorded environment and {len(paths)} source/binary hashes", flush=True)


def tests(directory, test_filter):
    directory.mkdir(parents=True, exist_ok=False)
    runtime = directory / "runtime"
    runtime.mkdir()
    env = support.fixture_environment()
    env.update(TEMP=str(runtime), TMP=str(runtime), QT_QPA_PLATFORM="windows")
    ctest = Path("C:/Program Files/Microsoft Visual Studio/18/BuildTools/Common7/IDE/"
                 "CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe")
    command = [str(ctest), "--test-dir", str(support.ROOT / "out/build/interview-validation"),
               "-C", "Release", "--timeout", "180", "--output-on-failure",
               "--output-junit", str(directory / "results.xml")]
    if test_filter:
        command.extend(["-R", test_filter])
    with (directory / "ctest.log").open("wb") as log:
        process = subprocess.Popen(command, cwd=support.ROOT, env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for line in process.stdout:
            log.write(line)
            log.flush()
            print(line.decode("utf-8", errors="replace"), end="", flush=True)
        code = process.wait()
    shutil.copyfile(support.ROOT / "out/build/interview-validation/Testing/Temporary/LastTest.log",
                    directory / "assertions.log")
    support.write_json(directory / "exit.json", {"exit_code": code, "command": command})
    return code


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=("snapshot", "tests", "faults", "capacity"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--label")
    parser.add_argument("--filter")
    parser.add_argument("--users", type=int, default=1000)
    parser.add_argument("--seconds", type=int, default=60)
    args = parser.parse_args()
    directory = args.output.resolve()
    directory.relative_to(support.ROOT / "bench_run/results")
    directory.mkdir(parents=True, exist_ok=True)
    if args.phase == "snapshot":
        if (directory / "manifest.json").exists():
            raise RuntimeError("Use a fresh snapshot directory")
        snapshot(directory)
        return 0
    if args.phase == "tests":
        return tests(directory / (args.label or "full-suite"), args.filter)
    if args.phase == "faults":
        bench_faults.RESULTS = directory
        bench_faults.main()
        return 0
    bench_capacity.RESULTS = directory
    sys.argv = ["bench_capacity.py", "--users", str(args.users), "--seconds", str(args.seconds),
                "--label", args.label or f"capacity-{args.users}-{args.seconds}s"]
    return bench_capacity.main()


if __name__ == "__main__":
    raise SystemExit(main())
