#!/bin/bash

# Clean up generated folders (Linux / WSL). Pair with start.sh to prepare.
#
#   source ./clean.sh   # wipe build trees (no +x needed — use this on CIFS/Samba)
#   bash ./clean.sh     # same, in a subshell
#   ./clean.sh          # needs +x — do NOT use sudo
#
# Does NOT touch source, CMakeLists, presets, vcpkg.json, ~/vcpkg, or doc/evidence/.
# build/ and bin/ are removed recursively (rm -rf).
# Runtime logs and message.db* are moved into doc/evidence/run-<UTC>/.

_CLEAN_SOURCED=0
if [ "${BASH_SOURCE[0]}" != "$0" ]; then
    _CLEAN_SOURCED=1
fi

_clean_cleanup() {
    popd > /dev/null 2>&1 || true
    unset -f _clean_cleanup rm_dir rm_file preserve_evidence 2>/dev/null || true
}

if [ "$(id -u)" -eq 0 ]; then
    echo "[ERROR] Refusing to run as root. sudo ./clean.sh would delete as root."
    echo "       Use:  source ./clean.sh    or    bash ./clean.sh"
    unset -f _clean_cleanup 2>/dev/null || true
    return 1 2>/dev/null || exit 1
fi

# $0 is the shell when sourced — always use BASH_SOURCE
pushd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null || {
    return 1 2>/dev/null || exit 1
}

echo "[INFO] Cleaning CMake / IDE / runtime leftovers in $(pwd)"

rm_dir() {
    if [ -d "$1" ]; then
        echo "[INFO] Removing $1/ (recursive)"
        if ! rm -rf "$1" 2>/dev/null; then
            echo "[WARN] Could not fully remove $1/ — file in use? Close IDE and re-run."
        fi
    fi
}

rm_file() {
    if [ -e "$1" ]; then
        echo "[INFO] Removing $1"
        rm -f "$1"
    fi
}

# Salvage DBG logs + SQLite DB into doc/evidence; wipe ephemeral copies after.
preserve_evidence() {
    local stamp src dest name
    stamp="$(date -u +%Y%m%d-%H%M%S)"
    dest="doc/evidence/run-${stamp}"
    local moved=0

    mkdir -p doc/evidence

    # Nested / alternate cwd logs about to disappear with build/bin wipe
    for src in \
        bin/logs \
        build/logs \
        build/bin/logs \
        out/logs \
        out/build/*/bin/logs \
        log
    do
        if [ -d "$src" ] && [ -n "$(ls -A "$src" 2>/dev/null || true)" ]; then
            mkdir -p "$dest"
            echo "[INFO] Preserving logs from $src/ -> $dest/"
            cp -a "$src"/. "$dest/" 2>/dev/null || true
            moved=1
        fi
    done

    # Top-level runtime logs/ (files + any old archived-* dirs)
    if [ -d logs ]; then
        shopt -s nullglob
        for src in logs/*; do
            name="$(basename "$src")"
            if [ -e "$src" ]; then
                mkdir -p "$dest"
                echo "[INFO] Moving logs/$name -> $dest/"
                # Flatten old archived-* batch into this run folder
                if [ -d "$src" ] && [[ "$name" == archived-* ]]; then
                    cp -a "$src"/. "$dest/" 2>/dev/null || true
                    rm -rf "$src"
                else
                    mv "$src" "$dest/"
                fi
                moved=1
            fi
        done
        shopt -u nullglob
    fi

    # SQLite message store (+ WAL/SHM/journal) — keep with evidence, not delete
    shopt -s nullglob
    for src in message.db message.db-wal message.db-shm message.db-journal \
               build/message.db build/bin/message.db bin/message.db \
               build/message.db-wal build/bin/message.db-wal bin/message.db-wal \
               build/message.db-shm build/bin/message.db-shm bin/message.db-shm
    do
        if [ -f "$src" ]; then
            mkdir -p "$dest"
            name="$(basename "$src")"
            # Avoid clobber if same basename appears from multiple trees
            if [ -e "$dest/$name" ]; then
                name="$(echo "$src" | tr '/\\' '__')"
            fi
            echo "[INFO] Moving $src -> $dest/$name"
            mv "$src" "$dest/$name"
            moved=1
        fi
    done
    shopt -u nullglob

    if [ "$moved" -eq 1 ]; then
        echo "[INFO] Evidence (logs/DB) saved under $dest/"
    fi

    # Runtime logs/ is ephemeral — remove so the next run starts clean
    rm_dir logs
}

preserve_evidence

# Preset / IDE / container trees (full recursive delete)
rm_dir out
rm_dir build
rm_dir bin
rm_dir cmake-build-debug
rm_dir cmake-build-release
rm_dir .vs
rm_dir .cache
rm_dir .idea
rm_dir ipch
rm_dir ci
# logs/ + message.db* already handled by preserve_evidence → doc/evidence

# In-tree `cmake .` state
rm_dir CMakeFiles
rm_dir CMakeScripts
rm_dir Testing
rm_dir .cmake
rm_dir vcpkg_installed

shopt -s nullglob
for dir in build-*; do
    if [ -d "$dir" ]; then
        echo "[INFO] Removing $dir/ (recursive)"
        rm -rf "$dir"
    fi
done
shopt -u nullglob

rm_file CMakeCache.txt
rm_file cmake_install.cmake
rm_file CTestTestfile.cmake
rm_file CTestCustom.cmake
rm_file DartConfiguration.tcl
rm_file compile_commands.json
rm_file CMakeUserPresets.json
rm_file CMakeSettings.json
rm_file build.ninja
rm_file rules.ninja
rm_file .ninja_log
rm_file .ninja_deps
rm_file Makefile
rm_file vcpkg-manifest-install.log
# message.db* moved by preserve_evidence (not deleted)
rm_file ci-summary.json
rm_file cppcheck_report.txt
rm_file lizard_result.txt
rm_file test_output.txt

rm -f ./*.gcda ./*.gcno ./*.gcov 2>/dev/null || true

# In-source configure pollution under apps/ src/ tests/ (cmake . or bad -B).
# Absolute paths end up baked into cmake_install.cmake / CMakeFiles — wipe them.
echo "[INFO] Removing nested CMakeFiles / cmake_install under apps src tests"
while IFS= read -r -d '' dir; do
    echo "[INFO] Removing $dir/"
    rm -rf "$dir" 2>/dev/null || echo "[WARN] Could not remove $dir/"
done < <(find apps src tests \( -type d -name CMakeFiles -o -type d -name CMakeScripts -o -type d -name Testing \) -print0 2>/dev/null)

find apps src tests -type f \( \
    -name 'cmake_install.cmake' -o \
    -name 'CTestTestfile.cmake' -o \
    -name 'CTestCustom.cmake' -o \
    -name 'CMakeCache.txt' -o \
    -name 'Makefile' -o \
    -name 'build.ninja' -o \
    -name 'rules.ninja' -o \
    -name '.ninja_log' -o \
    -name '.ninja_deps' -o \
    -name 'compile_commands.json' \
\) -print -delete 2>/dev/null || true

# Stray objects/libs from an old in-source `cmake .`
find src apps tests -type f \( -name '*.a' -o -name '*.o' -o -name '*.obj' -o -name '*.lib' -o -name '*.pdb' -o -name '*.ilk' \) -delete 2>/dev/null || true

echo "[INFO] Clean done. Source, CMakeLists, presets, vcpkg.json, and doc/evidence/ were left alone."
echo "[INFO] Salvaged logs/DB (if any) are under doc/evidence/run-<UTC-timestamp>/."
echo "[INFO] ~/vcpkg was not touched."
echo "[INFO] Prepare:  source ./start.sh"
echo "[INFO] Rebuild:  cmake -B build -S . && cmake --build build"
echo "[INFO] Test:     ctest --test-dir build --output-on-failure"

_clean_cleanup
unset _CLEAN_SOURCED
return 0 2>/dev/null || exit 0
