#include "ServiceEngine.h"
#include "DirectoryScanner.h"
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

    // Exclude app's own cache directory and high-churn system paths from FSEvents
    std::vector<std::string> exclusions;
    std::string cacheExclusion = config_.cachePath;
    const char* home = std::getenv("HOME");
    std::string homeStr = home ? home : "";

    if (cacheExclusion.empty() && !homeStr.empty()) {
        cacheExclusion = homeStr + "/Library/Caches/com.maceverything.app";
    }
    if (!cacheExclusion.empty()) {
        exclusions.push_back(cacheExclusion);
    }

    if (!homeStr.empty()) {
        exclusions.push_back(homeStr + "/Library/Caches");
        exclusions.push_back(homeStr + "/Library/Biome");
        exclusions.push_back(homeStr + "/Library/Logs");
    }
    exclusions.push_back("/private/var/db");
    exclusions.push_back("/private/var/log");

    // If scanning a specific subtree (e.g. unit tests running in a temp directory),
    // do not exclude any path overlapping scanRoot.
    if (root != "/") {
        exclusions.erase(
            std::remove_if(exclusions.begin(), exclusions.end(), [&](const std::string& ep) {
                return root.size() >= ep.size() ? (root.compare(0, ep.size(), ep) == 0)
                                                : (ep.compare(0, root.size(), root) == 0);
            }),
            exclusions.end()
        );
    }

    watcher_->setExclusionPaths(exclusions);

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
}

void ServiceEngine::stopMonitoring() {
    watcher_->stop();

    {
        std::lock_guard<std::mutex> lock(pendingRescanMutex_);
        if (rescanDebounceTimer_) {
            dispatch_source_cancel(rescanDebounceTimer_);
            rescanDebounceTimer_ = nullptr;
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

    dispatch_async(mutationQueue_, ^{
        if (shuttingDown_.load(std::memory_order_relaxed)) return;

        auto scanner = std::make_shared<DirectoryScanner>();
        scanner->scan(dir);
        auto freshRecords = scanner->takeResults();

        if (shuttingDown_.load(std::memory_order_relaxed)) return;

        engine->batchRescanPrefix(dir, std::move(freshRecords));

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
