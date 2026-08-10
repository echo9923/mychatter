# ChatServer Dynamic Discovery and Failover Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let ChatServer instances self-register in Redis, remove all static ChatServer peer/address lists, and let a logged-in Qt client reuse its current token to reconnect to another live ChatServer after a failure.

**Architecture:** Keep the existing Redis lease and least-load design, adding a Redis hash that carries each node's advertised TCP and gRPC endpoints. StatusServer, ChatServer, and ResourceServer consume that registry dynamically. GateServer exposes token-preserving reassignment, while TcpMgr and MainWindow implement a bounded `2s/4s/8s` reconnect state machine.

**Tech Stack:** C++17 servers, Boost.Asio, hiredis, nlohmann-json, gRPC/Protobuf, C++11 Qt 5 client, CMake/Ninja Multi-Config, existing headless integration harness.

**Execution constraint:** Preserve unrelated work and do not commit, push, reset, clean, or rewrite history unless the user explicitly authorizes it. The commit commands normally suggested by the planning workflow are intentionally replaced by review checkpoints.

---

### Task 1: Define the dynamic registry contract in tests

**Files:**
- Modify: `tests/integration/im_common.h`
- Modify: `tests/integration/im_redis.h`
- Modify: `tests/integration/im_redis.cpp`
- Modify: `tests/integration/im_harness.h`
- Modify: `tests/integration/im_harness.cpp`
- Modify: `tests/integration/im_scenarios.cpp`

- [ ] **Step 1: Add test constants and Redis hash helpers**

Add `CHAT3_TCP_PORT = 18092`, `CHAT3_GRPC_PORT = 15058`, and `CHATSERVER_REGISTRY_KEY = "chatserver:registry"`. Add binary-safe `HSet`, `HGet`, `HDel`, and `HGetAll` helpers to the integration Redis wrapper using `redisCommandArgv` and structured reply checks. Extend each scenario cleanup to remove registry fields and leases for all node names it starts.

- [ ] **Step 2: Make generated configuration express the target topology**

Change `MakeStatusIni()` so it contains only `[StatusServer]`, MySQL, and Redis blocks. Change `MakeChatIni()` to emit `RegisterHost = 127.0.0.1` and no `[PeerServer]` or per-peer section; give it an optional `advertised_rpc_port = 0` parameter that emits `RegisterRPCPort` when nonzero. Change `MakeResourceIni()` to omit both hard-coded ChatServer sections. Remove `MakeChatIniPeer()` only after all callers are converted.

- [ ] **Step 3: Rewrite the status-discovery scenario as the RED contract**

Make `ScenarioStatusDiscovery()` assert:

```text
no registered nodes -> NoAvailableChatServer
start chatserver1/2/3 -> registry JSON and leases exist
seed loads 3/1/2 -> chatserver2 selected
equal live loads -> deterministic rotation across all three names
delete/expire one lease -> that node is never selected
malformed JSON, mismatched name, empty endpoint, and non-decimal lease -> ignored
```

Do not seed registry metadata directly for the registration assertion; each ChatServer process must create its own field.

Launch the third process with `ExeRef{"chatserver1", "ChatServer.exe"}` and a unique harness working directory/config. The harness already separates current working directories, so no third deployed executable copy is required.

- [ ] **Step 4: Run the scenario and capture the expected failure**

Run in the VS developer PowerShell:

```powershell
cmake --build out/build/windows-ninja --config Debug --target im_integration_tests
ctest --test-dir out/build/windows-ninja -C Debug -R '^im_status-discovery$' --output-on-failure
```

Expected: build or scenario failure because Status still requires static nodes and ChatServer does not publish endpoint metadata.

- [ ] **Step 5: Review checkpoint**

Inspect `git diff -- tests/integration tests/CMakeLists.txt` and confirm only the new registry contract changed. Do not commit without explicit authorization.

### Task 2: Publish ChatServer endpoint metadata and select nodes dynamically

**Files:**
- Create: `server/common/include/ChatServerRegistry.h`
- Modify: `server/ChatServer/src/ChatServer.cpp`
- Modify: `server/ChatServer/config/chatserver1.ini`
- Modify: `server/ChatServer/config/chatserver2.ini`
- Modify: `server/StatusServer/include/StatusServiceImpl.h`
- Modify: `server/StatusServer/src/StatusServiceImpl.cpp`
- Modify: `server/StatusServer/config/config.ini`

- [ ] **Step 1: Add a dependency-free shared registry vocabulary**

Create a C++17 header containing only strings and the endpoint value type; keep JSON parsing in services so `llfc_server_common` does not acquire nlohmann-json:

```cpp
#pragma once
#include <string>

inline constexpr const char* kChatServerRegistryKey = "chatserver:registry";
inline constexpr const char* kChatServerLeasePrefix = "chatserver:lease:";

struct ChatServerEndpoint {
    std::string name;
    std::string tcp_host;
    std::string tcp_port;
    std::string rpc_host;
    std::string rpc_port;
};
```

- [ ] **Step 2: Publish metadata with every lease renewal**

In `ChatServer.cpp`, build endpoint JSON from `SelfServer.Name`, `RegisterHost`, `RegisterPort` (fallback `Port`), and `RegisterRPCPort` (fallback `RPCPort`). On startup and each timer callback execute:

```text
HSET chatserver:registry <name> <endpoint-json>
SET chatserver:lease:<name> <authenticated-count> EX <ttl>
```

Log either write failure and retry on the next timer. On graceful shutdown call `HDel(kChatServerRegistryKey, server_name)` and delete the lease before closing Redis. Default missing/invalid discovery values to report `2s`, TTL `6s`; set those values explicitly in both production INIs.

- [ ] **Step 3: Replace Status's static address book**

Remove constructor parsing of `[chatservers]`, `_servers`, and `_server_order`. Implement `getChatServer()` by `HGetAll(kChatServerRegistryKey)`, strict non-throwing JSON parsing, matching `name`, non-empty endpoints, lease lookup, and complete decimal load parsing with overflow rejection. Sort candidates by name before least-load/tie rotation so Redis hash order cannot affect tests.

- [ ] **Step 4: Remove static Status configuration**

Delete `[chatservers]`, `[chatserver1]`, and `[chatserver2]` from `server/StatusServer/config/config.ini`. Do not add a fallback list.

- [ ] **Step 5: Build and run the dynamic discovery test**

```powershell
cmake --build out/build/windows-ninja --config Debug --target StatusServer ChatServer im_integration_tests
ctest --test-dir out/build/windows-ninja -C Debug -R '^im_status-discovery$' --output-on-failure
```

Expected: `im_status-discovery` passes, including third-node registration and expired-node exclusion.

- [ ] **Step 6: Review checkpoint**

Search for remaining static Status topology:

```powershell
rg -n '\[chatservers\]|_server_order|_servers' server/StatusServer tests/integration
```

Expected: no production static address-book references. Do not commit without explicit authorization.

### Task 3: Resolve ChatServer-to-ChatServer gRPC channels from Redis

**Files:**
- Modify: `server/ChatServer/include/ChatGrpcClient.h`
- Modify: `server/ChatServer/src/ChatGrpcClient.cpp`
- Modify: `server/ChatServer/config/chatserver1.ini`
- Modify: `server/ChatServer/config/chatserver2.ini`
- Modify: `tests/integration/im_scenarios.cpp`

- [ ] **Step 1: Convert cross-server integration setup to dynamic metadata**

Remove `MakeChatIniPeer` use from `ScenarioCrossServer`. Start chatserver2 with its normal listening RPC port and `advertised_rpc_port = CHAT2_PROXY_GRPC_PORT`; start chatserver1 with the ordinary `MakeChatIni` call. The registry entry for chatserver2 then points chatserver1's outbound call at the proxy for the entire broken-RPC phase without racing metadata renewal. This keeps the existing retry/pending assertions while proving the client reads the registry.

- [ ] **Step 2: Run the cross-server test and verify RED**

```powershell
cmake --build out/build/windows-ninja --config Debug --target im_integration_tests
ctest --test-dir out/build/windows-ninja -C Debug -R '^im_cross-server$' --output-on-failure
```

Expected: failure with an unknown node because `ChatGrpcClient` still initializes `_channels` from `[PeerServer]`.

- [ ] **Step 3: Add endpoint-aware channel caching**

Replace the constructor-built map with a mutex-protected cache:

```cpp
struct CachedChannel {
    std::string endpoint;
    std::shared_ptr<grpc::Channel> channel;
};

std::shared_ptr<grpc::Channel> ResolveChannel(const std::string& server_name);
std::unordered_map<std::string, CachedChannel> channels_;
std::mutex channels_mutex_;
```

`ResolveChannel` must `HGet(kChatServerRegistryKey, server_name)`, require a live lease, parse matching `name` plus non-empty `rpc_host/rpc_port`, and build `rpc_host + ":" + rpc_port`. Reuse a cached channel only when its endpoint string still matches. Return `nullptr` for missing/expired/malformed metadata.

- [ ] **Step 4: Route every Chat gRPC operation through the resolver**

Update add-friend, auth-friend, text notification, and kick-user calls to resolve at call time. Preserve existing deadlines, retry classification, response mapping, and the rule that a failed live notification leaves the durable pending message intact. Unknown nodes must return `RPCFailed`, not a default-success response.

- [ ] **Step 5: Remove peer configuration**

Delete `[PeerServer]` and peer node sections from both production ChatServer INIs.

- [ ] **Step 6: Run cross-server and offline recovery scenarios**

```powershell
ctest --test-dir out/build/windows-ninja -C Debug -R '^im_(cross-server|offline|lost-ack)$' --output-on-failure
```

Expected: all selected scenarios pass.

- [ ] **Step 7: Review checkpoint**

```powershell
rg -n 'PeerServer|chatserver1.*50055|chatserver2.*50056' server/ChatServer tests/integration
```

Expected: no production peer list or hard-coded peer endpoint. Do not commit without explicit authorization.

### Task 4: Resolve ResourceServer image notifications from Redis

**Files:**
- Modify: `server/ResourceServer/include/ChatServerGrpcClient.h`
- Modify: `server/ResourceServer/src/ChatServerGrpcClient.cpp`
- Modify: `server/ResourceServer/config/config.ini`
- Modify: `tests/integration/im_harness.cpp`
- Modify: `tests/integration/im_scenarios.cpp`

- [ ] **Step 1: Make image integration configuration dynamic and verify RED**

Ensure `MakeResourceIni()` has no ChatServer sections, then run:

```powershell
cmake --build out/build/windows-ninja --config Debug --target ResourceServer im_integration_tests
ctest --test-dir out/build/windows-ninja -C Debug -R '^im_image-offline$' --output-on-failure
```

Expected: image notification cannot resolve its hard-coded channel.

- [ ] **Step 2: Apply the same endpoint-aware cache contract**

Give ResourceServer's `ChatServerGrpcClient` the same `CachedChannel`, mutex, and `ResolveChannel(server_name)` behavior used by ChatServer. Resolve before constructing each image-notification stub and preserve all existing deadline/retry/pending semantics.

- [ ] **Step 3: Remove static ResourceServer nodes**

Delete `[chatserver1]` and `[chatserver2]` from `server/ResourceServer/config/config.ini`.

- [ ] **Step 4: Run image and cross-server scenarios**

```powershell
ctest --test-dir out/build/windows-ninja -C Debug -R '^im_(image-offline|cross-server)$' --output-on-failure
```

Expected: both pass without static ChatServer configuration.

- [ ] **Step 5: Review checkpoint**

```powershell
rg -n '\[chatserver[0-9]+\]|_hash_channels\["chatserver' server/ResourceServer
```

Expected: no matches. Do not commit without explicit authorization.

### Task 5: Add token-preserving reassignment through Status and Gate

**Files:**
- Modify: `proto/status_service/status.proto`
- Modify: `server/StatusServer/src/StatusServiceImpl.cpp`
- Modify: `server/GateServer/include/StatusGrpcClient.h`
- Modify: `server/GateServer/src/StatusGrpcClient.cpp`
- Modify: `server/GateServer/src/LogicSystem.cpp`
- Modify: `tests/integration/im_http_client.h`
- Modify: `tests/integration/im_http_client.cpp`
- Modify: `tests/integration/im_scenarios.h`
- Modify: `tests/integration/im_scenarios.cpp`
- Modify: `tests/im_integration_tests.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Add a failover integration scenario before production changes**

Add scenario `chat-failover` that starts Status, Gate, and two ChatServers, obtains a real Gate login token, records its Redis TTL, stops the assigned ChatServer, waits for/removes its lease, calls `/reassign_chat`, and asserts:

```text
response error == 0
new server name differs from the stopped node
response token is absent or unchanged
utoken_<uid> value is unchanged
TTL after reassignment <= TTL before reassignment
same uid/token completes Chat login on the surviving node
forged token returns TokenInvalid and no host/port
```

Register it as `im_chat-failover` in CTest.

- [ ] **Step 2: Run the new scenario and verify RED**

```powershell
cmake --build out/build/windows-ninja --config Debug --target im_integration_tests
ctest --test-dir out/build/windows-ninja -C Debug -R '^im_chat-failover$' --output-on-failure
```

Expected: HTTP 404 or error because `/reassign_chat` does not exist.

- [ ] **Step 3: Extend the existing Status request without adding a second RPC**

Add `string token = 2` to `GetChatServerReq`. Change the Gate client signature to:

```cpp
GetChatServerRsp GetChatServer(int uid, const std::string& token = {});
```

Initial `/user_login` passes the default empty token. `/reassign_chat` passes the current token.

- [ ] **Step 4: Implement Status's two explicit modes**

Before selecting a server:

```text
request.token empty:
  select live node -> generate token -> SET utoken_<uid> <token> EX 86400
request.token non-empty:
  GET utoken_<uid> -> exact compare -> select live node -> return same token
```

On missing token return `UidInvalid`; on mismatch return `TokenInvalid`; on any error leave host/port/server_name empty. Reassignment must not call `SetEx` and must not extend the TTL.

- [ ] **Step 5: Register strict `POST /reassign_chat` handling in Gate**

Require a JSON object, positive integer `uid`, and non-empty string `token`. Call `StatusGrpcClient::GetChatServer(uid, token)` and return only:

```json
{"error":0,"server_name":"chatserver2","chathost":"127.0.0.1","chatport":"8091"}
```

Map `NoAvailableChatServer` unchanged and all transport errors to `RPCFailed`. Do not query MySQL, accept a password, or log the token. Also include `server_name` in the existing `/user_login` success response.

- [ ] **Step 6: Run authentication and failover scenarios**

```powershell
ctest --test-dir out/build/windows-ninja -C Debug -R '^im_(simple-auth|status-discovery|chat-failover)$' --output-on-failure
```

Expected: all three pass and the token TTL assertion proves reassignment did not renew credentials.

- [ ] **Step 7: Review checkpoint**

Search for accidental credential/session expansion:

```powershell
rg -n 'chat_ticket|session_token|reconnect.*password|/reconnect' server client proto tests
```

Expected: no newly introduced resumable-ticket or password-caching path. Do not commit without explicit authorization.

### Task 6: Implement the Qt bounded reconnect state machine

**Files:**
- Modify: `client/llfcchat/include/global.h`
- Modify: `client/llfcchat/include/httpmgr.h`
- Modify: `client/llfcchat/src/httpmgr.cpp`
- Modify: `client/llfcchat/include/tcpmgr.h`
- Modify: `client/llfcchat/src/tcpmgr.cpp`
- Modify: `client/llfcchat/include/mainwindow.h`
- Modify: `client/llfcchat/src/mainwindow.cpp`

- [ ] **Step 1: Extend client transport values without adding UI**

Add `ID_REASSIGN_CHAT = 1055`, `RECONNECTMOD`, and `_chat_server_name` to `ServerInfo` and its copy constructor. Add `HttpMgr::sig_reconnect_mod_finish` and dispatch `RECONNECTMOD` responses separately from login responses.

- [ ] **Step 2: Separate expected and unexpected Chat disconnects**

Add TcpMgr flags for `manual_close` and `reconnecting`. `CloseConnection()` marks the close as expected; the socket's disconnected callback emits `sig_connection_closed` only for unexpected disconnects and only once. Reset the flag after handling the event so later real failures remain observable.

- [ ] **Step 3: Add a reconnect-specific TcpMgr entry point**

Add:

```cpp
void ReconnectChat(const QString& server_name,
                   const QString& host,
                   quint16 port);
signals:
void sig_reconnect_finished(bool success, int error);
```

`ReconnectChat` updates only Chat fields in the retained `_server_info`, sets reconnect mode, clears frame assembly state, and connects the socket. In reconnect mode, the `connected` callback sends `{uid, token}` itself rather than emitting the initial-login signal consumed by the hidden LoginDialog.

- [ ] **Step 4: Branch the Chat login response for reconnect mode**

On reconnect login success:

- keep the existing `UserMgr` profile/friend/apply data;
- do not emit `sig_connect_resource` or `sig_swich_chatdlg`;
- clear reconnect mode;
- restart pending replay and offline pull;
- emit `sig_reconnect_finished(true, 0)`.

On TCP, JSON, or Chat-login failure, close the attempted socket without emitting another top-level disconnect and emit `sig_reconnect_finished(false, error)` instead of the initial-login `sig_con_success(false)` or `sig_login_failed` signals consumed by the hidden LoginDialog.

- [ ] **Step 5: Add MainWindow's single reconnect cycle**

Store one `QTimer`, attempt index, and `reconnect_in_progress` flag. On unexpected Chat disconnect in `CHAT_UI`, schedule delays `{2000, 4000, 8000}`. Each attempt posts:

```json
{"uid": UserMgr::GetInstance()->GetUid(),
 "token": UserMgr::GetInstance()->GetToken()}
```

to `gate_url_prefix + "/reassign_chat"`. A valid response calls `TcpMgr::ReconnectChat`. HTTP, assignment, TCP, and login failures advance the same attempt counter. Keep at most one HTTP/TCP attempt outstanding; schedule the next delay only after failure, and ignore callbacks after `reconnect_in_progress` becomes false.

- [ ] **Step 6: Define success, exhaustion, and non-failover exits**

Success cancels the timer and retains `CHAT_UI`. After attempt three fails, display one connection-failure message and call the existing `offlineLogin()` path, which clears the token. `SlotOffline` (server kick), explicit close/application shutdown, ResourceServer disconnect, and any event outside `CHAT_UI` continue using their current logout behavior and must cancel an active reconnect cycle.

- [ ] **Step 7: Build the Qt client**

```powershell
cmake --build out/build/windows-ninja --config Debug --target llfcchat
```

Expected: `out/run/Debug/llfcchat/llfcchat.exe` builds successfully with Qt AUTOMOC handling the new signals/slots.

- [ ] **Step 8: Review checkpoint**

Confirm the client never stores or resends a password during reconnect:

```powershell
rg -n 'passwd|password' client/llfcchat/src/mainwindow.cpp client/llfcchat/src/tcpmgr.cpp client/llfcchat/include/mainwindow.h client/llfcchat/include/tcpmgr.h
```

Expected: no reconnect credential path. Do not commit without explicit authorization.

### Task 7: Prove message recovery after node failure

**Files:**
- Modify: `tests/integration/im_scenarios.cpp`
- Modify: `tests/integration/im_scenarios.h`

- [ ] **Step 1: Extend `chat-failover` with a durable pending message**

While the receiver's assigned node is stopped, have the sender submit a text message. Assert the sender receives the normal persisted ACK, MySQL contains exactly one pending row, and `offline_msg:<receiver>` contains its message id.

- [ ] **Step 2: Reassign and authenticate the receiver with the same token**

Call `/reassign_chat`, connect to the surviving ChatServer, send `{uid, token}`, request offline messages, and ACK the recovered message.

- [ ] **Step 3: Assert cleanup after recovery**

Require the recovered envelope to preserve message id, unique id, thread id, sender, receiver, content, and timestamp. Poll until MySQL `delivery_status=1` and the Redis offline ZSET no longer contains the id.

- [ ] **Step 4: Run focused integration coverage**

```powershell
ctest --test-dir out/build/windows-ninja -C Debug -R '^im_(chat-failover|cross-server|offline|lost-ack)$' --output-on-failure
```

Expected: all focused scenarios pass without static ChatServer topology.

- [ ] **Step 5: Review checkpoint**

Run the integration cleanup probe already provided by the harness and confirm it reports no `imtest-*` rows or test Redis keys. Do not commit without explicit authorization.

### Task 8: Full build, static checks, and manual acceptance

**Files:**
- Modify only if verification exposes a scoped defect.

- [ ] **Step 1: Reconfigure after proto/CMake changes**

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\BuildTools\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -SkipAutomaticLocation
cmake --preset windows-ninja
```

Expected: configuration succeeds with `Ninja Multi-Config` and the repository-local vcpkg installation.

- [ ] **Step 2: Build all affected binaries**

```powershell
cmake --build out/build/windows-ninja --config Debug --target GateServer StatusServer ChatServer ResourceServer llfcchat im_integration_tests worker_pool_tests
```

Expected: all targets build successfully.

- [ ] **Step 3: Run unit and integration tests**

```powershell
ctest --test-dir out/build/windows-ninja -C Debug -R '^(worker_pool_tests|im_status-discovery|im_cross-server|im_image-offline|im_simple-auth|im_chat-failover)$' --output-on-failure
```

Expected: every selected test passes. If Redis/MySQL fixtures are unavailable, report those integration tests as unverified rather than treating a build as equivalent evidence.

- [ ] **Step 4: Verify static topology removal**

```powershell
rg -n '\[PeerServer\]|\[chatservers\]|\[chatserver1\]|\[chatserver2\]' server/StatusServer/config server/ChatServer/config server/ResourceServer/config
rg -n 'CreateChannel\(.*chatserver|_channels\["chatserver|_hash_channels\["chatserver' server/ChatServer server/ResourceServer
```

Expected: no static ChatServer membership/address definitions or hard-coded channel creation.

- [ ] **Step 5: Run the manual Qt failover demonstration**

Start Redis/MySQL, Status, Gate, Resource, and at least two ChatServers. Log in with the Qt client, identify its assigned node from the Gate response/log, stop that ChatServer, and verify:

```text
chat UI remains visible during retry
no password prompt appears
client reconnects to a surviving node within the bounded schedule
pending send/offline pull resumes
Resource connection is not unnecessarily recreated
```

Then stop all ChatServers and verify exactly three attempts occur before one notification and return to login.

- [ ] **Step 6: Final diff and worktree audit**

```powershell
git status --short
git diff --check
git diff --stat
```

Expected: no whitespace errors, no generated binaries/config copies tracked, and only scoped source/config/test/docs changes. Do not commit or push without explicit authorization.
