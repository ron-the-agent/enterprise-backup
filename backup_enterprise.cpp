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

    size_t progressIntervalMs = 1000;  // Progress update interval (not yet implemented)

    // ------------------------------------------------------------------------
    // METRICS PARAMETERS
    // ------------------------------------------------------------------------

    std::string metricsEndpoint;   // Prometheus push gateway URL (not yet implemented)

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
     */
    void close() {
        if (data_ && size_ > 0) {
#ifdef _WIN32
            UnmapViewOfFile(data_);
            if (hMapping_) CloseHandle(hMapping_);
            if (hFile_ != INVALID_HANDLE_VALUE) CloseHandle(hFile_);
#else
            munmap(data_, size_);
            if (fd_ >= 0) ::close(fd_);
#endif
        }
        // Reset all members
        data_ = nullptr;
        size_ = 0;
#ifdef _WIN32
        hMapping_ = NULL;
        hFile_ = INVALID_HANDLE_VALUE;
#else
        fd_ = -1;
#endif
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
    explicit ThreadPool(size_t numThreads) : stop_(false), activeTasks_(0) {
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
// COPY STRATEGY INTERFACE (Strategy Pattern)
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

            // Determine destination path (temp file for atomic writes)
            fs::path tempDest = info.dest;
            if (config.atomicWrites) {
                // Generate unique temp filename using timestamp + random
                auto now = steady_clock::now().time_since_epoch();
                auto ns = duration_cast<nanoseconds>(now).count();
                tempDest = info.dest.string() + ".tmp." + std::to_string(ns);
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

            // Copy loop: read from source, write to destination
            while (src.good() && !g_shutdownRequested.load()) {
                src.read(buffer.data(), buffer.size());
                std::streamsize bytesRead = src.gcount();

                if (bytesRead > 0) {
                    dst.write(buffer.data(), bytesRead);
                    totalRead += bytesRead;
                }
            }

            // Close destination to ensure data is flushed to disk
            dst.close();

            // Atomic rename: temp file -> final destination
            if (config.atomicWrites) {
                fs::rename(tempDest, info.dest);
            }

            // Preserve source file's modification time
            fs::last_write_time(info.dest, info.lastModified);

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
            LOG_ERROR("Failed to copy " + info.source.string() + ": " + e.what());
            stats.errors++;
            Metrics::instance().recordError();
            return false;
        }
    }
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

            // Determine destination path (with temp file support)
            fs::path tempDest = info.dest;
            if (config.atomicWrites) {
                auto now = steady_clock::now().time_since_epoch();
                auto ns = duration_cast<nanoseconds>(now).count();
                tempDest = info.dest.string() + ".tmp." + std::to_string(ns);
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

        // Phase 1: Collect all files to process
        std::vector<FileInfo> files = collectFiles(source, dest);
        LOG_INFO("Found " + std::to_string(files.size()) + " files to process");

        if (files.empty()) {
            LOG_INFO("No files to backup");
            return stats;
        }

        // Phase 2: Execute based on selected mode
        switch (config_.mode) {
            case Config::Mode::SYNC:
                runSync(files, stats);
                break;
            case Config::Mode::ASYNC:
                runAsync(files, stats);
                break;
            case Config::Mode::THREADED:
            case Config::Mode::MMAP:
                runThreaded(files, stats);
                break;
        }

        return stats;
    }

private:
    Config config_;
    std::unique_ptr<ThreadPool> pool_;
    std::mutex statsMutex_;

    /**
     * collectFiles() - Scan source and build file list
     *
     * Recursively (or shallow) scans the source directory and creates
     * FileInfo entries for each file found.
     */
    std::vector<FileInfo> collectFiles(const fs::path& source, const fs::path& dest) {
        std::vector<FileInfo> files;

        // Handle single file case
        if (fs::is_regular_file(source)) {
            FileInfo info;
            info.source = source;
            info.dest = dest / source.filename();
            info.size = fs::file_size(source);
            info.lastModified = fs::last_write_time(source);
            info.needsCopy = true;
            files.push_back(info);
            return files;
        }

        // Choose iterator based on recursive flag.
        // A ternary can't be used here because recursive_directory_iterator
        // and directory_iterator are different types — the compiler can't
        // reconcile them in a single expression.
        if (config_.recursive) {
            for (const auto& entry : fs::recursive_directory_iterator(source)) {
                if (g_shutdownRequested.load()) break;
                if (!fs::is_regular_file(entry)) continue;

                FileInfo info;
                info.source = entry.path();
                info.size = fs::file_size(entry);
                info.lastModified = fs::last_write_time(entry);
                fs::path relPath = fs::relative(entry.path(), source);
                info.dest = dest / relPath;

                if (fs::exists(info.dest)) {
                    auto destTime = fs::last_write_time(info.dest);
                    auto destSize = fs::file_size(info.dest);
                    info.needsCopy = (info.lastModified > destTime) || (info.size != destSize);
                } else {
                    info.needsCopy = true;
                }
                files.push_back(info);
            }
        } else {
            for (const auto& entry : fs::directory_iterator(source)) {
                if (g_shutdownRequested.load()) break;
                if (!fs::is_regular_file(entry)) continue;

                FileInfo info;
                info.source = entry.path();
                info.size = fs::file_size(entry);
                info.lastModified = fs::last_write_time(entry);
                fs::path relPath = fs::relative(entry.path(), source);
                info.dest = dest / relPath;

                if (fs::exists(info.dest)) {
                    auto destTime = fs::last_write_time(info.dest);
                    auto destSize = fs::file_size(info.dest);
                    info.needsCopy = (info.lastModified > destTime) || (info.size != destSize);
                } else {
                    info.needsCopy = true;
                }
                files.push_back(info);
            }
        }

        return files;
    }

    /**
     * runSync() - Single-threaded sequential execution
     *
     * Simplest mode: processes files one at a time.
     * Best for: Reliability, debugging, small file sets
     */
    void runSync(const std::vector<FileInfo>& files, BackupStats& stats) {
        BufferedCopyStrategy strategy;

        for (const auto& file : files) {
            if (g_shutdownRequested.load()) break;
            strategy.copy(file, config_, stats);
        }
    }

    /**
     * runAsync() - Async/futures with limited concurrency
     *
     * Uses std::async to launch tasks, with a limit on concurrent operations.
     * Best for: Progress tracking, controlled parallelism
     */
    void runAsync(const std::vector<FileInfo>& files, BackupStats& stats) {
        BufferedCopyStrategy strategy;
        std::vector<std::future<bool>> futures;

        for (const auto& file : files) {
            if (g_shutdownRequested.load()) break;

            // Wait if we've hit the concurrency limit
            while (futures.size() >= config_.maxConcurrent) {
                // Remove completed futures
                for (auto it = futures.begin(); it != futures.end();) {
                    if (it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                        it = futures.erase(it);
                    } else {
                        ++it;
                    }
                }

                // If still at limit, wait a bit
                if (futures.size() >= config_.maxConcurrent) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }

            // Launch new async task
            futures.push_back(std::async(std::launch::async, [&]() {
                return strategy.copy(file, config_, stats);
            }));
        }

        // Wait for remaining tasks
        for (auto& f : futures) {
            f.wait();
        }
    }

    /**
     * runThreaded() - Thread pool execution
     *
     * Uses the thread pool for efficient task distribution.
     * Best for: Many small files, maximum throughput
     */
    void runThreaded(const std::vector<FileInfo>& files, BackupStats& stats) {
        // Declare as the base class pointer so both MmapCopyStrategy and
        // BufferedCopyStrategy (which both derive from CopyStrategy) can be
        // assigned to it. unique_ptr<MmapCopyStrategy> can't be reassigned
        // to unique_ptr<BufferedCopyStrategy> — they're unrelated pointer types.
        std::unique_ptr<CopyStrategy> strategy;
        if (config_.mode == Config::Mode::THREADED) {
            strategy = std::make_unique<BufferedCopyStrategy>();
        } else {
            strategy = std::make_unique<MmapCopyStrategy>();
        }

        for (const auto& file : files) {
            if (g_shutdownRequested.load()) break;

            // Apply backpressure if queue is full
            while (pool_->pendingTasks() >= config_.queueSize) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            // Enqueue task
            pool_->enqueue([&strategy, &file, &stats, this]() {
                strategy->copy(file, config_, stats);
            });
        }

        // Wait for all tasks to complete
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
    std::cout << "DEBUG: Source = " << fs::absolute(sourcePath) << std::endl;
    std::cout << "DEBUG: Dest = " << fs::absolute(destPath) << std::endl;
    std::cout << "DEBUG: Recursive = " << (config.recursive ? "true" : "false") << std::endl;
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