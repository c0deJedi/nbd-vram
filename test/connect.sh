#!/usr/bin/env bash
# Root-free regression tests for the startup connection lifecycle.
set -euo pipefail

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT

# Inject command stubs into the actual script. Block-device operations never run;
# the final swapon stub exits before the script writes its real /run marker.
cat > "$test_dir/mocks.bash" <<'MOCKS'
record() { printf '%s\n' "$*" >> "$TEST_TRACE"; }

[() {
    case "$1" in
        -f) [[ "$2" == /run/nbd-vram-dev && "$TEST_CASE" == cleanup-* ]] ;;
        -b) [[ "$2" == /dev/nbd0 ]] ;;
        *) builtin [ "$@" ;;
    esac
}

modprobe() { :; }
cat() {
    case "$1" in
        /run/nbd-vram-dev) printf '/dev/nbd0\n' ;;
        /sys/block/*/pid) return 1 ;;
        *) command cat "$@" ;;
    esac
}

swapon() {
    if [[ "${1:-}" == --show=NAME ]]; then
        case "$TEST_CASE" in
            cleanup-active-*) printf '/dev/nbd0\n' ;;
            cleanup-query-failure) return 2 ;;
        esac
    else
        record activate
        exit 0
    fi
}

swapoff() {
    record swapoff
    [[ "$TEST_CASE" != cleanup-active-failure ]]
}

checks=0
nbd-client() {
    case "$1" in
        -d) record disconnect ;;
        -c)
            record status
            checks=$((checks + 1))
            case "$TEST_CASE" in
                wait-release) if ((checks <= 2)); then return 0; fi ;;
                wait-timeout) return 0 ;;
                status-error) return 2 ;;
            esac
            return 1
            ;;
        -unix) record connect ;;
        *) printf 'Unexpected nbd-client invocation\n' >&2; exit 99 ;;
    esac
}

sleep() { record wait; }
mkswap() { record format; }
rm() { record remove; }
MOCKS

run_case() {
    local name=$1 expected=$2 status
    : > "$test_dir/trace"
    if TEST_CASE="$name" TEST_TRACE="$test_dir/trace" \
        BASH_ENV="$test_dir/mocks.bash" \
        bash "$repo_dir/nbd-vram-connect.sh" \
        > "$test_dir/stdout" 2> "$test_dir/stderr"; then
        status=0
    else
        status=$?
    fi
    if [[ "$status" != "$expected" ]]; then
        printf 'FAIL: %s returned %s, expected %s\n' "$name" "$status" "$expected" >&2
        cat "$test_dir/stderr" >&2
        exit 1
    fi
}

has_event() { grep -Fxq "$1" "$test_dir/trace"; }
no_event() {
    if has_event "$1"; then
        printf 'FAIL: unexpected %s event\n' "$1" >&2
        exit 1
    fi
}

run_case cleanup-active-failure 1
has_event swapoff
no_event disconnect
no_event remove
no_event connect
grep -q 'preserving device and state' "$test_dir/stderr"
printf 'PASS: failed swapoff preserves the device and marker\n'

run_case cleanup-query-failure 2
no_event disconnect
no_event remove
no_event connect
printf 'PASS: swap status query failure aborts cleanup\n'

run_case cleanup-active-success 0
[[ "$(head -n 1 "$test_dir/trace")" == swapoff ]]
has_event remove
has_event activate
printf 'PASS: successful swapoff permits cleanup and activation\n'

run_case cleanup-inactive 0
no_event swapoff
has_event remove
has_event activate
printf 'PASS: inactive stale devices remain cleanable\n'

run_case wait-release 0
[[ "$(grep -Fxc wait "$test_dir/trace")" == 2 ]]
has_event activate
printf 'PASS: reconnect waits for the old configuration to release\n'

run_case wait-timeout 1
[[ "$(grep -Fxc status "$test_dir/trace")" == 200 ]]
no_event connect
no_event format
grep -q 'timed out' "$test_dir/stderr"
printf 'PASS: disconnect timeout prevents connection and formatting\n'

run_case status-error 1
no_event connect
no_event format
grep -q 'cannot check disconnect status' "$test_dir/stderr"
printf 'PASS: unknown disconnect status prevents connection and formatting\n'
