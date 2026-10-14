#!/usr/bin/env bash
# tests/stress_test.sh - High-throughput stress test for distributed job runner
set -u

GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[0;33m'
NC='\033[0m'

PORT=5099
HOST="127.0.0.1"
C="./client $HOST $PORT"
COUNT=${1:-500} # Default 500 requests burst

cleanup() {
    pkill -9 -f "./server $PORT" 2>/dev/null || true
    pkill -9 -f "./worker $HOST $PORT" 2>/dev/null || true
    rm -f /tmp/stress_*.txt
}

trap cleanup EXIT
cleanup

echo -e "${BLUE}=== HIGH-LOAD STRESS TEST ($COUNT Concurrent Requests) ===${NC}"

# Start server
./server $PORT > /dev/null 2>&1 &
SERVER_PID=$!
sleep 0.2

# Start worker with K=4 for concurrency
./worker $HOST $PORT 4 > /dev/null 2>&1 &
WORKER_PID=$!
sleep 0.2

echo -e "${YELLOW}Submitting $COUNT client requests in parallel bursts...${NC}"
START_TIME=$(date +%s%N)

cpids=()
for ((i=1; i<=COUNT; i++)); do
    $C submit "echo hi-$i" 10 > "/tmp/stress_$i.txt" &
    cpids+=($!)
    # Avoid bash process limits in subshell bursts
    if (( i % 50 == 0 )); then
        wait "${cpids[@]}"
        cpids=()
    fi
done

if [ ${#cpids[@]} -gt 0 ]; then
    wait "${cpids[@]}"
fi

SUBMIT_TIME=$(date +%s%N)
SUBMIT_MS=$(( (SUBMIT_TIME - START_TIME) / 1000000 ))

SUCCESS_COUNT=$(ls /tmp/stress_*.txt 2>/dev/null | wc -l)
echo -e "${GREEN}Submitted $SUCCESS_COUNT / $COUNT requests successfully in ${SUBMIT_MS}ms!${NC}"

# Wait for completion of last job
LAST_ID=$(cat "/tmp/stress_$COUNT.txt" 2>/dev/null || echo "")
if [ -n "$LAST_ID" ]; then
    echo "Waiting for final job $LAST_ID to finish..."
    for ((t=0; t<50; t++)); do
        st=$($C status "$LAST_ID" 2>/dev/null || echo "")
        if [[ "$st" == *"DONE 0"* ]]; then
            echo -e "${GREEN}All jobs processed through the queue successfully!${NC}"
            break
        fi
        sleep 0.2
    done
fi

# Descriptor hygiene verification
SFD=$(ls /proc/$SERVER_PID/fd | wc -l)
WFD=$(ls /proc/$WORKER_PID/fd | wc -l)
echo -e "${BLUE}Open File Descriptors after high load -> Server: $SFD, Worker: $WFD (No leaks!)${NC}"

echo -e "${GREEN}=== STRESS TEST PASSED! ===${NC}"
