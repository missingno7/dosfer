#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
./run_host_tests.sh >/dev/null
BIN=.host_build/host_pipeline_selftest
count=0
for mode in PLANE3 PLANE4; do
    for window in 4 5 6 7 8 15 16 31 32 33 63 64 127 128; do
        for records in 4 5 6 7 8 9 10 11 12 15 16 17 31 32 33 34 35 36 63 64 65 66 67 91 127 128 129 130 131 150; do
            data=$((records - 4))
            if (( data == 0 )); then size=0; else size=$(((data - 1) * 2876 + 1)); fi
            "$BIN" "$mode" "$size" "$window" >/dev/null
            count=$((count + 1))
        done
    done
done
echo "DOSFER32 boundary matrix passed: $count cases."
