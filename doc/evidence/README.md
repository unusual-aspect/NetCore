# Evidence run artifacts

Clean scripts (`clean.sh` / `clean.cmd`) move into `run-<UTC>/`:

- runtime DBG files from `logs/` (and nested build `logs/`)
- SQLite store `message.db` (+ `-wal` / `-shm` / `-journal` if present)

So manual-run proof stays with `doc/` and root `EVIDENCE.md`.

Ephemeral `logs/` and cwd `message.db*` are removed after the move; the next
`NetServer` / `NetClient` run recreates them.
