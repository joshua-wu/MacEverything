#include "ServiceEngine.h"
#include "DirectoryScanner.h"
#include "NetworkMounts.h"
#include "RescanDebounce.h"
#include "Logger.h"
#include <sys/stat.h>

// ═══════════════════════════════════════════════════════
//  FSEvents application (replay path)
// ═══════════════════════════════════════════════════════

void ServiceEngine::applyFSEvents(
    const std::vector<FileSystemWatcher::Event>& events,
    std::shared_ptr<SearchEngine> engine)
{
    // Collect all mutation ops first, then apply in a single batch lock
    std::vector<SearchEngine::MutationOp> ops;
    ops.reserve(events.size());

    // Content index updates are collected separately (they use their own lock)
    std::vector<std::pair<std::string, bool>> contentUpdates; // path, isRemove

    for (const auto& event : events) {
        const std::string& path = event.path;
        FSEventStreamEventFlags flags = event.flags;

        if (isInsideAppBundle(path)) continue;

        bool itemRemoved = (flags & kFSEventStreamEventFlagItemRemoved) != 0;
        bool itemRenamed = (flags & kFSEventStreamEventFlagItemRenamed) != 0;

        struct stat st;
        bool exists = (lstat(path.c_str(), &st) == 0);

        if (itemRemoved || (itemRenamed && !exists)) {
            contentUpdates.push_back({path, true});
            ops.push_back({SearchEngine::MutationOp::REMOVE, path, {}});
        } else if (exists) {
            std::string dirPath, fileName;
            size_t lastSlash = path.rfind('/');
            if (lastSlash != std::string::npos) {
                dirPath = path.substr(0, lastSlash);
                fileName = path.substr(lastSlash + 1);
            } else {
                dirPath = ".";
                fileName = path;
            }
            if (fileName.empty()) continue;

            uint8_t type = 4;
            if (S_ISREG(st.st_mode))       type = 1;
            else if (S_ISDIR(st.st_mode))   type = pathEndsWithApp(path) ? 5 : 2;
            else if (S_ISLNK(st.st_mode))   type = 3;

            FileRecord record;
            record.name = fileName;
            record.path = dirPath;
            record.type = type;
            record.size = S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0;
            record.modTime = st.st_mtime;
            record.inode = st.st_ino;
            record.devId = static_cast<int32_t>(st.st_dev);

            ops.push_back({SearchEngine::MutationOp::UPDATE, path, std::move(record)});

            if (type == 1) {
                contentUpdates.push_back({path, false});
            }
        }
    }

    // Apply content index updates (uses its own lock)
    for (const auto& [path, isRemove] : contentUpdates) {
        updateContentForPath(path, isRemove, engine);
    }

    // Apply all search engine mutations in a single lock acquisition
    engine->batchMutate(std::move(ops));
}

// ═══════════════════════════════════════════════════════
//  Live monitoring
// ═══════════════════════════════════════════════════════

void ServiceEngine::startMonitoring() {
    if (isMonitoring_.load(std::memory_order_relaxed)) return;

    std::string root = config_.scanRoot;

    // Exclude app's own cache directory from FSEvents
    std::string cacheExclusion = config_.cachePath;
    if (cacheExclusion.empty()) {
        const char* home = std::getenv("HOME");
        if (home) cacheExclusion = std::string(home) + "/Library/Caches/com.maceverything.app";
    }
    if (!cacheExclusion.empty()) {
        watcher_->setExclusionPaths({cacheExclusion});
    }

    watcher_->start(root, [this](std::vector<FileSystemWatcher::Event> events) {
        if (shuttingDown_.load(std::memory_order_relaxed)) return;

        auto engine = safeEngine();
        if (!engine) return;

        std::vector<std::string> rescanDirs;
        std::vector<SearchEngine::MutationOp> ops;
        ops.reserve(events.size());
        std::vector<std::pair<std::string, bool>> contentUpdates;
        bool changed = false;

        for (const auto& event : events) {
            const std::string& path = event.path;
            FSEventStreamEventFlags flags = event.flags;

            if (isInsideAppBundle(path)) continue;

            if (flags & kFSEventStreamEventFlagMustScanSubDirs) {
                rescanDirs.push_back(path);
                continue;
            }

            bool itemRemoved = (flags & kFSEventStreamEventFlagItemRemoved) != 0;
            bool itemRenamed = (flags & kFSEventStreamEventFlagItemRenamed) != 0;

            struct stat st;
            bool exists = (lstat(path.c_str(), &st) == 0);

            if (itemRemoved || (itemRenamed && !exists)) {
                contentUpdates.push_back({path, true});
                ops.push_back({SearchEngine::MutationOp::REMOVE, path, {}});
                changed = true;
            } else if (exists) {
                std::string dirPath, fileName;
                size_t lastSlash = path.rfind('/');
                if (lastSlash != std::string::npos) {
                    dirPath = path.substr(0, lastSlash);
                    fileName = path.substr(lastSlash + 1);
                } else {
                    dirPath = ".";
                    fileName = path;
                }
                if (fileName.empty()) continue;

                uint8_t type = 4;
                if (S_ISREG(st.st_mode))       type = 1;
                else if (S_ISDIR(st.st_mode))   type = pathEndsWithApp(fileName) ? 5 : 2;
                else if (S_ISLNK(st.st_mode))   type = 3;

                FileRecord record;
                record.name = fileName;
                record.path = dirPath;
                record.type = type;
                record.size = S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0;
                record.modTime = st.st_mtime;
                record.inode = st.st_ino;
                record.devId = static_cast<int32_t>(st.st_dev);

                ops.push_back({SearchEngine::MutationOp::UPDATE, path, std::move(record)});
                changed = true;

                if (type == 1) {
                    contentUpdates.push_back({path, false});
                }
            }
        }

        // Apply content index updates (uses its own lock)
        for (const auto& [path, isRemove] : contentUpdates) {
            updateContentForPath(path, isRemove, engine);
        }

        // Apply all search engine mutations in a single lock acquisition
        engine->batchMutate(std::move(ops));

        if (!rescanDirs.empty()) {
            scheduleRescanForPaths(rescanDirs);
        }

        if (changed && onIndexChanged) {
            onIndexChanged();
        }
    });

    isMonitoring_.store(true, std::memory_order_relaxed);

    // Periodic network volume rescan: FSEvents is unreliable on network
    // filesystems (especially WebDAV), so mounted SMB/NFS/WebDAV volumes are
    // re-reconciled on a slow cadence to keep their index entries fresh. The
    // debounce + throttle pipeline absorbs collisions with event-driven
    // rescans, and rescanSubtree skips reconciliation when a volume is
    // unreachable, so an offline NAS can't wipe its own index entries.
    if (!config_.scanNetworkVolumes) return;

    networkRescanTimer_ = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_TIMER, 0, 0, mutationQueue_);
    dispatch_source_set_timer(networkRescanTimer_,
        dispatch_time(DISPATCH_TIME_NOW,
            static_cast<uint64_t>(kNetworkRescanInitialDelaySec * NSEC_PER_SEC)),
        static_cast<uint64_t>(kNetworkRescanIntervalSec * NSEC_PER_SEC),
        60 * NSEC_PER_SEC);
    dispatch_source_set_event_handler(networkRescanTimer_, ^{
        if (shuttingDown_.load(std::memory_order_relaxed)) return;
        auto mounts = networkMountPoints();
        if (mounts.empty()) return;
        LOG_INFO("FSWatcher", "Periodic network volume rescan: "
                 << mounts.size() << " mount(s)");
        scheduleRescanForPaths(mounts);
    });
    dispatch_resume(networkRescanTimer_);
}

void ServiceEngine::stopMonitoring() {
    watcher_->stop();

    {
        std::lock_guard<std::mutex> lock(pendingRescanMutex_);
        if (rescanDebounceTimer_) {
            dispatch_source_cancel(rescanDebounceTimer_);
            rescanDebounceTimer_ = nullptr;
        }
        if (networkRescanTimer_) {
            dispatch_source_cancel(networkRescanTimer_);
            networkRescanTimer_ = nullptr;
        }
        pendingRescanPaths_.clear();
        lastRescanTime_.clear();
    }

    auto persistence = safePersistence();
    if (persistence) {
        persistence->stopAutoCompactionAndWait();
    }
    auto cp = safeContentPersistence();
    if (cp) {
        cp->stopAutoCompactionAndWait();
    }
    isMonitoring_.store(false, std::memory_order_relaxed);
}

// ═══════════════════════════════════════════════════════
//  Rescan debounce
// ═══════════════════════════════════════════════════════

void ServiceEngine::scheduleRescanForPaths(const std::vector<std::string>& paths) {
    std::lock_guard<std::mutex> lock(pendingRescanMutex_);

    pendingRescanPaths_ = mergeRescanPaths(pendingRescanPaths_, paths);

    LOG_INFO("FSWatcher", "Debounce: " << pendingRescanPaths_.size()
             << " pending rescan path(s), scheduling " << kRescanDebounceDelaySec << "s delay");

    if (rescanDebounceTimer_) {
        dispatch_source_cancel(rescanDebounceTimer_);
        rescanDebounceTimer_ = nullptr;
    }

    rescanDebounceTimer_ = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_TIMER, 0, 0, mutationQueue_);

    uint64_t delaySec = static_cast<uint64_t>(kRescanDebounceDelaySec * NSEC_PER_SEC);
    dispatch_source_set_timer(rescanDebounceTimer_, dispatch_time(DISPATCH_TIME_NOW, delaySec),
                              DISPATCH_TIME_FOREVER, NSEC_PER_SEC / 10);

    dispatch_source_set_event_handler(rescanDebounceTimer_, ^{
        this->flushPendingRescans();
    });
    dispatch_resume(rescanDebounceTimer_);
}

void ServiceEngine::flushPendingRescans() {
    std::set<std::string> pathsToRescan;
    std::set<std::string> throttledPaths;
    {
        std::lock_guard<std::mutex> lock(pendingRescanMutex_);
        for (const auto& path : pendingRescanPaths_) {
            if (shouldThrottleRescan(path, lastRescanTime_, kRescanThrottleIntervalSec)) {
                throttledPaths.insert(path);
                LOG_INFO("FSWatcher", "Throttled rescan for: " << path
                         << " (within " << kRescanThrottleIntervalSec << "s window)");
            } else {
                pathsToRescan.insert(path);
            }
        }
        pendingRescanPaths_ = throttledPaths;

        if (rescanDebounceTimer_) {
            dispatch_source_cancel(rescanDebounceTimer_);
            rescanDebounceTimer_ = nullptr;
        }
    }

    if (pathsToRescan.empty() && throttledPaths.empty()) return;

    LOG_INFO("FSWatcher", "Flushing debounced rescan: " << pathsToRescan.size()
             << " path(s) to rescan, " << throttledPaths.size() << " throttled");

    for (const auto& path : pathsToRescan) {
        rescanSubtree(path);

        std::lock_guard<std::mutex> lock(pendingRescanMutex_);
        lastRescanTime_[path] = std::chrono::steady_clock::now();
    }

    // Clean up old entries in lastRescanTime_ (> 2x throttle interval)
    {
        std::lock_guard<std::mutex> lock(pendingRescanMutex_);
        auto now = std::chrono::steady_clock::now();
        for (auto it = lastRescanTime_.begin(); it != lastRescanTime_.end(); ) {
            auto elapsed = std::chrono::duration<double>(now - it->second).count();
            if (elapsed > kRescanThrottleIntervalSec * 2.0) {
                it = lastRescanTime_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // If there are throttled paths, schedule a retry when throttle expires
    if (!throttledPaths.empty()) {
        std::lock_guard<std::mutex> lock(pendingRescanMutex_);
        if (!rescanDebounceTimer_) {
            rescanDebounceTimer_ = dispatch_source_create(
                DISPATCH_SOURCE_TYPE_TIMER, 0, 0, mutationQueue_);
            uint64_t delay = static_cast<uint64_t>(kRescanThrottleIntervalSec * NSEC_PER_SEC);
            dispatch_source_set_timer(rescanDebounceTimer_,
                                      dispatch_time(DISPATCH_TIME_NOW, delay),
                                      DISPATCH_TIME_FOREVER, NSEC_PER_SEC);
            dispatch_source_set_event_handler(rescanDebounceTimer_, ^{
                this->flushPendingRescans();
            });
            dispatch_resume(rescanDebounceTimer_);
        }
    }
}

void ServiceEngine::rescanSubtree(const std::string& dir) {
    auto engine = safeEngine();
    if (!engine) return;

    // An empty dir would make batchRescanPrefix tombstone the ENTIRE index
    // (every absolute path matches the empty prefix). Skip instead.
    if (dir.empty()) {
        LOG_WARN("FSWatcher", "Skipping rescan: empty subtree path");
        return;
    }

    // Blocks capture reference parameters by reference, and this block runs
    // async on mutationQueue_ after the caller's frame (and whatever the
    // reference points at — e.g. the local rescan set in flushPendingRescans)
    // is gone. The dangling read intermittently yields an empty string, which
    // used to wipe the whole index below. Copy before capturing.
    std::string dirCopy = dir;

    dispatch_async(mutationQueue_, ^{
        if (shuttingDown_.load(std::memory_order_relaxed)) return;

        auto scanner = std::make_shared<DirectoryScanner>();
        scanner->scan(dirCopy);
        auto freshRecords = scanner->takeResults();

        if (shuttingDown_.load(std::memory_order_relaxed)) return;

        // Root became unreachable (unmounted/deleted) between scheduling and
        // scanning, or the scan was cancelled mid-flight: the partial/empty
        // result set does NOT mean the files are gone. Skip reconciliation so
        // a transient mount blip can't wipe the index.
        if (scanner->hasFailed() || scanner->isCancelled()) {
            LOG_WARN("FSWatcher", "Skipping rescan reconcile (failed or cancelled): " << dirCopy);
            return;
        }

        engine->batchRescanPrefix(dirCopy, std::move(freshRecords));

        // Compact if tombstones exceed 30%
        uint32_t total = engine->recordCount();
        uint32_t live  = engine->liveRecordCount();
        if (total > live && (total - live) > total * 3 / 10) {
            auto remap = engine->compactRecords();
            if (!remap.empty()) {
                auto ci = safeContentIndex();
                if (ci) {
                    ci->remapFileIndices(remap);
                }
            }
        }

        if (onIndexChanged) {
            onIndexChanged();
        }
    });
}
