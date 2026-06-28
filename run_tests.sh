#!/usr/bin/env bash
# ===========================================================================
# run_tests.sh  -  Automated test driver for the Software Update Framework.
# ---------------------------------------------------------------------------
# Exercises the scenarios required by the project spec:
#   * Single client connection
#   * Multiple simultaneous clients
#   * Clients with outdated versions
#   * Clients already up to date
#   * Large file transfer (the 64KB+ package)
#   * Invalid client requests
#   * Concurrent downloads
#
# Run from the project root:
#     ./run_tests.sh
# ===========================================================================
set -u

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

# Choose which server to test:  ./run_tests.sh        -> base (threads)
#                               ./run_tests.sh adv    -> advanced (fork+IPC)
if [ "${1:-base}" = "adv" ]; then
    SRV="$ROOT/bin/server_adv"
    echo "### Testing ADVANCED server (fork + shared memory + semaphore + RT)"
else
    SRV="$ROOT/bin/server"
    echo "### Testing BASE server (thread-per-client)"
fi
CLI="$ROOT/bin/client"
SCONF="$ROOT/config/server.conf"
CCONF="$ROOT/config/client.conf"

PASS=0
FAIL=0
check() { # check "label" expected_rc actual_rc
    if [ "$2" -eq "$3" ]; then echo "  PASS: $1"; PASS=$((PASS+1));
    else echo "  FAIL: $1 (expected rc=$2, got rc=$3)"; FAIL=$((FAIL+1)); fi
}

echo "=== Building ==="
make --no-print-directory >/dev/null 2>&1 || { echo "build failed"; exit 1; }

# Fresh logs.
: > logs/server.log 2>/dev/null || true

echo "=== Starting server ==="
"$SRV" "$SCONF" &
SRV_PID=$!
sleep 1
if ! kill -0 "$SRV_PID" 2>/dev/null; then echo "server failed to start"; exit 1; fi
echo "  server pid=$SRV_PID"

# Client exit codes: 0 = up-to-date, 2 = updated, 1 = error.

echo "=== Test 1: single outdated client (expects update, rc=2) ==="
"$CLI" "$CCONF" 1 single_old >/dev/null 2>&1; check "outdated client downloads update" 2 $?

echo "=== Test 2: up-to-date client (version >= latest, rc=0) ==="
"$CLI" "$CCONF" 5 uptodate_cli >/dev/null 2>&1; check "up-to-date client gets no update" 0 $?

echo "=== Test 3: client ahead of latest (rc=0) ==="
"$CLI" "$CCONF" 9 ahead_cli >/dev/null 2>&1; check "client ahead is up to date" 0 $?

echo "=== Test 4: large file transfer integrity (downloaded == repo) ==="
"$CLI" "$CCONF" 1 xfer_cli >/dev/null 2>&1
if cmp -s server_repo/update_v5.bin client_storage/xfer_cli_update_v5.bin; then
    echo "  PASS: transferred file is byte-identical"; PASS=$((PASS+1))
else
    echo "  FAIL: transferred file differs"; FAIL=$((FAIL+1)); fi

echo "=== Test 5: invalid request (raw garbage, server must not crash) ==="
printf 'GARBAGE NONSENSE\n' | timeout 3 bash -c "cat > /dev/tcp/127.0.0.1/9090" 2>/dev/null
sleep 0.3
if kill -0 "$SRV_PID" 2>/dev/null; then
    echo "  PASS: server survived invalid request"; PASS=$((PASS+1))
else
    echo "  FAIL: server died on invalid request"; FAIL=$((FAIL+1)); fi

echo "=== Test 6: 20 concurrent clients (no blocking, all succeed) ==="
pids=()
for i in $(seq 1 20); do
    "$CLI" "$CCONF" 1 "conc_$i" >/dev/null 2>&1 &
    pids+=($!)
done
ok=0
for p in "${pids[@]}"; do wait "$p"; [ $? -eq 2 ] && ok=$((ok+1)); done
echo "  $ok / 20 concurrent clients downloaded successfully"
[ "$ok" -eq 20 ] && { echo "  PASS: all concurrent clients served"; PASS=$((PASS+1)); } \
                 || { echo "  FAIL: some concurrent clients failed"; FAIL=$((FAIL+1)); }

echo "=== Verifying concurrent downloads integrity ==="
bad=0
for i in $(seq 1 20); do
    cmp -s server_repo/update_v5.bin "client_storage/conc_${i}_update_v5.bin" || bad=$((bad+1))
done
[ "$bad" -eq 0 ] && { echo "  PASS: all 20 concurrent files intact"; PASS=$((PASS+1)); } \
                 || { echo "  FAIL: $bad concurrent files corrupt"; FAIL=$((FAIL+1)); }

echo "=== Shutting down server (graceful SIGINT) ==="
kill -INT "$SRV_PID" 2>/dev/null
wait "$SRV_PID" 2>/dev/null
echo "  server stopped"

echo
echo "==================== RESULTS ===================="
echo "  PASSED: $PASS"
echo "  FAILED: $FAIL"
echo "================================================="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
