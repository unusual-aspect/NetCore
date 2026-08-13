# NetProject

C++20 networked shared-message service (`0xBEEF` + JSON, SQLite WAL).

- Overview, wire, CLI, failure modes: [doc/README.md](doc/README.md)
- Why these libraries: [doc/DECISIONS.md](doc/DECISIONS.md)
- Build, containers, run: [doc/HOWTO.txt](doc/HOWTO.txt)
- Manual run evidence: [EVIDENCE.md](EVIDENCE.md) (DBG logs under [doc/evidence/](doc/evidence/))

CI: [`.github/workflows/ci.yml`](.github/workflows/ci.yml) (Linux debug/ASan/TSan and Windows debug; every gtest binary). `.gitea/` and the `ci-*` CMake presets are leftover template files and are **not** that pipeline. `Dockerfile.*` remain for optional container builds (see [doc/HOWTO.txt](doc/HOWTO.txt)).
