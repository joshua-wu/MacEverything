#pragma once
#include "FileRecord.h"
#include <vector>
#include <queue>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <string>

struct InodeKey {
    dev_t dev;
    uint64_t ino;
    bool operator==(const InodeKey& other) const {
        return dev == other.dev && ino == other.ino;
    }
};

struct InodeKeyHash {
    size_t operator()(const InodeKey& k) const {
        size_t h1 = std::hash<int32_t>{}(k.dev);
        size_t h2 = std::hash<uint64_t>{}(k.ino);
        return h1 ^ (h2 * 0x9e3779b97f4a7c15ULL + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

class DirectoryScanner {
public:
    struct Stats {
        std::atomic<uint64_t> fileCount{0};
        std::atomic<uint64_t> dirCount{0};
        std::atomic<uint64_t> symlinkCount{0};
        std::atomic<uint64_t> otherCount{0};
        std::atomic<uint64_t> errorCount{0};
    };

    void scan(const std::string& rootPath);
    void cancel() { cancelled_.store(true, std::memory_order_relaxed); }
    bool isCancelled() const { return cancelled_.load(std::memory_order_relaxed); }
    // True when scan() refused to run because the root path was empty or
    // unreachable (e.g. a volume unmounted after the rescan was scheduled).
    // Callers MUST NOT reconcile (tombstone) against results from a failed scan.
    bool hasFailed() const { return failed_.load(std::memory_order_relaxed); }
    const Stats& getStats() const { return stats_; }

    // Move all per-thread results into a single flat vector. Call after scan completes.
    std::vector<FileRecord> takeResults();

private:
    std::queue<std::string> workQueue_;
    std::mutex queueMutex_;
    std::condition_variable queueCV_;
    std::atomic<int> activeTasks_{0};
    std::atomic<bool> done_{false};
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> failed_{false};

    std::unordered_set<InodeKey, InodeKeyHash> visitedDirs_;
    std::mutex dedupMutex_;
    dev_t rootDevId_{0};  // device ID of root path — skip cross-mount directories

    std::vector<std::vector<FileRecord>> threadResults_;
    Stats stats_;

    void workerThread(int threadIndex);
    void scanDirectory(const std::string& dirPath, char* buffer, int threadIndex);
    bool tryVisitDirectory(dev_t dev, uint64_t ino);
};
