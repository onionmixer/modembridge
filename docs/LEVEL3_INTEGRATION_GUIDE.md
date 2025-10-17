# Level 3 Integration Guide

This guide demonstrates how to integrate and use the enhanced Level 3 features implemented according to LEVEL3_WORK_TODO.txt specifications.

## Overview

Level 3 provides sophisticated dual pipeline management between serial/modem connections and telnet servers with:
- **Enhanced State Machine**: 10-state system with timeout handling and DCD-based activation
- **Quantum-based Scheduling**: Fair round-robin with anti-starvation algorithms
- **Watermark-based Buffer Management**: Dynamic sizing with overflow protection
- **Performance Monitoring**: Latency tracking and comprehensive metrics

## Quick Start

### 1. Basic Configuration

Add Level 3 configuration to your `modembridge.conf`:

```ini
# Enable Level 3 pipeline management
ENABLE_LEVEL3=true

# Basic Level 3 settings
L3_PIPELINE_BUFFER_SIZE=8192
L3_MAX_BURST_SIZE=256
L3_FAIRNESS_TIME_SLICE_MS=50

# Enhanced scheduling
L3_BASE_QUANTUM_MS=50
L3_MIN_QUANTUM_MS=10
L3_MAX_QUANTUM_MS=200
L3_STARVATION_THRESHOLD_MS=500

# Buffer management
L3_WATERMARK_CRITICAL=95
L3_WATERMARK_HIGH=80
L3_WATERMARK_LOW=20

# Performance targets
L3_TARGET_LATENCY_MS=100
L3_TARGET_OVERFLOW_PCT=1
```

### 2. Running with Level 3

```bash
# Start ModemBridge with Level 3 enabled
./build/modembridge -c modembridge.conf -v

# Monitor Level 3 activity
tail -f /var/log/syslog | grep "Level 3"
```

## Integration Examples

### Example 1: Real-time Chat Server

```c
#include "level3.h"

// Initialize Level 3 for chat server optimization
int setup_chat_server_mode(l3_context_t *l3_ctx, bridge_ctx_t *bridge) {
    // Initialize Level 3 context
    int ret = l3_init(l3_ctx, bridge);
    if (ret != SUCCESS) {
        return ret;
    }

    // Configure for low-latency chat
    l3_ctx->sched_config.base_quantum_ms = 25;        // Shorter quantum for responsiveness
    l3_ctx->sched_config.adaptive_quantum_enabled = true;
    l3_ctx->sched_config.starvation_threshold_ms = 200; // Prevent chat starvation

    // Configure buffers for chat traffic
    l3_ctx->pipeline_serial_to_telnet.buffers.config.growth_step_size = 512;
    l3_ctx->pipeline_telnet_to_serial.buffers.config.growth_step_size = 512;

    // Start Level 3 management
    return l3_start(l3_ctx);
}
```

### Example 2: High-speed Data Transfer

```c
// Configure Level 3 for high-throughput scenarios
int setup_high_speed_mode(l3_context_t *l3_ctx) {
    // Increase quantum sizes for better throughput
    l3_ctx->sched_config.base_quantum_ms = 100;
    l3_ctx->sched_config.max_quantum_ms = 500;

    // Enable aggressive buffer sizing
    l3_ctx->pipeline_serial_to_telnet.buffers.config.adaptive_sizing_enabled = true;
    l3_ctx->pipeline_serial_to_telnet.buffers.config.growth_threshold = 70;
    l3_ctx->pipeline_serial_to_telnet.buffers.config.growth_step_size = 2048;

    // Prioritize serial-to-telnet direction
    l3_ctx->fair_queue.serial_weight = 7;
    l3_ctx->fair_queue.telnet_weight = 3;

    return SUCCESS;
}
```

### Example 3: Low-speed Modem (1200 bps)

```c
// Configure Level 3 for low-speed modem optimization
int setup_low_speed_mode(l3_context_t *l3_ctx) {
    // Longer quantum for low-speed efficiency
    l3_ctx->sched_config.base_quantum_ms = 200;
    l3_ctx->sched_config.min_quantum_ms = 50;

    // Conservative buffer sizing for low-speed
    l3_ctx->pipeline_serial_to_telnet.buffers.config.growth_step_size = 256;
    l3_ctx->pipeline_telnet_to_serial.buffers.config.growth_step_size = 256;

    // Balanced scheduling for low-speed
    l3_ctx->fair_queue.serial_weight = 5;
    l3_ctx->fair_queue.telnet_weight = 5;

    // Enable low-speed boost
    l3_ctx->sched_config.low_speed_fairness = true;
    l3_ctx->sched_config.low_speed_boost_factor = 2;

    return SUCCESS;
}
```

## Monitoring and Debugging

### Performance Monitoring

```c
// Get comprehensive statistics
void monitor_level3_performance(l3_context_t *l3_ctx) {
    l3_scheduling_stats_t sched_stats;
    l3_buffer_metrics_t serial_metrics, telnet_metrics;

    // Get scheduling statistics
    l3_get_scheduling_statistics(l3_ctx, &sched_stats);

    printf("=== Level 3 Performance ===\n");
    printf("Current Quantum: %dms\n", sched_stats.current_quantum_ms);
    printf("Avg Latency (S→T): %.2fms\n", sched_stats.serial_to_telnet_avg_latency_ms);
    printf("Avg Latency (T→S): %.2fms\n", sched_stats.telnet_to_serial_avg_latency_ms);
    printf("System Efficiency: %.1f%%\n", sched_stats.system_efficiency_pct);
    printf("Consecutive Slices: %d\n", sched_stats.consecutive_slices);

    // Get buffer metrics
    l3_get_buffer_metrics(&l3_ctx->pipeline_serial_to_telnet.buffers, &serial_metrics);
    l3_get_buffer_metrics(&l3_ctx->pipeline_telnet_to_serial.buffers, &telnet_metrics);

    printf("\n=== Buffer Metrics ===\n");
    printf("Serial→Telnet - Usage: %zu bytes, Peak: %zu bytes, Overflows: %llu\n",
           serial_metrics.current_usage, serial_metrics.peak_usage, serial_metrics.overflow_events);
    printf("Telnet→Serial - Usage: %zu bytes, Peak: %zu bytes, Overflows: %llu\n",
           telnet_metrics.current_usage, telnet_metrics.peak_usage, telnet_metrics.overflow_events);
}
```

### Debug Logging

Enable detailed Level 3 logging by setting log level to DEBUG:

```bash
# Start with maximum debugging
./build/modembridge -c modembridge.conf -v -d

# Filter Level 3 messages
tail -f /var/log/syslog | grep -E "(Level 3|L3_|quantum|watermark|state transition)"
```

### State Machine Monitoring

```c
// Monitor state machine transitions
void monitor_state_machine(l3_context_t *l3_ctx) {
    static l3_system_state_t last_state = L3_STATE_UNINITIALIZED;

    pthread_mutex_lock(&l3_ctx->state_mutex);
    l3_system_state_t current_state = l3_ctx->system_state;
    pthread_mutex_unlock(&l3_ctx->state_mutex);

    if (current_state != last_state) {
        printf("State transition: %s → %s\n",
               l3_system_state_to_string(last_state),
               l3_system_state_to_string(current_state));
        last_state = current_state;

        // Log state-specific information
        switch (current_state) {
            case L3_STATE_DATA_TRANSFER:
                printf("✓ Data transfer active - Level 3 fully operational\n");
                break;
            case L3_STATE_CONNECTING:
                printf("🔄 Establishing connection...\n");
                break;
            case L3_STATE_ERROR:
                printf("❌ Error state detected - check logs\n");
                break;
            default:
                break;
        }
    }
}
```

## Configuration Tuning

### Latency Optimization

For ultra-low latency applications:

```ini
# Aggressive latency settings
L3_BASE_QUANTUM_MS=10
L3_MIN_QUANTUM_MS=5
L3_MAX_QUANTUM_MS=50
L3_STARVATION_THRESHOLD_MS=100
L3_PIPELINE_BUFFER_SIZE=4096
L3_MAX_BURST_SIZE=64
```

### Throughput Optimization

For maximum throughput:

```ini
# High-throughput settings
L3_BASE_QUANTUM_MS=200
L3_MIN_QUANTUM_MS=100
L3_MAX_QUANTUM_MS=1000
L3_STARVATION_THRESHOLD_MS=1000
L3_PIPELINE_BUFFER_SIZE=32768
L3_MAX_BURST_SIZE=2048
```

### Memory Optimization

For memory-constrained environments:

```ini
# Memory-efficient settings
L3_PIPELINE_BUFFER_SIZE=2048
L3_MAX_BURST_SIZE=128
L3_WATERMARK_CRITICAL=90
L3_WATERMARK_HIGH=70
L3_WATERMARK_LOW=30
```

## Testing and Validation

### Unit Testing

Run the comprehensive test suite:

```bash
# Run all Level 3 tests
cd tests
./test_level3.sh

# Run performance benchmarks
./benchmark_level3.sh
```

### Integration Testing

Test with actual hardware:

```bash
# Test with USB serial device
./build/modembridge -c modembridge.conf -v \
    --serial-device=/dev/ttyUSB0 \
    --baudrate=1200

# Test with virtual serial port
socat -d -d pty,raw,echo=0,link=/tmp/virtual_serial pty,raw,echo=0 &
./build/modembridge -c modembridge.conf -v \
    --serial-device=/tmp/virtual_serial
```

## Troubleshooting

### Common Issues

1. **Level 3 not activating**
   - Check if Level 1 and Level 2 are ready
   - Verify DCD signal is detected
   - Look for state machine transition logs

2. **High latency**
   - Reduce quantum size: `L3_BASE_QUANTUM_MS=25`
   - Enable adaptive scheduling
   - Check for buffer overflows

3. **Buffer overflows**
   - Increase buffer size: `L3_PIPELINE_BUFFER_SIZE=16384`
   - Lower growth threshold: `L3_WATERMARK_HIGH=70`
   - Enable backpressure: `L3_BACKPRESSURE_ENABLED=true`

4. **Poor performance**
   - Monitor scheduling statistics
   - Check for starvation events
   - Adjust fair queue weights

### Debug Commands

```bash
# Check Level 3 initialization
grep "Level 3.*initialized" /var/log/syslog

# Monitor state transitions
grep "Level 3 state transition" /var/log/syslog

# Check quantum enforcement
grep "Quantum expired" /var/log/syslog

# Monitor buffer events
grep -E "(overflow|underflow|Watermark)" /var/log/syslog

# Check latency metrics
grep "latency.*ms" /var/log/syslog
```

## API Reference

### Key Functions

- `l3_init()`: Initialize Level 3 context
- `l3_start()`: Start Level 3 management thread
- `l3_set_system_state()`: Control state machine transitions
- `l3_process_pipeline_with_quantum()`: Process data with quantum enforcement
- `l3_get_scheduling_statistics()`: Get performance metrics
- `l3_enhanced_double_buffer_init()`: Initialize enhanced buffers

### Performance Targets

According to LEVEL3_WORK_TODO.txt:
- **Latency**: <100ms average processing time
- **Buffer Overflow**: <1% of total operations
- **Throughput**: Support for 1200 bps low-speed optimization
- **Compatibility**: Real-time chat server compatibility

### Monitoring Commands

```bash
# Get real-time statistics
kill -USR1 $(pgrep modembridge)  # Sends stats to syslog

# Check system health
./build/modembridge --health-check

# Generate performance report
./build/modembridge --report-level3
```

## Conclusion

The Level 3 implementation provides enterprise-grade pipeline management with sophisticated scheduling, buffer management, and performance monitoring. The system is designed to meet the demanding requirements specified in LEVEL3_WORK_TODO.txt while maintaining compatibility with existing ModemBridge functionality.

For production deployment, start with conservative settings and gradually tune based on observed performance metrics and specific use case requirements.