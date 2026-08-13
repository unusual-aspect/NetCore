# Design decisions

Why the tree looks the way it does. Read this before swapping a library or changing the wire.
CLI and layout: [README.md](README.md). Build: [HOWTO.txt](HOWTO.txt).

---

## What the code does

`NetServer` is a long-lived process. It listens on TCP (`127.0.0.1:9555` by default bind; **port is fixed** at `9555`), keeps **one** shared message in SQLite, and lets any connected client read or replace that message. Every read and write is appended to `access_log` with the same UTC timestamp as the data row.

`NetClient` is a short-lived or long-lived peer:

- one-shot `Read` / `Set` / `Shutdown`
- `--live`: one TCP session, prompt for the next Set, stay connected so a server goodbye still arrives at the prompt

On accept the server sends `VersionSrv`. Clients then send opcodes. The server answers `Ok` (or `Error`). A `Shutdown` from one client is broadcast to every **other** live session as `Shutdown` `"server is shutting down"`, then the requester gets `Ok` and the listen loop stops.

There is no TLS, no auth, no multi-document store. That is intentional (see below), not a missing feature.

---

## CI template leftover

**We use:** [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) on Ubuntu (`linux-debug`, `linux-asan` ASan+UBSan, `linux-tsan`) and Windows (`win-debug`). Each job configures, builds, and runs every gtest binary (AppConfig, Protocol, ProtocolStress, MessageStore, NetIntegration).

**We keep but do not rely on:** `.gitea/workflows/build.yaml`, `Dockerfile.base`, `Dockerfile.builder`, and the `ci-linux` / `ci-win` / `ci-testing` presets (Dockerfiles for optional container builds; Gitea workflow/presets for historical reference). See `.gitea/workflows/README.md`.

**Prefer:** GitHub Actions, or local `cmake --preset linux-debug|linux-asan|linux-tsan|win-debug` + `ctest`.

---

## Layers

```
argv          NetConfig          cxxopts + magic_enum
bytes         UsRuntime          uSockets + libuv
socket→session NetTransport      factory (server) or attach (client); Metrics counters
stream        AbstractNetSession feed() → FrameCodec
envelope      NetProto           0xBEEF + JSON WireEnvelope
opcodes       ProtocolStack      Vx (always) then V1 (major ≥ 1); add V2/V80 the same way
handlers      ProtocolLayers     versioned parse in net_proto; sessions supply hooks only
queue         StoreWorker        dedicated thread; get/put jobs + completion callbacks
row           MessageStore       SQLite WAL, id = 1 + access_log (never on the I/O thread)
peer gate     PeerUtils          isLoopbackPeer() for Shutdown
```

Sessions never call `us_*` directly. `NetTransport` owns the loop and maps sockets. Protocol code never opens SQLite. Apps (`ServerApp`, `ClientApp` + thin `ServerSession` / `ClientSession`) wire store/transport/CLI hooks into `protocol::makeServerStack` / `makeClientStack`; they are not a second protocol implementation.

Server Read/Set: `ServerSession` enqueues on `StoreWorker`; the worker runs `MessageStore`; completion `post()`s back onto the uSockets thread (and skips send if the accept socket already closed) before `Opcode::Ok` / `Error`. Slow disk or `busy_timeout` no longer freezes accepts. Completion lambdas capture `NetTransport*` + socket, **not** `ServerSession*`: the session can be destroyed if the client disconnects before SQLite finishes.

That split exists so a wire change stays in `src/net_proto`, a listen/connect change stays in `src/net_handler`, and a schema change stays in `src/net_store`.

---

## Libraries

Pinned in `vcpkg.json`. Do not add a second library that solves the same job.

### uSockets + libuv — I/O

**We use:** [uSockets](https://github.com/uNetworking/uSockets) with `LIBUS_USE_LIBUV`.

**It does:** one event loop, listen/accept or outbound connect, `on_data` / `on_close` / `on_writable`. `UsRuntime` is the C++ wrap: context ext holds `UsRuntime*`, socket ext holds unsent bytes, `post()` + `us_wakeup_loop` runs jobs on the loop thread (needed for `--live` stdin).

**Not Boost.Asio / raw `socket()` / WinSock:** Asio would pull Boost or stand-alone Asio and a different callback model into every session. Raw sockets would mean we write the Windows/Linux accept loop, non-blocking write buffer, and wakeup ourselves. uSockets is already that loop; libuv is its portable backend (Windows IOCP, Linux epoll) and is a vcpkg dependency of this build.

**Not µWebSockets:** we are not speaking HTTP/WebSocket. The HTTP layer would be unused weight.

**Constraint:** uSockets is not thread-safe. Anything that sends (`sendLiveSet`, `endLive`) must run on the loop thread via `UsRuntime::post`. Do not `us_socket_write` from the stdin thread.

### reflectcpp — envelope serialize

**We use:** [reflectcpp](https://github.com/getml/reflect-cpp) `rfl::json::write` / `read` on `WireEnvelope`.

**Why reflectcpp (not another JSON library):** one `WireEnvelope` struct is the schema. Reflection maps `Type` / `Seq` / `Data` without hand-written `to_json`, without a `.proto` compiler, and without a second source of truth next to `Opcode.hpp`. The same struct stays readable in DBG and in `ProtocolTests` (tests build JSON on purpose). Versioning is a string we bump, not a schema-compat dance.

**It does today:** JSON only on the wire (`FrameCodec.cpp`).

**Msgpack / XML features in vcpkg — intentional, not dead weight:** enabled so this tree is a **POC that the same `WireEnvelope` can serialize another way later** without swapping libraries or rewriting the envelope. Production path stays JSON until we deliberately add a codec (and then: one codec per port — do not mix). Do **not** strip msgpack/xml from the manifest “to clean unused deps”; that removes the future-proofing this choice paid for.

**Not nlohmann/json / RapidJSON:** those want hand-written `to_json` or DOM walk; they also do not give us the multi-format path above from one struct.

**Not protobuf / FlatBuffers / Cap’n Proto:** compiler in CI, generated sources, opaque payloads in DBG/tests.

### magic_enum — names on the wire and CLI

**We use:** `magic_enum::enum_name` / `enum_cast`.

**It does:** `Opcode` → JSON `Type` (`"Read"`, `"Set"`, …) and back. Unknown `Type` becomes `Opcode::None` plus `invalidType()`, not a crash.

**Not a hand-written `opcodeToString` table:** that table always drifts from the enum. magic_enum fails at compile time if the enum is not reflectable; adding a value is one line in `Opcode.hpp`.

### cxxopts — argv

**We use:** cxxopts in `AppConfig` for both binaries (`NetServer`, `NetClient`).

**It does:** `--bind` / `--db` / hardening flags on the server (listen port is **fixed** at `NET_DEFAULT_PORT`, not a flag); `--host` / `--port` / `--read` / `--set` / `--shutdown` / `--live` on the client.

**Not Boost.Program_options / CLI11 / `getopt`:** one small parser, already in vcpkg, enough for these flags. All argv logic lives in `NetConfig` so apps do not each invent a parser.

### SQLite — persistence

**We use:** SQLite 3, `PRAGMA journal_mode=WAL`, `PRAGMA synchronous=NORMAL`, `BEGIN IMMEDIATE` on `put()`.

**It does:** table `message` with `CHECK(id = 1)` (the schema itself forbids a second live row) and table `access_log` (append-only). `put()` writes body + ts and the audit row in **one** transaction, same `utc::now_ns()` on both. `get()` logs a READ then returns the in-memory cache (loaded at open). Process crash: committed WAL survives; an in-flight `BEGIN` does not.

**Not a folder of files:** no atomic rename story, no audit in the same commit as the body, easy to tear on crash.

**Not LMDB / RocksDB:** those are KV engines. We need a SQL constraint, a second table, and a transaction that covers both. SQLite is that, in-process, no extra daemon.

**Not PostgreSQL / MySQL:** this process is the service. A server DB would add ops (listen, users, migrations) for one row.

**Warm cache:** constructor loads `message.id=1`. A zero-length BLOB makes `sqlite3_column_blob` return NULL; if the row exists, `has_cache_` is set. New empty Sets are not inserted (protocol and `put` no-op).

**Server I/O thread:** do not call `MessageStore` from the uSockets callbacks. `StoreWorker` owns a queue + one thread; completions hop back with `NetTransport::post` before send.

### Concurrent Set (message lock)

**We use:** an exclusive **message-Set lock** on `StoreWorker` (`message_set_mutex_`) around every `put`, plus `MessageStore`’s own mutex and SQLite `BEGIN IMMEDIATE`.

**It does:** if 10 clients Set at once, their puts **queue** and run **one at a time**. Each still gets `Ok` when its write commits. The live row is always the **last successful commit** (last writer wins). No torn reads/writes of `message.id=1`.

**DBG:** when a Set is enqueued behind others → `SET waiting for message lock`; around the write → `Message Set lock acquired/released`.

**Not optimistic concurrency / version tokens:** clients do not pass an expected prior body. If you need compare-and-swap later, that is a protocol bump.

**Not “message locked” Error on contention:** wait-in-queue is the sync model so CLI `--set` does not fail spuriously under load.

### Message size cap (1 MiB body)

**We use:** `kMaxMessageBytes` (1 MiB) on client stdin/`--set`, `makeClientRequest`, server `Set` handler, `MessageStore::put`, and `FrameCodec::encode`. Wire JSON max is `kMaxPayloadLen` (1 MiB + 256 KiB headroom). Oversize declared frame lengths are fatal in `try_decode` without waiting for the body.

**It does:** stop a pasted/typed gigabyte from blowing RAM, SQLite, or the TCP write path — even without TLS.

**Not unbounded getline + hope the peer is polite:** stdin is read with a hard cap; surplus bytes to EOL are discarded.

**Not a JSON nesting / depth cap:** `kMaxMessageBytes` / `kMaxPayloadLen` already bound RAM and parse cost. Envelope validity is the codec (`rfl::json::read` today).

**Not storing an empty Set:** empty `Data` is `Ok` and a no-op — the live row and `access_log` stay as they were.

### SQLite — no SQL injection (DB integrity without TLS)

TLS is still out of scope. Hostile **message bodies** and **peer strings** must not become SQL.

**We use:**

1. **Prepared statements + binds only** for every value that comes from the wire or peer formatting (`sqlite3_bind_blob` / `bind_text` / `bind_int64`). SQL text is a fixed literal; payloads are never concatenated.
2. **`exec()` literals only** — pragma, schema, `BEGIN`/`COMMIT`/`ROLLBACK`. Comment contract: do not pass client data into `exec`.
3. **`sqlite3_open_v2` without `SQLITE_OPEN_URI`** — `--db` is a filesystem path, not a `file:` URI.
4. **Defensive config** — `SQLITE_DBCONFIG_DEFENSIVE`, trusted-schema off, loadable extensions disabled.
5. **Authorizer denylist** — deny ATTACH, DETACH, DROP*, ALTER, CREATE TRIGGER/VIEW/VTABLE. CRUD, pragma, and CREATE TABLE/INDEX stay allowed.

**It does:** a Set of `'; DROP TABLE message;--` stores that text as the message body and leaves the schema intact.

**Not “sanitize quotes by hand”:** escaping drifts; binds do not.

**Not encryption at rest / TLS:** this is query integrity only. Anyone with the DB file can still read it.

### GoogleTest — tests

**We use:** gtest via CTest (`AppConfigTests`, `ProtocolTests`, `MessageStoreTests`, `NetIntegrationTests`).

**It does:** argv/hardening edge cases; **invalid frames** (bad magic, oversize length, truncated header, unknown `Type`, Seq round-trip, stack Vx/V1); SQLite put/get/retention/SQL-injection-as-data; and real TCP app scenarios (Shutdown gate, max connections, recover, concurrent Sets, live, restart). Protocol unit tests drive `AbstractNetSession::feed` with no socket; integration tests spin `ServerApp` / `ClientApp`.

**Not Catch2 / a main() that prints "ok":** gtest is already in the vcpkg set and matches `ctest`. Test binaries use an in-tree `tests/test_main.cpp` (not `gtest_main` as a DLL) so `TEST()` registrars and `RUN_ALL_TESTS()` share one `UnitTest` instance. On Windows vcpkg's gtest is a DLL, so test targets also define `GTEST_LINKED_AS_SHARED_LIBRARY`.

---

## Protocol

### Binary frame + JSON body

```
[0xBEEF u16 BE][payload_len u32 BE][version u16 BE][JSON WireEnvelope]
```

**Why a magic + length, not newline-delimited text:** TCP is a byte stream. Text lines break on binary payloads, partial reads, and “is this two messages or one”. The 8-byte header (magic + length + wire version) tells `feed()` when a message is complete. `0xBEEF` vs `0xEFBE` also catches endian mistakes immediately (`FrameCodec` treats wrong magic as fatal and closes).

**Why JSON inside, not a packed C struct:** `Type` and `Version` stay readable in tests and DBG. Adding a field is a reflectcpp member, not a packed-layout comment. The length prefix still bounds the parse (`kMaxPayloadLen` ≈ 1.25 MiB for JSON); Set **body** is capped at `kMaxMessageBytes` (1 MiB) before store/encode/stdin. Declared lengths above the wire cap are fatal without buffering the rest — a “1 GB length” header never allocates 1 GB.

**Why not HTTP:** we are not a browser client. HTTP would add methods, headers, and framing we do not use.

### Versioning (`Vx` then `V1`, then `V2` / `V80` later)

`kNetworkVersion` is `"1.0"`. **Major** changes when `Opcode` changes. **Minor** changes when an existing opcode’s `Data` layout changes.

`ProtocolStack` and the versioned handlers live in `src/net_proto` (`ProtocolLayers`). Layer 0 (`Vx`: handshake, unknown type, client `Shutdown` goodbye, errors, **unsupported major**) always runs. Layer 1 (`V1`: server `Read`/`Set`/`Shutdown`, client `Ok`) runs only if Vx did not consume and the peer major is in `[1, kProtoMajor]` (today `1`). Peer major **above** `kProtoMajor` gets `Error` `"unsupported protocol major"` — do not silently run V1 against a future wire. New majors are another `addLayer(N, …, N)` inside `makeServerStack` / `makeClientStack` — sessions stay thin hooks (store, stop, exit codes).

Unknown `Type` is **not** fatal: keep the connection, log a human sentence. Bad magic / oversize / broken JSON **is** fatal: the stream is not ours.

### `Seq`

Echoed on `Ok` so a reply can match a request. Live can send another Set after `Ok`; `request_sent_` clears on success. Unused “flags” were removed from the envelope on purpose — do not put them back without a caller.

### No encryption

The service is meant for a trusted network (localhost / lab). TLS would sit under `UsRuntime`, not inside `FrameCodec`. Do not sprinkle crypto into JSON. If that changes, terminate TLS in front or wrap the socket once — do not invent a second envelope.

---

## Time

All store timestamps are Unix-epoch nanoseconds (`utc::now_ns()`). DBG clock is the same instant formatted `HH:MM:SS:mmm` UTC (`gmtime_s` / `gmtime_r`, never `localtime`). Two hosts in different zones still compare equal `ts` values.

Each process also writes DBG to `logs/{NetServer|NetClient}-{YYYYMMDD-HHMMSS-mmm}-{pid}.log` under the **process working directory** (create `logs/` if missing; refuse to start if the folder or first file cannot be created).

**DBG file rotation (daily):** on each UTC calendar-day change (`utc::date_id`), logging closes the current file and opens a new stamp. Within a day, a segment also rotates if it reaches `kDbgLogMaxBytes` (10 MiB). Newest `kDbgLogMaxFiles` (14) files per role are kept; older segments are deleted. stderr still gets the same lines. Under systemd, set `WorkingDirectory` to a writable data dir so retained logs land next to the DB.

---

## Client recovery and `--live`

The server is expected to run forever and come back after a crash.

- One-shot Read/Set: if the first connection fails or drops before `Ok`, wait **5 seconds**, try **once**, then exit 1 (“not available”).
- `Shutdown` is **not** retried. A retry would send Shutdown to a service that just restarted.
- `--live` keeps the TCP session open. `Ok` does not close. Stdin runs on a side thread; Set is `post()`’d onto the loop. Incoming `Shutdown` or a dead peer ends `run()`. Crash → same 5 s / one reconnect. Graceful goodbye → exit 0, no reconnect.

uSockets `on_close` / connect error on a **client** transport calls `stop()` so `us_loop_run()` returns. Without that, a dead server leaves the client hung in the loop.

---

## Hardening

Production-oriented defaults that do **not** add TLS or auth. Trusted-network assumption remains; these reduce accidental exposure and crash risk.

### Bind default `127.0.0.1` + `--bind`

**We use:** listen on localhost unless `--bind` / `-B` says otherwise.

**It does:** avoid advertising the service on all interfaces by accident when someone only meant a lab port.

**Not `0.0.0.0` as default:** convenient for LAN demos, but pairs badly with unauthenticated `Shutdown`. Opt in with `--bind 0.0.0.0` when you really want that.

**Constraint:** bind address is independent of client `--host`. Windows and Linux behave the same. Server listen **port is not configurable via argv** (always `NET_DEFAULT_PORT` / `9555`); clients still pass `--port` when talking to a non-default lab build. A second `NetServer` on the same host therefore always hits bind failure.

### Fixed port + listen before SQLite

**We use:** fixed listen port; `ServerApp::run` calls `listen` first and opens `MessageStore` only after bind succeeds.

**It does:** a duplicate server exits 1 with no SQLite open/WAL side effects. Operators cannot dodge the collision by picking another `--port`.

**Not a DB lock file / PID file:** bind exclusivity on the fixed port is enough for “one service per host bind”. Integration tests may still set `ServerSettings.port` in-process to avoid clashing with a developer’s local instance.

**Not server `--port`:** re-adding it would allow two processes on different ports sharing one `--db` and diverging in-memory caches.

### Shutdown gate (loopback-only)

**We use:** accept `Shutdown` only from loopback peers (`PeerUtils::isLoopbackPeer` — `127.0.0.1`, `::1`, IPv4-mapped), unless `--allow-remote-shutdown`.

**It does:** stop drive-by process kills from anyone who can reach the port, without inventing crypto.

**Not auth tokens / TLS client certs (yet):** still not authentication — a process on the same host can still shut the server down. Remote admin must pass the flag **and** understand the risk.

**Not “disable Shutdown entirely” as the only switch:** loopback keeps local ops (`NetClient --shutdown`, systemd stop via signal) working.

### Store errors → `Opcode::Error`

**We use:** catch SQLite failures on Read/Set in `ProtocolLayers`, reply `Error` with fixed `kStoreUnavailableMsg` (`"store unavailable"`), keep the connection and the loop alive. The real `exception.what()` / SQLite text goes to DBG only (`STORE ERROR`).

**It does:** a full disk or busy DB does not abort the process through an uncaught exception on the uSockets data path, and peers do not learn internal store diagnostics.

**Not abort-on-throw:** surviving blips beats a dead service; clients must handle `Error` (NetClient already exits non-zero on `Error`).

**Not leak `sqlite3_errmsg` on the wire:** operators read DBG; clients only need a stable failure token.

### Connection, idle, and receive-buffer caps

**We use:** `kMaxConnections` (128), `kMaxUnsentBytes` (4 MiB per socket), `kMaxTotalReceiveBytes` (16 MiB across sessions), and `kIdleTimeoutSec` (120) only while a session has an **incomplete** frame buffered (`us_socket_timeout`; cleared once `feed()` finishes a message).

**It does:** bound memory and accept flood damage; excess accepts are closed; oversized unsent queues drop the socket; slowloris-style drip of a partial header/body is cut; quiet `--live` clients waiting at a prompt are **not** killed for silence alone.

**Not unlimited accept + unbounded `unsent_bytes_`:** lab convenience loses under a slow consumer or SYN flood of sessions.

**Trade-off:** legitimate bursts above the cap look like failures — raise the constants if you have a measured need. Idle granularity follows uSockets (~4 s).

### No in-process DDoS / network-abuse protection

**Not covered in this code:** DDoS, connection floods beyond the local accept cap, per-IP rate limits, SYN cookies, bot/abuse scoring, or other network-abuse systems. Size caps (`kMaxMessageBytes` / `kMaxPayloadLen`) and `kMaxConnections` only bound *this process*. They are not a DDoS product.

**Must run behind a provisioned edge:** production (and any non-loopback bind) must sit under a proxy / load balancer / WAF / host firewall that already owns volume protection, TLS termination if needed, and hiding of port `9555`. Do not put the raw listener on an untrusted network and expect `NetServer` to absorb floods.

**Why not in-process:** duplicating token-buckets and SYN policy here would add knobs without replacing the edge. Loopback (`--bind 127.0.0.1`, the default) is the lab path; a provisioned proxy is the deployed path.

**Not a substitute for TLS/auth** (still out of scope).

### Signals (`SIGINT` / `SIGTERM`)

**We use:** signal handler → `NetTransport::requestStopFromSignal()` → atomic flag + `us_wakeup_loop`. The loop’s wakeup callback (`drainPosted`) sees the flag and calls `UsRuntime::stop()` on the loop thread. Both `NetServer` (`main`) and `NetClient` (`ClientApp`) install this path.

**It does:** Ctrl+C / SIGTERM end the loop without a TCP `Shutdown` opcode. Useful for developers and for systemd `KillSignal=SIGTERM`. On the client it leaves a prompt/connect without hanging the uSockets loop.

**Why not `post(stop)` from the handler:** `post()` takes a mutex and allocates — not async-signal-safe; a signal during another `post()` could deadlock. The flag+wakeup path avoids locks in the handler.

**Not ignore signals and rely only on Shutdown:** local testing still expects Ctrl+C to work. Opcode `Shutdown` remains the graceful goodbye path for live clients.

### SQLite `synchronous=NORMAL` + `busy_timeout`

**We use:** keep `PRAGMA synchronous=NORMAL`, add `busy_timeout=5000`, and fail loudly if access_log inserts fail.

**It does:** good commit latency for a local WAL DB; wait briefly on lock contention; audit correctness matches body writes.

**Not `FULL`/`EXTRA` by default:** stronger durability on power loss, slower every `put`. Document if you change it.

**Not silent skip on READ audit failure:** old code ignored prepare/step errors; that hid broken audit trails.

### `access_log` max-rows retention (audit rotation)

**We use:** `--log-retain` (default 100000); prune oldest rows after audit inserts. PUT `detail` is truncated to `kMaxAccessLogDetailBytes` (4 KiB) with a `...(truncated,len=N)` suffix. After deletes, periodically `PRAGMA wal_checkpoint(TRUNCATE)` + `incremental_vacuum` so the **on-disk** file / WAL do not keep growing overnight. Clean shutdown also checkpoints.

**The live message does not need rotation:** `message` is one row (`id=1`); Sets overwrite it. Growth is the **access_log** (and WAL), not a pile of messages.

**It does:** bound disk growth for a long-lived process (row count, per-row size, and reclaim after prune).

**`--log-retain 0`:** disables prune (unbounded row growth). Intentional ops escape hatch — do not use on a forever-running host without external log shipping.

**Not a full DB file rotate** (rename `message.db` → dated copy): row prune + WAL reclaim is enough for this one-row service. **Not time-based purge as the first knob:** row count is simpler and deterministic in SQLite.

**Trade-off:** old audit history is deleted; export elsewhere if you need long retention. Truncated `detail` is not a full payload archive. `auto_vacuum=INCREMENTAL` applies fully on **new** DB files.

### DBG body redaction + `--verbose`

**We use:** `netdbg::event` redacts `MSG=` payloads unless `--verbose`.

**It does:** less PII/noise in default logs and on-disk DBG files.

**Not always-log full bodies:** convenient when debugging Set payloads, expensive and leaky in shared logs. Pass `--verbose` locally when you need the bytes.

### DBG file rotation (per UTC day)

**We use:** rotate the on-disk DBG file when the UTC date changes; also rotate if a segment hits `kDbgLogMaxBytes`; prune to `kDbgLogMaxFiles` newest `{role}-*.log` files.

**It does:** overnight / multi-day `NetServer` runs do not grow a single unbounded log file. Operators get one primary file per day (plus extras only if that day is very chatty).

**Not logrotate(8) as a hard dependency:** in-process rotation works the same on Windows and Linux. **Not local-time midnight:** UTC matches store/DBG clocks and avoids DST double-rolls.

### Health = existing `Read`

**We use:** `NetClient --read` (or any successful TCP + Version/Read) as the readiness check.

**It does:** avoid a new opcode and version bump for a one-row service.

**Every Read writes `access_log`:** intentional. Health probes are audit events like any other Read — keep that trail; do not special-case probes to skip logging.

**Not a dedicated Ping opcode / HTTP `/healthz`:** extra surface for little gain here.

### In-process metrics (no DBG timer)

**We use:** counters on `NetTransport::Metrics` (accepts, rejected_conns, reads, sets, shutdowns, store_errors). Server accept/reject and `ServerSession` Read/Set/Shutdown paths bump them. Integration tests assert on these counters.

**It does:** cheap introspection without a scrape port. Values stay in memory for the process lifetime.

**Not a 5 s DBG flush / exit dump:** that timer was removed — it duplicated noise already covered by ACCEPT/DISCONNECT/`STORE ERROR` event lines and made long runs chatty. Prefer DBG events + `ctest` for ops feedback; add a scrape endpoint later if operators need live export.

**Not Prometheus/StatsD endpoint:** out of scope.

### Packaging = `install` + sample unit

**We use:** CMake `install(TARGETS … RUNTIME DESTINATION bin)` and `doc/netserver.service.example` (`ProtectSystem=strict`, `ReadWritePaths=/var/lib/netserver`, `PrivateTmp`, `NoNewPrivileges`).

**It does:** enough for a host install without owning a full distro or runtime container story. Create `/var/lib/netserver` for the service user before enable.

**Not a mandatory Docker production image in-tree:** run how you prefer. The CI Dockerfiles / Gitea workflow are **template leftovers** and are not verified for this tree (see [CI template leftover](#ci-template-leftover)).

**Not live metrics scrape / journald-only DBG:** counters stay in-process for tests; DBG files under `<cwd>/logs/`. Add scrape/journal-only later if operators need it.

### Encryption

**Still none.** TLS would sit under `UsRuntime`, not in `FrameCodec`. Hardening above does not replace encryption on an untrusted network.

---

## What not to do

| Temptation | Why not |
| :--- | :--- |
| Second persistence (files next to SQLite) | Two sources of truth; audit and body must commit together |
| Extra vcpkg JSON library | `WireEnvelope` already serializes |
| Asio next to uSockets | Two loops, two thread rules |
| Text line protocol | Breaks framing and versioning; the wire is `0xBEEF` |
| Retry Shutdown | Kills the instance that just came back |
| Write the socket from stdin | uSockets is loop-thread only — use `post()` |
| `char*` / short names (`rt_`, `s`) on new code | Sessions, peers, and DBG lines need readable names |
| Lecture comments (“do not mix both”) | Comments are flow + contracts for the next person in six months |
| Re-default bind to all interfaces | Accidental LAN exposure with unauthenticated opcodes |
| Accept remote Shutdown without `--allow-remote-shutdown` | Anyone who can connect can stop the process |
| Let store exceptions escape the data callback | Aborts the whole server on a SQLite blip |
| Log full message bodies by default | PII and disk noise; use `--verbose` when needed |
| Add a second health opcode for a one-row store | `Read` is enough; avoid version churn |
| Re-add server `--port` | Lets a second process dodge bind collision and share one DB with a stale cache |
| Open SQLite before listen | Duplicate instance leaves WAL/side effects even when bind fails |
| Put DBG under the binary install dir | `/usr/local/bin/logs` is not writable for the service user — use cwd/`WorkingDirectory` |
| Run V1 handlers for peer major > `kProtoMajor` | Invent a new layer; reply `Error` until then |
| Concatenate client/peer/body into SQL | SQL injection — use `sqlite3_bind_*` only; keep authorizer |
| Enable SQLite ATTACH / load_extension | Extra DB files and native code load from a compromised payload path |
| Accept unbounded Set bodies / frame lengths | Blows RAM, SQLite, and the write queue — enforce `kMaxMessageBytes` / `kMaxPayloadLen` |
| Let DBG grow one file forever on a long-lived server | Rotate per UTC day (+ size) and prune old segments |
| Run MessageStore get/put on the uSockets thread | Disk stalls freeze all clients — use `StoreWorker` queue |
| Add in-process Read/Set rate limits “for DDoS” | DDoS/network abuse is out of scope here — provision a proxy in front |
| Skip `access_log` for health Reads | Probes are audit data; retention already bounds growth |
| Put SQLite / exception text in Opcode::Error Data | Peers only get `store unavailable`; detail stays in DBG |
| Run concurrent Sets without the message-Set lock | Torn writes / races — queue + `message_set_mutex_` + store mutex |
| Strip msgpack/xml from reflectcpp “because unused” | Intentional POC for a future codec on the same `WireEnvelope` |

---

## Changing something

1. New opcode: add to `Opcode`, handle in `ProtocolLayers` V1 (or Vx if it is version-wide), bump **major** if the enum meaning changed for old peers. New major protocol: `addLayer(N, …)` in `makeServerStack` / `makeClientStack`.
2. New envelope field: add to `WireEnvelope`, bump **minor** (or major if old peers cannot ignore it).
3. New CLI flag: `AppConfig` only, then the app. Do not parse `argv` in `main` a second time.
4. New library: vcpkg first, one job per library, update this file with the rejected alternative.
