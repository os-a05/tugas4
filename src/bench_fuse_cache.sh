#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'Usage: %s MEDIA_A_DIRECTORY [REPETITIONS]\n' "$0" >&2
    exit 2
}

[[ $# -ge 1 && $# -le 2 ]] || usage

media_dir=$1
repetitions=${2:-5}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fuse_program=${MINI_FUSE_PROGRAM:-"$script_dir/mini_fuse"}

[[ -d "$media_dir" && -w "$media_dir" ]] || {
    printf 'Media A directory must exist and be writable: %s\n' "$media_dir" >&2
    exit 1
}
[[ "$repetitions" =~ ^[1-9][0-9]*$ ]] || {
    printf 'Repetitions must be a positive integer\n' >&2
    exit 2
}
[[ -x "$fuse_program" ]] || {
    printf 'FUSE program not found or not executable: %s\n' "$fuse_program" >&2
    exit 1
}

media_dir=$(cd -- "$media_dir" && pwd)
result_dir=$(mktemp -d "$media_dir/fuse-cache-bench.XXXXXX")
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/fuse-cache-bench.XXXXXX")
mount_pid=
mount_path=
unmount_command=

cleanup() {
    if [[ -n "$mount_pid" ]]; then
        if [[ -n "$mount_path" ]] && mountpoint -q "$mount_path"; then
            unmount_mount 2>/dev/null || true
        fi
        kill "$mount_pid" 2>/dev/null || true
        wait "$mount_pid" 2>/dev/null || true
    fi
    rm -rf -- "$work_dir"
}
trap cleanup EXIT

if command -v fusermount3 >/dev/null 2>&1; then
    unmount_command=$(command -v fusermount3)
elif command -v fusermount >/dev/null 2>&1; then
    unmount_command=$(command -v fusermount)
else
    unmount_command=umount
fi

unmount_mount() {
    if [[ "$unmount_command" == "umount" ]]; then
        "$unmount_command" "$mount_path"
    else
        "$unmount_command" -u "$mount_path"
    fi
}

start_mount() {
    local mode=$1
    local log_file=$2
    mount_path="$work_dir/$mode"
    mkdir -p "$mount_path"
    "$fuse_program" --cache "$mode" "$mount_path" -f >"$log_file" 2>&1 &
    mount_pid=$!

    for _ in {1..100}; do
        if mountpoint -q "$mount_path"; then
            return 0
        fi
        if ! kill -0 "$mount_pid" 2>/dev/null; then
            printf 'FUSE mount failed; see %s\n' "$log_file" >&2
            return 1
        fi
        sleep 0.1
    done
    printf 'Timed out waiting for FUSE mount; see %s\n' "$log_file" >&2
    return 1
}

stop_mount() {
    unmount_mount
    wait "$mount_pid" || true
    mount_pid=
    mount_path=
}

run_reads() {
    local label=$1
    local file_path=$2
    local timing_file=$3
    local repetition
    local start_ns
    local end_ns

    : > "$timing_file"
    for ((repetition = 0; repetition < repetitions; repetition++)); do
        start_ns=$(date +%s%N)
        cat -- "$file_path" >/dev/null
        end_ns=$(date +%s%N)
        printf '%s\t%s\n' "$label" "$((end_ns - start_ns))" >> "$timing_file"
    done
}

print_wall_summary() {
    local label=$1
    local timing_file=$2
    awk -F '\t' -v label="$label" '
        { total += $2; count++ }
        END {
            if (count > 0) {
                printf "  %-22s wall avg: %.3f ms (%d reads)\n",
                       label, total / count / 1000000, count
            }
        }
    ' "$timing_file"
}

print_callback_summary() {
    local log_file=$1
    local file_path=$2
    local skip_lines=$3
    awk -v wanted="$file_path" -v skip_lines="$skip_lines" '
        NR <= skip_lines { next }
        $1 == "CALLBACK" && $2 == "read" {
            path = ""
            latency = ""
            for (i = 3; i <= NF; i++) {
                if ($i ~ /^path=/) {
                    path = substr($i, 6)
                } else if ($i ~ /^latency_ns=/) {
                    latency = substr($i, 12)
                }
            }
            if (path == wanted && latency ~ /^[0-9]+$/) {
                total += latency
                count++
            }
        }
        END {
            if (count > 0) {
                printf "  %-22s .read callbacks: %d, avg callback latency: %.0f ns\n",
                       wanted, count, total / count
            } else {
                printf "  %-22s .read callbacks: 0, avg callback latency: n/a\n",
                       wanted
            }
        }
    ' "$log_file"
}

benchmark_fuse_mode() {
    local mode=$1
    local log_file="$result_dir/$mode.log"
    local hello_timing="$result_dir/${mode}_hello.tsv"
    local random_timing="$result_dir/${mode}_random.tsv"
    local callback_baseline

    start_mount "$mode" "$log_file"
    if [[ ! -f "$result_dir/hello.txt" ]]; then
        cp -- "$mount_path/hello.txt" "$result_dir/hello.txt"
        cp -- "$mount_path/random.bin" "$result_dir/random.bin"
    fi

    callback_baseline=$(wc -l < "$log_file")
    run_reads "$mode hello.txt" "$mount_path/hello.txt" "$hello_timing"
    run_reads "$mode random.bin" "$mount_path/random.bin" "$random_timing"
    stop_mount

    printf '\n[%s]\n' "$mode"
    print_callback_summary "$log_file" /hello.txt "$callback_baseline"
    print_callback_summary "$log_file" /random.bin "$callback_baseline"
    print_wall_summary "$mode hello.txt" "$hello_timing"
    print_wall_summary "$mode random.bin" "$random_timing"
}

benchmark_plain_file() {
    local source_name=$1
    local timing_file="$result_dir/plain_${source_name}.tsv"

    run_reads "Media A $source_name" "$result_dir/$source_name" "$timing_file"
    print_wall_summary "Media A $source_name" "$timing_file"
}

printf 'Media A copies and callback logs: %s\n' "$result_dir"
benchmark_fuse_mode direct_io
benchmark_plain_file hello.txt
benchmark_plain_file random.bin
benchmark_fuse_mode kernel_cache
