#pragma once
#include "FileRecord.h"
#include <string>
#include <vector>
#include <mutex>
#include <cstdio>
#include <cstdint>
#include <atomic>

enum class WALOp : uint8_t {
    Add    = 1,
    Remove = 2,
    Update = 3
};

struct WALEntry {
    WALOp op;
    std::string fullPath;
    FileRecord record; // populated for Add/Update, empty for Remove
};

class IndexWAL {
public:
    IndexWAL() = default;
    ~IndexWAL();

    IndexWAL(const IndexWAL&) = delete;
    IndexWAL& operator=(const IndexWAL&) = delete;

    /// Open WAL file for appending. Creates if not exists.
    bool open(const std::string& walPath);

    /// Append a mutation entry. Thread-safe.
    /// Data is flushed to OS buffers but fsync is deferred (batched).
    bool append(WALOp op, const std::string& fullPath, const FileRecord& record = {});

    /// Force fsync to disk. Call when you need durability guarantees.
    void sync();

    /// Read all valid entries from a WAL file (stops at first corrupt entry).
    static std::vector<WALEntry> readAll(const std::string& walPath);

    /// Number of entries written since open.
    uint64_t entryCount() const { return entryCount_; }

    /// True if append() has been called since the last clearDirty().
    bool isDirty() const { return dirty_.load(std::memory_order_relaxed); }
    void clearDirty() { dirty_.store(false, std::memory_order_relaxed); }

    /// Set the number of entries between automatic fsyncs (0 = never auto-fsync).
    void setSyncInterval(uint64_t entries) { syncInterval_ = entries; }

    /// Current WAL file size in bytes.
    size_t currentSize() const { return currentSize_; }

    /// Close the WAL file (fsyncs pending data before closing).
    void close();

    /// Close and delete the WAL file.
    void closeAndDelete();

    /// Update the internal path after an external rename (e.g., compact rename).
    void updatePath(const std::string& newPath);

    bool isOpen() const { return file_ != nullptr; }

    /// CRC32 utility (ISO 3309 / zlib polynomial).
    static uint32_t crc32(const void* data, size_t len) { return crc32Update(0, data, len); }
    /// Continue a CRC: crc32Update(crc32(a), b) == crc32(a + b).
    static uint32_t crc32Update(uint32_t prev, const void* data, size_t len);

    /// Maximum WAL file size (50 MB). append() returns false when exceeded.
    static constexpr size_t kMaxWALSize = 50 * 1024 * 1024;

    /// H-3: WAL file header constants for format identification and versioning
    static constexpr uint32_t kMagic   = 0x57414C31; // "WAL1"
    static constexpr uint32_t kVersion = 1;

private:
    FILE* file_ = nullptr;
    std::string path_;
    uint64_t entryCount_ = 0;
    uint64_t unflushedCount_ = 0;
    uint64_t syncInterval_ = 64;  // fsync every 64 entries by default
    size_t currentSize_ = 0;
    std::atomic<bool> dirty_{false};
    std::mutex mutex_;

    static bool writeRecord(FILE* f, const FileRecord& record);
    static bool readRecord(FILE* f, FileRecord& record);
};
