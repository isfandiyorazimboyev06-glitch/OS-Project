#!/usr/bin/env bash
# tests/self_check.sh - Automated test script verifying all 12 rows of Section 5.4
set -u

GREEN='\033[0;32m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

PORT=5000
HOST="127.0.0.1"
C="./client $HOST $PORT"

cleanup() {
    pkill -9 -f "./server $PORT" 2>/dev/null || true
    pkill -9 -f "./worker $HOST $PORT" 2>/dev/null || true
    pkill -9 -f "sleep 100" 2>/dev/null || true
    pkill -9 -f "sleep 30" 2>/dev/null || true
    pkill -9 -f "sleep 3" 2>/dev/null || true
    pkill -9 -f "sleep 2" 2>/dev/null || true
    rm -f /tmp/test_*.log /tmp/id_*.txt
}

trap cleanup EXIT
cleanup

echo -e "${BLUE}=== RUNNING OS TERM PROJECT SELF-CHECK (§5.4) ===${NC}"

# 1. Row 1: Building and starting programs
echo -n "Row 1 (Build & Start server/worker): "
make clean >/dev/null 2>&1
make >/dev/null 2>&1
if [ ! -f server ] || [ ! -f worker ] || [ ! -f client ]; then
    echo -e "${RED}FAIL (Missing executables)${NC}"
    exit 1
fi

./server $PORT > /tmp/test_server.log 2>&1 &
SERVER_PID=$!
sleep 0.2

./worker $HOST $PORT 1 > /tmp/test_worker.log 2>&1 &
WORKER_PID=$!
sleep 0.3

S_LOG=$(head -n 1 /tmp/test_server.log)
W_LOG=$(head -n 1 /tmp/test_worker.log)

if [[ "$S_LOG" == *"server: listening on port $PORT"* ]] && [[ "$W_LOG" == *"worker: connected to $HOST:$PORT, K=1"* ]]; then
    echo -e "${GREEN}PASS${NC}"
else
    echo -e "${RED}FAIL: Server: '$S_LOG', Worker: '$W_LOG'${NC}"
    exit 1
fi

# 2. Row 2: echo hello
echo -n "Row 2 (Submit echo hello -> DONE 0 & fetch hello): "
ID2=$($C submit "echo hello" 10)
sleep 2
STATUS2=$($C status "$ID2")
FETCH2=$($C fetch "$ID2")

if [[ "$STATUS2" == "$ID2 DONE 0" ]] && [[ "$FETCH2" == "hello" ]]; then
    echo -e "${GREEN}PASS${NC}"
else
    echo -e "${RED}FAIL (Status: '$STATUS2', Fetch: '$FETCH2')${NC}"
    exit 1
fi

# 3. Row 3: exit 3 and ls /nonexistent
echo -n "Row 3 (Exit 3 & ls nonexistent -> DONE 3, DONE 2 with error output): "
ID3_1=$($C submit "exit 3" 10)
ID3_2=$($C submit "ls /nonexistent" 10)
sleep 1.5
STATUS3_1=$($C status "$ID3_1")
STATUS3_2=$($C status "$ID3_2")
FETCH3_2=$($C fetch "$ID3_2" 2>&1)

if [[ "$STATUS3_1" == "$ID3_1 DONE 3" ]] && [[ "$STATUS3_2" == "$ID3_2 DONE 2" ]] && [[ "$FETCH3_2" == *"No such file"* ]]; then
    echo -e "${GREEN}PASS${NC}"
else
    echo -e "${RED}FAIL (Status1: '$STATUS3_1', Status2: '$STATUS3_2', Fetch2: '$FETCH3_2')${NC}"
    exit 1
fi

# 4. Row 4: Timeout enforcement
echo -n "Row 4 (Timeout kill sleep 100 -> KILLED TIMEOUT & pgrep clean): "
ID4=$($C submit "sleep 100" 2)
sleep 4
STATUS4=$($C status "$ID4")
P4=$(pgrep -xf "sleep 100" 2>/dev/null || true)

if [[ "$STATUS4" == "$ID4 KILLED TIMEOUT" ]] && [ -z "$P4" ]; then
    echo -e "${GREEN}PASS${NC}"
else
    echo -e "${RED}FAIL (Status: '$STATUS4', pgrep: '$P4')${NC}"
    exit 1
fi

# 5. Row 5: Cancel running job
echo -n "Row 5 (Cancel running sleep 30 -> RUNNING, CANCELLED, CANCELLED): "
ID5=$($C submit "sleep 30" 60)
sleep 1
STATUS5_1=$($C status "$ID5")
CANCEL5=$($C cancel "$ID5")
STATUS5_2=$($C status "$ID5")
P5=$(pgrep -xf "sleep 30" 2>/dev/null || true)

if [[ "$STATUS5_1" == "$ID5 RUNNING" ]] && [[ "$CANCEL5" == "$ID5 CANCELLED" ]] && [[ "$STATUS5_2" == "$ID5 CANCELLED" ]] && [ -z "$P5" ]; then
    echo -e "${GREEN}PASS${NC}"
else
    echo -e "${RED}FAIL (S1: '$STATUS5_1', Cancel: '$CANCEL5', S2: '$STATUS5_2', pgrep: '$P5')${NC}"
    exit 1
fi

# 6. Row 6: K=1 serialization
echo -n "Row 6 (K=1 three sleep 3 serialization <= 1 running, done within 12s): "
ID6_1=$($C submit "sleep 3" 10)
ID6_2=$($C submit "sleep 3" 10)
ID6_3=$($C submit "sleep 3" 10)

MAX_RUNNING=0
START_6=$(date +%s)
while true; do
    CUR=$(pgrep -xfc "sleep 3" 2>/dev/null | tail -n 1)
    CUR=${CUR:-0}
    if [ "$CUR" -gt "$MAX_RUNNING" ]; then
        MAX_RUNNING=$CUR
    fi
    S1=$($C status "$ID6_1")
    S2=$($C status "$ID6_2")
    S3=$($C status "$ID6_3")
    if [[ "$S1" == *"DONE 0"* ]] && [[ "$S2" == *"DONE 0"* ]] && [[ "$S3" == *"DONE 0"* ]]; then
        break
    fi
    sleep 0.4
done
END_6=$(date +%s)
DUR_6=$((END_6 - START_6))

if [ "$MAX_RUNNING" -le 1 ] && [ "$DUR_6" -le 12 ]; then
    echo -e "${GREEN}PASS (${DUR_6}s, max concurrent: ${MAX_RUNNING})${NC}"
else
    echo -e "${RED}FAIL (max concurrent: $MAX_RUNNING, duration: ${DUR_6}s)${NC}"
    exit 1
fi

# 7. Row 7: Eight clients concurrent submit
echo -n "Row 7 (8 clients concurrent submit within 2s, all done in 25s): "
START_7=$(date +%s)
CPIDS=()
for i in {1..8}; do
    $C submit "sleep 2" 10 > "/tmp/id_$i.txt" &
    CPIDS+=($!)
done
wait "${CPIDS[@]}"
SUB_7=$(date +%s)
SUB_DUR=$((SUB_7 - START_7))

IDS=($(cat /tmp/id_*.txt | sort | uniq))
if [ "${#IDS[@]}" -ne 8 ] || [ "$SUB_DUR" -gt 2 ]; then
    echo -e "${RED}FAIL (Distinct IDs: ${#IDS[@]}, Submit time: ${SUB_DUR}s)${NC}"
    exit 1
fi

while true; do
    DONE_CNT=0
    for id in "${IDS[@]}"; do
        st=$($C status "$id")
        if [[ "$st" == *"DONE 0"* ]]; then
            DONE_CNT=$((DONE_CNT + 1))
        fi
    done
    if [ "$DONE_CNT" -eq 8 ]; then
        break
    fi
    sleep 0.5
done
END_7=$(date +%s)
DUR_7=$((END_7 - START_7))

if [ "$DUR_7" -le 25 ]; then
    echo -e "${GREEN}PASS (All 8 done in ${DUR_7}s)${NC}"
else
    echo -e "${RED}FAIL (Took ${DUR_7}s > 25s)${NC}"
    exit 1
fi

# 8. Row 8: Idle client does not block others
echo -n "Row 8 (Idle nc does not block submit within 2s): "
nc $HOST $PORT &
NCPID=$!
sleep 0.2

START_8=$(date +%s)
ID8=$($C submit "echo x" 10)
END_8=$(date +%s)
DUR_8=$((END_8 - START_8))

kill -9 $NCPID 2>/dev/null || true

if [ "$DUR_8" -le 2 ] && [[ "$ID8" == *"T"* ]]; then
    echo -e "${GREEN}PASS (Issued $ID8 in ${DUR_8}s)${NC}"
else
    echo -e "${RED}FAIL (Took ${DUR_8}s)${NC}"
    exit 1
fi

# 9. Row 9: Unknown ID query
echo -n "Row 9 (Query unknown ID T12-99999 -> exit code 2 & UNKNOWN): "
OUT9=$($C status T12-99999 2>&1)
CODE9=$?

if [ "$CODE9" -eq 2 ] && [[ "$OUT9" == "T12-99999 UNKNOWN"* ]]; then
    echo -e "${GREEN}PASS${NC}"
else
    echo -e "${RED}FAIL (Output: '$OUT9', Exit code: $CODE9)${NC}"
    exit 1
fi

# 10. Row 10: Zero zombies
echo -n "Row 10 (Zero zombies in worker process tree): "
ZOMBIES=$(ps -o pid,ppid,stat,comm --ppid $WORKER_PID 2>/dev/null | grep ' Z' || true)

if [ -z "$ZOMBIES" ]; then
    echo -e "${GREEN}PASS (No zombies)${NC}"
else
    echo -e "${RED}FAIL (Found zombies: $ZOMBIES)${NC}"
    exit 1
fi

# 11. Row 11: Zero descriptor leaks before and after 100 jobs
echo -n "Row 11 (Zero descriptor leaks across 100 jobs): "
SFD_BEFORE=$(ls /proc/$SERVER_PID/fd | wc -l)
WFD_BEFORE=$(ls /proc/$WORKER_PID/fd | wc -l)

for i in {1..100}; do
    $C submit "echo hi" 10 >/dev/null
done

# Wait for 100th job to complete
LAST_ID=$($C submit "echo last" 10)
while true; do
    st=$($C status "$LAST_ID")
    if [[ "$st" == *"DONE 0"* ]]; then
        break
    fi
    sleep 0.1
done
sleep 0.5

SFD_AFTER=$(ls /proc/$SERVER_PID/fd | wc -l)
WFD_AFTER=$(ls /proc/$WORKER_PID/fd | wc -l)

if [ "$SFD_BEFORE" -eq "$SFD_AFTER" ] && [ "$WFD_BEFORE" -eq "$WFD_AFTER" ]; then
    echo -e "${GREEN}PASS (Server: $SFD_BEFORE -> $SFD_AFTER, Worker: $WFD_BEFORE -> $WFD_AFTER)${NC}"
else
    echo -e "${RED}FAIL (Server: $SFD_BEFORE -> $SFD_AFTER, Worker: $WFD_BEFORE -> $WFD_AFTER)${NC}"
    exit 1
fi

# 12. Row 12: Worker SIGTERM & Server SIGTERM clean restart
echo -n "Row 12 (Worker SIGTERM -> KILLED SIGNAL 9, Server restart on same port): "
ID12=$($C submit "sleep 30" 60)
sleep 1
kill -TERM $WORKER_PID
sleep 2

STATUS12=$($C status "$ID12")
P12=$(pgrep -xf "sleep 30" 2>/dev/null || true)
W_DEAD=$(kill -0 $WORKER_PID 2>&1 || echo "dead")

kill -TERM $SERVER_PID
sleep 0.5

./server $PORT > /tmp/test_server_new.log 2>&1 &
NEW_SERVER_PID=$!
sleep 0.5

S_NEW_LOG=$(head -n 1 /tmp/test_server_new.log)
kill -TERM $NEW_SERVER_PID 2>/dev/null || true

if [[ "$STATUS12" == "$ID12 KILLED SIGNAL 9" ]] && [ -z "$P12" ] && [[ "$S_NEW_LOG" == *"server: listening on port $PORT"* ]]; then
    echo -e "${GREEN}PASS${NC}"
else
    echo -e "${RED}FAIL (Status: '$STATUS12', pgrep: '$P12', New server: '$S_NEW_LOG')${NC}"
    exit 1
fi

echo -e "${GREEN}=== ALL 12 SELF-CHECK TESTS PASSED SUCCESSFULLY! ===${NC}"
