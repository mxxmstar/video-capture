#!/usr/bin/env bash
# Interactive frontend; all compilation is delegated to CMake.
set -euo pipefail
PROJECT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$PROJECT_ROOT"

usage() {
    cat <<'EOF'
Usage: bash scripts/build-menu.sh [options]
  --platform generic|rockchip   Non-interactive platform selection
  --type Release|Debug         Build type (default: Release)
  --action build|configure|clean  Default: build (configure, then build)
  --config FILE                Trusted Bash configuration file
  --dry-run                    Print commands without executing them
  --list                       Show menu and exit
  -h, --help                   Show this help
Environment: JOBS=4 (default), EMBEDDED_BUILD_CONFIG=/path/to/local.sh
EOF
}
menu() {
    cat <<'EOF'
========== embedded-device build ==========
1  : generic  Release (native Linux)
2  : generic  Debug
3  : rockchip Release (NanoPC-T4 ARM64)
4  : rockchip Debug
c1/c2/c3/c4 : clean the corresponding build
h : show menu    q : quit
Rockchip currently builds media contracts/core, not a full media backend.
EOF
}
fail() { printf 'Error: %s\n' "$*" >&2; exit 1; }
platform= mode=Release action=build dry_run=0 list_only=0
config=${EMBEDDED_BUILD_CONFIG:-"$PROJECT_ROOT/config/build.local.sh"}
explicit_config=0
[[ -z ${EMBEDDED_BUILD_CONFIG:-} ]] || explicit_config=1
while (($#)); do
    case "$1" in
        --platform|--type|--action|--config)
            (($# >= 2)) || fail "Missing value for $1"
            case "$1" in
                --platform) platform=$2 ;;
                --type) mode=$2 ;;
                --action) action=$2 ;;
                --config) config=$2; explicit_config=1 ;;
            esac
            shift 2 ;;
        --dry-run) dry_run=1; shift ;;
        --list) list_only=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) fail "Unknown option: $1" ;;
    esac
done
if ((list_only)); then menu; exit 0; fi
case "$mode" in Release|Debug) ;; *) fail "Unsupported build type: $mode" ;; esac
case "$action" in build|configure|clean) ;; *) fail "Unsupported action: $action" ;; esac
if [[ -z $platform ]]; then
    menu
    while true; do
        if ! read -r -p 'Select: ' choice; then exit 0; fi
        case "$choice" in
            q|Q) exit 0 ;;
            h|H) menu; continue ;;
            1|2|3|4|c1|c2|c3|c4)
                action=build
                [[ $choice != c* ]] || action=clean
                case "${choice#c}" in
                    1) platform=generic; mode=Release ;;
                    2) platform=generic; mode=Debug ;;
                    3) platform=rockchip; mode=Release ;;
                    4) platform=rockchip; mode=Debug ;;
                esac
                break ;;
            *) printf 'Invalid selection; enter 1-4, c1-c4, h or q.\n' ;;
        esac
    done
fi
case "$platform" in generic|rockchip) ;; *) fail "Unsupported platform: $platform" ;; esac

# Arrays preserve spaces in dependency paths. Local files are trusted shell code.
GENERIC_CMAKE_ARGS=()
ROCKCHIP_CMAKE_ARGS=()
COMMON_CMAKE_ARGS=()
BUILD_ROOT="$PROJECT_ROOT/build/menu"
JOBS=${JOBS:-4}
if [[ -f $config ]]; then
    source "$config"
elif ((explicit_config)); then
    fail "Configuration file not found: $config"
else
    fail "Copy config/build.example.sh to config/build.local.sh and set dependency paths first."
fi
[[ $JOBS =~ ^[1-9][0-9]*$ ]] || fail 'JOBS must be a positive integer'
[[ $BUILD_ROOT == /* ]] || BUILD_ROOT="$PROJECT_ROOT/$BUILD_ROOT"
build_dir="$BUILD_ROOT/$platform/$mode"
printf 'Platform: %s | Type: %s | Action: %s\nConfig: %s\nBuild: %s\n' \
    "$platform" "$mode" "$action" "$config" "$build_dir"
run() {
    printf '+'; printf ' %q' "$@"; printf '\n'
    if ((!dry_run)); then "$@"; fi
}
if [[ $action == clean ]]; then
    if ((!dry_run)) && [[ ! -f $build_dir/CMakeCache.txt ]]; then
        fail "No configured build at $build_dir"
    fi
    run cmake --build "$build_dir" --target clean
    exit 0
fi
args=("${COMMON_CMAKE_ARGS[@]}")
case "$platform" in
    generic) args+=("${GENERIC_CMAKE_ARGS[@]}") ;;
    rockchip) args+=("${ROCKCHIP_CMAKE_ARGS[@]}") ;;
esac
run cmake -S "$PROJECT_ROOT" -B "$build_dir" "${args[@]}" \
    "-DEMBEDDED_PLATFORM=$platform" "-DCMAKE_BUILD_TYPE=$mode"
if [[ $action == build ]]; then
    run cmake --build "$build_dir" --parallel "$JOBS"
fi
