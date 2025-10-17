#!/bin/bash

# Test script for Level 2 telnet functionality
# Tests multi-language text transmission as specified in LEVEL2_TELNET_TEST.txt

echo "=== Level 2 Telnet Test Script ==="
echo "Testing multi-language text transmission"
echo "Test strings: \"abcd\", \"한글\", \"こんにちは。\""
echo "Each string will be sent at 3-second intervals"
echo ""

# Check if modembridge binary exists
if [ ! -f "../build/modembridge" ]; then
    echo "Error: modembridge binary not found. Please run 'make' first."
    exit 1
fi

# Create log directory
mkdir -p logs

# Test with different telnet servers
declare -a servers=(
    "127.0.0.1:9091"  # line_mode_server
    "127.0.0.1:9092"  # char_mode_server
    "127.0.0.1:9093"  # line_mode_binary_server
)

for server in "${servers[@]}"; do
    IFS=':' read -r host port <<< "$server"

    echo "Testing with server: $host:$port"

    # Update config for this server
    sed -i.tmp "s/TELNET_PORT=.*/TELNET_PORT=\"$port\"/" ../modembridge.conf

    echo "Starting modembridge with verbose logging..."

    # Run modembridge for 30 seconds to capture telnet reception and test transmissions
    timeout 30s ../build/modembridge -c ../modembridge.conf -v 2>&1 | tee "logs/test_server_${port}.log" &

    # Get the PID
    MB_PID=$!

    echo "ModemBridge PID: $MB_PID"

    # Wait for the test to complete
    wait $MB_PID

    # Check if modembridge exited cleanly
    if [ $? -eq 124 ]; then
        echo "Test completed (30 seconds elapsed)"
    elif [ $? -eq 0 ]; then
        echo "Test completed successfully"
    else
        echo "Test failed with exit code $?"
    fi

    echo "Log saved to: logs/test_server_${port}.log"
    echo "----------------------------------------"
    echo ""
done

# Restore original config
mv ../modembridge.conf.tmp ../modembridge.conf

echo "=== Test Summary ==="
echo "All tests completed. Check logs directory for detailed output."
echo ""
echo "Key things to look for in logs:"
echo "- Telnet connection establishment"
echo "- Text reception from telnet server (30 seconds)"
echo "- Test string transmissions: \"abcd\", \"한글\", \"こんにちは。\""
echo "- Each string sent at 3-second intervals"
echo "- Echo responses from telnet server"
echo ""
echo "Example log entries to look for:"
echo "[TEST] === TELNET TEST STARTED ==="
echo "[TEST] [HH:MM:SS] Sent: \"abcd\" (4 bytes)"
echo "[TEST] [HH:MM:SS] Sent: \"한글\" (6 bytes)"
echo "[TEST] [HH:MM:SS] Sent: \"こんにちは。\" (16 bytes)"
echo "[TEST] === TELNET TEST STOPPED ==="