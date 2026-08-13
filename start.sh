#!/bin/bash

# Prepare the tree (Linux / WSL). Pair with clean.sh to wipe generated folders.
#
#   source ./start.sh   # prepare env in THIS shell (no +x needed — use this on CIFS/Samba)
#   bash ./start.sh     # same prep (persists via ~/.bashrc) + open Linux VSCodium if present
#   ./start.sh          # same as bash ./start.sh — needs +x
#
# Then: cmake -B build -S . && cmake --build build
# Wipe: source ./clean.sh
# Do NOT use sudo. That installs vcpkg into /root and writes /root/.bashrc.

# Keep in sync with cmake_minimum_required in CMakeLists.txt
CMAKE_MIN=3.22

_START_SOURCED=0
if [ "${BASH_SOURCE[0]}" != "$0" ]; then
    _START_SOURCED=1
fi

_start_cleanup() {
    popd > /dev/null 2>&1 || true
}

# Native Linux VSCodium only — ignore Windows PATH stubs (/mnt/c/.../codium, *.exe)
find_linux_codium() {
    local candidate path
    for candidate in codium /usr/bin/codium /usr/local/bin/codium "$HOME/.local/bin/codium"; do
        path="$(command -v "$candidate" 2>/dev/null || true)"
        if [ -z "$path" ] && [ -x "$candidate" ]; then
            path="$candidate"
        fi
        [ -n "$path" ] || continue
        case "$path" in
            /mnt/*|*.exe|*.EXE) continue ;;
        esac
        path="$(readlink -f "$path" 2>/dev/null || echo "$path")"
        case "$path" in
            /mnt/*|*.exe|*.EXE) continue ;;
        esac
        if [ -x "$path" ]; then
            printf '%s\n' "$path"
            return 0
        fi
    done
    return 1
}

# $1 >= $2  (dotted versions; no GNU sort -V — Alpine busybox has none)
_ver_ge() {
    local a_raw b_raw ai bi i
    a_raw="${1%%-*}"
    a_raw="${a_raw%%+*}"
    b_raw="${2%%-*}"
    b_raw="${b_raw%%+*}"
    local IFS=.
    # shellcheck disable=SC2206
    local a=($a_raw) b=($b_raw)
    for i in 0 1 2; do
        ai="${a[$i]:-0}"
        bi="${b[$i]:-0}"
        ai="${ai%%[!0-9]*}"
        bi="${bi%%[!0-9]*}"
        [ -n "$ai" ] || ai=0
        [ -n "$bi" ] || bi=0
        if [ "$ai" -gt "$bi" ]; then return 0; fi
        if [ "$ai" -lt "$bi" ]; then return 1; fi
    done
    return 0
}

# Set _OS_ID / _PKG_FAMILY from os-release (do not source — this script is often sourced).
_os_release_field() {
    local key="$1" line val
    [ -r /etc/os-release ] || return 0
    line="$(grep -E "^${key}=" /etc/os-release 2>/dev/null | head -n1)" || true
    val="${line#*=}"
    val="${val#\"}"
    val="${val%\"}"
    val="${val#\'}"
    val="${val%\'}"
    printf '%s\n' "$val"
}

_detect_distro() {
    _OS_ID="$(_os_release_field ID)"
    _OS_LIKE="$(_os_release_field ID_LIKE)"
    [ -n "$_OS_ID" ] || _OS_ID=unknown
    _PKG_FAMILY=unknown
    case "$_OS_ID" in
        debian|ubuntu|linuxmint|pop|raspbian|elementary|kali|neon) _PKG_FAMILY=apt ;;
        fedora|rhel|centos|rocky|almalinux|ol|nobara|amzn) _PKG_FAMILY=dnf ;;
        opensuse*|sles|sled|suse*) _PKG_FAMILY=zypper ;;
        arch|manjaro|endeavouros|garuda|artix|cachyos) _PKG_FAMILY=pacman ;;
        alpine) _PKG_FAMILY=apk ;;
        void) _PKG_FAMILY=xbps ;;
        gentoo) _PKG_FAMILY=emerge ;;
    esac
    if [ "$_PKG_FAMILY" = unknown ]; then
        case " $_OS_LIKE " in
            *"debian"*|*"ubuntu"*) _PKG_FAMILY=apt ;;
            *"rhel"*|*"fedora"*|*"centos"*) _PKG_FAMILY=dnf ;;
            *"suse"*) _PKG_FAMILY=zypper ;;
            *"arch"*) _PKG_FAMILY=pacman ;;
        esac
    fi
    if [ "$_PKG_FAMILY" = unknown ]; then
        if command -v apt-get > /dev/null 2>&1; then _PKG_FAMILY=apt
        elif command -v dnf > /dev/null 2>&1; then _PKG_FAMILY=dnf
        elif command -v yum > /dev/null 2>&1; then _PKG_FAMILY=yum
        elif command -v zypper > /dev/null 2>&1; then _PKG_FAMILY=zypper
        elif command -v pacman > /dev/null 2>&1; then _PKG_FAMILY=pacman
        elif command -v apk > /dev/null 2>&1; then _PKG_FAMILY=apk
        elif command -v xbps-install > /dev/null 2>&1; then _PKG_FAMILY=xbps
        elif command -v emerge > /dev/null 2>&1; then _PKG_FAMILY=emerge
        fi
    fi
    # CentOS/RHEL 7: dnf family but only yum is present
    if [ "$_PKG_FAMILY" = dnf ] && ! command -v dnf > /dev/null 2>&1 \
       && command -v yum > /dev/null 2>&1; then
        _PKG_FAMILY=yum
    fi
}

_print_install_hint() {
    echo "[ERROR] Install with (detected: ${_OS_ID} / ${_PKG_FAMILY}):"
    case "$_PKG_FAMILY" in
        apt)
            echo "       sudo apt-get update"
            echo "       sudo apt-get install -y build-essential cmake ninja-build pkg-config curl zip unzip tar git"
            ;;
        dnf)
            echo "       sudo dnf install -y gcc-c++ cmake ninja-build pkgconf-pkg-config curl zip unzip tar git"
            ;;
        yum)
            echo "       sudo yum install -y gcc-c++ cmake ninja-build pkgconfig curl zip unzip tar git"
            ;;
        zypper)
            echo "       sudo zypper install -y gcc-c++ cmake ninja pkg-config curl zip unzip tar git"
            ;;
        pacman)
            echo "       sudo pacman -Syu --needed base-devel cmake ninja pkgconf curl zip unzip tar git"
            ;;
        apk)
            echo "       sudo apk add build-base cmake ninja pkgconf curl zip unzip tar git"
            echo "       export VCPKG_FORCE_SYSTEM_BINARIES=1   # musl: vcpkg glibc binaries will not run"
            ;;
        xbps)
            echo "       sudo xbps-install -S gcc cmake ninja pkg-config curl zip unzip tar git"
            ;;
        emerge)
            echo "       sudo emerge --ask sys-devel/gcc dev-build/cmake dev-build/ninja \\"
            echo "           dev-util/pkgconf net-misc/curl app-arch/zip app-arch/unzip app-arch/tar dev-vcs/git"
            ;;
        *)
            echo "       Need binaries: git cmake g++ (or clang++) ninja pkg-config curl zip unzip tar"
            echo "       Install the matching packages with your distro's package manager."
            ;;
    esac
}

# Abort the *script* (works when sourced or executed). Must run at top level,
# not inside a helper — `return` from a function only leaves that function.
_start_abort() {
    _start_cleanup
    unset -f _start_cleanup _start_abort find_linux_codium \
             _ver_ge _os_release_field _detect_distro _print_install_hint 2>/dev/null || true
}

if [ "$(id -u)" -eq 0 ]; then
    echo "[ERROR] Refusing to run as root. sudo ./start.sh installs vcpkg into /root/vcpkg."
    echo "       Use:  source ./start.sh    or    bash ./start.sh"
    _start_abort
    return 1 2>/dev/null || exit 1
fi

_detect_distro

pushd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null || exit 1

# Path to vcpkg installation
VCPKG_DEST="$HOME/vcpkg"
# Global binary cache to avoid recompiling dependencies for every build folder
VCPKG_CACHE="$HOME/.vcpkg/archives"

echo "[INFO] Preflight on ${_OS_ID} (${_PKG_FAMILY}) — need: git cmake>=${CMAKE_MIN} g++|clang++ ninja pkg-config curl zip unzip tar"

MISSING_TOOLS=()
for tool in git cmake ninja curl zip unzip tar; do
    if ! command -v "$tool" > /dev/null 2>&1; then
        MISSING_TOOLS+=("$tool")
    fi
done
if ! command -v g++ > /dev/null 2>&1 && ! command -v clang++ > /dev/null 2>&1; then
    MISSING_TOOLS+=("g++/clang++")
fi
if ! command -v pkg-config > /dev/null 2>&1 && ! command -v pkgconf > /dev/null 2>&1; then
    MISSING_TOOLS+=("pkg-config")
fi
if [ "${#MISSING_TOOLS[@]}" -gt 0 ]; then
    echo "[ERROR] Missing tools: ${MISSING_TOOLS[*]}"
    _print_install_hint
    echo "[ERROR] Then re-run as your user (not sudo):  source ./start.sh"
    _start_abort
    return 1 2>/dev/null || exit 1
fi

_cmake_ver="$(cmake --version 2>/dev/null | awk 'NR==1 { print $3 }')"
if ! _ver_ge "${_cmake_ver:-0}" "$CMAKE_MIN"; then
    echo "[ERROR] CMake ${_cmake_ver:-unknown} is too old (need >= ${CMAKE_MIN})."
    _print_install_hint
    _start_abort
    return 1 2>/dev/null || exit 1
fi
if command -v g++ > /dev/null 2>&1; then
    echo "[INFO] Preflight OK (cmake ${_cmake_ver}, g++ $(g++ -dumpversion 2>/dev/null))"
else
    echo "[INFO] Preflight OK (cmake ${_cmake_ver}, clang++ $(clang++ -dumpversion 2>/dev/null))"
    export CXX="${CXX:-clang++}"
    export CC="${CC:-clang}"
fi
unset _cmake_ver

# musl toolchains cannot run vcpkg's glibc bootstrap binary
if [ "$_PKG_FAMILY" = apk ]; then
    export VCPKG_FORCE_SYSTEM_BINARIES=1
fi

# Check path for 'vcpkg' executable
if [ ! -f "$VCPKG_DEST/vcpkg" ]; then
    echo "[INFO] vcpkg not found. Starting installation in $VCPKG_DEST..."

    if [ ! -d "$VCPKG_DEST" ]; then
        echo "[INFO] Cloning vcpkg repository..."
        if ! git clone https://github.com/microsoft/vcpkg.git "$VCPKG_DEST"; then
            echo "[ERROR] Failed to clone vcpkg repository. Check your internet connection."
            _start_abort
            return 1 2>/dev/null || exit 1
        fi
    fi

    echo "[INFO] Running bootstrap-vcpkg..."
    if ! "$VCPKG_DEST/bootstrap-vcpkg.sh" "-disableMetrics"; then
        echo "[ERROR] bootstrap-vcpkg failed."
        echo "[ERROR] Incomplete tree left at $VCPKG_DEST — fix tools, then re-run start.sh."
        echo "[ERROR] To start clean:  rm -rf \"$VCPKG_DEST\""
        _start_abort
        return 1 2>/dev/null || exit 1
    fi
fi

if [ ! -f "$VCPKG_DEST/scripts/buildsystems/vcpkg.cmake" ]; then
    echo "[ERROR] vcpkg toolchain missing: $VCPKG_DEST/scripts/buildsystems/vcpkg.cmake"
    _start_abort
    return 1 2>/dev/null || exit 1
fi

# Persist env for future shells
if [ -f "$HOME/.bashrc" ] && ! grep -q "VCPKG_ROOT" "$HOME/.bashrc"; then
    echo "[INFO] Writing VCPKG_ROOT to ~/.bashrc..."
    echo "export VCPKG_ROOT=\"$VCPKG_DEST\"" >> "$HOME/.bashrc"
    echo "export VCPKG_DISABLE_METRICS=1" >> "$HOME/.bashrc"
    echo "export CMAKE_GENERATOR=Ninja" >> "$HOME/.bashrc"
    if [ "$_PKG_FAMILY" = apk ]; then
        echo "export VCPKG_FORCE_SYSTEM_BINARIES=1" >> "$HOME/.bashrc"
    fi
fi

# Set env variables for 'this' session (effective when sourced)
export VCPKG_ROOT="$VCPKG_DEST"
export VCPKG_DISABLE_METRICS=1
export CMAKE_GENERATOR=Ninja

if [ ! -d "$VCPKG_CACHE" ]; then
    mkdir -p "$VCPKG_CACHE"
fi
export VCPKG_DEFAULT_BINARY_CACHE="$VCPKG_CACHE"

# Out-of-source build dir (keeps src/ clean). Override with BUILD_DIR=...
BUILD_DIR="${BUILD_DIR:-build}"

# Incomplete/failed manifest install → "uSockets NOT found"
_usockets_ok=0
for _root in "$BUILD_DIR" .; do
    if [ -f "$_root/vcpkg_installed/x64-linux/include/libusockets.h" ] \
       || [ -f "$_root/vcpkg_installed/arm64-linux/include/libusockets.h" ]; then
        _usockets_ok=1
        break
    fi
done
if [ "$_usockets_ok" -eq 0 ]; then
    if [ -f CMakeCache.txt ] || [ -d vcpkg_installed ] || [ -d "$BUILD_DIR" ]; then
        echo "[INFO] Clearing incomplete CMake/vcpkg state..."
        rm -f CMakeCache.txt cmake_install.cmake compile_commands.json \
              CTestTestfile.cmake DartConfiguration.tcl vcpkg-manifest-install.log \
              build.ninja rules.ninja .ninja_log .ninja_deps Makefile
        rm -rf CMakeFiles CMakeScripts Testing .cmake vcpkg_installed "$BUILD_DIR"
    fi
fi
unset _usockets_ok _root

echo "[INFO] VCPKG_ROOT is set to: $VCPKG_ROOT"
echo "[INFO] CMAKE_GENERATOR=$CMAKE_GENERATOR"
echo "[INFO] Binary dir: $BUILD_DIR/"
echo "[INFO] Vcpkg metrics are disabled."
echo "[INFO] Binary caching is enabled at: $VCPKG_CACHE"

CODIUM_BIN="$(find_linux_codium || true)"
if [ -n "$CODIUM_BIN" ]; then
    echo "[INFO] Linux VSCodium found ($CODIUM_BIN). Launching..."
    "$CODIUM_BIN" .
    echo "[INFO] Setup complete. IDE launched."
else
    echo "[INFO] Linux VSCodium not found — environment prepared only."
fi

echo "[INFO] Ready for out-of-source build:"
echo "       cmake -B $BUILD_DIR -S ."
echo "       cmake --build $BUILD_DIR"
echo "       ctest --test-dir $BUILD_DIR --output-on-failure"
echo "[INFO] Wipe generated folders:  source ./clean.sh"

_start_cleanup
unset -f _start_cleanup _start_abort find_linux_codium \
         _ver_ge _os_release_field _detect_distro _print_install_hint 2>/dev/null || true
unset _OS_ID _OS_LIKE _PKG_FAMILY
return 0 2>/dev/null || exit 0
