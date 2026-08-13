# NetProject

C++20 TCP service: one persistable shared message, many clients, access audit.

Build, container, and run steps: [HOWTO.txt](HOWTO.txt).
Why these libraries and this split: [DECISIONS.md](DECISIONS.md).

> **Note:** [`.gitea/workflows/`](../.gitea/workflows/) and `ci-*` presets in `CMakePresets.json` are leftover from a previous project template and are **not** verified for this tree. Prefer local `cmake --preset` + `ctest`. Dockerfiles remain for optional container builds — see [HOWTO.txt](HOWTO.txt) and [DECISIONS.md](DECISIONS.md#ci-template-leftover).

---

## What it is

`NetServer` holds a single message (`message.id = 1`) in SQLite WAL and serves it over TCP.
`NetClient` reads, sets, or shuts the server down. `--live` keeps the socket open and prompts for the next Set so a Shutdown goodbye still reaches a client sitting at the prompt.

Not a MOTD toy and not a text line protocol. The wire is binary `0xBEEF` plus a JSON envelope.

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
| `Set` | `put()`, `Ok`; store failure → `Error` |
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

## Stack

CMake 3.22, C++20, vcpkg (`vcpkg.json`):

`usockets`, `libuv`, `reflectcpp` (JSON / msgpack / xml), `magic-enum`, `cxxopts`, `sqlite3`, `gtest`.

Platforms for local/native builds: Linux (GCC) and Windows (MSVC). Presets live in `CMakePresets.json`. Bind and Shutdown loopback rules apply the same on Windows. (`ci-*` presets / `.gitea` workflow are template leftovers — see [DECISIONS.md](DECISIONS.md#ci-template-leftover).)

---

## Tests

```text
AppConfigTests       argv / defaults / bind / hardening flags / no server --port
ProtocolTests        round-trip, bad magic, oversize, truncated, unknown Type, Seq echo,
                     ProtocolStack Vx/V1, store→Error, unsupported major, oversize Set
MessageStoreTests    put/get, reopen, access_log retention/truncate, SQL-injection payloads
                     stay literal, oversize reject, PeerUtils loopback
NetIntegrationTests  TCP + app scenarios (timeout, Set/Read, overwrite, Shutdown goodbye,
                     remote Shutdown refused, max connections, recover-during-wait,
                     two clients, persist across restart, live two Sets, concurrent Sets,
                     kill server while connected)
```

Run after a configure: `ctest --test-dir out/build/<preset> --output-on-failure`.
