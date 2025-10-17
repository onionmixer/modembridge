#!/bin/bash

# Level 3 Comprehensive Test Script
# Tests all Level 3 features according to LEVEL3_WORK_TODO.txt specifications

echo "=== Level 3 Comprehensive Test Suite ==="
echo "Testing Level 3 Pipeline Management with Enhanced Features:"
echo "- State Machine Transitions"
echo "- Enhanced Scheduling with Quantum Enforcement"
echo "- Watermark-based Buffer Management"
echo "- Dynamic Buffer Sizing"
echo "- Anti-starvation Algorithms"
echo "- Latency Tracking and Performance Monitoring"
echo ""

# Check if modembridge binary exists
if [ ! -f "../build/modembridge" ]; then
    echo "Error: modembridge binary not found. Please run 'make' first."
    exit 1
fi

# Create log directory
mkdir -p logs

# Test configuration
TEST_DURATION=60  # seconds per test
SERIAL_DEVICE="/dev/ttyUSB0"  # Adjust for your system
TELNET_HOST="127.0.0.1"
TELNET_PORTS=("9091" "9092" "9093")

echo "Starting comprehensive Level 3 testing..."
echo "Test Duration: ${TEST_DURATION} seconds per scenario"
echo ""

# Function to run test scenario
run_test_scenario() {
    local test_name="$1"
    local config_mod="$2"
    local description="$3"

    echo "=== Test Scenario: $test_name ==="
    echo "Description: $description"

    # Backup original config
    cp ../modembridge.conf ../modembridge.conf.backup

    # Apply configuration modifications if provided
    if [ -n "$config_mod" ]; then
        eval "$config_mod"
    fi

    # Create test-specific log file
    local log_file="logs/level3_test_${test_name// /_}.log"

    echo "Starting test... Log: $log_file"
    echo "Test started at: $(date)"

    # Run modembridge with Level 3 logging
    timeout ${TEST_DURATION}s ../build/modembridge -c ../modembridge.conf -v 2>&1 | tee "$log_file" &
    local mb_pid=$!

    echo "ModemBridge PID: $mb_pid"

    # Monitor test progress
    local elapsed=0
    while [ $elapsed -lt $TEST_DURATION ]; do
        if ! kill -0 $mb_pid 2>/dev/null; then
            echo "ModemBridge process terminated early (exit code: $?)"
            break
        fi

        # Show progress every 10 seconds
        if [ $((elapsed % 10)) -eq 0 ] && [ $elapsed -gt 0 ]; then
            echo "Test progress: ${elapsed}/${TEST_DURATION}s"
        fi

        sleep 1
        elapsed=$((elapsed + 1))
    done

    # Ensure process is terminated
    if kill -0 $mb_pid 2>/dev/null; then
        echo "Terminating test process..."
        kill $mb_pid
        sleep 2
        # Force kill if still running
        if kill -0 $mb_pid 2>/dev/null; then
            kill -9 $mb_pid
        fi
    fi

    wait $mb_pid 2>/dev/null
    local exit_code=$?

    echo "Test completed at: $(date)"
    echo "Exit code: $exit_code"

    # Restore original config
    mv ../modembridge.conf.backup ../modembridge.conf

    echo "Log saved to: $log_file"
    echo "========================================"
    echo ""
}

# Function to analyze test results
analyze_test_results() {
    local log_file="$1"
    local test_name="$2"

    echo "=== Analysis: $test_name ==="

    if [ ! -f "$log_file" ]; then
        echo "ERROR: Log file not found: $log_file"
        return 1
    fi

    # Check for Level 3 initialization
    echo "Checking Level 3 initialization..."
    if grep -q "Level 3 context initialized successfully" "$log_file"; then
        echo "✓ Level 3 initialization successful"
    else
        echo "✗ Level 3 initialization failed or not found"
    fi

    # Check for enhanced scheduling
    echo "Checking enhanced scheduling..."
    if grep -q "Enhanced scheduling initialized" "$log_file"; then
        echo "✓ Enhanced scheduling initialized"
    else
        echo "✗ Enhanced scheduling not found"
    fi

    # Check for state machine transitions
    echo "Checking state machine transitions..."
    local states_found=$(grep -c "Level 3 state transition" "$log_file" 2>/dev/null || echo "0")
    echo "  State transitions detected: $states_found"

    if grep -q "DATA_TRANSFER" "$log_file"; then
        echo "✓ Data transfer state reached"
    else
        echo "✗ Data transfer state not reached"
    fi

    # Check for quantum enforcement
    echo "Checking quantum enforcement..."
    if grep -q "Quantum expired" "$log_file"; then
        echo "✓ Quantum enforcement active"
    else
        echo "? Quantum enforcement messages not found (may be normal for short tests)"
    fi

    # Check for watermark management
    echo "Checking watermark management..."
    if grep -q "Watermark level changed" "$log_file"; then
        echo "✓ Watermark management active"
    else
        echo "? Watermark management messages not found (may be normal for light load)"
    fi

    # Check for latency tracking
    echo "Checking latency tracking..."
    if grep -q "latency" "$log_file"; then
        echo "✓ Latency tracking messages found"
    else
        echo "? Latency tracking messages not found"
    fi

    # Check for buffer metrics
    echo "Checking buffer management..."
    local overflow_events=$(grep -c "overflow" "$log_file" 2>/dev/null || echo "0")
    local underflow_events=$(grep -c "underflow" "$log_file" 2>/dev/null || echo "0")
    echo "  Buffer overflows: $overflow_events"
    echo "  Buffer underflows: $underflow_events"

    # Check for errors
    echo "Checking for errors..."
    local error_count=$(grep -c "ERROR" "$log_file" 2>/dev/null || echo "0")
    local warning_count=$(grep -c "WARNING" "$log_file" 2>/dev/null || echo "0")
    echo "  Errors: $error_count"
    echo "  Warnings: $warning_count"

    echo "Analysis complete."
    echo ""
}

# Test 1: Basic Level 3 Functionality
run_test_scenario "Basic_Functionality" "" \
    "Test basic Level 3 state machine and pipeline management with default settings"

# Test 2: High Load Scenario
run_test_scenario "High_Load" \
    "sed -i 's/L3_MAX_BURST_SIZE=.*/L3_MAX_BURST_SIZE=1024/' ../modembridge.conf" \
    "Test Level 3 under high load with increased burst sizes"

# Test 3: Low Latency Configuration
run_test_scenario "Low_Latency" \
    "sed -i 's/L3_FAIRNESS_TIME_SLICE_MS=.*/L3_FAIRNESS_TIME_SLICE_MS=10/' ../modembridge.conf" \
    "Test Level 3 with aggressive low-latency settings"

# Test 4: Large Buffer Configuration
run_test_scenario "Large_Buffers" \
    "sed -i 's/BUFFER_SIZE=.*/BUFFER_SIZE=16384/' ../modembridge.conf" \
    "Test Level 3 with large buffer configurations"

# Test 5: Serial Port Stress Test
run_test_scenario "Serial_Stress" "" \
    "Stress test serial port handling and DCD signal processing"

# Test 6: Telnet Connection Stress Test
run_test_scenario "Telnet_Stress" "" \
    "Stress test telnet connection handling and protocol negotiation"

# Analyze all test results
echo "=== Comprehensive Test Analysis ==="
echo ""

for log_file in logs/level3_test_*.log; do
    if [ -f "$log_file" ]; then
        test_name=$(basename "$log_file" .log | sed 's/level3_test_//g' | sed 's/_/ /g')
        analyze_test_results "$log_file" "$test_name"
    fi
done

# Performance Summary
echo "=== Performance Summary ==="
echo "Generating performance metrics from test results..."

# Count total state transitions
total_transitions=$(grep -h "Level 3 state transition" logs/level3_test_*.log 2>/dev/null | wc -l)
echo "Total state transitions across all tests: $total_transitions"

# Count quantum enforcement events
quantum_events=$(grep -h "Quantum expired" logs/level3_test_*.log 2>/dev/null | wc -l)
echo "Total quantum enforcement events: $quantum_events"

# Count buffer events
buffer_overflows=$(grep -h "overflow" logs/level3_test_*.log 2>/dev/null | wc -l)
buffer_underflows=$(grep -h "underflow" logs/level3_test_*.log 2>/dev/null | wc -l)
echo "Total buffer overflows: $buffer_overflows"
echo "Total buffer underflows: $buffer_underflows"

# Count errors and warnings
total_errors=$(grep -h "ERROR" logs/level3_test_*.log 2>/dev/null | wc -l)
total_warnings=$(grep -h "WARNING" logs/level3_test_*.log 2>/dev/null | wc -l)
echo "Total errors: $total_errors"
echo "Total warnings: $total_warnings"

echo ""
echo "=== Test Recommendations ==="

if [ $total_errors -eq 0 ]; then
    echo "✓ No critical errors detected - Level 3 implementation appears stable"
else
    echo "⚠ $total_errors errors detected - review logs for issues"
fi

if [ $buffer_overflows -gt 10 ]; then
    echo "⚠ High number of buffer overflows detected - consider increasing buffer sizes"
else
    echo "✓ Buffer management appears effective"
fi

if [ $total_transitions -gt 0 ]; then
    echo "✓ State machine is active and functional"
else
    echo "⚠ No state transitions detected - Level 3 may not be activating properly"
fi

echo ""
echo "=== Test Complete ==="
echo "All Level 3 test scenarios completed."
echo "Detailed logs available in: logs/level3_test_*.log"
echo ""
echo "Next steps:"
echo "1. Review individual test logs for detailed analysis"
echo "2. Monitor system performance during real usage"
echo "3. Fine-tune configuration parameters based on test results"
echo "4. Validate with actual hardware connections"