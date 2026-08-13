# NetProject

C++20 networked shared-message service (`0xBEEF` + JSON, SQLite WAL).

- Overview, wire, CLI: [doc/README.md](doc/README.md)
- Why these libraries: [doc/DECISIONS.md](doc/DECISIONS.md)
- Build, containers, run: [doc/HOWTO.txt](doc/HOWTO.txt)
- Manual run evidence: [EVIDENCE.md](EVIDENCE.md) (DBG logs under [doc/evidence/](doc/evidence/))

**Note:** `.gitea/` and the `ci-*` CMake presets are leftover from a previous project template and are **not** verified for this NetProject. `Dockerfile.*` remain for optional container builds (see [doc/HOWTO.txt](doc/HOWTO.txt)). Prefer local `cmake --preset` + `ctest`.
