#include "ServiceEngine.h"
#include "DirectoryScanner.h"
#include "RescanDebounce.h"
#include "Logger.h"
#include <sys/stat.h>

// ═══════════════════════════════════════════════════════
//  FSEvents application (replay path)
// ═══════════════════════════════════════════════════════

namespace {

struct EventBatch {
    std::vector<SearchEngine::MutationOp> ops;
    std::vector<std::pair<std::string, bool>> contentUpdates; // path, isRemove
    std::vector<std::string> rescanDirs;
};

/// Translate FSEvents into index mutations. The current lstat() result is the
/// source of truth: event flags are coalesced (a path can carry Created and
/// Removed at once), so they only decide whether a directory needs a rescan.
void collectMutations(const std::vector<FileSystemWatcher::Event>& events,
                      bool (*insideAppBundle)(const std::string&),
                      bool (*endsWithApp)(const std::string&),
                      EventBatch& batch) {
    batch.ops.reserve(events.size());
    for (const auto& event : events) {
        const std::string& path = event.path;
        FSEventStreamEventFlags flags = event.flags;

        if (insideAppBundle(path)) continue;

        if (flags & kFSEventStreamEventFlagMustScanSubDirs) {
            batch.rescanDirs.push_back(path);
            continue;
        }

        struct stat st;
        if (lstat(path.c_str(), &st) != 0) {
            // Only a definite "gone" removes; EACCES etc. leave the index alone.
            if (errno == ENOENT || errno == ENOTDIR) {
                batch.contentUpdates.push_back({path, true});
                batch.ops.push_back({SearchEngine::MutationOp::REMOVE, path, {}});
            }
            continue;
        }

        size_t lastSlash = path.rfind('/');
        std::string dirPath = lastSlash != std::string::npos ? path.substr(0, lastSlash) : ".";
        std::string fileName = lastSlash != std::string::npos ? path.substr(lastSlash + 1) : path;
        if (fileName.empty()) continue;

        uint8_t type = 4;
        if (S_ISREG(st.st_mode))       type = 1;
        else if (S_ISDIR(st.st_mode))   type = endsWithApp(fileName) ? 5 : 2;
        else if (S_ISLNK(st.st_mode))   type = 3;

        // A directory moved/renamed into place arrives as a single event for
        // the directory; its contents must be scanned explicitly.
        if (type == 2 && (flags & kFSEventStreamEventFlagItemRenamed)) {
            batch.rescanDirs.push_back(path);
        }

        FileRecord record;
        record.name = fileName;
        record.path = dirPath;
        record.type = type;
        record.size = S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0;
        record.modTime = st.st_mtime;
        record.inode = st.st_ino;
        record.devId = static_cast<int32_t>(st.st_dev);

        batch.ops.push_back({SearchEngine::MutationOp::UPDATE, path, std::move(record)});
        if (type == 1) {
            batch.contentUpdates.push_back({path, false});
        }
    }
}

} // namespace

void ServiceEngine::applyFSEvents(
    const std::vector<FileSystemWatcher::Event>& events,
    std::shared_ptr<SearchEngine> engine)
{
    EventBatch batch;
    collectMutations(events, &ServiceEngine::isInsideAppBundle, &ServiceEngine::pathEndsWithApp, batch);

    // Apply content index updates (uses its own lock)
    for (const auto& [path, isRemove] : batch.contentUpdates) {
        updateContentForPath(path, isRemove, engine);
    }

    // Apply all search engine mutations in a single lock acquisition
    engine->batchMutate(std::move(batch.ops));

    if (!batch.rescanDirs.empty()) {
        scheduleRescanForPaths(batch.rescanDirs);
    }
}

// ═══════════════════════════════════════════════════════
//  Live monitoring
// ═══════════════════════════════════════════════════════

// Paths whose FSEvents are ignored: the app's own cache and high-churn system
// paths. Shared by the replay and live watchers.
std::vector<std::string> ServiceEngine::fsEventExclusions() const {
    std::string root = config_.scanRoot;
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

    return exclusions;
}

void ServiceEngine::startMonitoring() {
    if (isMonitoring_.load(std::memory_order_relaxed)) return;

    std::string root = config_.scanRoot;

    watcher_->setExclusionPaths(fsEventExclusions());

    watcher_->start(root, [this](std::vector<FileSystemWatcher::Event> events) {
        if (shuttingDown_.load(std::memory_order_relaxed)) return;

        auto engine = safeEngine();
        if (!engine) return;

        EventBatch batch;
        collectMutations(events, &ServiceEngine::isInsideAppBundle, &ServiceEngine::pathEndsWithApp, batch);
        bool changed = !batch.ops.empty();

        // Apply content index updates (uses its own lock)
        for (const auto& [path, isRemove] : batch.contentUpdates) {
            updateContentForPath(path, isRemove, engine);
        }

        // Apply all search engine mutations in a single lock acquisition
        engine->batchMutate(std::move(batch.ops));

        if (!batch.rescanDirs.empty()) {
            scheduleRescanForPaths(batch.rescanDirs);
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

void ServiceEngine::rescanSubtree(const std::string& dirRef) {
    auto engine = safeEngine();
    if (!engine) return;

    // Copy before dispatching: a block captures a C++ reference as a reference,
    // and callers pass elements of a container that is destroyed on return.
    const std::string dir = dirRef;
    dispatch_async(mutationQueue_, ^{
        if (shuttingDown_.load(std::memory_order_relaxed)) return;

        auto scanner = std::make_shared<DirectoryScanner>();
        scanner->scan(dir);
        auto freshRecords = scanner->takeResults();

        if (shuttingDown_.load(std::memory_order_relaxed)) return;

        size_t freshCount = freshRecords.size();
        uint32_t removed = engine->batchRescanPrefix(dir, std::move(freshRecords));
        LOG_INFO("FSWatcher", "Rescanned " << dir << ": removed " << removed
                 << ", scanned " << freshCount);

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
