"""Generate derived tables from preserved per-stage benchmark evidence."""
import argparse
import csv
import json
from pathlib import Path
import statistics


def summarize(root):
    summaries = []
    invalid = {"workers8-pool12-retry0-100-60s", "workers8-pool12-retry2-200-60s"}
    for path in root.glob("*/result.json"):
        result = json.loads(path.read_text(encoding="utf-8"))
        if "target_messages_per_second" not in result:
            continue
        parameters = result.get("server_parameters", {})
        samples = json.loads((path.parent / "samples.json").read_text(encoding="utf-8"))
        measured = [s for s in samples if s["phase"] == "measurement"]
        before = json.loads((path.parent / "database-before.json").read_text(encoding="utf-8"))
        after = json.loads((path.parent / "database-after.json").read_text(encoding="utf-8"))
        commits_before = next((r for r in before["digests"] if r["sql"] == "COMMIT"), {})
        commits_after = next((r for r in after["digests"] if r["sql"] == "COMMIT"), {})
        commits = commits_after.get("count", 0)-commits_before.get("count", 0)
        commit_ms = (commits_after.get("time_ps", 0)-commits_before.get("time_ps", 0))/1e9
        with (path.parent / "chat/server.log").open(encoding="utf-8", errors="replace") as log:
            deadlock_attempts = sum("AddChatMsg SQLException code=1213" in line for line in log)
        summaries.append({
            "stage": path.parent.name, "started_at": result["started_at"],
            "valid_build": path.parent.name not in invalid,
            "users": result["users"], "seconds": result["duration_seconds"],
            "logic_workers": parameters.get("logic_workers", 4),
            "delivery_workers": parameters.get("delivery_workers", 4),
            "pool_size": parameters.get("mysql_pool_size_requested", 5),
            "deadlock_retries": parameters.get("deadlock_retries_requested", 0),
            "attempted": result["attempted_messages"], "acked": result["successful_acks"],
            "received": result["received_unique"], "errors": sum(result["ack_errors"].values()),
            "disconnects": len(result["disconnects"]),
            "ack_p99_ms": result["ack_latency_ms"]["p99"],
            "receive_p99_ms": result["receiver_latency_ms"]["p99"],
            "receive_max_ms": result["receiver_latency_ms"]["max"],
            "scheduler_p99_ms": result["scheduler_lateness_ms"]["p99"],
            "receive_backlog_peak": max(s["pending_receive"] for s in measured),
            "receive_backlog_last": measured[-1]["pending_receive"],
            "chat_cpu_one_core_pct": result["resources"]["chat_cpu_mean_pct_one_core"],
            "driver_cpu_one_core_pct": result["resources"]["driver_cpu_mean_pct_one_core"],
            "host_cpu_pct": result["resources"]["host_cpu_mean_pct"],
            "chat_rss_peak_mib": result["resources"]["chat_peak_rss_mib"],
            "commit_mean_ms": round(commit_ms/commits, 3) if commits else None,
            "row_lock_wait_ms": result["database_status_delta"].get("Innodb_row_lock_time"),
            "deadlocks": result["database_status_delta"].get("lock_deadlocks"),
            "logged_deadlock_attempts": deadlock_attempts,
            "reconciled": result["database_reconciled"], "passed": result["passed"],
            "binary_sha256": result["binary_sha256"]})
    summaries.sort(key=lambda row: row["started_at"])
    (root / "summary.json").write_text(json.dumps(summaries, indent=2), encoding="utf-8")
    if summaries:
        with (root / "summary.csv").open("w", newline="", encoding="utf-8-sig") as output:
            writer = csv.DictWriter(output, fieldnames=summaries[0].keys())
            writer.writeheader()
            writer.writerows(summaries)
    for row in summaries:
        print(f"{row['stage']}: {row['acked']}/{row['attempted']} "
              f"recvP99={row['receive_p99_ms']}ms backlog={row['receive_backlog_last']} "
              f"commit={row['commit_mean_ms']}ms pass={row['passed']} valid={row['valid_build']}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    summarize(parser.parse_args().output.resolve())
