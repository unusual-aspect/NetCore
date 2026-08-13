# NetProject

C++20 TCP service: one persistable shared message, many clients, access audit.

Build, container, and run steps: [HOWTO.txt](HOWTO.txt).
Why these libraries and this split: [DECISIONS.md](DECISIONS.md).

> **Note:** CI for this tree is [`.github/workflows/ci.yml`](../.github/workflows/ci.yml). [`.gitea/workflows/`](../.gitea/workflows/) and `ci-*` presets in `CMakePresets.json` are leftover from a previous project template and are **not** the verified pipeline. Dockerfiles remain for optional container builds — see [HOWTO.txt](HOWTO.txt) and [DECISIONS.md](DECISIONS.md#ci-template-leftover).

---

## What it is

`NetServer` holds a single message (`message.id = 1`) in SQLite WAL and serves it over TCP.
`NetClient` reads, sets, or shuts the server down. `--live` keeps the socket open and prompts for the next Set so a Shutdown goodbye still reaches a client sitting at the prompt.

The wire is binary `0xBEEF` plus a JSON envelope, not a text line protocol.

---

## Layout

```
apps/NetServer    listen, accept, StoreWorker, broadcast Shutdown
apps/NetClient    one-shot Read/Set/Shutdown, or --live
src/net_config    argv → ServerSettings / ClientSettings
src/net_proto     Opcode, WireEnvelope, FrameCodec, NetProtocol, ProtocolStack, ProtocolLayers (Vx/V1)
src/net_handler   UsRuntime, NetTransport, AbstractNetSession
src/net_client    INTERFACE alias of NetHandler (apps use ClientSession)
src/net_store     MessageStore (SQLite WAL + access_log), StoreWorker (queue thread)
src/PeerUtils.hpp loopback peer check (Shutdown gate)
src/NetDefaults.hpp  port/bind/db defaults + hardening caps
src/Dbg.hpp       [HH:MM:SS:mmm] … (stderr + <cwd>/logs/{role}-{stamp}-{pid}.log, daily UTC rotate)
tests/            AppConfigTests, ProtocolTests, MessageStoreTests, NetIntegrationTests
```

Flow: `bytes → UsRuntime → NetTransport → Session.feed → FrameCodec → ProtocolLayers → StoreWorker → MessageStore`.

---

## Wire

Frame (big-endian):

```
[0xBEEF u16][payload_len u32][version u16 BE][JSON WireEnvelope]
```

`version` is major.minor packed in one u16 (high byte = major). Envelope keys: `Type`, `Seq`, `Data` (JSON). The human `Version` string also appears in handshake `Data` for `VersionSrv`.

- Protocol version string is `1.0` (`kNetworkVersion`). Major bump when `Opcode` changes; minor when an existing opcode's `Data` layout changes.
- `Type` is `magic_enum::enum_name(Opcode)`: `Read`, `Set`, `Shutdown`, `VersionSrv`, `Ok`, `Error`, …
- Unknown `Type` is not a fatal frame error: it becomes `Opcode::None` and the session logs it.
- Payload cap: message body `kMaxMessageBytes` (1 MiB); wire JSON `kMaxPayloadLen` (~1.25 MiB). Bad magic, oversize length, or bad JSON is fatal and the socket closes.

Handshake: on accept the server sends `VersionSrv`. Then:

| Client sends | Server does |
| :--- | :--- |
| `Read` | `get()`, `Ok` with body (empty if never set); store failure → `Error` |
| `Set` | Non-empty: `put()`, `Ok`; empty body: `Ok`, store unchanged; store failure → `Error` |
| `Shutdown` | From loopback (or with `--allow-remote-shutdown`): broadcast goodbye, `Ok`, stop. Otherwise `Error` `"shutdown not allowed"` |

Timestamps on store rows are UTC nanoseconds; DBG event lines use UTC `HH:MM:SS:mmm`. Message bodies in DBG are redacted unless `--verbose`.

---

## Apps

Defaults: bind/host `127.0.0.1`, port `9555` (server port fixed; client may override), database `message.db` (created/opened by the server after bind).

```text
NetServer [--bind 127.0.0.1] [--db message.db]
          [--allow-remote-shutdown] [--log-retain 100000] [--verbose]

NetClient [--host 127.0.0.1] [--port 9555] [--verbose]
          [--read | --set TEXT | --shutdown | --live]
```

- Server listens on `--bind` (default localhost, not all interfaces). Listen port is always `9555` (no `--port` on the server).
- A second `NetServer` fails bind and exits without opening the DB.
- `Shutdown` from non-loopback peers is refused unless `--allow-remote-shutdown`.
- No client operation flag → prompt `What message we like to send?` and send `Set`.
- `--live` → one TCP connection, prompt `Set new message:` in a loop. Ok does not close. Incoming Shutdown or a dead peer ends the session. Crash/unavailable: wait 5 s, one reconnect, then exit. Graceful Shutdown is not retried (a retry would hit a restarted service).
- One-shot Read/Set use the same 5 s / one-retry recovery. `--shutdown` does not.
- Health probe: use `NetClient --read` against a live server (no dedicated ping opcode).
- Concurrent Sets queue on `StoreWorker` (one writer at a time); last successful commit wins.
- `SIGINT`/`SIGTERM` stop the server loop (developer / systemd convenience; primary ops path is service stop or `--shutdown`). The client also stops its loop on `SIGINT`/`SIGTERM` (no TCP Shutdown).
- In-process metrics counters (accepts, rejects, reads, sets, shutdowns, store_errors) live on `NetTransport` for tests/ops; they are not printed to DBG on a timer.
- DBG files go under `<cwd>/logs/` at runtime; `clean.sh` / `clean.cmd` move them into `doc/evidence/run-<UTC>/`. Rotate each UTC day (and at 10 MiB within a day); keep 14 newest per role; refuse to start if logs/ cannot be created.

---

## Failure modes and limitations

What happens when something goes wrong, and what this process will not do. The assessment leaves message length and the wire format unspecified; the choices below are the ones this tree actually implements.

### Malformed frames

`FrameCodec::try_decode` never throws. Bad magic, declared payload length above `kMaxPayloadLen` (~1.25 MiB), or a failed envelope parse (`rfl::json::read` Result or exception — JSON today) is **fatal**: the session logs the peer and closes the TCP connection. Incomplete headers/bodies wait for more bytes (`fatal=false`). Unknown `Type` and a different protocol **minor** are **not** fatal: they become `Opcode::None` / a versioned `NetProtocol` and the connection stays up. Peer **major** above `kProtoMajor` gets `Error` `"unsupported protocol major"`.

A declared length above the cap is rejected from the 8-byte header alone — the server does not wait for, or allocate, the claimed body.

### Oversized requests

The stored message has no assessment-imposed size. The **network and process** still need a bound: a pasted gigabyte would otherwise land in RAM, SQLite, and the write queue.

- Set **body** cap: `kMaxMessageBytes` (1 MiB), enforced on stdin/`--set`, encode, server `Set`, and `MessageStore::put`.
- Wire JSON cap: `kMaxPayloadLen` (1 MiB + 256 KiB for envelope overhead).
- Oversize `Set` that still fits on the wire is `Error` `"message too large"`; the stored row is unchanged.
- Oversize declared frame length is fatal (connection closed).

1 MiB is large enough for a shared textual message and small enough to be a defensible DoS limit. Raising it is a constant change, not a protocol bump.

### Client disconnect

If the peer closes after a complete `Set`/`Read` has been parsed, the store operation still runs. The reply is skipped when the accept socket is already gone (`hasSession(socket, session_id)` on the loop thread; callbacks capture `NetTransport*` and a session id, not the session). Socket pointers are reused after close, so the id is required. An incomplete frame is dropped with the connection. The live message is whatever last **committed**.

One-shot clients arm `kOneShotReplyTimeoutSec` (3 s) after send so a closed-port or black-hole peer cannot sit in `us_loop_run` forever. `--live` at a prompt is not armed.

### Shutdown

- Opcode `Shutdown` from loopback (or `--allow-remote-shutdown`): broadcast goodbye, `Ok` to the requester, then `post(stop)` so the current `on_data` callback can return before sockets/sessions are destroyed. Then drain `StoreWorker`.
- `SIGINT` / `SIGTERM`: same stop path (`requestStopFromSignal` → atomic flag + `us_wakeup_loop`). No TCP goodbye. Committed WAL survives; an in-flight `BEGIN` does not.
- Remote `Shutdown` without the flag: `Error` `"shutdown not allowed"`.

### Persistence / recovery

SQLite WAL, `message.id=1`. `put` writes body + `access_log` PUT in one `BEGIN IMMEDIATE`. An empty Set is ignored (no insert, no audit, `Ok` to the client). Crash after `COMMIT` keeps the last non-empty Set. Restart warms the cache from the row (a legacy zero-length BLOB is still a row, not never-set). Duplicate `NetServer` fails bind and never opens the DB.

### Concurrency and consistency

One uSockets thread for I/O; one `StoreWorker` thread for SQLite. Concurrent Sets **queue** and run one at a time (`message_set_mutex_` + store mutex + SQLite reserved lock). Last successful commit wins. Readers never see a half-written row. There is no compare-and-swap / expected-version token.

### Access log

Every successful store `get`/`put` appends `access_log` (`ts` UTC ns, `peer`, `op` `READ`/`PUT`, PUT `detail`). Failed protocol (bad frame, oversize Set, store error) and empty Sets are **not** a message access: they go to DBG only. PUT `detail` is truncated at 4 KiB. `--log-retain` (default 100000) prunes old rows; `0` disables prune.

DBG wire lines are `TIME=… UTC IP=… OP=… MSG=…`. Bodies are redacted unless `--verbose`.

### Resource limits (server)

| Cap | Default | Effect |
| :--- | :--- | :--- |
| Connections | 128 | extra accepts closed |
| Unsent bytes / socket | 4 MiB | socket dropped |
| Aggregate incomplete receive | 16 MiB | socket dropped |
| Incomplete-frame idle | 120 s | socket dropped; quiet `--live` is not idled |
| One-shot reply | 3 s | client socket dropped if no Ok/Error |
| Set body | 1 MiB | `Error` / encode refuse |
| Frame payload | ~1.25 MiB | fatal close |

### Known limitations (intentional)

- No TLS, no authentication. Bind defaults to `127.0.0.1`.
- No in-process DDoS / network-abuse system. Production must sit behind a provisioned proxy; the default bind is loopback.
- `synchronous=NORMAL` (not `FULL`): faster commits, weaker power-loss durability.
- No libFuzzer in the default build; `ProtocolStressTests` is a deterministic malformed-input stress suite.
- Protocol major 2+ is rejected rather than implemented.
- Health checks are `Read` and therefore write `access_log`.

---

## Stack

CMake 3.22, C++20, vcpkg (`vcpkg.json`):

`usockets`, `libuv`, `reflectcpp` (JSON / msgpack / xml), `magic-enum`, `cxxopts`, `sqlite3`, `gtest`.

Platforms for local/native builds: Linux (GCC) and Windows (MSVC). Presets live in `CMakePresets.json`. Bind and Shutdown loopback rules apply the same on Windows.

CI: [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) (Ubuntu: `linux-debug`, `linux-asan`, `linux-tsan`; Windows: `win-debug`). Each job runs every gtest binary. `.gitea/workflows/` and `ci-*` presets are leftover template files — see [DECISIONS.md](DECISIONS.md#ci-template-leftover).

---

## Tests

```text
AppConfigTests        argv / defaults / bind / hardening flags / no server --port
ProtocolTests         round-trip, bad magic, oversize, truncated, unknown Type, Seq echo,
                      ProtocolStack Vx/V1, store→Error, unsupported major, oversize Set,
                      empty Set ignored, invalid envelope, zero-length payload, DBG event format
ProtocolStressTests   deterministic garbage / oversize-header / split-frame stress (no libFuzzer)
MessageStoreTests     put/get, reopen, empty put is a no-op, access_log columns/retention,
                      SQL-injection payloads stay literal, oversize reject, directory-path open fail
NetIntegrationTests   TCP + app scenarios (timeout, Set/Read, empty Set no-op, overwrite, Shutdown goodbye,
                      remote Shutdown refused, max connections, recover-during-wait,
                      two clients, persist across restart, live two Sets, concurrent Sets,
                      concurrent readers, mixed readers+writers, connect/disconnect churn,
                      client disconnect during Set, malformed/oversized/unknown opcode,
                      signal-stop + restart, store open failure, kill server while connected)
```

Run after a configure: `ctest --test-dir out/build/<preset> --output-on-failure`.

Sanitizers (Linux): `cmake --preset linux-asan` (ASan+UBSan) or `linux-tsan` (TSan), then the same `cmake --build` / `ctest`.
