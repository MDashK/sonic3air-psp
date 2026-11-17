#!/bin/bash
# Benchmarks a few acts on the PSP through PSPLink: FPS, drawing CPU time, script time, and the heaviest script functions
#   ./bench_levels.sh [zone_act...]      (hex, default: 0000 0001 0100 0101 0200)
HOST_DIR="${HOST_DIR:-/mnt/e/_PSP/ppsspp_win/memstick/PSP/GAME/Sonic3AIR}"
FRAMES="${FRAMES:-1800}"
cd "$(dirname "$0")"
LEVELS="${@:-0000 0001 0100 0101 0200}"
for level in $LEVELS; do
	printf "S3AIR_BENCH=1\nS3AIR_BENCH_FRAMES=$FRAMES\nS3AIR_BENCH_INPUT=1\nS3AIR_BENCH_LEVEL=$level\nS3AIR_BENCH_LOG=bench.log\nS3AIR_BENCH_PROFILE=1\n" > "$HOST_DIR/bench.txt"
	rm -f "$HOST_DIR/render.log" "$HOST_DIR/bench.log"
	timeout 240 ./psplink_run.sh $((FRAMES / 30 + 25)) > /dev/null 2>&1
	echo "=== $level"
	tail -n +3 "$HOST_DIR/render.log" 2>/dev/null | awk '{for(i=1;i<=NF;i++){split($i,a,"="); v[a[1]]+=a[2]; if(a[1]=="fps" && (min==""||a[2]<min)) min=a[2]; if(a[1]=="render_cpu_ms" && a[2]>maxr) maxr=a[2]}; n++} END {printf "fps avg %.1f min %.1f | render_cpu avg %.1f max %.1f ms | ge_wait %.2f ms\n", v["fps"]/n, min, v["render_cpu_ms"]/n, maxr, v["ge_wait_ms"]/n}'
	grep SUMMARY "$HOST_DIR/bench.log" | grep -oE "avg_ms=[0-9.]+ p50=[0-9.]+ p95=[0-9.]+ p99=[0-9.]+"
	grep -A6 "profile:" "$HOST_DIR/bench.log" | tail -7
done
