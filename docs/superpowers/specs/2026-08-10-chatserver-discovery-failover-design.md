# ChatServer Dynamic Discovery and Failover Design

## Purpose

Make ChatServer scaling and failure visible as a practical distributed-system capability without adding a new infrastructure product or restoring the previously removed resumable-ticket design.

The finished system must support these two demonstrations:

1. Start `chatserver3` with only its own configuration. StatusServer, existing ChatServers, and ResourceServer discover it without editing their configuration.
2. Stop the ChatServer used by a logged-in Qt client. The client keeps its current token and chat UI, obtains another live ChatServer through GateServer, reconnects automatically up to three times, and resumes pending/offline delivery.

## Scope Boundaries

Included:

- Redis-backed ChatServer registration, lease renewal, expiry, and least-loaded selection.
- Dynamic ChatServer gRPC endpoint resolution in ChatServer and ResourceServer.
- A token-preserving GateServer reassignment route.
- Three bounded Qt client reconnect attempts with `2s`, `4s`, and `8s` delays.
- Integration coverage for an unconfigured third node, dynamic cross-node delivery, token-preserving reassignment, and recovery of pending messages.

Excluded:

- MySQL replication, Redis Sentinel/Cluster, and high availability for GateServer, StatusServer, or ResourceServer.
- Consul, etcd, ZooKeeper, Kubernetes, a message broker, or a new proxy tier.
- TCP session migration or transparent preservation of an existing socket.
- mTLS, `chat_ticket`, persistent `session_token`, password caching, or a resumable-session protocol.
- Chat message schema changes.

## Registry Contract

Redis remains the only coordination dependency.

`chatserver:registry` is a hash. Each field is a unique ChatServer name and each value is JSON:

```json
{
  "name": "chatserver3",
  "tcp_host": "127.0.0.1",
  "tcp_port": "8092",
  "rpc_host": "127.0.0.1",
  "rpc_port": "50057"
}
```

`chatserver:lease:<name>` remains a string containing the non-negative authenticated-session count and carrying a TTL.

Each ChatServer reads `SelfServer.Name`, `SelfServer.Port`, `SelfServer.RPCPort`, and the advertised `SelfServer.RegisterHost`. Optional `SelfServer.RegisterPort` and `SelfServer.RegisterRPCPort` support a mapped public/proxy port and otherwise fall back to the listening ports. `RegisterHost` falls back to `SelfServer.Host` only for compatibility. Production configuration uses a 2-second report interval and a 6-second lease TTL.

On startup and every renewal, ChatServer writes its metadata hash field and lease. On graceful shutdown it removes both. After a crash, stale metadata is harmless because every consumer must ignore registry entries whose lease is missing, expired, malformed, or negative.

## Discovery and Routing

StatusServer no longer loads `[chatservers]` or per-node address sections. For each assignment it:

1. Reads all metadata from `chatserver:registry`.
2. Parses only complete JSON entries whose `name` matches the Redis hash field.
3. Reads the corresponding lease and accepts only a complete non-negative decimal load.
4. Selects the minimum load, using node-name sorting plus the existing atomic round-robin counter for deterministic tie rotation.
5. Returns `NoAvailableChatServer` when no valid live node remains.

ChatServer and ResourceServer continue routing users by the existing `uip_<uid> -> server_name` value. Their gRPC clients resolve that name through `HGET chatserver:registry <server_name>`, create a channel on demand, and cache `{endpoint, channel}`. If the registered endpoint changes, the cache entry is replaced. Missing, expired, or malformed nodes return the existing RPC failure result; durable/pending message behavior remains unchanged.

## Assignment and Token Behavior

The existing Status `GetChatServer` RPC gains an optional `token` request field:

```protobuf
message GetChatServerReq {
  int32 uid = 1;
  string token = 2;
}
```

- Empty token means initial password login. Status selects a node, creates a new token, stores `utoken_<uid>` with the existing 86400-second TTL, and returns it.
- Non-empty token means reassignment. Status must read `utoken_<uid>`, require an exact match, select a live node, and return the same token without changing Redis or extending its TTL.
- Invalid or missing tokens fail closed and do not return an address.

GateServer adds:

```text
POST /reassign_chat
{"uid": 1019, "token": "existing-token"}
```

The request must be a JSON object with a positive integer `uid` and non-empty string `token`. Gate forwards both fields to Status and maps the reply without accessing the password database.

Success response:

```json
{
  "error": 0,
  "server_name": "chatserver2",
  "chathost": "127.0.0.1",
  "chatport": "8091"
}
```

Initial `/user_login` also includes `server_name` so the client knows its current assignment.

## Qt Reconnect State Machine

An unexpected Chat TCP disconnect while `CHAT_UI` is visible starts one reconnect cycle. User-requested close, server kick/offline notification, ResourceServer failure, and a disconnect while already on the login UI do not start it.

The reconnect cycle:

1. Keeps `UserMgr`'s `uid` and token, current ChatDialog, pending outgoing requests, and ResourceServer connection.
2. Stops Chat retry/offline-pull timers until Chat authentication succeeds again.
3. Calls `/reassign_chat` after `2s`, then `4s`, then `8s` if the preceding HTTP, TCP, or Chat login attempt fails.
4. Updates the stored ChatServer name/host/port and reconnects only the Chat socket.
5. Sends the existing Chat login frame `{uid, token}` directly from TcpMgr's reconnect path.
6. On success, restarts pending replay and offline pull without appending duplicate profile/friend data or reconnecting ResourceServer.
7. After the third failure, shows one failure notification, clears the token through the existing logout path, and returns to the login page.

Only one reconnect cycle and one outstanding HTTP/TCP attempt may run at once. The next attempt is not scheduled until the current attempt has failed. Callbacks are ignored after the cycle is cancelled or completed. A successful reconnect cancels the timer and resets all reconnect flags.

## Error and Recovery Semantics

- Redis registry write failure is logged without credentials and retried at the next report interval.
- Status never falls back to static addresses or an expired node.
- Dynamic gRPC resolution failure does not remove or acknowledge a pending message. The existing MySQL-first and Redis offline-pending paths remain the recovery source.
- Reassignment does not rotate or extend a token, so a lost HTTP response cannot invalidate the next retry.
- If all ChatServers are unavailable, reassignment returns `NoAvailableChatServer`; the client continues its bounded retry schedule and then returns to login.
- This feature does not make the whole application available when Redis, MySQL, StatusServer, or GateServer is down.

## Acceptance Tests

- StatusServer starts with no static ChatServer list and returns `NoAvailableChatServer` before any node registers.
- `chatserver1`, `chatserver2`, and a test-only `chatserver3` self-register complete TCP/RPC metadata and live leases.
- Least-load and tie rotation operate across the dynamically registered nodes; an expired lease is excluded.
- Two ChatServers with no `[PeerServer]` sections exchange a live text notification through dynamic gRPC resolution.
- ResourceServer with no `[chatserver1]`/`[chatserver2]` sections resolves the recipient node dynamically for image notification.
- `/reassign_chat` rejects malformed, missing, and forged tokens; a valid token is returned unchanged and its Redis TTL is not extended.
- After the assigned ChatServer is stopped, the same token authenticates on a newly selected node and pending/offline messages are pulled.
- Qt manual verification confirms the chat UI stays open during the bounded reconnect cycle and returns to login only after all three attempts fail.
