#include "IndexPersistence.h"
#include "Logger.h"
#include <cerrno>
#include <cstring>
#include <cmath>

IndexPersistence::IndexPersistence(std::shared_ptr<SearchEngine> engine,
                                   const std::string& basePath,
                                   const std::string& walPath,
                                   const std::string& pagesPath,
                                   const std::string& ptablePath,
                                   const std::string& v6Path)
    : engine_(std::move(engine))
    , basePath_(basePath)
    , walPath_(walPath)
    , pagedWriter_(std::make_unique<PagedIndexWriter>(pagesPath, ptablePath))
    , flatWriter_(std::make_unique<FlatIndexWriter>(v6Path))
    , v6Path_(v6Path)
{}

IndexPersistence::~IndexPersistence() {
    stopTimer();
    if (engine_) engine_->detachWAL();
    {
        std::lock_guard<std::mutex> lock(walMutex_);
        if (wal_) wal_->close();
    }
}

uint64_t IndexPersistence::load() {
    uint64_t lastEventId = 0;

    // 1. Try v6 flat SoA format first (fast path)
    bool loaded = false;
    if (flatWriter_->exists()) {
        IndexMetadata meta;
        loaded = flatWriter_->load(*engine_, &meta);
        if (loaded) {
            lastEventId = meta.lastEventId;
            LOG_INFO("IndexPersistence", "Loaded v6 flat index, lastEventId=" << lastEventId
                      << ", liveRecords=" << engine_->liveRecordCount());
            if (flatWriter_->loadedVersion() < FlatIndexWriter::kVersion) {
                // Rewrite in the mappable layout so the index stops living in the heap.
                if (flatWriter_->fullRewrite(*engine_, meta)) {
                    LOG_INFO("IndexPersistence", "Migrated v6 index to mappable layout");
                }
            }
        } else {
            LOG_ERROR("IndexPersistence", "v6 flat index corrupt, trying paged format");
        }
    }

    // 2. Fallback to paged format (v5)
    if (!loaded && pagedWriter_->exists()) {
        IndexMetadata meta;
        loaded = pagedWriter_->load(*engine_, &meta);
        if (loaded) {
            lastEventId = meta.lastEventId;
            LOG_INFO("IndexPersistence", "Loaded paged index, lastEventId=" << lastEventId
                      << ", liveRecords=" << engine_->liveRecordCount());
            // Auto-migrate to v6 flat format
            IndexMetadata migrateMeta;
            migrateMeta.lastEventId = lastEventId;
            if (flatWriter_->fullRewrite(*engine_, migrateMeta)) {
                LOG_INFO("IndexPersistence", "Migrated paged index to v6 flat format");
            }
        } else {
            LOG_ERROR("IndexPersistence", "Paged index corrupt, trying legacy format");
        }
    }

    // 3. Fallback to legacy v3 format
    if (!loaded) {
        loaded = engine_->loadFromFile(basePath_, &lastEventId);
        if (loaded) {
            LOG_INFO("IndexPersistence", "Loaded legacy index, lastEventId=" << lastEventId
                      << ", liveRecords=" << engine_->liveRecordCount());
            // Auto-migrate to v6 flat format
            IndexMetadata migrateMeta;
            migrateMeta.lastEventId = lastEventId;
            if (flatWriter_->fullRewrite(*engine_, migrateMeta)) {
                LOG_INFO("IndexPersistence", "Migrated legacy index to v6 flat format");
            }
        } else {
            LOG_INFO("IndexPersistence", "No base index found at " << basePath_);
        }
    }

    // 3. In-place WAL replay using pathIndex_ for O(1) lookups
    auto entries = IndexWAL::readAll(walPath_);
    if (!entries.empty()) {
        LOG_INFO("IndexPersistence", "Replaying " << entries.size() << " WAL entries (in-place mode)");
        engine_->replayWALEntries(std::move(entries));
        LOG_INFO("IndexPersistence", "WAL replay done, liveRecords=" << engine_->liveRecordCount());
    }

    return lastEventId;
}

void IndexPersistence::attachWAL() {
    auto newWal = std::make_shared<IndexWAL>();
    if (newWal->open(walPath_)) {
        {
            std::lock_guard<std::mutex> lock(walMutex_);
            wal_ = newWal;
        }
        engine_->attachWAL(newWal);
        LOG_INFO("IndexPersistence", "WAL attached at " << walPath_);
    } else {
        LOG_ERROR("IndexPersistence", "Failed to open WAL at " << walPath_);
    }
}

void IndexPersistence::flush(uint64_t lastEventId, bool force) {
    IndexMetadata meta;
    meta.lastEventId = lastEventId;
    flush(meta, force);
}

void IndexPersistence::flush(const IndexMetadata& metadata, bool force) {
    // Skip logic:
    //   - No WAL → skip.
    //   - force=true: skip only if WAL file is header-only (no entries at all,
    //     including stale entries from a previous session).
    //   - Otherwise: skip if not dirty or below threshold.
    static constexpr size_t kWALHeaderSize = 2 * sizeof(uint32_t); // magic + version
    {
        std::lock_guard<std::mutex> lock(walMutex_);
        if (!wal_) {
            LOG_INFO("IndexPersistence", "Skipping flush — no WAL");
            return;
        }
        if (force) {
            if (wal_->currentSize() <= kWALHeaderSize) {
                LOG_INFO("IndexPersistence", "Skipping flush — WAL is empty");
                return;
            }
        } else {
            if (!wal_->isDirty()) {
                LOG_INFO("IndexPersistence", "Skipping flush — no mutations since last flush");
                return;
            }
            if (wal_->entryCount() < kCompactThreshold) {
                LOG_INFO("IndexPersistence", "Skipping flush — only "
                          << wal_->entryCount() << " entries (threshold=" << kCompactThreshold << ")");
                return;
            }
        }
    }

    // Check if full compaction is needed (tombstone ratio)
    uint32_t totalCount = engine_->recordCount();
    uint32_t liveCount = engine_->liveRecordCount();
    double tombstoneRatio = totalCount > 0
        ? static_cast<double>(totalCount - liveCount) / totalCount
        : 0.0;

    if (tombstoneRatio > kTombstoneCompactRatio) {
        LOG_INFO("IndexPersistence", "Tombstone ratio " << (tombstoneRatio * 100)
                  << "% > " << (kTombstoneCompactRatio * 100) << "% — triggering full compaction");
        fullCompact(metadata);
        return;
    }

    // 1. Open a fresh WAL before detaching the old one
    auto newWal = std::make_shared<IndexWAL>();
    std::string newWalPath = walPath_ + ".new";
    if (!newWal->open(newWalPath)) {
        LOG_ERROR("IndexPersistence", "Failed to open new WAL for flush");
        return;
    }

    // 2. Atomically swap WAL
    std::shared_ptr<IndexWAL> oldWal;
    {
        std::lock_guard<std::mutex> lock(walMutex_);
        oldWal = wal_;
        wal_ = newWal;
    }
    engine_->attachWAL(newWal);

    // 3. Full rewrite v6 flat format
    bool writeOk = flatWriter_->fullRewrite(*engine_, metadata);

    if (writeOk) {
        LOG_INFO("IndexPersistence", "Flushed v6 flat index, lastEventId=" << metadata.lastEventId
                  << ", liveRecords=" << engine_->liveRecordCount());
    } else {
        LOG_ERROR("IndexPersistence", "Failed to flush paged index — keeping old WAL for recovery");
        if (oldWal) oldWal->close();
        return;
    }

    // 4. Rename new WAL to replace old
    if (rename(newWalPath.c_str(), walPath_.c_str()) != 0) {
        LOG_ERROR("IndexPersistence", "Failed to rename WAL: " << newWalPath
                  << " -> " << walPath_ << " (errno=" << errno << ": " << strerror(errno) << ")");
    } else {
        std::lock_guard<std::mutex> lock(walMutex_);
        if (wal_) wal_->updatePath(walPath_);
    }

    // 5. Close old WAL
    if (oldWal) oldWal->close();
}

void IndexPersistence::fullCompact(const IndexMetadata& metadata) {
    // 1. Open a fresh WAL
    auto newWal = std::make_shared<IndexWAL>();
    std::string newWalPath = walPath_ + ".new";
    if (!newWal->open(newWalPath)) {
        LOG_ERROR("IndexPersistence", "Failed to open new WAL for full compaction");
        return;
    }

    // 2. Swap WAL
    std::shared_ptr<IndexWAL> oldWal;
    {
        std::lock_guard<std::mutex> lock(walMutex_);
        oldWal = wal_;
        wal_ = newWal;
    }
    engine_->attachWAL(newWal);

    // 3. Compact in-memory records (remove tombstones)
    uint32_t beforeTotal = engine_->recordCount();
    uint32_t beforeLive = engine_->liveRecordCount();
    auto remap = engine_->compactRecords();
    uint32_t reclaimed = beforeTotal - beforeLive;
    if (reclaimed > 0) {
        LOG_INFO("IndexPersistence", "Reclaimed " << reclaimed << " tombstones ("
                  << beforeTotal << " -> " << beforeLive << " records)");
    }
    if (!remap.empty() && contentIndex_) {
        contentIndex_->remapFileIndices(remap);
        // Flush content index to disk so its base file stays in sync with the
        // search engine's compacted indices.  Without this, the content base
        // file retains old indices that point to wrong records after restart.
        if (contentPersistence_) {
            contentPersistence_->compact(true);
        }
    }

    // 4. Full rewrite v6 flat format
    if (flatWriter_->fullRewrite(*engine_, metadata)) {
        LOG_INFO("IndexPersistence", "Full compaction done, lastEventId=" << metadata.lastEventId
                  << ", liveRecords=" << engine_->liveRecordCount());
    } else {
        LOG_ERROR("IndexPersistence", "Failed to write full compaction — keeping old WAL");
        if (oldWal) oldWal->close();
        return;
    }

    // 5. Rename WAL
    if (rename(newWalPath.c_str(), walPath_.c_str()) != 0) {
        LOG_ERROR("IndexPersistence", "Failed to rename WAL: " << newWalPath
                  << " -> " << walPath_);
    } else {
        std::lock_guard<std::mutex> lock(walMutex_);
        if (wal_) wal_->updatePath(walPath_);
    }

    // 6. Close old WAL
    if (oldWal) oldWal->close();
}

double IndexPersistence::computeAdaptiveInterval() const {
    auto dirtyPages = engine_->getDirtyPageNumbers();
    uint32_t dirtyCount = static_cast<uint32_t>(dirtyPages.size());
    uint32_t liveCount = engine_->liveRecordCount();
    uint32_t totalPages = (liveCount + SearchEngine::kRecordsPerPage - 1) / SearchEngine::kRecordsPerPage;
    if (totalPages == 0) totalPages = 1;

    double dirtyRatio = static_cast<double>(dirtyCount) / totalPages;

    double interval;
    if (dirtyRatio > 0.3) {
        interval = kMinIntervalSec;
    } else if (dirtyRatio > 0.1) {
        interval = kBaseIntervalSec * 0.5;
    } else if (dirtyRatio > 0.01) {
        interval = kBaseIntervalSec;
    } else {
        interval = kMaxIntervalSec;
    }

    // WAL size override
    size_t walSize = 0;
    {
        std::lock_guard<std::mutex> lock(walMutex_);
        if (wal_) walSize = wal_->currentSize();
    }
    if (walSize > kWALSizeFlushThreshold) {
        interval = std::min(interval, kMinIntervalSec);
    }

    return interval;
}

void IndexPersistence::stopTimer() {
    if (timer_) {
        dispatch_source_cancel(timer_);
        dispatch_release(timer_);
        timer_ = nullptr;
    }
    if (timerQueue_) {
        dispatch_sync(timerQueue_, ^{});
        dispatch_release(timerQueue_);
        timerQueue_ = nullptr;
    }
    currentIntervalSec_ = 0;
}

void IndexPersistence::rescheduleTimer(double intervalSec) {
    if (!timer_) return;
    if (std::abs(intervalSec - currentIntervalSec_) < 1.0) return;

    uint64_t intervalNs = static_cast<uint64_t>(intervalSec * NSEC_PER_SEC);
    dispatch_source_set_timer(timer_,
                              dispatch_time(DISPATCH_TIME_NOW, intervalNs),
                              intervalNs,
                              30 * NSEC_PER_SEC);
    currentIntervalSec_ = intervalSec;
}

void IndexPersistence::startAutoCompaction(double intervalSec, std::shared_ptr<FileSystemWatcher> watcher) {
    stopTimer();

    currentIntervalSec_ = intervalSec;
    timerQueue_ = dispatch_queue_create("com.maceverything.index.compaction", DISPATCH_QUEUE_SERIAL);
    timer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, timerQueue_);

    uint64_t intervalNs = static_cast<uint64_t>(intervalSec * NSEC_PER_SEC);
    dispatch_source_set_timer(timer_,
                              dispatch_time(DISPATCH_TIME_NOW, intervalNs),
                              intervalNs,
                              30 * NSEC_PER_SEC);

    auto* self = this;
    dispatch_source_set_event_handler(timer_, ^{
        uint64_t eventId = watcher ? watcher->getLastEventId() : 0;
        self->flush(eventId);
        double newInterval = self->computeAdaptiveInterval();
        self->rescheduleTimer(newInterval);
    });

    dispatch_resume(timer_);
    LOG_INFO("IndexPersistence", "Auto-compaction started (initial interval " << intervalSec << "s)");
}

void IndexPersistence::stopAutoCompactionAndWait() {
    stopTimer();
}
