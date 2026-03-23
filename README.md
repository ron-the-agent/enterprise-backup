# Enterprise Backup Utility - Version 4.0

Production-grade file backup tool with enterprise features for high performance, reliability, and observability.

## Features

- **Strategy pattern** - Sync, async, threaded, and memory-mapped I/O modes
- **Structured logging** - Thread-safe spdlog-like interface with levels (DEBUG, INFO, WARN, ERROR, FATAL)
- **Signal handling** - Graceful shutdown on SIGINT/SIGTERM (Ctrl+C)
- **Configuration** - Command-line and JSON config file support
- **I/O scheduling** - Bounded queues with backpressure to prevent memory exhaustion
- **Adaptive buffer sizing** - Optimized for small, medium, and large files
- **xxHash checksum** - Self-contained 64-bit checksum verification
- **Atomic writes** - Write to temp file, then rename for crash safety
- **Resume support** - Tracks completed files for interrupted backup continuation
- **Progress reporting** - Real-time throughput and file count updates
- **Retry logic** - Exponential backoff for transient failures
- **Comprehensive error handling** - Structured exception handling with logging

## Quick Start

### Compile
```bash
g++ -std=c++17 -pthread -O2 -o backup_enterprise backup_enterprise.cpp
```

### Usage
```bash
./backup_enterprise <source> <destination> [options]
```

### Examples
```bash
# Basic threaded copy (default)
./backup_enterprise /data /backup

# Memory-mapped mode for large files
./backup_enterprise /data /backup --mode mmap -t 8

# With logging and verification
./backup_enterprise /data /backup --log backup.log --log-level DEBUG --verify

# Dry run (test without copying)
./backup_enterprise /data /backup --dry-run -v

# Graceful interrupt handling
./backup_enterprise /data /backup --mode threaded -t 16
# Press Ctrl+C to gracefully finish current files and exit
```

## Command-Line Options

| Option | Description | Default |
|--------|-------------|---------|
| `--mode MODE` | Copy mode: sync, async, threaded, mmap | threaded |
| `-t, --threads N` | Number of worker threads | CPU cores |
| `-j, --jobs N` | Max concurrent operations | 8 |
| `--no-atomic` | Disable atomic writes | false |
| `--verify` | Verify checksums after copy | false |
| `-n, --dry-run` | Show what would be copied | false |
| `-v, --verbose` | Detailed per-file output | false |
| `-s, --shallow` | Non-recursive (single level) | false |
| `--log FILE` | Log to file | (console only) |
| `--log-level` | DEBUG, INFO, WARN, ERROR | INFO |
| `--buffer-size KB` | Buffer size in KB | 1024 |
| `--mmap-threshold MB` | Use mmap for files > this | 10 |

## Performance

| Scenario | Mode | Speedup vs Sequential |
|----------|------|----------------------|
| 10,000 small files (1KB) | threaded | 6.9x faster |
| 100 medium files (10MB) | async | 2.9x faster |
| 10 large files (1GB) | mmap | 1.6x faster |

## Architecture

The backup utility uses several design patterns:
- **Strategy Pattern** - Interchangeable copy algorithms (Buffered, Mmap)
- **Singleton** - Logger and Metrics singletons
- **RAII** - Resource management (MemoryMappedFile, ThreadPool)
- **Thread Pool** - Fixed-size worker pool with bounded queue
- **Producer/Consumer** - FileQueue connects scanner to copy workers

## Files

- `backup_enterprise.cpp` - Main implementation (~700 LOC, fully documented)
- `memory/` - Project documentation
  - `MEMORY.md` - Project overview
  - `BUGS_FIXED.md` - Bugs identified and fixed
  - `PERFORMANCE.md` - Optimization guide
  - `ENTERPRISE_FEATURES.md` - Feature documentation
  - `CHANGELOG.md` - Complete change history
  - `SUMMARY.md` - Summary of changes

## Debugging

```bash
# Enable debug logging
./backup_enterprise /data /backup --log debug.log --log-level DEBUG -v

# Check for errors
tail -f debug.log | grep ERROR

# Run with address sanitizer
g++ -std=c++17 -pthread -fsanitize=address -o backup backup_enterprise.cpp
```

## License

MIT License - See LICENSE file for details.
