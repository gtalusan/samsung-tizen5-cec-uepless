#!/bin/sh
# End-to-end test inside the builder container: feed synthetic CEC frames
# through a FIFO "micom-cec", then verify the JSON, the depth rule and the
# persistence format.
set -e
cd "$(dirname "$0")/.."
rm -f /tmp/cec-test-fifo /tmp/cec-test-state.txt /tmp/cec-test.log
mkfifo /tmp/cec-test-fifo
CECD_DEV=/tmp/cec-test-fifo CECD_STATE=/tmp/cec-test-state.txt CECD_PORT=18787 \
  ./cecdump 2>/tmp/cec-test.log &
PID=$!
( cat test/feed.bin > /tmp/cec-test-fifo ) &
sleep 1
JSON=$(curl -fsS -m 5 http://127.0.0.1:18787/)
echo "$JSON"
echo "$JSON" | grep -q '"hdmi3":"RX-V4A"' || { echo "FAIL: hdmi3 depth rule"; exit 1; }
echo "$JSON" | grep -q '"hdmi1":null' || { echo "FAIL: hdmi1 null"; exit 1; }
echo "$JSON" | grep -q '"a8":{"name":"PlayStation 4","port":3}' || { echo "FAIL: devices map"; exit 1; }
grep -q '^N5|RX-V4A$' /tmp/cec-test-state.txt || { echo "FAIL: state N5"; exit 1; }
grep -q '^P8|3|1$' /tmp/cec-test-state.txt || { echo "FAIL: state P8"; exit 1; }
kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
rm -f /tmp/cec-test-fifo /tmp/cec-test-state.txt /tmp/cec-test.log
echo "CECD TEST: PASS"
