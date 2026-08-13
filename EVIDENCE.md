# Manual run evidence

Sources used (project tree only — nothing invented):

- DBG files under `doc/evidence/run-20260812-204647/` (migrated from runtime `logs/`; UTC wall clock in filenames / line prefixes)
- Live SQLite file (queried 2026-08-12 as cwd `message.db`; after clean it lives under `doc/evidence/run-*/message.db`)

No separate terminal transcript or pasted `sqlite3` dump was present in the repo; client/server **commands below are inferred from DBG** (`Command Read` / `Command Set` / `Live Set loop` / `Command Shutdown` / `Start listen`). Where a scenario is incomplete in these logs, it is flagged — not filled in.

---

## 1. Set (client modify)

**Inferred commands:** `NetServer` (already listening); `NetClient` Set payload `lol^DROP` (DBG: `Command Set: lol^DROP`).

**Client** — `doc/evidence/run-20260812-204647/NetClient-20260812-151456-708-62328.log`:

```
[15:15:04:358] ../apps/NetClient/ClientApp.cpp@run[106]                                             : Command Set: lol^DROP
[15:15:04:361] ../apps/NetClient/ClientSession.cpp@sendRequest[105]                                 : TIME=15:15:04:361 UTC IP=127.0.0.1 OP=SEND SET MSG=<redacted len=8>
[15:15:04:372] ../net_proto/ProtocolLayers.cpp@logRecv[231]                                         : TIME=15:15:04:372 UTC IP=127.0.0.1 OP=RECV Ok
[15:15:04:372] ../apps/NetClient/ClientApp.cpp@runOnce[135]                                         : Exit 0
```

**Server** — `doc/evidence/run-20260812-204647/NetServer-20260812-151453-206-62310.log`:

```
[15:15:04:366] ../apps/NetServer/ServerSession.cpp@onConnected[69]                                  : TIME=15:15:04:366 UTC IP=127.0.0.1 OP=ACCEPT
[15:15:04:368] ../net_proto/ProtocolLayers.cpp@logRecv[231]                                         : TIME=15:15:04:368 UTC IP=127.0.0.1 OP=RECV Set MSG=<redacted len=8>
[15:15:04:369] ../net_store/StoreWorker.cpp@workerMain[147]                                         : Message Set lock acquired — peer=127.0.0.1
[15:15:04:371] ../net_store/MessageStore.cpp@put[269]                                               : TIME=15:15:04:371 UTC IP=127.0.0.1 OP=STORE PUT MSG=<redacted len=8>
[15:15:04:371] ../net_store/StoreWorker.cpp@workerMain[156]                                         : Message Set lock released — peer=127.0.0.1
[15:15:04:371] ../net_proto/ProtocolLayers.cpp@operator()[91]                                       : TIME=15:15:04:371 UTC IP=127.0.0.1 OP=SET MSG=<redacted len=8>
```

**DB** — `access_log` row for that PUT (from `message.db`):

```
(1, 1786547704369507941, '127.0.0.1', 'PUT', 'lol^DROP')
```

---

## 2. Read (client read)

**Inferred commands:** `NetClient --read` against a live `NetServer` (DBG: `Command Read`).

**Client** — `doc/evidence/run-20260812-204647/NetClient-20260812-152302-317-63794.log` (`--verbose` run; body visible):

```
[15:23:02:336] ../apps/NetClient/ClientApp.cpp@run[103]                                             : Command Read
[15:23:02:338] ../apps/NetClient/ClientSession.cpp@sendRequest[102]                                 : TIME=15:23:02:338 UTC IP=127.0.0.1 OP=SEND READ
[15:23:02:343] ../net_proto/ProtocolLayers.cpp@logRecv[231]                                         : TIME=15:23:02:343 UTC IP=127.0.0.1 OP=RECV VersionSrv MSG=1.0
[15:23:02:346] ../net_proto/ProtocolLayers.cpp@logRecv[231]                                         : TIME=15:23:02:346 UTC IP=127.0.0.1 OP=RECV Ok MSG=lol
[15:23:02:347] ../apps/NetClient/ClientApp.cpp@runOnce[135]                                         : Exit 0
```

**Server** — `doc/evidence/run-20260812-204647/NetServer-20260812-152252-122-63767.log`:

```
[15:23:02:338] ../apps/NetServer/ServerSession.cpp@onConnected[69]                                  : TIME=15:23:02:338 UTC IP=127.0.0.1 OP=ACCEPT
[15:23:02:342] ../net_proto/ProtocolLayers.cpp@logRecv[231]                                         : TIME=15:23:02:342 UTC IP=127.0.0.1 OP=RECV Read
[15:23:02:346] ../net_store/MessageStore.cpp@get[314]                                               : TIME=15:23:02:346 UTC IP=127.0.0.1 OP=STORE READ MSG=lol
[15:23:02:346] ../net_proto/ProtocolLayers.cpp@operator()[59]                                       : TIME=15:23:02:346 UTC IP=127.0.0.1 OP=READ MSG=lol
```

**DB** — matching READ audit (latest rows at query time):

```
(10, 1786548182342800995, '127.0.0.1', 'READ', None)
(9, 1786548175452819233, '127.0.0.1', 'READ', None)
```

---

## 3. Persist-after-kill

**Story in the logs:** live Sets commit `lol` under server `…-151757-503-62799`, that process ends with `Metrics stop` and `shutdowns=0` (not a TCP Shutdown). A **new** server process `…-152252-122-63767` later serves `STORE READ MSG=lol`. Live row in `message.db` is still `lol`.

**Write (verbose server)** — `doc/evidence/run-20260812-204647/NetServer-20260812-151757-503-62799.log`:

```
[15:18:15:184] ../net_proto/ProtocolLayers.cpp@logRecv[231]                                         : TIME=15:18:15:184 UTC IP=127.0.0.1 OP=RECV Set MSG=lol
[15:18:15:190] ../net_store/MessageStore.cpp@put[269]                                               : TIME=15:18:15:190 UTC IP=127.0.0.1 OP=STORE PUT MSG=lol
[15:19:48:318] ../apps/NetServer/ServerApp.cpp@logMetrics[28]                                       : Metrics stop accepts=1 rejected=0 reads=0 sets=2 shutdowns=0 store_errors=0
```

**Read after new process** — `doc/evidence/run-20260812-204647/NetServer-20260812-152252-122-63767.log`:

```
[15:22:52:137] ../net_handler/NetTransport.cpp@listen[142]                                          : Start listen on 127.0.0.1:9555
[15:22:55:459] ../net_store/MessageStore.cpp@get[314]                                               : TIME=15:22:55:459 UTC IP=127.0.0.1 OP=STORE READ MSG=lol
```

**DB** — current singleton + the PUT that stored `lol`:

```
=== message ===
(1, 3, b'lol')
=== access_log ===
(8, 1786547895185186705, '127.0.0.1', 'PUT', 'lol')
```

**Gap note:** logs do not contain an OS kill line (e.g. `kill -9`). Persistence is shown across **process stop + later restart** with the same on-disk DB; if the brief requires an explicit SIGKILL demo, re-run and capture that.

---

## 4. Multi-client

**Inferred commands:** `NetClient --live` (stays connected) plus a second `NetClient --read` while the live session is still up.

**Live client still connected** — `doc/evidence/run-20260812-204647/NetClient-20260812-151730-543-62728.log`:

```
[15:17:30:560] ../apps/NetClient/ClientApp.cpp@run[77]                                              : Live Set loop → 127.0.0.1:9555
[15:17:30:563] ../apps/NetClient/ClientSession.cpp@onConnected[63]                                  : Connected to server!
[15:17:55:670] ../net_handler/NetTransport.cpp@operator()[97]                                       : TIME=15:17:55:670 UTC IP=127.0.0.1 OP=DISCONNECT
```

**Overlapping accept + Read on server** — `doc/evidence/run-20260812-204647/NetServer-20260812-151718-162-62694.log` (live ACCEPT at `:30`, second ACCEPT/Read at `:37`, live DISCONNECT at `:55`):

```
[15:17:30:563] ../apps/NetServer/ServerSession.cpp@onConnected[69]                                  : TIME=15:17:30:563 UTC IP=127.0.0.1 OP=ACCEPT
[15:17:37:100] ../apps/NetServer/ServerSession.cpp@onConnected[69]                                  : TIME=15:17:37:100 UTC IP=127.0.0.1 OP=ACCEPT
[15:17:37:101] ../net_proto/ProtocolLayers.cpp@logRecv[231]                                         : TIME=15:17:37:101 UTC IP=127.0.0.1 OP=RECV Read
[15:17:37:106] ../net_store/MessageStore.cpp@get[314]                                               : TIME=15:17:37:106 UTC IP=127.0.0.1 OP=STORE READ MSG=DROP
[15:17:55:670] ../net_handler/NetTransport.cpp@operator()[93]                                       : TIME=15:17:55:670 UTC IP=127.0.0.1 OP=DISCONNECT
[15:17:55:672] ../apps/NetServer/ServerApp.cpp@logMetrics[28]                                       : Metrics stop accepts=3 rejected=0 reads=2 sets=1 shutdowns=0 store_errors=0
```

---

## 5. Shutdown (client terminate + server stop)

**Inferred commands:** `NetClient --shutdown` while `NetServer` is listening.

**Client** — `doc/evidence/run-20260812-204647/NetClient-20260812-152314-619-63832.log`:

```
[15:23:14:644] ../apps/NetClient/ClientApp.cpp@run[109]                                             : Command Shutdown
[15:23:14:647] ../apps/NetClient/ClientSession.cpp@sendRequest[108]                                 : TIME=15:23:14:647 UTC IP=127.0.0.1 OP=SEND SHUTDOWN
[15:23:14:656] ../net_proto/ProtocolLayers.cpp@logRecv[231]                                         : TIME=15:23:14:656 UTC IP=127.0.0.1 OP=RECV Ok
[15:23:14:657] ../apps/NetClient/ClientApp.cpp@runOnce[135]                                         : Exit 0
```

**Server** — `doc/evidence/run-20260812-204647/NetServer-20260812-152252-122-63767.log`:

```
[15:23:14:647] ../apps/NetServer/ServerSession.cpp@onConnected[69]                                  : TIME=15:23:14:647 UTC IP=127.0.0.1 OP=ACCEPT
[15:23:14:653] ../net_proto/ProtocolLayers.cpp@logRecv[231]                                         : TIME=15:23:14:653 UTC IP=127.0.0.1 OP=RECV Shutdown
[15:23:14:653] ../net_proto/ProtocolLayers.cpp@parseServerV1[111]                                   : TIME=15:23:14:653 UTC IP=127.0.0.1 OP=SHUTDOWN MSG=Graceful stop request
[15:23:14:655] ../apps/NetServer/ServerApp.cpp@logMetrics[28]                                       : Metrics stop accepts=3 rejected=0 reads=2 sets=0 shutdowns=1 store_errors=0
```

### Missing: Shutdown **broadcast** to another live client

These logs show loopback Shutdown → requester `Ok` → server exit (`shutdowns=1`).  
They do **not** show a second connected client receiving `Shutdown` / `server is shutting down` (no live peer present at 15:23:14).

**Action for you:** re-run `NetClient --live` in one terminal, `NetClient --shutdown` in another, and capture the live client’s RECV Shutdown / goodbye lines.

---

## 6. Duplicate-bind rejection

**Inferred commands:** first `NetServer` holds `127.0.0.1:9555`; second `NetServer` started while the first is still up.

**First instance listening** — `doc/evidence/run-20260812-204647/NetServer-20260812-152459-689-64188.log`:

```
[15:24:59:705] ../net_handler/NetTransport.cpp@listen[142]                                          : Start listen on 127.0.0.1:9555
[15:24:59:729] ../apps/NetServer/ServerApp.cpp@run[43]                                              : Start NetServer on 127.0.0.1:9555
```

**Second instance rejected** — `doc/evidence/run-20260812-204647/NetServer-20260812-152503-213-64190.log`:

```
[15:25:03:234] ../net_handler/NetTransport.cpp@listen[138]                                          : Cannot listen on 127.0.0.1:9555 — bind failed (address/port in use, or no permission).
[15:25:03:235] ../apps/NetServer/ServerApp.cpp@run[24]                                              : Cannot start NetServer on 127.0.0.1:9555 — listen/bind failed (address/port in use, or no permission).
```

(Same pattern also in `…-152145-612-63554.log` vs `…-152146-602-63558.log`.)

---

## 7. SIGTERM / signal stop (no TCP Shutdown)

**What the logs show:** server starts, then `Metrics stop` with `shutdowns=0` and **no** `RECV Shutdown` — i.e. stop via the signal/`stop()` path, not the Shutdown opcode.

**Example** — `doc/evidence/run-20260812-204647/NetServer-20260812-152145-612-63554.log`:

```
[15:21:45:661] ../net_handler/NetTransport.cpp@listen[142]                                          : Start listen on 127.0.0.1:9555
[15:21:45:760] ../apps/NetServer/ServerApp.cpp@run[77]                                              : Start NetServer on 127.0.0.1:9555
[15:21:47:151] ../apps/NetServer/ServerApp.cpp@logMetrics[28]                                       : Metrics stop accepts=0 rejected=0 reads=0 sets=0 shutdowns=0 store_errors=0
```

**Another** — `doc/evidence/run-20260812-204647/NetServer-20260812-152024-471-63257.log`:

```
[15:20:24:498] ../net_handler/NetTransport.cpp@listen[142]                                          : Start listen on 127.0.0.1:9555
[15:20:26:502] ../apps/NetServer/ServerApp.cpp@logMetrics[28]                                       : Metrics stop accepts=0 rejected=0 reads=0 sets=0 shutdowns=0 store_errors=0
```

### Gap: logs never name `SIGTERM` / `SIGINT`

DBG has no `SIGTERM`/`SIGINT` string. Evidence supports “stopped without Shutdown opcode”; it does **not** prove which signal (vs Ctrl+C / IDE stop).  
**Action for you:** `kill -TERM <pid>` (or Ctrl+C) and keep a one-line terminal note next to the matching `Metrics stop` file, or add a single DBG line on signal if you want self-contained proof.

---

## Audit log (assessment requirement — not a separate scenario)

From `message.db` at evidence time (peer IP, op, optional PUT detail, UTC ns timestamp):

```
(1, 1786547704369507941, '127.0.0.1', 'PUT', 'lol^DROP')
(5, 1786547854959243220, '127.0.0.1', 'PUT', 'DROP')
(8, 1786547895185186705, '127.0.0.1', 'PUT', 'lol')
(9, 1786548175452819233, '127.0.0.1', 'READ', None)
(10, 1786548182342800995, '127.0.0.1', 'READ', None)
```

---

## Requirement → evidence map

Assessment surface (Intermedia-style): **server** shared message + persist + multi-client + audit; **client** read / modify / terminate. Hardening extras from this tree called out separately.

| # | Requirement | Evidence section | Status in these logs |
| :--- | :--- | :--- | :--- |
| S1 | Clients can **read** the shared message | §2 Read | **Covered** |
| S2 | Clients can **update/set** the shared message | §1 Set | **Covered** |
| S3 | Message **survives process death / restart** | §3 Persist-after-kill | **Covered** (restart after stop; not labeled SIGKILL) |
| S4 | **Multiple clients** concurrently | §4 Multi-client | **Covered** |
| S5 | **Access audit** (time, peer IP, read/modify, modify detail) | Audit log + §1/§2 DB rows | **Covered** |
| S6 | Server **listens / accepts** TCP peers | §1–§5 listen + ACCEPT lines | **Covered** |
| S7 | Server can be **stopped by client terminate** | §5 Shutdown | **Partial** — terminate + stop yes; **broadcast to other live clients missing** |
| C1 | Client **Read** | §2 | **Covered** |
| C2 | Client **Set / modify** | §1 (also `--live` Sets in §3/§4) | **Covered** |
| C3 | Client **terminate server** (`--shutdown`) | §5 | **Covered** |
| H1 | Duplicate bind rejected | §6 | **Covered** (ops hardening; not always on the brief) |
| H2 | SIGTERM/SIGINT stop without Shutdown opcode | §7 | **Weak** — stop without Shutdown yes; signal name not in DBG |

---

## Please re-run / attach if you want a clean checklist

1. **Shutdown broadcast:** `--live` + `--shutdown` so the live client log shows goodbye / `RECV Shutdown`.
2. **SIGTERM labeled:** `kill -TERM` (or Ctrl+C) + terminal one-liner tied to the matching `NetServer-*.log`.
3. Optional: paste terminal stdout for `NetClient --read` printing `lol` if the reviewer wants human-visible output beyond DBG.
4. Optional: if the brief insists on **kill -9**, capture Set → kill -9 → restart → Read (DBG already supports the softer “process stop” story).
