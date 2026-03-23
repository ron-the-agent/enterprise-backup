# Enterprise Backup Utility - Project Memory

## Overview

Enterprise-grade file backup utility written in C++17, designed for production environments requiring high performance, reliability, and observability.

**Version**: 4.0
**Last Updated**: 2026-03-24

## Project Structure

```
enterprise-backup/
├── backup_enterprise.cpp       # Unified enterprise implementation (~2000 LOC)
├── README.md                   # User documentation
├── Changelog.txt               # Legacy changelog
└── memory/                     # Project documentation
    ├── MEMORY.md              # This file - project overview
    ├── BUGS_FIXED.md          # Bugs identified and fixed
    ├── PERFORMANCE.md         # Performance optimization guide
    ├── ENTERPRISE_FEATURES.md # Enterprise feature documentation
    ├── CHANGELOG.md           # Complete change history
    └── SUMMARY.md             # Summary of changes v4.0
```

## Quick Start

### Compile
```bash
g++ -std=c++17 -pthread -O2 -o backup_enterprise backup_enterprise.cpp
```

### Run
```bash
# Basic usage
./backup_enterprise source_dir dest_dir

# Threaded mode (default)
./backup_enterprise source_dir dest_dir --mode threaded -t 8

# Memory-mapped for large files
./backup_enterprise source_dir dest_dir --mode mmap --mmap-threshold 50

# With logging
./backup_enterprise source_dir dest_dir --log backup.log --log-level DEBUG
```

## Implementation History

| Version | File | Status |
|---------|------|--------|
| v1 | backup_utility.cpp | Legacy - simple sequential copy |
| v2 | backup_threaded.cpp | Legacy - thread pool implementation |
| v3 | backup_async.cpp | Legacy - async/futures implementation |
| v3 | backup_advanced.cpp | Legacy - memory-mapped I/O |
| v4 | backup_enterprise.cpp | **Current** - unified all features |

## Key Components

1. **Config** - Centralized configuration with validation
2. **Logger** - Thread-safe singleton with log levels
3. **Metrics** - Atomic counters for performance tracking
4. **MemoryMappedFile** - RAII wrapper for mmap I/O
5. **ThreadPool** - Worker pool with bounded queue
6. **FileQueue** - Producer/consumer queue (scanner → workers)
7. **CopyStrategy** - Base class for copy algorithms
8. **BufferedCopyStrategy** - Standard buffered I/O
9. **MmapCopyStrategy** - Memory-mapped file copying
10. **BackupEngine** - Orchestrates backup operation
11. **xxHash** - Embedded checksum verification

## Design Patterns

1. **Strategy Pattern** - Interchangeable copy modes
2. **Singleton** - Logger, Metrics
3. **RAII** - Resource cleanup (MemoryMappedFile, ThreadPool)
4. **Thread Pool** - Fixed workers with task queue
5. **Producer/Consumer** - FileQueue with backpressure
6. **Atomic Operations** - Lock-free statistics

## Mode Selection Guide

| Scenario | Recommended Mode | Flags |
|----------|-----------------|-------|
| Many small files (<1MB) | threaded | `-t 16 -j 16` |
| Mixed workload | async | `-j 8` |
| Large files (>100MB) | mmap | `--mmap-threshold 50` |
| Network drives | async | `-j 4` |
| HDD storage | threaded | `-t 4 -j 4` |
| SSD storage | threaded | `-t 16 -j 16` |
| Debugging | sync | `-v --log debug.log` |

## Important Notes

- Always test with `--dry-run` first
- For HDDs, limit concurrency to avoid thrashing
- Memory-mapped uses virtual address space, not physical RAM
- Atomic writes require 2x disk space during copy
- Graceful shutdown on Ctrl+C finishes current files

## Related Files

- [BUGS_FIXED.md](./BUGS_FIXED.md) - Known issues and fixes
- [PERFORMANCE.md](./PERFORMANCE.md) - Optimization details
- [ENTERPRISE_FEATURES.md](./ENTERPRISE_FEATURES.md) - Production features
- [CHANGELOG.md](./CHANGELOG.md) - Version history
- [SUMMARY.md](./SUMMARY.md) - Changes summary
