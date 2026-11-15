#!/usr/bin/env bash
#
# BashMenuGenerator build script
#
#   ./build.sh            build for this machine (on 64-bit x86 it also tries the 32-bit build)
#   ./build.sh x64        64-bit x86 build  ->  BashMenuGenerator
#   ./build.sh x86        32-bit x86 build  ->  BashMenuGenerator_x86   (needs a 32-bit toolchain)
#   ./build.sh release    x64 + x86 + SHA256SUMS, ready to attach to a GitHub release
#   ./build.sh clean      delete everything this script builds
#
#   Optional environment variables:
#   CXX          compiler to use (default: g++)
#   EXTRA_FLAGS  extra compiler flags, for example EXTRA_FLAGS="-Wall -Wextra"

cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1

SOURCE="BashMenuGenerator.cpp"
CXX="${CXX:-g++}"

# Speed and size : -O2, unused code and symbols removed, stripped.
# Safety         : stack protector, fortified libc calls, read-only relocations.
# Portability    : no -march=native, so the binary runs on any CPU of its kind.
COMMON_FLAGS=(-std=c++14 -O2 -pipe -s
    -ffunction-sections -fdata-sections -Wl,--gc-sections
    -fstack-protector-strong -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2
    -Wl,-z,relro -Wl,-z,now)

# Linking is tried from the most portable mode down. A fully static binary needs no libraries on the
# target machine at all, so it runs on any Linux distribution, old or new.
LINK_MODES=("-static-pie" "-static" "-static-libgcc -static-libstdc++")
LINK_NAMES=("fully static (static-pie)" "fully static" "dynamic glibc (only libgcc and libstdc++ are static)")

LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

box()  { echo "========================================"; }
ok()   { box; echo "  [SUCCESS] $1"; shift; for line in "$@"; do echo "  $line"; done; box; }
fail() { box; echo "  [ERROR] $*"; box; }
note() { echo "  [NOTE] $*"; }

usage() {
    sed -n '3,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

human_size() {
    awk -v s="$1" 'BEGIN { if (s >= 1048576) printf "%.1f MB", s / 1048576; else printf "%.0f KB", s / 1024 }'
}

# Prints the oldest Linux kernel the binary says it needs (from its ELF note), if readelf is available.
min_kernel() {
    command -v readelf >/dev/null 2>&1 || return 0
    readelf -n "$1" 2>/dev/null | sed -n 's/.*ABI: \(.*\)$/\1/p' | head -n 1
}

# build_one OUTPUT [ARCH_FLAGS...]
build_one() {
    local out="$1"; shift
    local i built=""
    for i in 0 1 2; do
        # shellcheck disable=SC2086  (the link mode and EXTRA_FLAGS are meant to be split into words)
        if "$CXX" "${COMMON_FLAGS[@]}" "$@" $EXTRA_FLAGS ${LINK_MODES[$i]} -o "$out" "$SOURCE" >"$LOG" 2>&1; then
            built="${LINK_NAMES[$i]}"
            break
        fi
        rm -f "$out"
    done

    if [ -z "$built" ]; then
        fail "Build of $out failed! Compiler output:"
        tail -n 25 "$LOG"
        return 1
    fi

    local details=("Link mode  : $built" "Size       : $(human_size "$(wc -c < "$out" | tr -d ' ')")")
    local kernel
    kernel="$(min_kernel "$out")"
    [ -n "$kernel" ] && details+=("Min kernel : $kernel")
    if "./$out" -h >/dev/null 2>&1; then
        details+=("Smoke test : passed")
    else
        details+=("Smoke test : could not run it on this machine")
    fi
    ok "$out built!" "${details[@]}"
    if [ "$i" -eq 2 ]; then
        note "The static libraries are missing, so this binary needs the system's glibc at run time."
        note "For a fully static one install them: Debian/Ubuntu have them in libc6-dev, Fedora needs glibc-static and libstdc++-static."
    fi
    return 0
}

have_32bit_toolchain() {
    local tmp rc
    tmp="$(mktemp)"
    "$CXX" -m32 -x c++ -o "$tmp" - <<<'int main() { return 0; }' >/dev/null 2>&1
    rc=$?
    rm -f "$tmp"
    return $rc
}

show_32bit_hint() {
    fail "No 32-bit (x86) C++ toolchain found, so BashMenuGenerator_x86 cannot be built here."
    echo "  Install it first, for example:"
    echo "    Debian / Ubuntu :  sudo apt install g++-multilib"
    echo "    Fedora          :  sudo dnf install glibc-devel.i686 libstdc++-devel.i686"
    echo "  Other distributions: install the 32-bit (multilib) glibc and libstdc++ development packages,"
    echo "  or run this script on a 32-bit machine."
}

build_x64() {
    if [ "$HOST" != "x64" ]; then
        fail "The x64 build needs a 64-bit x86 machine (this one is $(uname -m))."
        return 1
    fi
    echo "Compiling BashMenuGenerator (x64)..."
    build_one BashMenuGenerator -m64
}

build_x86() {
    echo "Compiling BashMenuGenerator_x86 (32-bit)..."
    case "$HOST" in
        x64) have_32bit_toolchain || { show_32bit_hint; return 1; } ;;
        x86) ;;
        *)   fail "The x86 build needs an x86 or x86-64 machine (this one is $(uname -m))."; return 1 ;;
    esac
    build_one BashMenuGenerator_x86 -m32 -march=i686 -mtune=generic -D_FILE_OFFSET_BITS=64
}

write_checksums() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$@" > SHA256SUMS
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$@" > SHA256SUMS
    else
        note "sha256sum was not found, SHA256SUMS was not written."
        return 1
    fi
    echo "  Wrote SHA256SUMS:"
    sed 's/^/    /' SHA256SUMS
}

MODE="${1:-auto}"
case "$MODE" in
    help|-h|--help) usage; exit 0 ;;
    clean)
        rm -f BashMenuGenerator BashMenuGenerator_x86 SHA256SUMS
        echo "Removed BashMenuGenerator, BashMenuGenerator_x86 and SHA256SUMS."
        exit 0 ;;
    auto|x64|x86|release) ;;
    *) fail "Unknown option: $MODE"; usage; exit 2 ;;
esac

if [ ! -f "$SOURCE" ]; then
    fail "$SOURCE was not found next to build.sh."
    exit 1
fi
if ! command -v "$CXX" >/dev/null 2>&1; then
    fail "The C++ compiler '$CXX' was not found. Install g++ (Debian/Ubuntu: sudo apt install g++) or set CXX."
    exit 1
fi

case "$(uname -m)" in
    x86_64|amd64) HOST="x64" ;;
    i?86)         HOST="x86" ;;
    *)            HOST="other" ;;
esac

STATUS=0
case "$MODE" in
    x64) build_x64 || STATUS=1 ;;
    x86) build_x86 || STATUS=1 ;;
    release)
        if [ "$HOST" != "x64" ]; then
            fail "Release builds (x64 + x86) need a 64-bit x86 machine with a 32-bit toolchain."
            exit 1
        fi
        build_x64 || STATUS=1
        build_x86 || STATUS=1
        if [ "$STATUS" -eq 0 ]; then
            write_checksums BashMenuGenerator BashMenuGenerator_x86 || STATUS=1
        else
            note "SHA256SUMS was not written because a build failed."
        fi ;;
    auto)
        if [ "$HOST" = "x64" ]; then
            build_x64 || STATUS=1
            if have_32bit_toolchain; then
                build_x86 || STATUS=1
            else
                note "No 32-bit toolchain installed: skipped BashMenuGenerator_x86 (run './build.sh x86' for details)."
            fi
        else
            echo "Compiling BashMenuGenerator (Linux native)..."
            build_one BashMenuGenerator || STATUS=1
        fi ;;
esac
exit "$STATUS"
