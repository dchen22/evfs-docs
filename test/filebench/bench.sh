#!/bin/bash
#
# Filebench webserver macrobenchmark with and without background remapper.
#
# Usage: ./bench.sh
#
# Runs two back-to-back 60s filebench webserver runs on evfs-sandbox-20gb:
#   Run 1 (baseline)  – filebench alone
#   Run 2 (remapper)  – filebench + remapper spinning in the background
#
# Prints the IO Summary line from each run for direct comparison.

set -e

SCRIPT_DIR=$(dirname "$(realpath "$0")")
REPO_ROOT=$(realpath "$SCRIPT_DIR/../..")
BUILD_DIR="$REPO_ROOT/test/build"
MOUNTPT="$REPO_ROOT/evfs-sandbox-20gb"
WORKLOAD="$SCRIPT_DIR/webserver-bench.f"

POOL_DIR="$MOUNTPT/remap_pool"
POOL_SIZE=500      # number of same-size files in the pool
POOL_BLOCK_COUNT=16  # 16 x 1KB blocks = 16KB per file
REMAP_DURATION=70  # slightly longer than filebench run so it covers the full 60s

# Build remapper if needed
make -C "$REPO_ROOT/test" build/remapper.x 2>&1

# Create remap pool on the benchmark filesystem (skip if already exists)
if [ ! -d "$POOL_DIR" ]; then
	echo "=== Creating remap pool ($POOL_SIZE files x ${POOL_BLOCK_COUNT}KB) ==="
	mkdir -p "$POOL_DIR"
	for i in $(seq 1 $POOL_SIZE); do
		dd if=/dev/zero bs=1024 count=$POOL_BLOCK_COUNT 2>/dev/null \
			| tr '\0' '\252' > "$POOL_DIR/f$i"
	done
	sync
	echo "Pool created."
else
	echo "=== Remap pool already exists, reusing ==="
fi

# --------------------------------------------------------------------------
echo ""
echo "=== Run 1: baseline (filebench alone) ==="
BASELINE_OUT=$(sudo filebench -f "$WORKLOAD" 2>&1)
echo "$BASELINE_OUT" | tail -20
BASELINE_SUMMARY=$(echo "$BASELINE_OUT" | grep "IO Summary")
echo ""
echo "Baseline: $BASELINE_SUMMARY"

# --------------------------------------------------------------------------
echo ""
echo "=== Run 2: filebench + background remapper ==="

"$BUILD_DIR/remapper.x" "$MOUNTPT" "$POOL_DIR" $REMAP_DURATION &
REMAPPER_PID=$!

REMAP_OUT=$(sudo filebench -f "$WORKLOAD" 2>&1)
echo "$REMAP_OUT" | tail -20
REMAP_SUMMARY=$(echo "$REMAP_OUT" | grep "IO Summary")

wait $REMAPPER_PID
echo ""
echo "With remapper: $REMAP_SUMMARY"

# --------------------------------------------------------------------------
echo ""
echo "=== Summary ==="
echo "Baseline:      $BASELINE_SUMMARY"
echo "With remapper: $REMAP_SUMMARY"
