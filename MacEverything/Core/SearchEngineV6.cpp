#include "SearchEngine.h"
#include "StringUtils.h"
#include "Logger.h"
#include <algorithm>
#include <thread>

// ---------------------------------------------------------------------------
// v6 Flat SoA: loadRecordsV6, completePhase2, snapshotForV6
// ---------------------------------------------------------------------------

void SearchEngine::loadRecordsV6(StringPool&& origNamePool,
                                  StringPool&& namePool,
                                  std::vector<uint32_t>&& pathIndices,
                                  StringPool&& pathPool,
                                  StringPool&& lowerPathPool,
                                  std::vector<uint8_t>&& types,
                                  std::vector<uint64_t>&& sizes,
                                  std::vector<int64_t>&& modTimes,
                                  std::vector<uint64_t>&& inodes,
                                  std::vector<int32_t>&& devIds) {
    std::unique_lock lock(mutex_);

    uint32_t n = origNamePool.entryCount();

    // Install SoA columns directly (zero-copy from v6 file)
    origNamePool_ = std::move(origNamePool);
    namePool_ = std::move(namePool);
    pathIndices_ = std::move(pathIndices);
    pathPool_ = std::move(pathPool);
    lowerPathPool_ = std::move(lowerPathPool);
    types_ = std::move(types);
    sizes_ = std::move(sizes);
    modTimes_ = std::move(modTimes);
    inodes_ = std::move(inodes);
    devIds_ = std::move(devIds);

    rebuildPathLookupsUnlocked();
    uint32_t actualLive = rebuildPathIndexUnlocked();

    liveCount_.store(actualLive, std::memory_order_relaxed);

    // Phase 2: defer trigram index building to background
    phase2Pending_.store(true, std::memory_order_release);
    phase2StartRecordCount_ = static_cast<uint32_t>(types_.size());

    // Initialize dirty page bitmap
    uint32_t pageCount = (n + kRecordsPerPage - 1) / kRecordsPerPage;
    dirtyPages_.assign(pageCount, false);
    fullRewriteNeeded_.store(false, std::memory_order_relaxed);

    LOG_INFO("SearchEngine", "loadRecordsV6: loaded " << n << " records (" << actualLive
             << " live), Phase 2 pending");
}

void SearchEngine::completePhase2() {
    if (!phase2Pending_.load(std::memory_order_acquire)) return;

    LOG_INFO("SearchEngine", "Phase 2: building trigram indices in background...");

    // Snapshot data needed for building indices (under shared lock)
    // Only snapshot types_ (lightweight ~5MB for 5.5M records) instead of
    // the full SoA data (~440MB if using FileRecord structs).
    std::vector<uint8_t> snapTypes;
    std::vector<int64_t> snapModTimes;
    StringPool snapNamePool;
    StringPool snapLowerPathPool;
    std::vector<uint32_t> snapPathIndices;
    uint32_t snapPathPoolSize;
    uint32_t snapSize;
    {
        std::shared_lock lock(mutex_);
        snapTypes = types_;
        snapModTimes = modTimes_;
        snapNamePool = namePool_;
        snapLowerPathPool = lowerPathPool_;
        snapPathIndices = pathIndices_;
        snapPathPoolSize = pathPool_.entryCount();
        snapSize = static_cast<uint32_t>(types_.size());
    }

    // Build all indices without holding any lock (~3s)
    auto trigramIndex = buildTrigramIndexFromData(snapTypes, snapNamePool);
    auto pathTrigramIndex = buildPathTrigramIndexFromData(snapLowerPathPool);
    auto pathIdxToRecords = buildPathIdxToRecordsFromData(snapTypes, snapPathIndices, snapPathPoolSize);
    auto recentCache = buildRecentCacheFromData(snapTypes, snapModTimes, kRecentCacheSize);
    auto extensionIndex = buildExtensionIndexFromData(snapTypes, snapNamePool);

    LOG_INFO("SearchEngine", "Phase 2: indices built, swapping under lock...");

    // Swap under unique lock and replay mutations that occurred during build
    {
        std::unique_lock lock(mutex_);

        nameTrigramIndex_ = std::move(trigramIndex);
        pathTrigramIndex_ = std::move(pathTrigramIndex);
        pathIdxToRecords_ = std::move(pathIdxToRecords);
        recentCache_ = std::move(recentCache);
        extensionIndex_ = std::move(extensionIndex);

        // Replay tombstones: records live in the snapshot but deleted during build.
        // namePool_[i] is already tombstoned, so use the snapshot name.
        uint32_t replayCount = 0;
        for (uint32_t i = 0; i < snapSize && i < types_.size(); i++) {
            if (snapTypes[i] != 0 && types_[i] == 0) {
                removeTrigramsForName(i, snapNamePool.view(i));
                removeExtensionForName(i, snapNamePool.view(i));
                removePathTrigramsForRecord(i);
                replayCount++;
            }
        }

        // Replay records added during Phase 2 build
        uint32_t currentSize = static_cast<uint32_t>(types_.size());
        for (uint32_t i = snapSize; i < currentSize; i++) {
            if (types_[i] == 0) continue;
            addTrigramsForRecord(i, namePool_.data(i), namePool_.length(i));
            addPathTrigramsForRecord(i);
            addExtensionForRecord(i);
            replayCount++;
        }
        // In-place metadata updates during the build may have changed mod times.
        rebuildRecentCache();

        phase2Pending_.store(false, std::memory_order_release);

        LOG_INFO("SearchEngine", "Phase 2 complete: replayed " << replayCount
                 << " mutations, trigram indices active");
    }
}

SearchEngine::V6Snapshot SearchEngine::snapshotForV6() const {
    std::shared_lock lock(mutex_);
    V6Snapshot snap;
    snap.origNamePool = origNamePool_;
    snap.namePool = namePool_;
    snap.pathIndices = pathIndices_;
    snap.pathPool = pathPool_;
    snap.lowerPathPool = lowerPathPool_;
    snap.types = types_;
    snap.sizes = sizes_;
    snap.modTimes = modTimes_;
    snap.inodes = inodes_;
    snap.devIds = devIds_;
    snap.liveCount = liveCount_.load(std::memory_order_relaxed);
    return snap;
}
