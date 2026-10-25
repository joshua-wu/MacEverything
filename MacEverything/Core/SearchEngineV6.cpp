#include "SearchEngine.h"
#include "StringUtils.h"
#include "Logger.h"
#include <algorithm>
#include <thread>

// ---------------------------------------------------------------------------
// Flat storage: loadStorage, persistStorage, completePhase2
// ---------------------------------------------------------------------------

void SearchEngine::loadStorage(Storage&& st) {
    WriteGuard lock(*this);

    uint32_t n = st.origNamePool.entryCount();

    origNamePool_ = std::move(st.origNamePool);
    namePool_ = std::move(st.namePool);
    pathIndices_ = std::move(st.pathIndices);
    pathPool_ = std::move(st.pathPool);
    lowerPathPool_ = std::move(st.lowerPathPool);
    types_ = std::move(st.types);
    sizes_ = std::move(st.sizes);
    modTimes_ = std::move(st.modTimes);
    inodes_ = std::move(st.inodes);
    devIds_ = std::move(st.devIds);

    uint32_t actualLive;
    if (st.hasLookups) {
        pathLookup_ = std::move(st.pathLookup);
        lowerPathLookup_ = std::move(st.lowerPathLookup);
        pathIndex_ = std::move(st.pathIndex);
        actualLive = st.liveCount;
    } else {
        rebuildPathLookupsUnlocked();
        actualLive = rebuildPathIndexUnlocked();
    }
    liveCount_.store(actualLive, std::memory_order_relaxed);

    if (st.hasDirRecords) {
        pathIdxToRecords_ = std::move(st.dirRecords);
        dirRecordsBuilt_ = true;
    } else {
        pathIdxToRecords_ = {};
        dirRecordsBuilt_ = false;
    }
    nameTrigramIndex_.clear();
    pathTrigramIndex_.clear();
    extensionIndex_.clear();
    recentCache_.clear();

    // Phase 2: defer secondary index building to background
    phase2Pending_.store(true, std::memory_order_release);
    phase2StartRecordCount_ = static_cast<uint32_t>(types_.size());

    // Initialize dirty page bitmap
    uint32_t pageCount = (n + kRecordsPerPage - 1) / kRecordsPerPage;
    dirtyPages_.assign(pageCount, false);
    fullRewriteNeeded_.store(false, std::memory_order_relaxed);

    LOG_INFO("SearchEngine", "loadStorage: loaded " << n << " records (" << actualLive
             << " live, lookups " << (st.hasLookups ? "loaded" : "rebuilt")
             << ", dir lists " << (st.hasDirRecords ? "loaded" : "deferred") << "), Phase 2 pending");
}

bool SearchEngine::persistStorage(const std::function<bool(const StorageView&)>& write,
                                  const std::function<bool(Storage&)>& reload) {
    std::lock_guard<std::mutex> gate(writeGate_);
    {
        std::shared_lock lock(mutex_);
        StorageView view{origNamePool_, namePool_, pathIndices_, pathPool_, lowerPathPool_,
                         types_, sizes_, modTimes_, inodes_, devIds_,
                         pathLookup_, lowerPathLookup_, pathIndex_, pathIdxToRecords_,
                         dirRecordsBuilt_, liveCount_.load(std::memory_order_relaxed)};
        if (!write(view)) return false;
    }

    Storage fresh;
    if (!reload || !reload(fresh)) {
        LOG_ERROR("SearchEngine", "persistStorage: written copy could not be reloaded, keeping current storage");
        return true;
    }

    std::unique_lock lock(mutex_);
    if (!fresh.hasLookups || fresh.types.size() != types_.size() ||
        fresh.pathPool.entryCount() != pathPool_.entryCount() ||
        fresh.namePool.rawSize() != namePool_.rawSize()) {
        LOG_ERROR("SearchEngine", "persistStorage: reloaded copy does not match, keeping current storage");
        return true;
    }
    // Same contents, new backing: indices into the columns stay valid.
    origNamePool_ = std::move(fresh.origNamePool);
    namePool_ = std::move(fresh.namePool);
    pathIndices_ = std::move(fresh.pathIndices);
    pathPool_ = std::move(fresh.pathPool);
    lowerPathPool_ = std::move(fresh.lowerPathPool);
    types_ = std::move(fresh.types);
    sizes_ = std::move(fresh.sizes);
    modTimes_ = std::move(fresh.modTimes);
    inodes_ = std::move(fresh.inodes);
    devIds_ = std::move(fresh.devIds);
    pathLookup_ = std::move(fresh.pathLookup);
    lowerPathLookup_ = std::move(fresh.lowerPathLookup);
    pathIndex_ = std::move(fresh.pathIndex);
    if (fresh.hasDirRecords) {
        pathIdxToRecords_ = std::move(fresh.dirRecords);
        dirRecordsBuilt_ = true;
    }
    return true;
}

void SearchEngine::completePhase2() {
    if (!phase2Pending_.load(std::memory_order_acquire)) return;

    if (!trigramIndexEnabled_) {
        // Without trigram indices the remaining structures are cheap linear
        // passes over the columns (tens of ms); build them in place instead of
        // copying the columns for a lock-free build.
        WriteGuard lock(*this);
        if (!phase2Pending_.load(std::memory_order_acquire)) return;
        if (!dirRecordsBuilt_) rebuildPathIdxToRecords();
        buildExtensionIndex();
        rebuildRecentCache();
        phase2Pending_.store(false, std::memory_order_release);
        LOG_INFO("SearchEngine", "Phase 2 complete: secondary indices active");
        return;
    }

    LOG_INFO("SearchEngine", "Phase 2: building secondary indices in background...");

    // Snapshot data needed for building indices (under shared lock)
    Column<uint8_t> snapTypes;
    Column<int64_t> snapModTimes;
    StringPool snapNamePool;
    StringPool snapLowerPathPool;
    Column<uint32_t> snapPathIndices;
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
    decltype(extensionIndex_) extensionIndex;
    if (extensionIndexEnabled_) extensionIndex = buildExtensionIndexFromData(snapTypes, snapNamePool);

    LOG_INFO("SearchEngine", "Phase 2: indices built, swapping under lock...");

    // Swap under unique lock and replay mutations that occurred during build
    {
        WriteGuard lock(*this);

        nameTrigramIndex_ = std::move(trigramIndex);
        pathTrigramIndex_ = std::move(pathTrigramIndex);
        pathIdxToRecords_ = std::move(pathIdxToRecords);
        dirRecordsBuilt_ = true;
        recentCache_ = std::move(recentCache);
        extensionIndex_ = std::move(extensionIndex);

        // Replay tombstones: records live in the snapshot but deleted during build.
        // namePool_[i] is already tombstoned, so use the snapshot name.
        uint32_t replayCount = 0;
        for (uint32_t i = 0; i < snapSize && i < types_.size(); i++) {
            if (snapTypes[i] != 0 && types_[i] == 0) {
                removeTrigramsForName(i, snapNamePool.view(i));
                removeExtensionForName(i, snapNamePool.view(i));
                pathIdxToRecords_.eraseSorted(snapPathIndices[i], i);
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
                 << " mutations, secondary indices active");
    }
}
