
 ENTERPRISE BACKUP UTILITY - Version 4.0
 ======================================
Production-grade backup tool with enterprise features:
  - Strategy pattern (sync/async/threaded/mmap modes)
  - Structured logging with spdlog-like interface
  - Signal handling for graceful shutdown
  - Configuration file support (JSON)
  - I/O scheduling with bounded queues
  - Adaptive buffer sizing
  - xxHash checksum verification
  - Atomic writes (write to temp, then rename)
  - Resume support with SQLite metadata
  - Progress callbacks and metrics export
  - Comprehensive error handling and retry logic

    
 Compile: g++ -std=c++17 -pthread -O2 -o backup_enterprise backup_enterprise.cpp

 
 Usage:   ./backup_enterprise <source> <destination> [options]
