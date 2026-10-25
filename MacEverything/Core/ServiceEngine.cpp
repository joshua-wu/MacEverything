#include "ServiceEngine.h"
#include "DirectoryScanner.h"
#include "NetworkMounts.h"
#include "PathUtils.h"
#include "Logger.h"
#include <sys/stat.h>
#include <filesystem>

namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════
//  Construction / Destruction
// ═══════════════════════════════════════════════════════

ServiceEngine::ServiceEngine(const ServiceConfig& config)
    : config_(config)
{
    engine_ = std::make_shared<SearchEngine>();
    watcher_ = std::make_shared<FileSystemWatcher>("live");
    contentIndex_ = std::make_shared<ContentIndex>();
    mutationQueue_ = dispatch_queue_create("com.maceverything.mutation", DISPATCH_QUEUE_SERIAL);
    backgroundGroup_ = dispatch_group_create();
    contentIndexingSemaphore_ = dispatch_semaphore_create(0);
}

ServiceEngine::~ServiceEngine() {
    shutdown();
}

// ═══════════════════════════════════════════════════════
//  Thread-safe accessors
// ═══════════════════════════════════════════════════════

std::shared_ptr<SearchEngine> ServiceEngine::safeEngine() {
    std::shared_lock lock(engineMutex_);
    return engine_;
}

void ServiceEngine::setEngine(std::shared_ptr<SearchEngine> engine) {
    std::unique_lock lock(engineMutex_);
    engine_ = engine;
}

std::shared_ptr<ContentIndex> ServiceEngine::safeContentIndex() {
    std::shared_lock lock(contentMutex_);
    return contentIndex_;
}

std::shared_ptr<IndexPersistence> ServiceEngine::safePersistence() {
    std::shared_lock lock(persistenceMutex_);
    return persistence_;
}

void ServiceEngine::setPersistence(std::shared_ptr<IndexPersistence> persistence) {
    std::unique_lock lock(persistenceMutex_);
    persistence_ = persistence;
}

std::shared_ptr<ContentIndexPersistence> ServiceEngine::safeContentPersistence() {
    std::shared_lock lock(contentPersistenceMutex_);
    return contentPersistence_;
}

void ServiceEngine::setContentPersistence(std::shared_ptr<ContentIndexPersistence> persistence) {
    std::unique_lock lock(contentPersistenceMutex_);
    contentPersistence_ = persistence;
}

// ═══════════════════════════════════════════════════════
//  State queries
// ═══════════════════════════════════════════════════════

uint32_t ServiceEngine::recordCount() {
    auto engine = safeEngine();
    return engine ? engine->recordCount() : 0;
}

uint32_t ServiceEngine::liveRecordCount() {
    auto engine = safeEngine();
    return engine ? engine->liveRecordCount() : 0;
}

// ═══════════════════════════════════════════════════════
//  Metadata builder (pure C++ — no NSProcessInfo)
// ═══════════════════════════════════════════════════════

IndexMetadata ServiceEngine::buildMetadata() {
    IndexMetadata meta;
    meta.lastEventId = watcher_ ? watcher_->getLastEventId() : 0;
    meta.extra[IndexMetadata::kScanRoot] = config_.scanRoot;
    meta.extra[IndexMetadata::kAppVersion] = kAppVersion;
    meta.extra[IndexMetadata::kRecordFormat] = "v6_flat";
    meta.extra[IndexMetadata::kOSVersion] = PathUtils::getOSVersionString();
    return meta;
}

// ═══════════════════════════════════════════════════════
//  Full scan
// ═══════════════════════════════════════════════════════

void ServiceEngine::startFullScan(StartupCallback completion) {
    isScanning_.store(true, std::memory_order_relaxed);
    stopMonitoring();

    LOG_INFO("ServiceEngine", "startFullScan from: " << config_.scanRoot);

    dispatch_group_async(backgroundGroup_, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto scanStart = std::chrono::steady_clock::now();
        auto scanner = std::make_shared<DirectoryScanner>();
        if (config_.scanNetworkVolumes) {
            for (const auto& mnt : networkMountPoints()) {
                LOG_INFO("ServiceEngine", "Including network volume in scan: " << mnt);
                scanner->addExtraRoot(mnt);
            }
        }

        // Progress polling timer
        dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
            dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0));
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0),
                                  200 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC);
        std::weak_ptr<DirectoryScanner> scannerWeak = scanner;
        auto progressCb = this->onScanProgress;
        dispatch_source_set_event_handler(timer, ^{
            if (!progressCb) return;
            auto s = scannerWeak.lock();
            if (!s) return;
            const auto& stats = s->getStats();
            progressCb(stats.fileCount.load(std::memory_order_relaxed),
                       stats.dirCount.load(std::memory_order_relaxed));
        });
        dispatch_resume(timer);

        scanner->scan(config_.scanRoot);
        dispatch_source_cancel(timer);

        auto results = scanner->takeResults();
        auto engine = std::make_shared<SearchEngine>();
        engine->loadRecords(std::move(results));
        uint32_t count = engine->liveRecordCount();

        auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - scanStart).count();
        LOG_INFO("ServiceEngine", "Scan completed: " << count << " records in " << elapsed << "s");

        this->setEngine(engine);
        this->isScanning_.store(false, std::memory_order_relaxed);

        if (completion) completion(count, true);

        this->startMonitoring();

        // Content indexing in background
        dispatch_group_async(this->backgroundGroup_, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            if (this->shuttingDown_.load(std::memory_order_acquire)) return;
            this->setupContentPersistence();
            this->startContentIndexing();
        });
    });
}

// ═══════════════════════════════════════════════════════
//  Incremental load (cached index + background sync)
// ═══════════════════════════════════════════════════════

void ServiceEngine::startIncremental(StartupCallback completion) {
    isScanning_.store(true, std::memory_order_relaxed);
    startupCompleted_.store(false, std::memory_order_relaxed);
    isSyncing_.store(false, std::memory_order_relaxed);
    stopMonitoring();

    // Acquire single-instance lock
    {
        std::string cacheDir = config_.cachePath;
        fs::create_directories(cacheDir);
        std::string lockPath = cacheDir + "/.instance.lock";
        if (!instanceLock_.tryLock(lockPath)) {
            LOG_WARN("ServiceEngine", "Another instance may be running — proceeding with caution");
        }
    }

    LOG_INFO("ServiceEngine", "startIncremental from: " << config_.scanRoot);

    dispatch_group_async(backgroundGroup_, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto incrementalStart = std::chrono::steady_clock::now();
        auto engine = std::make_shared<SearchEngine>();

        std::string cacheStr = config_.cachePath + "/index.bin";
        std::string walStr   = config_.cachePath + "/index.wal";
        std::string pagesStr = config_.cachePath + "/index.pages";
        std::string ptableStr = config_.cachePath + "/index.ptable";
        std::string v6Str    = config_.cachePath + "/index.v6";

        auto persistence = std::make_unique<IndexPersistence>(
            engine, cacheStr, walStr, pagesStr, ptableStr, v6Str);

        uint64_t lastEventId = persistence->load();
        auto indexLoadDone = std::chrono::steady_clock::now();
        uint32_t loadedCount = engine->liveRecordCount();

        if (lastEventId > 0 && loadedCount > 0) {
            // Have cached index: deliver immediately, then sync in background
            auto sharedPersistence = std::shared_ptr<IndexPersistence>(std::move(persistence));

            bool expected = false;
            if (!this->startupCompleted_.compare_exchange_strong(expected, true,
                    std::memory_order_acq_rel)) {
                return;
            }

            this->setEngine(engine);
            this->setPersistence(sharedPersistence);
            this->isScanning_.store(false, std::memory_order_relaxed);
            this->isSyncing_.store(true, std::memory_order_relaxed);

            // Auto-start HTTP server once engine is available
            if (config_.httpPort > 0) {
                this->startHttpServer(config_.httpPort);
            }

            sharedPersistence->attachWAL();
            sharedPersistence->setContentIndex(this->safeContentIndex());

            uint32_t count = engine->liveRecordCount();
            if (completion) completion(count, false);

            // Dispatch Phase 2: build trigram indices in background
            if (engine->isPhase2Pending()) {
                dispatch_group_async(this->backgroundGroup_, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                    if (this->shuttingDown_.load(std::memory_order_acquire)) return;
                    auto eng = this->safeEngine();
                    if (eng) eng->completePhase2();
                });
            }

            this->backgroundSyncEngine(engine, sharedPersistence, lastEventId, incrementalStart, indexLoadDone);
            return;
        }

        // No cache: full scan
        this->startFullScan([this, completion](uint32_t count, bool) {
            bool expected = false;
            if (!this->startupCompleted_.compare_exchange_strong(expected, true,
                    std::memory_order_acq_rel)) {
                return;
            }

            std::string cacheStr = config_.cachePath + "/index.bin";
            std::string walStr   = config_.cachePath + "/index.wal";
            std::string pagesStr = config_.cachePath + "/index.pages";
            std::string ptableStr = config_.cachePath + "/index.ptable";
            std::string v6Str    = config_.cachePath + "/index.v6";

            auto newPersistence = std::make_shared<IndexPersistence>(
                this->safeEngine(), cacheStr, walStr, pagesStr, ptableStr, v6Str);
            this->setPersistence(newPersistence);
            newPersistence->attachWAL();
            newPersistence->setContentIndex(this->safeContentIndex());
            newPersistence->startAutoCompaction(300.0, this->watcher_);

            auto meta = this->buildMetadata();
            newPersistence->flush(meta, /*force=*/true);

            // Auto-start HTTP server once engine is available
            if (config_.httpPort > 0) {
                this->startHttpServer(config_.httpPort);
            }

            if (completion) completion(count, true);
        });
    });
}

// ═══════════════════════════════════════════════════════
//  Background sync (FSEvents replay or full scan)
// ═══════════════════════════════════════════════════════

void ServiceEngine::backgroundSyncEngine(
    std::shared_ptr<SearchEngine> engine,
    std::shared_ptr<IndexPersistence> sharedPersistence,
    uint64_t lastEventId,
    std::chrono::steady_clock::time_point incrementalStart,
    std::chrono::steady_clock::time_point indexLoadDone)
{
    dispatch_group_async(backgroundGroup_, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        // Try FSEvents replay
        auto replayDone = std::make_shared<std::atomic<bool>>(false);
        auto journalTruncated = std::make_shared<std::atomic<bool>>(false);

        auto watcherForReplay = std::make_unique<FileSystemWatcher>("replay");
        auto* watcherPtr = watcherForReplay.get();

        dispatch_semaphore_t sem = dispatch_semaphore_create(0);

        watcherPtr->start(
            config_.scanRoot,
            lastEventId,
            [this, engine](std::vector<FileSystemWatcher::Event> events) {
                this->applyFSEvents(events, engine);
            },
            [replayDone, journalTruncated, watcherPtr, sem] {
                replayDone->store(true);
                journalTruncated->store(watcherPtr->isJournalTruncated());
                dispatch_semaphore_signal(sem);
            }
        );

        long result = dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
        watcherForReplay->stop();

        if (result == 0 && replayDone->load() && !journalTruncated->load()) {
            // Replay succeeded
            auto now = std::chrono::steady_clock::now();
            auto loadTime = std::chrono::duration<double>(indexLoadDone - incrementalStart).count();
            auto replayTime = std::chrono::duration<double>(now - indexLoadDone).count();
            auto totalTime = std::chrono::duration<double>(now - incrementalStart).count();
            LOG_INFO("ServiceEngine", "Incremental startup completed: "
                     << engine->liveRecordCount() << " records — "
                     << "index load " << loadTime << "s, "
                     << "FSEvents replay " << replayTime << "s, "
                     << "total " << totalTime << "s");

            this->isSyncing_.store(false, std::memory_order_relaxed);
            this->startMonitoring();
            sharedPersistence->startAutoCompaction(300.0, this->watcher_);

            dispatch_group_async(this->backgroundGroup_, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                if (this->shuttingDown_.load(std::memory_order_acquire)) return;
                this->setupContentPersistence();
                this->startContentIndexing();
            });
            if (this->onIndexChanged) this->onIndexChanged();
            return;
        }

        // Replay failed: background full scan
        LOG_WARN("ServiceEngine", "FSEvents replay failed — background full scan");

        auto scanner = std::make_shared<DirectoryScanner>();
        if (config_.scanNetworkVolumes) {
            for (const auto& mnt : networkMountPoints()) {
                LOG_INFO("ServiceEngine", "Including network volume in scan: " << mnt);
                scanner->addExtraRoot(mnt);
            }
        }

        // Progress reporting
        std::weak_ptr<DirectoryScanner> scannerWeak = scanner;
        dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
            dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0));
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0),
                                  200 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC);
        auto progressCb = this->onScanProgress;
        dispatch_source_set_event_handler(timer, ^{
            if (!progressCb) return;
            auto s = scannerWeak.lock();
            if (!s) return;
            const auto& stats = s->getStats();
            progressCb(stats.fileCount.load(std::memory_order_relaxed),
                       stats.dirCount.load(std::memory_order_relaxed));
        });
        dispatch_resume(timer);

        scanner->scan(config_.scanRoot);
        dispatch_source_cancel(timer);

        auto freshRecords = scanner->takeResults();
        engine->loadRecords(std::move(freshRecords));
        uint32_t finalCount = engine->liveRecordCount();

        auto scanNow = std::chrono::steady_clock::now();
        auto loadTime = std::chrono::duration<double>(indexLoadDone - incrementalStart).count();
        auto scanTime = std::chrono::duration<double>(scanNow - indexLoadDone).count();
        auto totalTime = std::chrono::duration<double>(scanNow - incrementalStart).count();
        LOG_INFO("ServiceEngine", "Background scan completed: "
                 << finalCount << " records — "
                 << "index load " << loadTime << "s, "
                 << "rescan " << scanTime << "s, "
                 << "total " << totalTime << "s");

        this->isSyncing_.store(false, std::memory_order_relaxed);

        sharedPersistence->stopAutoCompactionAndWait();

        std::string cacheStr = config_.cachePath + "/index.bin";
        std::string walStr   = config_.cachePath + "/index.wal";
        std::string pagesStr = config_.cachePath + "/index.pages";
        std::string ptableStr = config_.cachePath + "/index.ptable";
        std::string v6Str    = config_.cachePath + "/index.v6";

        auto newPersistence = std::make_shared<IndexPersistence>(
            engine, cacheStr, walStr, pagesStr, ptableStr, v6Str);
        this->setPersistence(newPersistence);
        newPersistence->attachWAL();
        newPersistence->setContentIndex(this->safeContentIndex());

        this->startMonitoring();
        newPersistence->startAutoCompaction(300.0, this->watcher_);

        auto meta = this->buildMetadata();
        newPersistence->flush(meta, /*force=*/true);

        dispatch_group_async(this->backgroundGroup_, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            if (this->shuttingDown_.load(std::memory_order_acquire)) return;
            this->setupContentPersistence();
            this->startContentIndexing();
        });
        if (this->onIndexChanged) this->onIndexChanged();
    });
}

// ═══════════════════════════════════════════════════════
//  Compaction
// ═══════════════════════════════════════════════════════

void ServiceEngine::compactIndex() {
    LOG_INFO("ServiceEngine", "compactIndex started");
    auto persistence = safePersistence();
    if (persistence && watcher_) {
        LOG_TIMER("ServiceEngine", "compactIndex");
        uint64_t eventId = watcher_->getLastEventId();
        persistence->setContentIndex(safeContentIndex());
        persistence->setContentIndexPersistence(safeContentPersistence());
        persistence->compact(eventId);
    }
}

// ═══════════════════════════════════════════════════════
//  HTTP Server
// ═══════════════════════════════════════════════════════

void ServiceEngine::startHttpServer(uint16_t port) {
    std::unique_lock lock(engineMutex_); // unique_lock: creates/mutates httpServer_
    if (!httpServer_) {
        httpServer_ = std::make_shared<HttpServer>();
    }
    bool ok = httpServer_->start(port,
        [this]() -> std::shared_ptr<SearchEngine> { return this->safeEngine(); },
        [this]() -> std::shared_ptr<ContentIndex> { return this->safeContentIndex(); });
    if (!ok) {
        LOG_ERROR("ServiceEngine", "HTTP server failed to start on port " << port);
        return;
    }

    if (adminCallbacks.onRebuildIndex || adminCallbacks.onRebuildContentIndex) {
        httpServer_->setAdminCallbacks(adminCallbacks);
    }
}

void ServiceEngine::stopHttpServer() {
    std::unique_lock lock(engineMutex_); // unique_lock: mutates httpServer_
    if (httpServer_) {
        httpServer_->stop();
    }
}

// ═══════════════════════════════════════════════════════
//  Shutdown
// ═══════════════════════════════════════════════════════

void ServiceEngine::shutdown() {
    bool expected = false;
    if (!shuttingDown_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return; // Already shutting down or shut down
    }

    stopHttpServer();
    LOG_INFO("ServiceEngine", "shutdown started");

    cancelContentIndexing_.store(true, std::memory_order_relaxed);
    contentIndexGeneration_.fetch_add(1, std::memory_order_acq_rel);

    // Wait for background GCD blocks with a timeout.
    // Background blocks check shuttingDown_ and bail quickly, so this typically completes in < 500ms.
    // The 3s timeout prevents infinite hangs if a block is stuck in I/O, ensuring the critical
    // flush below can proceed before macOS's termination watchdog kills the process.
    long waitResult = dispatch_group_wait(backgroundGroup_,
        dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
    if (waitResult != 0) {
        LOG_WARN("ServiceEngine", "Background tasks did not complete within 3s — proceeding with flush");
    }

    uint64_t lastEventId = watcher_ ? watcher_->getLastEventId() : 0;

    stopMonitoring();

    // Final compaction
    auto persistence = safePersistence();
    if (persistence) {
        persistence->setContentIndex(safeContentIndex());
        persistence->setContentIndexPersistence(safeContentPersistence());
        persistence->compact(lastEventId, /*force=*/true);
    }
    auto cp = safeContentPersistence();
    if (cp) {
        cp->compact(/*force=*/true);
    }

    // Release GCD objects
    if (mutationQueue_) {
        dispatch_release(mutationQueue_);
        mutationQueue_ = nullptr;
    }
    if (backgroundGroup_) {
        dispatch_release(backgroundGroup_);
        backgroundGroup_ = nullptr;
    }
    if (contentIndexingSemaphore_) {
        dispatch_release(contentIndexingSemaphore_);
        contentIndexingSemaphore_ = nullptr;
    }

    LOG_INFO("ServiceEngine", "shutdown completed");
}

// ═══════════════════════════════════════════════════════
//  Static helpers
// ═══════════════════════════════════════════════════════

bool ServiceEngine::isInsideAppBundle(const std::string& path) {
    size_t pos = 0;
    while ((pos = path.find(".app/", pos)) != std::string::npos) {
        if (pos >= 1 && path[pos - 1] != '/') {
            return true;
        }
        pos += 5;
    }
    return false;
}

bool ServiceEngine::pathEndsWithApp(const std::string& path) {
    return path.size() > 4 &&
           path[path.size()-4] == '.' && path[path.size()-3] == 'a' &&
           path[path.size()-2] == 'p' && path[path.size()-1] == 'p';
}
