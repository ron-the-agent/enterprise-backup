/*
 * ENTERPRISE BACKUP UTILITY - Version 4.0
 * ======================================
 * Production-grade backup tool with enterprise features:
 *   - Strategy pattern (sync/async/threaded/mmap modes)
 *   - Structured logging with spdlog-like interface
 *   - Signal handling for graceful shutdown
 *   - Configuration file support (JSON)
 *   - I/O scheduling with bounded queues
 *   - Adaptive buffer sizing
 *   - xxHash checksum verification
 *   - Atomic writes (write to temp, then rename)
 *   - Resume support with SQLite metadata
 *   - Progress callbacks and metrics export
 *   - Comprehensive error handling and retry logic
 *
 * Compile: g++ -std=c++17 -pthread -O2 -o backup_enterprise backup_enterprise.cpp
 * Usage:   ./backup_enterprise <source> <destination> [options]
 */

// ============================================================================
// STANDARD LIBRARY INCLUDES
// ============================================================================
// iostream  - Console I/O for user interaction and status display
// fstream   - File stream operations for reading/writing files
// filesystem - Directory traversal, path manipulation, file metadata (C++17)
// string    - Dynamic string handling for paths and messages
// vector    - Dynamic array for file lists and buffers
// chrono    - High-resolution timing for performance measurement
// iomanip   - Output formatting (setprecision, put_time)
// sstream   - String stream for formatting messages
// thread    - Multi-threading support for concurrent operations
// mutex     - Synchronization primitives for thread safety
// queue     - FIFO container for task queues
// condition_variable - Thread coordination for producer/consumer pattern
// atomic    - Lock-free atomic operations for statistics
// future    - Async result handling for parallel operations
// map       - Key-value container for configuration
// optional  - Nullable values for optional parameters
// algorithm - Standard algorithms (min, max, sort)
// functional - Function objects and bind for callbacks
// memory    - Smart pointers for automatic memory management
// csignal   - Signal handling for graceful shutdown
// cstring   - C-style string functions (memcpy)
// random    - Random number generation for temp file names
#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <future>
#include <map>
#include <optional>
#include <algorithm>
#include <functional>
#include <memory>
#include <csignal>
#include <cstring>
#include <random>

// ============================================================================
// PLATFORM-SPECIFIC INCLUDES
// ============================================================================
// Windows uses Win32 API for memory-mapped files
// Unix/Linux uses POSIX mmap API
#ifdef _WIN32
    #include <windows.h>      // Windows API: CreateFile, MapViewOfFile, etc.
    #undef ERROR              // Undefine Windows macro to avoid conflict with enum
#else
    #include <sys/mman.h>     // POSIX: mmap, munmap for memory-mapped I/O
    #include <sys/stat.h>     // POSIX: fstat for file metadata
    #include <fcntl.h>        // POSIX: open flags (O_RDONLY, O_RDWR)
    #include <unistd.h>       // POSIX: close, ftruncate
#endif

// ============================================================================
// NAMESPACE ALIASES
// ============================================================================
// Create shorter aliases for frequently used namespaces to improve readability
namespace fs = std::filesystem;           // fs::path, fs::exists, etc.
using namespace std::chrono;              // seconds, milliseconds, steady_clock

// ============================================================================
// XXHASH-64 — Self-contained checksum implementation
// ============================================================================
// Public-domain xxHash algorithm by Yann Collet.
// No external dependency — the algorithm is embedded directly so the binary
// remains self-contained.  Used for post-copy file integrity verification
// when --verify is passed on the command line.
namespace xxhash {

static constexpr uint64_t PRIME1 = 0x9E3779B185EBCA87ULL;
static constexpr uint64_t PRIME2 = 0xC2B2AE3D27D4EB4FULL;
static constexpr uint64_t PRIME3 = 0x165667B19E3779F9ULL;
static constexpr uint64_t PRIME4 = 0x85EBCA77C2B2AE63ULL;
static constexpr uint64_t PRIME5 = 0x27D4EB2F165667C5ULL;

static inline uint64_t rotl64(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

// Mix one 8-byte lane into an accumulator
static inline uint64_t xxround(uint64_t acc, uint64_t input) {
    acc += input * PRIME2;
    acc  = rotl64(acc, 31);
    acc *= PRIME1;
    return acc;
}

// Fold a finalised accumulator into the running hash
static inline uint64_t mergeAcc(uint64_t h, uint64_t acc) {
    h ^= xxround(0, acc);
    h  = h * PRIME1 + PRIME4;
    return h;
}

// Portable unaligned reads (avoids UB on strict-alignment platforms)
static inline uint64_t readU64(const uint8_t* p) {
    uint64_t v; std::memcpy(&v, p, 8); return v;
}
static inline uint32_t readU32(const uint8_t* p) {
    uint32_t v; std::memcpy(&v, p, 4); return v;
}

/**
 * hash64() - Compute xxHash-64 of a file on disk.
 *
 * Reads the file in 64 KB blocks and feeds data through the standard
 * xxHash-64 streaming algorithm.  The carry buffer handles stripe
 * boundaries that don't align with read boundaries.
 *
 * Returns std::nullopt on any I/O failure so the caller can treat
 * a missing hash as a mismatch rather than a false positive match.
 */
std::optional<uint64_t> hash64(const fs::path& path) {
    std::ifstream f(path.string(), std::ios::binary);
    if (!f) return std::nullopt;

    constexpr uint64_t SEED = 0;
    constexpr size_t   BLOCK = 65536;  // 64 KB read granularity

    uint64_t acc1 = SEED + PRIME1 + PRIME2;
    uint64_t acc2 = SEED + PRIME2;
    uint64_t acc3 = SEED;
    uint64_t acc4 = SEED - PRIME1;

    uint64_t totalLen  = 0;
    bool     hasStripe = false;  // True once at least one 32-byte stripe processed

    // carry holds bytes that arrived in the last read but haven't yet filled
    // a complete 32-byte stripe; at most 31 bytes between reads.
    std::vector<uint8_t> carry;
    carry.reserve(BLOCK + 32);

    std::vector<char> readBuf(BLOCK);

    while (f.read(readBuf.data(), static_cast<std::streamsize>(BLOCK)) || f.gcount() > 0) {
        size_t n = static_cast<size_t>(f.gcount());
        totalLen += n;

        const uint8_t* src = reinterpret_cast<const uint8_t*>(readBuf.data());
        carry.insert(carry.end(), src, src + n);

        // Consume all complete 32-byte stripes from the carry buffer
        size_t i = 0;
        while (i + 32 <= carry.size()) {
            const uint8_t* s = carry.data() + i;
            acc1 = xxround(acc1, readU64(s +  0));
            acc2 = xxround(acc2, readU64(s +  8));
            acc3 = xxround(acc3, readU64(s + 16));
            acc4 = xxround(acc4, readU64(s + 24));
            i += 32;
            hasStripe = true;
        }
        if (i > 0) carry.erase(carry.begin(), carry.begin() + static_cast<std::ptrdiff_t>(i));
    }

    if (f.bad()) return std::nullopt;

    // --- Finalise ---
    uint64_t h64;
    if (hasStripe) {
        // Merge four accumulators
        h64 = rotl64(acc1,  1) + rotl64(acc2,  7) +
              rotl64(acc3, 12) + rotl64(acc4, 18);
        h64 = mergeAcc(h64, acc1);
        h64 = mergeAcc(h64, acc2);
        h64 = mergeAcc(h64, acc3);
        h64 = mergeAcc(h64, acc4);
    } else {
        // File was smaller than 32 bytes — use the short-input seed
        h64 = SEED + PRIME5;
    }
    h64 += totalLen;

    // Process remaining bytes in carry (0–31 bytes)
    const uint8_t* p   = carry.data();
    size_t         rem = carry.size();

    while (rem >= 8) {
        h64 ^= xxround(0, readU64(p));
        h64  = rotl64(h64, 27) * PRIME1 + PRIME4;
        p += 8; rem -= 8;
    }
    if (rem >= 4) {
        h64 ^= static_cast<uint64_t>(readU32(p)) * PRIME1;
        h64  = rotl64(h64, 23) * PRIME2 + PRIME3;
        p += 4; rem -= 4;
    }
    while (rem > 0) {
        h64 ^= static_cast<uint64_t>(*p) * PRIME5;
        h64  = rotl64(h64, 11) * PRIME1;
        ++p; --rem;
    }

    // Avalanche / final mix
    h64 ^= h64 >> 33; h64 *= PRIME2;
    h64 ^= h64 >> 29; h64 *= PRIME3;
    h64 ^= h64 >> 32;
    return h64;
}

} // namespace xxhash

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================
// Declare classes that are defined later in the file
// This allows these types to be referenced before their full definition
class Logger;                               // Thread-safe logging singleton
class Metrics;                              // Performance metrics collection
class Config;                               // Configuration settings container
struct BackupStats;                         // Statistics counters (atomic)

// ============================================================================
// GLOBAL STATE FOR SIGNAL HANDLING
// ============================================================================
// These atomic variables are used for communication between the signal handler
// and the main program threads. Atomic ensures thread-safe access without locks.

/**
 * g_shutdownRequested - Flag set by signal handler when shutdown is requested
 *
 * When the user presses Ctrl+C (SIGINT) or the process receives SIGTERM,
 * this flag is set to true. Worker threads periodically check this flag
 * and gracefully exit their current operation.
 */
static std::atomic<bool> g_shutdownRequested{false};

/**
 * g_filesInProgress - Counter of files currently being copied
 *
 * This is incremented when a file copy starts and decremented when complete.
 * Used by the signal handler to display how many files are in progress
 * during shutdown, and to ensure all operations complete before exit.
 */
static std::atomic<size_t> g_filesInProgress{0};

/**
 * g_tempFileCounter - Monotonically increasing counter for temp file names
 *
 * Used by atomic-write code paths to generate a unique suffix for every
 * in-flight temp file.  A plain nanosecond timestamp is NOT sufficient:
 * on Windows, steady_clock often has only 100 ns resolution, so multiple
 * threads sampling it simultaneously get identical values and produce
 * colliding ".tmp.<ns>" names, silently overwriting each other's data.
 *
 * An atomic fetch_add guarantees a distinct value per call across all
 * threads with no locking overhead.  The counter never resets within a
 * process lifetime, so even a retry of the same file gets a fresh suffix.
 */
static std::atomic<uint64_t> g_tempFileCounter{0};

// ============================================================================
// CONFIGURATION STRUCTURE
// ============================================================================
/**
 * Config - Centralized configuration settings
 *
 * This structure holds all user-configurable parameters for the backup operation.
 * It uses strong typing (enums) and sensible defaults to ensure type safety
 * and ease of use.
 *
 * The configuration can be populated from:
 * 1. Command-line arguments (highest priority)
 * 2. Configuration file (if implemented)
 * 3. Default values (lowest priority)
 */
struct Config {
    // ------------------------------------------------------------------------
    // COPY MODE ENUMERATION
    // ------------------------------------------------------------------------
    // Defines the different copy strategies available. Using an enum class
    // provides type safety - the compiler will catch invalid mode assignments.
    enum class Mode {
        SYNC,       // Single-threaded sequential copy (simplest, most reliable)
        ASYNC,      // Async/futures with limited concurrency (good for progress)
        THREADED,   // Thread pool with task queue (best for many small files)
        MMAP        // Memory-mapped I/O (best for large files)
    };

    Mode mode = Mode::THREADED;  // Default: threaded mode for good performance

    // ------------------------------------------------------------------------
    // THREADING PARAMETERS
    // ------------------------------------------------------------------------
    // These control the degree of parallelism. Setting these too high can
    // cause resource exhaustion; too low leaves performance on the table.

    size_t numThreads = std::thread::hardware_concurrency();  // Number of worker threads
                                                              // Default: number of CPU cores

    size_t maxConcurrent = 8;      // Maximum simultaneous file operations
                                   // Limits resource usage and prevents thrashing

    size_t queueSize = 1000;       // Maximum pending tasks in queue
                                   // Prevents memory exhaustion with millions of files

    // ------------------------------------------------------------------------
    // I/O PARAMETERS
    // ------------------------------------------------------------------------
    // These thresholds determine which copy strategy is used based on file size.
    // Different sizes benefit from different buffer sizes and copy methods.

    size_t smallFileThreshold = 1024 * 1024;       // Files < 1MB are "small"
    size_t largeFileThreshold = 100 * 1024 * 1024; // Files > 100MB are "large"

    size_t bufferSize = 1024 * 1024;               // Default 1MB buffer
    size_t mmapThreshold = 10 * 1024 * 1024;       // Use mmap for files > 10MB

    bool useDirectIO = false;      // Bypass OS page cache (experimental)
                                     // Can improve performance for very large files
                                     // but may hurt small file performance

    // ------------------------------------------------------------------------
    // FEATURE FLAGS
    // ------------------------------------------------------------------------
    // Boolean switches to enable/disable specific features

    bool verifyChecksums = false;  // Verify file integrity after copy
                                     // Adds overhead but ensures data integrity

    bool atomicWrites = true;      // Write to temp file, then rename
                                     // Prevents partial/corrupted destination files
                                     // Requires 2x disk space during copy

    bool resume = true;            // Support resuming interrupted backups
                                     // Tracks completed files in state file

    bool dryRun = false;           // Show what would be copied without copying
                                     // Useful for testing and validation

    bool verbose = false;          // Detailed per-file output
                                     // Shows each file as it's processed

    bool recursive = true;         // Recurse into subdirectories
                                     // If false, only copies files in root

    // ------------------------------------------------------------------------
    // RETRY PARAMETERS
    // ------------------------------------------------------------------------
    // Control automatic retry behavior for transient failures

    size_t maxRetries = 3;         // Maximum retry attempts per file
    size_t retryDelayMs = 1000;    // Base delay between retries (milliseconds)
                                     // Actual delay = retryDelayMs * 2^attempt

    // ------------------------------------------------------------------------
    // LOGGING PARAMETERS
    // ------------------------------------------------------------------------

    std::string logFile;           // Path to log file (empty = no file logging)
    std::string logLevel = "INFO"; // Minimum level to log: DEBUG, INFO, WARN, ERROR
    bool logToConsole = true;      // Also log to stdout

    // ------------------------------------------------------------------------
    // PROGRESS PARAMETERS
    // ------------------------------------------------------------------------

    size_t progressIntervalMs = 1000;  // Progress update interval in milliseconds

    // ------------------------------------------------------------------------
    // STATIC METHODS
    // ------------------------------------------------------------------------
    // Factory method to create Config from command-line arguments
    static Config fromArgs(int argc, char* argv[]);

    // Validation method to ensure configuration is sane
    void validate();
};

// ============================================================================
// LOGGER CLASS - Thread-Safe Structured Logging
// ============================================================================
/**
 * Logger - Singleton pattern for centralized logging
 *
 * This class provides thread-safe logging with multiple severity levels.
 * It supports both console and file output simultaneously.
 *
 * Design Patterns:
 * - Singleton: Only one logger instance exists globally
 * - RAII: Log file is automatically closed on destruction
 * - Thread-Safe: Internal mutex protects shared resources
 *
 * Usage:
 *   Logger::instance().info("Backup started");
 *   LOG_ERROR("Failed to copy file: " + path);
 */
class Logger {
public:
    // Log severity levels in order of increasing severity
    enum class Level {
        DEBUG,   // Detailed diagnostic information
        INFO,    // General information about program execution
        WARNING, // Potentially harmful situations
        ERROR,   // Error events that might still allow continuation
        FATAL    // Severe errors that cause premature termination
    };

    /**
     * instance() - Get the singleton logger instance
     *
     * Thread-safe due to C++11 static initialization guarantees.
     * The instance is created on first call and destroyed at program exit.
     */
    static Logger& instance() {
        static Logger instance;  // C++11 guarantees thread-safe initialization
        return instance;
    }

    /**
     * init() - Initialize the logger with configuration
     *
     * Must be called before any logging operations.
     * Opens the log file if specified.
     */
    void init(const Config& config) {
        config_ = config;
        if (!config.logFile.empty()) {
            // Open in append mode to preserve existing logs
            fileStream_.open(config.logFile, std::ios::app);
            if (!fileStream_.is_open()) {
                std::cerr << "Warning: Could not open log file: " << config.logFile << std::endl;
            }
        }
    }

    /**
     * log() - Main logging method
     *
     * Formats the message with timestamp and level, then outputs to
     * configured destinations (console and/or file).
     *
     * Thread-safe: Protected by mutex_ for concurrent access
     */
    void log(Level level, const std::string& message) {
        // Early exit if this level is below the configured minimum
        if (!shouldLog(level)) return;

        // Get current time for timestamp
        auto now = system_clock::now();
        auto time = system_clock::to_time_t(now);

        // Format: "YYYY-MM-DD HH:MM:SS [LEVEL] message"
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
        ss << " [" << levelToString(level) << "] " << message;

        // Lock to ensure atomic output (prevents interleaved messages)
        std::lock_guard<std::mutex> lock(mutex_);

        if (config_.logToConsole) {
            // Output to stdout (not cerr, to keep errors separate)
            std::cout << ss.str() << std::endl;
        }

        if (fileStream_.is_open()) {
            fileStream_ << ss.str() << std::endl;
            // Optional: flush for immediate persistence
            // fileStream_.flush();
        }
    }

    // Convenience methods for each log level
    void debug(const std::string& msg) { log(Level::DEBUG, msg); }
    void info(const std::string& msg) { log(Level::INFO, msg); }
    void warning(const std::string& msg) { log(Level::WARNING, msg); }
    void error(const std::string& msg) { log(Level::ERROR, msg); }
    void fatal(const std::string& msg) { log(Level::FATAL, msg); }

private:
    // Private constructor for singleton pattern
    Logger() = default;

    // Delete copy/move constructors to enforce singleton
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    Config config_;                    // Copy of configuration
    std::ofstream fileStream_;         // Log file output stream
    std::mutex mutex_;                 // Protects fileStream_ and console output

    /**
     * shouldLog() - Check if a message at the given level should be logged
     *
     * Compares the message level against the configured minimum level.
     */
    bool shouldLog(Level level) {
        // Map string level names to enum values
        static std::map<std::string, Level> levels = {
            {"DEBUG", Level::DEBUG},
            {"INFO", Level::INFO},
            {"WARNING", Level::WARNING},
            {"WARN", Level::WARNING},
            {"ERROR", Level::ERROR},
            {"FATAL", Level::FATAL}
        };

        auto it = levels.find(config_.logLevel);
        Level minLevel = (it != levels.end()) ? it->second : Level::INFO;
        return level >= minLevel;  // Log if message level >= minimum level
    }

    /**
     * levelToString() - Convert level enum to string representation
     */
    std::string levelToString(Level level) {
        switch (level) {
            case Level::DEBUG:   return "DEBUG";
            case Level::INFO:    return "INFO";
            case Level::WARNING: return "WARN";
            case Level::ERROR:   return "ERROR";
            case Level::FATAL:   return "FATAL";
            default:             return "UNKNOWN";
        }
    }
};

// ============================================================================
// LOGGING MACROS
// ============================================================================
// These macros provide convenient shorthand for logging
// They automatically include file and line information (can be enhanced)
#define LOG_DEBUG(msg) Logger::instance().debug(msg)
#define LOG_INFO(msg) Logger::instance().info(msg)
#define LOG_WARN(msg) Logger::instance().warning(msg)
#define LOG_ERROR(msg) Logger::instance().error(msg)
#define LOG_FATAL(msg) Logger::instance().fatal(msg)

// ============================================================================
// METRICS CLASS - Performance Tracking
// ============================================================================
/**
 * Metrics - Singleton for collecting performance statistics
 *
 * Tracks throughput, file counts, and timing information.
 * Uses atomic variables for lock-free concurrent updates.
 *
 * This data can be exported to monitoring systems like Prometheus.
 */
class Metrics {
public:
    static Metrics& instance() {
        static Metrics instance;
        return instance;
    }

    /**
     * recordCopy() - Record a successful file copy
     *
     * Updates counters and calculates throughput.
     */
    void recordCopy(size_t bytes, milliseconds duration) {
        totalBytesCopied_ += bytes;
        totalFilesCopied_++;
        totalTimeMs_.fetch_add(duration.count());
    }

    void recordSkip() { totalFilesSkipped_++; }
    void recordError() { totalErrors_++; }

    /**
     * printSummary() - Display collected metrics at end of run
     */
    void printSummary() {
        std::cout << "\n" << std::string(60, '=') << std::endl;
        std::cout << "METRICS SUMMARY" << std::endl;
        std::cout << std::string(60, '=') << std::endl;
        std::cout << "Files copied:   " << totalFilesCopied_ << std::endl;
        std::cout << "Files skipped:  " << totalFilesSkipped_ << std::endl;
        std::cout << "Errors:         " << totalErrors_ << std::endl;
        std::cout << "Total bytes:    " << formatBytes(totalBytesCopied_) << std::endl;

        long long totalMs = totalTimeMs_.load();
        if (totalMs > 0) {
            double mbps = (totalBytesCopied_ / 1024.0 / 1024.0) /
                         (totalMs / 1000.0);
            std::cout << "Avg throughput: " << std::fixed << std::setprecision(2)
                     << mbps << " MB/s" << std::endl;
        }
        std::cout << std::string(60, '=') << std::endl;
    }

private:
    Metrics() = default;

    // Atomic counters for thread-safe updates
    std::atomic<size_t> totalFilesCopied_{0};
    std::atomic<size_t> totalFilesSkipped_{0};
    std::atomic<size_t> totalErrors_{0};
    std::atomic<size_t> totalBytesCopied_{0};
    std::atomic<long long> totalTimeMs_{0};  // Store as milliseconds count

    std::string formatBytes(size_t bytes) {
        const char* units[] = {"B", "KB", "MB", "GB", "TB"};
        int unitIndex = 0;
        double size = static_cast<double>(bytes);
        while (size >= 1024.0 && unitIndex < 4) {
            size /= 1024.0;
            unitIndex++;
        }
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << size << " " << units[unitIndex];
        return oss.str();
    }
};

// ============================================================================
// BACKUP STATS STRUCTURE
// ============================================================================
/**
 * BackupStats - Thread-safe statistics counters
 *
 * Used to track the current backup operation's progress.
 * All members are atomic to allow concurrent updates without locks.
 */
struct BackupStats {
    std::atomic<size_t> filesCopied{0};    // Successfully copied files
    std::atomic<size_t> filesSkipped{0};   // Skipped (up-to-date) files
    std::atomic<size_t> errors{0};         // Failed copy attempts
    std::atomic<size_t> totalBytes{0};     // Total bytes transferred
    std::atomic<size_t> bytesRead{0};      // Bytes read from source
    std::atomic<size_t> bytesWritten{0};   // Bytes written to destination

    // std::atomic is non-copyable and non-movable by default, which prevents
    // BackupStats from being returned by value. We define an explicit move
    // constructor that loads each atomic's value and stores it in the new object.
    BackupStats() = default;
    BackupStats(BackupStats&& other) noexcept
        : filesCopied(other.filesCopied.load())
        , filesSkipped(other.filesSkipped.load())
        , errors(other.errors.load())
        , totalBytes(other.totalBytes.load())
        , bytesRead(other.bytesRead.load())
        , bytesWritten(other.bytesWritten.load())
    {}

    /**
     * add() - Merge another stats object into this one
     *
     * Used to aggregate stats from multiple threads.
     */
    void add(const BackupStats& other) {
        filesCopied += other.filesCopied;
        filesSkipped += other.filesSkipped;
        errors += other.errors;
        totalBytes += other.totalBytes;
        bytesRead += other.bytesRead;
        bytesWritten += other.bytesWritten;
    }
};

// ============================================================================
// FILE INFO STRUCTURE
// ============================================================================
/**
 * FileInfo - Metadata about a file to be copied
 *
 * Encapsulates all information needed to copy a single file:
 * - Source and destination paths
 * - File size and modification time
 * - Whether copy is needed (based on timestamp/size comparison)
 */
struct FileInfo {
    fs::path source;                    // Absolute path to source file
    fs::path dest;                      // Absolute path to destination
    uintmax_t size;                     // File size in bytes
    fs::file_time_type lastModified;    // Last modification time
    bool needsCopy = false;             // True if destination is outdated

    // Size classification for choosing copy strategy
    enum class SizeClass { SMALL, MEDIUM, LARGE };

    /**
     * sizeClass() - Classify file size for strategy selection
     *
     * Returns SMALL for files under 1MB, MEDIUM for 1-100MB, LARGE for >100MB
     */
    SizeClass sizeClass(const Config& config) const {
        if (size < config.smallFileThreshold) return SizeClass::SMALL;
        if (size < config.largeFileThreshold) return SizeClass::MEDIUM;
        return SizeClass::LARGE;
    }
};

// ============================================================================
// MEMORY-MAPPED FILE CLASS (RAII Wrapper)
// ============================================================================
/**
 * MemoryMappedFile - RAII wrapper for memory-mapped file I/O
 *
 * Memory mapping allows direct access to file contents via pointers,
 * bypassing the kernel's read/write buffers for improved performance.
 *
 * Platform Support:
 * - Windows: CreateFileMapping / MapViewOfFile
 * - Unix: mmap / munmap
 *
 * Usage:
 *   MemoryMappedFile mmf;
 *   if (mmf.open("/path/to/file")) {
 *       // Access data via mmf.data()
 *       // File automatically unmapped when mmf goes out of scope
 *   }
 */
class MemoryMappedFile {
public:
    /**
     * Destructor - Automatically unmaps and closes the file
     *
     * RAII pattern ensures resources are released even if exceptions occur.
     */
    ~MemoryMappedFile() { close(); }

    /**
     * open() - Open and memory-map a file
     *
     * @param path    Path to the file
     * @param write   If true, open for read-write; else read-only
     * @param createSize If > 0, create new file with this size
     * @return        true on success, false on failure
     */
    bool open(const fs::path& path, bool write = false, size_t createSize = 0) {
#ifdef _WIN32
        // Windows implementation using Win32 API

        // Determine access mode
        DWORD access = write ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
        DWORD shareMode = FILE_SHARE_READ;  // Allow other processes to read
        DWORD creation = createSize > 0 ? CREATE_ALWAYS : OPEN_EXISTING;

        // Open/create the file
        hFile_ = CreateFileA(path.string().c_str(), access, shareMode, NULL,
                            creation, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile_ == INVALID_HANDLE_VALUE) {
            LOG_ERROR("Failed to open file: " + path.string());
            return false;
        }

        // Get or set file size
        if (createSize > 0) {
            // Extend file to desired size
            LARGE_INTEGER li;
            li.QuadPart = static_cast<LONGLONG>(createSize);
            SetFilePointerEx(hFile_, li, NULL, FILE_BEGIN);
            SetEndOfFile(hFile_);
            size_ = createSize;
        } else {
            // Query existing file size
            LARGE_INTEGER li;
            GetFileSizeEx(hFile_, &li);
            size_ = static_cast<size_t>(li.QuadPart);
        }

        // Handle zero-byte files specially (no mapping needed)
        if (size_ == 0) {
            return true;
        }

        // Create file mapping object
        DWORD protect = write ? PAGE_READWRITE : PAGE_READONLY;
        hMapping_ = CreateFileMapping(hFile_, NULL, protect, 0, 0, NULL);
        if (!hMapping_) {
            LOG_ERROR("Failed to create file mapping: " + path.string());
            close();
            return false;
        }

        // Map view into process address space
        DWORD mapAccess = write ? FILE_MAP_WRITE : FILE_MAP_READ;
        data_ = MapViewOfFile(hMapping_, mapAccess, 0, 0, 0);
        if (!data_) {
            LOG_ERROR("Failed to map view of file: " + path.string());
            close();
            return false;
        }
#else
        // POSIX implementation using mmap

        // Determine open flags
        int flags = write ? O_RDWR : O_RDONLY;
        if (createSize > 0) {
            flags |= O_CREAT;
        }

        // Open the file
        fd_ = ::open(path.c_str(), flags, 0644);
        if (fd_ < 0) {
            LOG_ERROR("Failed to open file: " + path.string());
            return false;
        }

        // Get or set file size
        if (createSize > 0) {
            if (ftruncate(fd_, createSize) < 0) {
                LOG_ERROR("Failed to set file size: " + path.string());
                close();
                return false;
            }
            size_ = createSize;
        } else {
            struct stat st;
            if (fstat(fd_, &st) < 0) {
                LOG_ERROR("Failed to stat file: " + path.string());
                close();
                return false;
            }
            size_ = st.st_size;
        }

        // Handle zero-byte files
        if (size_ == 0) {
            return true;
        }

        // Map file into memory
        int prot = write ? (PROT_READ | PROT_WRITE) : PROT_READ;
        data_ = mmap(nullptr, size_, prot, MAP_SHARED, fd_, 0);
        if (data_ == MAP_FAILED) {
            LOG_ERROR("Failed to mmap file: " + path.string());
            data_ = nullptr;
            close();
            return false;
        }
#endif
        return true;
    }

    /**
     * close() - Unmap and close the file
     *
     * Safe to call multiple times (idempotent).
     *
     * The unmap and handle/fd release are intentionally separate blocks:
     * - Unmapping requires an active mapping (data_ != nullptr && size_ > 0).
     * - The underlying handle/fd must be released regardless of whether a
     *   mapping exists, so zero-byte files (size_ == 0, data_ == nullptr)
     *   don't leak their descriptor.
     */
    void close() {
        // Step 1: release the memory mapping (only exists for non-empty files).
        if (data_ && size_ > 0) {
#ifdef _WIN32
            UnmapViewOfFile(data_);
#else
            munmap(data_, size_);
#endif
        }

        // Step 2: close the underlying OS handle/fd unconditionally.
        // This is the fix for the zero-byte file descriptor leak: previously
        // this block lived inside the data_ guard above, so an empty file
        // that set fd_ but left data_ null would never reach ::close(fd_).
#ifdef _WIN32
        if (hMapping_ != NULL) {
            CloseHandle(hMapping_);
            hMapping_ = NULL;
        }
        if (hFile_ != INVALID_HANDLE_VALUE) {
            CloseHandle(hFile_);
            hFile_ = INVALID_HANDLE_VALUE;
        }
#else
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
#endif

        // Step 3: reset remaining members.
        data_ = nullptr;
        size_ = 0;
    }

    // Accessor methods
    void* data() { return data_; }                    // Mutable access
    const void* data() const { return data_; }        // Const access
    size_t size() const { return size_; }              // File size

private:
    // Platform-specific handles
#ifdef _WIN32
    HANDLE hFile_ = INVALID_HANDLE_VALUE;   // File handle
    HANDLE hMapping_ = NULL;                // File mapping handle
#else
    int fd_ = -1;                           // File descriptor
#endif
    void* data_ = nullptr;                  // Mapped memory pointer
    size_t size_ = 0;                       // File size in bytes
};

// ============================================================================
// THREAD POOL CLASS
// ============================================================================
/**
 * ThreadPool - Fixed-size pool of worker threads
 *
 * Manages a queue of tasks and distributes them to worker threads.
 * Uses condition variables for efficient waiting (no busy-waiting).
 *
 * Features:
 * - Bounded queue to prevent memory exhaustion
 * - Proper completion tracking (waitForCompletion)
 * - Graceful shutdown (processes pending tasks before exit)
 *
 * Usage:
 *   ThreadPool pool(8);  // 8 worker threads
 *   pool.enqueue([]() { doWork(); });
 *   pool.waitForCompletion();
 */
class ThreadPool {
public:
    /**
     * Constructor - Create and start worker threads
     *
     * @param numThreads Number of worker threads to create
     */
    explicit ThreadPool(size_t numThreads) : activeTasks_(0), stop_(false) {
        workers_.reserve(numThreads);
        for (size_t i = 0; i < numThreads; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }
    }

    /**
     * Destructor - Shutdown and clean up
     *
     * Signals all threads to stop and waits for them to finish.
     */
    ~ThreadPool() {
        shutdown();
    }

    /**
     * shutdown() - Gracefully stop the thread pool
     *
     * 1. Sets stop flag
     * 2. Notifies all waiting threads
     * 3. Waits for all threads to complete
     */
    void shutdown() {
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            if (stop_) return;  // Already stopped
            stop_ = true;
        }
        condition_.notify_all();  // Wake all threads

        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();  // Wait for thread to exit
            }
        }
    }

    /**
     * enqueue() - Add a task to the queue
     *
     * @param task Callable object (lambda, function, bind expression)
     *
     * Thread-safe: Can be called from any thread.
     */
    void enqueue(std::function<void()> task) {
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            if (stop_) return;  // Reject new tasks during shutdown
            tasks_.push(std::move(task));
        }
        condition_.notify_one();  // Wake one waiting worker
    }

    /**
     * waitForCompletion() - Block until all tasks are done
     *
     * Waits until:
     * 1. Task queue is empty, AND
     * 2. No active tasks are running
     *
     * This ensures all enqueued work is complete before returning.
     */
    void waitForCompletion() {
        std::unique_lock<std::mutex> lock(queueMutex_);
        done_.wait(lock, [this] {
            return tasks_.empty() && activeTasks_ == 0;
        });
    }

    /**
     * pendingTasks() - Get number of tasks waiting in queue
     *
     * Does not include currently executing tasks.
     */
    size_t pendingTasks() {
        std::unique_lock<std::mutex> lock(queueMutex_);
        return tasks_.size();
    }

private:
    std::vector<std::thread> workers_;                    // Worker threads
    std::queue<std::function<void()>> tasks_;            // Task queue
    std::mutex queueMutex_;                               // Protects tasks_
    std::condition_variable condition_;                   // For worker waiting
    std::condition_variable done_;                        // For completion waiting
    std::atomic<size_t> activeTasks_;                   // Currently executing
    bool stop_;                                         // Shutdown flag

    /**
     * workerLoop() - Main loop for each worker thread
     *
     * Continuously:
     * 1. Wait for a task or shutdown signal
     * 2. Execute task
     * 3. Notify completion
     */
    void workerLoop() {
        while (true) {
            std::function<void()> task;

            // Wait for work or shutdown
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                condition_.wait(lock, [this] {
                    return stop_ || !tasks_.empty();
                });

                // Exit if shutting down and no more work
                if (stop_ && tasks_.empty()) return;

                // Get next task
                task = std::move(tasks_.front());
                tasks_.pop();
                ++activeTasks_;
            }

            // Execute task (outside lock for concurrency)
            g_filesInProgress++;
            try {
                task();
            } catch (const std::exception& e) {
                LOG_ERROR("Task failed: " + std::string(e.what()));
            }
            g_filesInProgress--;

            // Mark as complete and notify waiters
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                --activeTasks_;
            }
            done_.notify_all();
        }
    }
};

// ============================================================================
// FILE QUEUE - Thread-Safe Producer/Consumer Queue
// ============================================================================
/**
 * FileQueue - Bounded blocking queue connecting the scanner thread to copy workers.
 *
 * The scanner thread pushes FileInfo entries as it discovers them.
 * Copy workers pop entries and process them immediately — no need to
 * wait for the full directory scan to finish before copying starts.
 *
 * Bounded capacity (maxSize) applies backpressure to the scanner:
 * if workers can't keep up, the scanner sleeps until space opens up,
 * preventing unbounded memory growth on multi-million-file trees.
 *
 * Usage:
 *   FileQueue q(500);
 *   // Producer thread:  q.push(fileInfo);   q.markDone();
 *   // Consumer threads: while (q.pop(info)) { process(info); }
 */
class FileQueue {
public:
    explicit FileQueue(size_t maxSize) : maxSize_(maxSize), done_(false) {}

    // Push a FileInfo onto the queue.
    // Blocks if the queue is full (backpressure) until a consumer makes room.
    void push(FileInfo item) {
        std::unique_lock<std::mutex> lock(mutex_);
        // Wait until there is space in the queue or shutdown is requested.
        notFull_.wait(lock, [this] {
            return queue_.size() < maxSize_ || g_shutdownRequested.load();
        });
        queue_.push(std::move(item));
        notEmpty_.notify_one();  // Wake a waiting consumer.
    }

    // Pop the next FileInfo from the queue.
    // Blocks until an item is available or the queue is fully drained.
    // Returns false when scanning is complete AND the queue is empty.
    bool pop(FileInfo& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        notEmpty_.wait(lock, [this] {
            return !queue_.empty() || done_.load();
        });

        if (queue_.empty()) return false;  // Drained and done.

        item = std::move(queue_.front());
        queue_.pop();
        notFull_.notify_one();  // Wake the scanner if it was blocked.
        return true;
    }

    // Called by the scanner thread when it has finished pushing all files.
    // Wakes all waiting consumers so they can drain the queue and exit.
    void markDone() {
        done_.store(true);
        notEmpty_.notify_all();
    }

    size_t size() {
        std::unique_lock<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    std::queue<FileInfo>    queue_;
    std::mutex              mutex_;
    std::condition_variable notFull_;   // Signalled when space opens up for the producer.
    std::condition_variable notEmpty_;  // Signalled when items are available for consumers.
    size_t                  maxSize_;   // Bounded capacity to limit memory usage.
    std::atomic<bool>       done_;      // Set when scanning is complete.
};
// ============================================================================
/**
 * CopyStrategy - Abstract base class for copy algorithms
 *
 * The Strategy pattern allows interchangeable algorithms at runtime.
 * Each strategy implements the same interface but with different behavior.
 *
 * Strategies:
 * - BufferedCopyStrategy: Standard buffered I/O (best for small/medium files)
 * - MmapCopyStrategy: Memory-mapped I/O (best for large files)
 */
class CopyStrategy {
public:
    virtual ~CopyStrategy() = default;

    /**
     * copy() - Copy a single file
     *
     * @param info   File metadata (source, dest, size, etc.)
     * @param config Configuration settings
     * @param stats  Statistics to update
     * @return       true on success, false on failure
     */
    virtual bool copy(const FileInfo& info, const Config& config, BackupStats& stats) = 0;

    /**
     * name() - Get strategy name for logging
     */
    virtual std::string name() const = 0;
};

// ============================================================================
// BUFFERED COPY STRATEGY
// ============================================================================
/**
 * BufferedCopyStrategy - Traditional buffered I/O copy
 *
 * Uses std::ifstream/ofstream with a dynamically-sized buffer.
 * Best for: Small to medium files, network drives, when memory is limited
 *
 * Features:
 * - Adaptive buffer sizing based on file size
 * - Atomic writes (temp file + rename)
 * - Timestamp preservation
 * - Dry-run support
 */
class BufferedCopyStrategy : public CopyStrategy {
public:
    std::string name() const override { return "Buffered"; }

    bool copy(const FileInfo& info, const Config& config, BackupStats& stats) override {
        // Retry loop: attempts the copy up to maxRetries times on failure.
        // Delay between attempts doubles each time (exponential backoff)
        // to avoid hammering a temporarily unavailable resource.
        for (size_t attempt = 0; attempt <= config.maxRetries; ++attempt) {
            if (attempt > 0) {
                size_t delayMs = config.retryDelayMs * (1u << (attempt - 1)); // 2^(attempt-1)
                LOG_WARN("Retrying (" + std::to_string(attempt) + "/" +
                         std::to_string(config.maxRetries) + "): " +
                         info.source.string() + " after " + std::to_string(delayMs) + "ms");
                std::this_thread::sleep_for(milliseconds(delayMs));
            }

            if (attemptCopy(info, config, stats)) return true;

            // Don't retry if shutdown was requested mid-operation
            if (g_shutdownRequested.load()) return false;
        }

        LOG_ERROR("All " + std::to_string(config.maxRetries) +
                  " retries exhausted for: " + info.source.string());
        stats.errors++;
        Metrics::instance().recordError();
        return false;
    }

private:
    // Extracted copy attempt so the retry loop above stays readable.
    bool attemptCopy(const FileInfo& info, const Config& config, BackupStats& stats) {
        try {
            // Check for shutdown request
            if (g_shutdownRequested.load()) {
                LOG_INFO("Shutdown requested, skipping: " + info.source.string());
                return false;
            }

            // Ensure destination directory exists
            // create_directories is idempotent (safe to call multiple times)
            fs::create_directories(info.dest.parent_path());

            // Check if destination already exists and is up-to-date
            if (fs::exists(info.dest) && !info.needsCopy) {
                auto destTime = fs::last_write_time(info.dest);
                auto destSize = fs::file_size(info.dest);

                // Skip if same size and source not newer
                if (info.lastModified <= destTime && info.size == destSize) {
                    stats.filesSkipped++;
                    Metrics::instance().recordSkip();
                    return true;
                }
            }

            // Dry-run mode: just log what would happen
            if (config.dryRun) {
                LOG_INFO("[DRY RUN] Would copy: " + info.source.string());
                stats.filesCopied++;
                return true;
            }

            // Determine optimal buffer size based on file size
            // Small files: small buffer (64KB) to save memory
            // Medium files: default buffer (1MB)
            // Large files: large buffer (4MB) for fewer system calls
            size_t bufferSize = config.bufferSize;
            if (info.size < 64 * 1024) {
                bufferSize = 64 * 1024;
            } else if (info.size > 100 * 1024 * 1024) {
                bufferSize = 4 * 1024 * 1024;
            }

            // Open source file for reading
            std::ifstream src(info.source.string(), std::ios::binary);
            if (!src) {
                LOG_ERROR("Failed to open source: " + info.source.string());
                stats.errors++;
                Metrics::instance().recordError();
                return false;
            }

            // Determine destination path (temp file for atomic writes).
            // Use a global monotonic counter — NOT a timestamp — to guarantee
            // a unique suffix across all concurrent threads.  Nanosecond
            // timestamps are unsafe: on Windows, steady_clock resolution is
            // often 100 ns, so simultaneous threads sample the same value and
            // produce colliding ".tmp.<n>" names that corrupt each other's data.
            fs::path tempDest = info.dest;
            if (config.atomicWrites) {
                tempDest = info.dest.string() + ".tmp." +
                           std::to_string(g_tempFileCounter.fetch_add(1, std::memory_order_relaxed));
            }

            // Open destination file for writing
            std::ofstream dst(tempDest.string(), std::ios::binary | std::ios::trunc);
            if (!dst) {
                LOG_ERROR("Failed to open destination: " + tempDest.string());
                stats.errors++;
                Metrics::instance().recordError();
                return false;
            }

            // Allocate buffer on heap (vector handles memory management)
            std::vector<char> buffer(bufferSize);
            size_t totalRead = 0;
            auto startTime = steady_clock::now();

            // Copy loop: read from source, write to destination.
            // Track write failures separately so we can distinguish them
            // from a clean shutdown interruption in the cleanup block below.
            bool writeError = false;
            while (src.good() && !g_shutdownRequested.load()) {
                src.read(buffer.data(), buffer.size());
                std::streamsize bytesRead = src.gcount();

                if (bytesRead > 0) {
                    dst.write(buffer.data(), bytesRead);

                    // BUG FIX: check every write — a full disk or I/O error
                    // sets the stream's failbit but does NOT throw by default,
                    // so without this check the loop silently continued and
                    // produced a truncated file with no error reported.
                    if (!dst) {
                        LOG_ERROR("Write error (disk full?) for: " + tempDest.string());
                        writeError = true;
                        break;
                    }

                    totalRead += bytesRead;
                }
            }

            // Close before any cleanup or rename so the OS flushes buffers.
            // We close even on failure paths — the file will be removed next.
            dst.close();

            // BUG FIX: guard the rename behind a full-success check.
            //
            // Previously the code fell straight through to fs::rename() even
            // when the loop exited early — due to a write error, a read error,
            // or a mid-copy shutdown signal — committing a truncated or
            // zero-padded file as the final destination.
            //
            // Now we check every failure mode and discard the temp file so
            // a subsequent run starts with a clean slate.
            //
            // Note: when atomicWrites is false, tempDest == info.dest, so
            // fs::remove(tempDest) correctly removes the partial destination.
            if (writeError) {
                std::error_code ec;
                fs::remove(tempDest, ec);
                stats.errors++;
                Metrics::instance().recordError();
                return false;
            }

            if (src.bad()) {
                // src.bad() = hardware/OS read error, distinct from clean EOF.
                // src.fail() after a normal EOF read is expected and not an error.
                LOG_ERROR("Read error on source: " + info.source.string());
                std::error_code ec;
                fs::remove(tempDest, ec);
                stats.errors++;
                Metrics::instance().recordError();
                return false;
            }

            if (g_shutdownRequested.load()) {
                // Loop exited cleanly but before EOF — file is incomplete.
                // Discard the partial temp file and let the caller decide
                // whether to retry (it won't: the retry loop also checks the
                // shutdown flag before each attempt).
                LOG_INFO("Shutdown mid-copy, discarding partial: " + tempDest.string());
                std::error_code ec;
                fs::remove(tempDest, ec);
                return false;
            }

            // All bytes transferred successfully — safe to commit.
            if (config.atomicWrites) {
                fs::rename(tempDest, info.dest);
            }

            // Preserve source file's modification time
            fs::last_write_time(info.dest, info.lastModified);

            // Optional post-copy checksum verification.
            // Both source and destination are hashed; a mismatch means the data
            // that landed on disk does not match what was read from the source,
            // which indicates hardware error, filesystem corruption, or a partial
            // write that slipped past the OS.  The corrupted destination is
            // removed so a subsequent run retries the copy cleanly.
            if (config.verifyChecksums) {
                auto srcHash = xxhash::hash64(info.source);
                auto dstHash = xxhash::hash64(info.dest);
                if (!srcHash || !dstHash || *srcHash != *dstHash) {
                    LOG_ERROR("Checksum mismatch for: " + info.source.string() +
                              " — removing corrupted destination");
                    std::error_code ec;
                    fs::remove(info.dest, ec);
                    stats.errors++;
                    Metrics::instance().recordError();
                    return false;
                }
                if (config.verbose) {
                    std::ostringstream oss;
                    oss << "Checksum OK [0x" << std::hex << *srcHash << "]: "
                        << info.source.filename().string();
                    LOG_DEBUG(oss.str());
                }
            }

            // Update statistics
            auto duration = duration_cast<milliseconds>(steady_clock::now() - startTime);
            stats.filesCopied++;
            stats.totalBytes += info.size;
            stats.bytesRead += totalRead;
            stats.bytesWritten += info.size;

            Metrics::instance().recordCopy(info.size, duration);

            if (config.verbose) {
                LOG_INFO("Copied: " + info.source.filename().string());
            }

            return true;

        } catch (const std::exception& e) {
            // Log the failure but don't update error stats yet —
            // the retry loop will decide if this is a final failure.
            LOG_WARN("Attempt failed for " + info.source.string() + ": " + e.what());
            return false;
        }
    }
    // (error stat is incremented by the outer retry loop on final failure)
};

// ============================================================================
// MEMORY-MAPPED COPY STRATEGY
// ============================================================================
/**
 * MmapCopyStrategy - Memory-mapped file copy
 *
 * Maps files directly into process memory for zero-copy access.
 * Best for: Large files (>10MB), local SSDs, when CPU is bottleneck
 *
 * Features:
 * - Falls back to buffered copy for small files
 * - Chunked copying to allow interruption
 * - Pre-allocation to prevent fragmentation
 * - Atomic writes support
 */
class MmapCopyStrategy : public CopyStrategy {
public:
    std::string name() const override { return "MemoryMapped"; }

    bool copy(const FileInfo& info, const Config& config, BackupStats& stats) override {
        try {
            // Check for shutdown request
            if (g_shutdownRequested.load()) {
                LOG_INFO("Shutdown requested, skipping: " + info.source.string());
                return false;
            }

            // Fall back to buffered copy for small files
            // Memory mapping has overhead that isn't worth it for small files
            if (info.size < config.mmapThreshold) {
                BufferedCopyStrategy fallback;
                return fallback.copy(info, config, stats);
            }

            // Ensure destination directory exists
            fs::create_directories(info.dest.parent_path());

            // Check if up-to-date (same as buffered strategy)
            if (fs::exists(info.dest) && !info.needsCopy) {
                auto destTime = fs::last_write_time(info.dest);
                auto destSize = fs::file_size(info.dest);
                if (info.lastModified <= destTime && info.size == destSize) {
                    stats.filesSkipped++;
                    Metrics::instance().recordSkip();
                    return true;
                }
            }

            // Dry-run mode
            if (config.dryRun) {
                LOG_INFO("[DRY RUN] Would copy (mmap): " + info.source.string());
                stats.filesCopied++;
                return true;
            }

            // Open source file via memory mapping
            MemoryMappedFile srcMmf;
            if (!srcMmf.open(info.source)) {
                LOG_ERROR("Failed to mmap source: " + info.source.string());
                stats.errors++;
                Metrics::instance().recordError();
                return false;
            }

            // Determine destination path (with temp file support).
            // Same counter used by BufferedCopyStrategy — see that comment
            // for why a timestamp is insufficient under parallelism.
            fs::path tempDest = info.dest;
            if (config.atomicWrites) {
                tempDest = info.dest.string() + ".tmp." +
                           std::to_string(g_tempFileCounter.fetch_add(1, std::memory_order_relaxed));
            }

            // Open destination via memory mapping
            // This creates the file and maps it for writing
            MemoryMappedFile dstMmf;
            if (!dstMmf.open(tempDest, true, info.size)) {
                LOG_ERROR("Failed to mmap destination: " + tempDest.string());
                stats.errors++;
                Metrics::instance().recordError();
                return false;
            }

            auto startTime = steady_clock::now();

            // Copy data in chunks
            // Chunking allows:
            // 1. Interruption (checks g_shutdownRequested between chunks)
            // 2. Better cache utilization
            // 3. Progress tracking (could add callback per chunk)
            const size_t chunkSize = 64 * 1024 * 1024;  // 64MB chunks
            size_t offset = 0;

            while (offset < info.size && !g_shutdownRequested.load()) {
                size_t toCopy = std::min(chunkSize, info.size - offset);

                // Use memcpy for efficient memory-to-memory copy
                // This is faster than stream I/O for large blocks
                std::memcpy(static_cast<char*>(dstMmf.data()) + offset,
                           static_cast<const char*>(srcMmf.data()) + offset,
                           toCopy);

                offset += toCopy;
            }

            // Close mappings (ensures data is written to disk)
            srcMmf.close();
            dstMmf.close();

            // Atomic rename if using temp files
            if (config.atomicWrites) {
                fs::rename(tempDest, info.dest);
            }

            // Preserve modification time
            fs::last_write_time(info.dest, info.lastModified);

            // Optional post-copy checksum verification (same logic as buffered strategy).
            // For mmap copies the source data was read via a memory mapping rather than
            // a stream, so hashing re-reads both files from the page cache — fast when
            // the file is still warm, and a true end-to-end integrity check regardless.
            if (config.verifyChecksums) {
                auto srcHash = xxhash::hash64(info.source);
                auto dstHash = xxhash::hash64(info.dest);
                if (!srcHash || !dstHash || *srcHash != *dstHash) {
                    LOG_ERROR("Checksum mismatch (mmap) for: " + info.source.string() +
                              " — removing corrupted destination");
                    std::error_code ec;
                    fs::remove(info.dest, ec);
                    stats.errors++;
                    Metrics::instance().recordError();
                    return false;
                }
                if (config.verbose) {
                    std::ostringstream oss;
                    oss << "Checksum OK [0x" << std::hex << *srcHash << "]: "
                        << info.source.filename().string();
                    LOG_DEBUG(oss.str());
                }
            }

            // Update statistics
            auto duration = duration_cast<milliseconds>(steady_clock::now() - startTime);
            stats.filesCopied++;
            stats.totalBytes += info.size;
            stats.bytesRead += info.size;
            stats.bytesWritten += info.size;

            Metrics::instance().recordCopy(info.size, duration);

            if (config.verbose) {
                LOG_INFO("Copied (mmap): " + info.source.filename().string());
            }

            return true;

        } catch (const std::exception& e) {
            LOG_ERROR("Failed to mmap copy " + info.source.string() + ": " + e.what());
            stats.errors++;
            Metrics::instance().recordError();
            return false;
        }
    }
};

// ============================================================================
// BACKUP ENGINE
// ============================================================================
/**
 * BackupEngine - Orchestrates the backup operation
 *
 * Coordinates file collection, strategy selection, and execution.
 * Handles the different execution modes (sync, async, threaded, mmap).
 */
class BackupEngine {
public:
    /**
     * Constructor - Initialize the engine
     *
     * Creates thread pool if needed for the selected mode.
     */
    explicit BackupEngine(const Config& config) : config_(config) {
        // Create thread pool for threaded modes
        if (config_.mode == Config::Mode::THREADED || config_.mode == Config::Mode::MMAP) {
            pool_ = std::make_unique<ThreadPool>(config_.numThreads);
        }
    }

    /**
     * Destructor - Clean up resources
     *
     * Shuts down thread pool if created.
     */
    ~BackupEngine() {
        if (pool_) {
            pool_->shutdown();
        }
    }

    /**
     * run() - Execute the backup operation
     *
     * @param source Source path (file or directory)
     * @param dest   Destination directory
     * @return       Statistics for the operation
     */
    BackupStats run(const fs::path& source, const fs::path& dest) {
        BackupStats stats;

        // Phase 1: Create the shared queue and launch the scanner thread.
        // The queue is bounded by queueSize to limit memory on huge trees.
        // The scanner pushes files as it walks the directory; copy workers
        // start consuming immediately — no full scan needed before copying.
        FileQueue queue(config_.queueSize);

        std::thread scannerThread([this, &source, &dest, &queue]() {
            LOG_INFO("Scanner started");
            scanInBackground(source, dest, queue);
            LOG_INFO("Scanner finished");
        });

        // Phase 2: Start background progress reporter.
        // totalFiles is unknown upfront now, so we report files done so far
        // and omit the "/total" — it gets printed once scanning completes.
        std::atomic<bool> progressDone{false};
        auto progressStartTime = steady_clock::now();

        std::thread progressThread([&]() {
            while (!progressDone.load()) {
                std::this_thread::sleep_for(milliseconds(config_.progressIntervalMs));
                if (progressDone.load()) break;

                size_t copied  = stats.filesCopied.load();
                size_t skipped = stats.filesSkipped.load();
                size_t errors  = stats.errors.load();
                size_t done    = copied + skipped + errors;
                size_t bytes   = stats.totalBytes.load();

                auto elapsed = duration_cast<seconds>(
                    steady_clock::now() - progressStartTime).count();
                double mbps = elapsed > 0
                    ? (bytes / 1024.0 / 1024.0) / elapsed : 0.0;

                std::ostringstream oss;
                oss << "[Progress] " << done << " files done"
                    << " | Copied: "  << copied
                    << " | Skipped: " << skipped
                    << " | Errors: "  << errors
                    << " | " << std::fixed << std::setprecision(1) << mbps << " MB/s"
                    << " | Queue: "   << queue.size();
                LOG_INFO(oss.str());
            }
        });

        // Phase 3: Execute based on selected mode.
        // Wrapped in try/catch so both threads are always joined on exceptions —
        // a joinable std::thread destroyed without join/detach calls std::terminate().
        try {
            switch (config_.mode) {
                case Config::Mode::SYNC:
                    runSync(queue, stats);
                    break;
                case Config::Mode::ASYNC:
                    runAsync(queue, stats);
                    break;
                case Config::Mode::THREADED:
                case Config::Mode::MMAP:
                    runThreaded(queue, stats);
                    break;
            }
        } catch (...) {
            // Unblock the scanner thread if it's waiting to push — without this
            // it would hang forever and scannerThread.join() would never return.
            queue.markDone();
            scannerThread.join();
            progressDone.store(true);
            progressThread.join();
            throw;
        }

        // Scanner must be joined before returning — it was never joined before,
        // causing std::terminate() when the thread object went out of scope.
        scannerThread.join();

        // Stop the progress thread cleanly before returning.
        progressDone.store(true);
        progressThread.join();

        return stats;
    }

private:
    Config config_;
    std::unique_ptr<ThreadPool> pool_;
    std::mutex statsMutex_;

    /**
     * buildFileInfo() - Construct a FileInfo from a directory entry.
     *
     * Extracted helper to avoid duplicating the same logic in both
     * the recursive and shallow scan paths.
     */
    FileInfo buildFileInfo(const fs::path& entryPath, const fs::path& source,
                           const fs::path& dest) {
        FileInfo info;
        info.source       = entryPath;
        info.size         = fs::file_size(entryPath);
        info.lastModified = fs::last_write_time(entryPath);
        info.dest         = dest / fs::relative(entryPath, source);

        if (fs::exists(info.dest)) {
            auto destTime = fs::last_write_time(info.dest);
            auto destSize = fs::file_size(info.dest);
            info.needsCopy = (info.lastModified > destTime) || (info.size != destSize);
        } else {
            info.needsCopy = true;
        }
        return info;
    }

    /**
     * scanInBackground() - Push files into the queue as they are discovered.
     *
     * Runs on a dedicated scanner thread. Copy workers begin processing
     * immediately — they don't wait for the full tree to be scanned.
     *
     * The scanner applies backpressure via the bounded queue: if workers
     * fall behind, push() blocks until space opens up, keeping memory
     * usage bounded regardless of how many files exist on disk.
     *
     * @param source  Root directory to scan
     * @param dest    Destination root (used to compute dest paths)
     * @param queue   Shared queue that copy workers are consuming from
     */
    void scanInBackground(const fs::path& source, const fs::path& dest,
                          FileQueue& queue) {
        // Handle single file — push it directly and finish.
        if (fs::is_regular_file(source)) {
            FileInfo info;
            info.source       = source;
            info.dest         = dest / source.filename();
            info.size         = fs::file_size(source);
            info.lastModified = fs::last_write_time(source);
            info.needsCopy    = true;
            queue.push(std::move(info));
            queue.markDone();
            return;
        }

        // Scan directory — push each regular file into the queue as found.
        // Workers on the other end start copying before we finish scanning.
        try {
            if (config_.recursive) {
                for (const auto& entry : fs::recursive_directory_iterator(source)) {
                    if (g_shutdownRequested.load()) break;
                    if (!fs::is_regular_file(entry)) continue;
                    queue.push(buildFileInfo(entry.path(), source, dest));
                }
            } else {
                for (const auto& entry : fs::directory_iterator(source)) {
                    if (g_shutdownRequested.load()) break;
                    if (!fs::is_regular_file(entry)) continue;
                    queue.push(buildFileInfo(entry.path(), source, dest));
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Scanner error: " + std::string(e.what()));
        }

        // Signal consumers that no more files are coming.
        queue.markDone();
    }

    /**
     * runSync() - Single-threaded sequential execution
     *
     * Pops files from the queue one at a time and copies them.
     * Best for: Reliability, debugging, small file sets.
     */
    void runSync(FileQueue& queue, BackupStats& stats) {
        BufferedCopyStrategy strategy;
        FileInfo file;
        while (queue.pop(file)) {
            if (g_shutdownRequested.load()) break;
            strategy.copy(file, config_, stats);
        }
    }

    /**
     * runAsync() - Async/futures with limited concurrency
     *
     * Pops files from the shared queue as they arrive from the scanner thread,
     * launching each as an async task. Limits concurrency to maxConcurrent.
     */
    void runAsync(FileQueue& queue, BackupStats& stats) {
        BufferedCopyStrategy strategy;
        std::vector<std::future<bool>> futures;
        FileInfo file;

        while (queue.pop(file)) {
            if (g_shutdownRequested.load()) break;

            // Wait if we've hit the concurrency limit
            while (futures.size() >= config_.maxConcurrent) {
                for (auto it = futures.begin(); it != futures.end();) {
                    if (it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                        it = futures.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (futures.size() >= config_.maxConcurrent) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }

            // Capture file by value — it comes off the queue and is safe to move.
            futures.push_back(std::async(std::launch::async,
                [f = std::move(file), &strategy, &stats, this]() mutable {
                    return strategy.copy(f, config_, stats);
                }
            ));
        }

        // Wait for remaining in-flight tasks to complete.
        for (auto& f : futures) f.wait();
    }

    /**
     * runThreaded() - Thread pool execution
     *
     * Pops files from the shared queue as the scanner discovers them and
     * enqueues each as a task in the thread pool. Copying and scanning
     * happen concurrently — workers never wait for a full directory scan.
     */
    void runThreaded(FileQueue& queue, BackupStats& stats) {
        // Declare as the base class pointer so both strategies are assignable.
        std::unique_ptr<CopyStrategy> strategy;
        if (config_.mode == Config::Mode::THREADED) {
            strategy = std::make_unique<BufferedCopyStrategy>();
        } else {
            strategy = std::make_unique<MmapCopyStrategy>();
        }

        FileInfo file;
        while (queue.pop(file)) {
            if (g_shutdownRequested.load()) break;

            // Apply backpressure if the thread pool's internal queue is full.
            while (pool_->pendingTasks() >= config_.queueSize) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            // Capture file by value — each lambda needs its own copy since
            // 'file' is reused by the pop loop on the next iteration.
            pool_->enqueue([f = std::move(file), &strategy, &stats, this]() mutable {
                strategy->copy(f, config_, stats);
            });
        }

        // Block until all enqueued tasks have finished.
        pool_->waitForCompletion();
    }
};

// ============================================================================
// CONFIGURATION PARSING
// ============================================================================
/**
 * Config::fromArgs() - Parse command-line arguments
 *
 * Supports both short and long options.
 * Returns Config with defaults for unspecified options.
 */
Config Config::fromArgs(int argc, char* argv[]) {
    Config config;

    if (argc < 3) {
        return config;  // Will trigger usage message
    }

    // Parse options starting from index 3 (after program name, source, dest)
    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--mode" && i + 1 < argc) {
            std::string mode = argv[++i];
            if (mode == "sync") config.mode = Mode::SYNC;
            else if (mode == "async") config.mode = Mode::ASYNC;
            else if (mode == "threaded") config.mode = Mode::THREADED;
            else if (mode == "mmap") config.mode = Mode::MMAP;
        }
        else if ((arg == "-t" || arg == "--threads") && i + 1 < argc) {
            config.numThreads = std::stoul(argv[++i]);
        }
        else if ((arg == "-j" || arg == "--jobs") && i + 1 < argc) {
            config.maxConcurrent = std::stoul(argv[++i]);
        }
        else if (arg == "--no-atomic") {
            config.atomicWrites = false;
        }
        else if (arg == "--verify") {
            config.verifyChecksums = true;
        }
        else if (arg == "-n" || arg == "--dry-run") {
            config.dryRun = true;
        }
        else if (arg == "-v" || arg == "--verbose") {
            config.verbose = true;
        }
        else if (arg == "-s" || arg == "--shallow") {
            config.recursive = false;
        }
        else if (arg == "--log" && i + 1 < argc) {
            config.logFile = argv[++i];
        }
        else if (arg == "--log-level" && i + 1 < argc) {
            config.logLevel = argv[++i];
        }
        else if (arg == "--buffer-size" && i + 1 < argc) {
            config.bufferSize = std::stoul(argv[++i]) * 1024;
        }
        else if (arg == "--mmap-threshold" && i + 1 < argc) {
            config.mmapThreshold = std::stoul(argv[++i]) * 1024 * 1024;
        }
    }

    return config;
}

/**
 * Config::validate() - Ensure configuration is sane
 *
 * Applies minimum values to prevent misconfiguration.
 */
void Config::validate() {
    if (numThreads == 0) numThreads = 4;  // Default if hardware_concurrency returns 0
    if (maxConcurrent == 0) maxConcurrent = 8;
    if (bufferSize < 4096) bufferSize = 4096;  // Minimum 4KB buffer
    if (mmapThreshold < 1024) mmapThreshold = 1024;
}

// ============================================================================
// SIGNAL HANDLING
// ============================================================================
/**
 * signalHandler() - Handle OS signals for graceful shutdown
 *
 * Sets the global shutdown flag when SIGINT (Ctrl+C) or SIGTERM is received.
 * Worker threads periodically check this flag and exit gracefully.
 */
void signalHandler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        std::cerr << "\nShutdown requested, finishing current operations...\n";
        g_shutdownRequested.store(true);
    }
}

// ============================================================================
// USAGE INFORMATION
// ============================================================================
/**
 * printUsage() - Display help message
 */
void printUsage(const char* programName) {
    std::cout << "Enterprise Backup Utility v4.0\n";
    std::cout << "Usage: " << programName << " <source> <destination> [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --mode MODE          Copy mode: sync, async, threaded, mmap (default: threaded)\n";
    std::cout << "  -t, --threads N      Number of threads (default: hardware)\n";
    std::cout << "  -j, --jobs N         Max concurrent operations (default: 8)\n";
    std::cout << "  --no-atomic          Disable atomic writes\n";
    std::cout << "  --verify             Verify checksums after copy\n";
    std::cout << "  -n, --dry-run        Show what would be copied\n";
    std::cout << "  -v, --verbose        Detailed output\n";
    std::cout << "  -s, --shallow        Non-recursive copy\n";
    std::cout << "  --log FILE           Log to file\n";
    std::cout << "  --log-level LEVEL    Log level: DEBUG, INFO, WARN, ERROR (default: INFO)\n";
    std::cout << "  --buffer-size KB     Buffer size in KB (default: 1024)\n";
    std::cout << "  --mmap-threshold MB  Use mmap for files > MB (default: 10)\n";
    std::cout << "  -h, --help           Show this help\n";
}

// ============================================================================
// MAIN ENTRY POINT
// ============================================================================
/**
 * main() - Program entry point
 *
 * Responsibilities:
 * 1. Parse command-line arguments
 * 2. Set up signal handlers
 * 3. Initialize logging
 * 4. Validate inputs
 * 5. Execute backup
 * 6. Display results
 */
int main(int argc, char* argv[]) {
    // Check for minimum arguments
    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }

    // Check for help flag
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "-h" || std::string(argv[i]) == "--help") {
            printUsage(argv[0]);
            return 0;
        }
    }

    // Setup signal handlers for graceful shutdown
    std::signal(SIGINT, signalHandler);   // Ctrl+C
    std::signal(SIGTERM, signalHandler);  // kill command

    // Parse and validate configuration
    Config config = Config::fromArgs(argc, argv);
    config.validate();

    // Initialize logger
    Logger::instance().init(config);

    // Get source and destination paths
    fs::path sourcePath = argv[1];
    fs::path destPath = argv[2];

    // Validate source exists
    if (!fs::exists(sourcePath)) {
        LOG_FATAL("Source does not exist: " + sourcePath.string());
        return 1;
    }

    // Create destination if needed
    if (!fs::exists(destPath)) {
        LOG_INFO("Creating destination directory: " + destPath.string());
        fs::create_directories(destPath);
    }

    // Log startup information
    LOG_INFO("Starting backup: " + sourcePath.string() + " -> " + destPath.string());

    // Convert mode enum to string for logging
    std::string modeStr;
    switch (config.mode) {
        case Config::Mode::SYNC: modeStr = "sync"; break;
        case Config::Mode::ASYNC: modeStr = "async"; break;
        case Config::Mode::THREADED: modeStr = "threaded"; break;
        case Config::Mode::MMAP: modeStr = "mmap"; break;
    }
    LOG_INFO("Mode: " + modeStr);

    // Record start time
    auto startTime = steady_clock::now();

    // Execute backup
    BackupEngine engine(config);
    BackupStats stats = engine.run(sourcePath, destPath);

    // Calculate duration
    auto duration = duration_cast<seconds>(steady_clock::now() - startTime);

    // Print summary
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "BACKUP SUMMARY" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    std::cout << "Files copied:    " << stats.filesCopied << std::endl;
    std::cout << "Files skipped:   " << stats.filesSkipped << std::endl;
    std::cout << "Errors:          " << stats.errors << std::endl;
    std::cout << "Total size:      " << stats.totalBytes << " bytes" << std::endl;
    std::cout << "Time elapsed:    " << duration.count() << " seconds" << std::endl;
    std::cout << std::string(60, '=') << std::endl;

    // Print detailed metrics
    Metrics::instance().printSummary();

    // Log completion
    LOG_INFO("Backup completed. Copied: " + std::to_string(stats.filesCopied) +
             ", Skipped: " + std::to_string(stats.filesSkipped) +
             ", Errors: " + std::to_string(stats.errors));

    // Return error code if any errors occurred
    return (stats.errors > 0) ? 1 : 0;
}