#include "SearchEngine.h"
#include "StringUtils.h"
#include "IndexWAL.h"
#include "Logger.h"
#include <algorithm>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstring>

std::string SearchEngine::makeFullPath(std::string_view path, std::string_view name) {
    if (path.empty() || path.back() == '/') {
        std::string result;
        result.reserve(path.size() + name.size());
        result.append(path);
        result.append(name);
        return result;
    }
    std::string result;
    result.reserve(path.size() + 1 + name.size());
    result.append(path);
    result += '/';
    result.append(name);
    return result;
}

uint32_t SearchEngine::internPath(const std::string& path) {
    uint32_t found = pathLookup_.get(path, poolKeyOf(pathPool_));
    if (found != UINT32_MAX) return found;
    uint32_t pIdx = pathPool_.append(path);
    lowerPathPool_.append(me::toLower(path));
    pathLookup_.assignSelf(pIdx, poolKeyOf(pathPool_));
    lowerPathLookup_.assignSelf(pIdx, poolKeyOf(lowerPathPool_));
    return pIdx;
}

void SearchEngine::tombstoneAt(uint32_t idx) {
    types_[idx] = 0;
    markPageDirty(idx);
    sizes_[idx] = 0;
    modTimes_[idx] = 0;
    if (idx < inodes_.size()) inodes_[idx] = 0;
    if (idx < devIds_.size()) devIds_[idx] = 0;
    namePool_.tombstone(idx);
    origNamePool_.tombstone(idx);
}

void SearchEngine::pushRecord(FileRecord&& rec) {
    reserveForAppend(types_);
    reserveForAppend(sizes_);
    reserveForAppend(modTimes_);
    reserveForAppend(inodes_);
    reserveForAppend(devIds_);
    reserveForAppend(pathIndices_);
    types_.push_back(rec.type);
    sizes_.push_back(rec.size);
    modTimes_.push_back(static_cast<int64_t>(rec.modTime));
    inodes_.push_back(rec.inode);
    devIds_.push_back(rec.devId);
}

void SearchEngine::loadRecords(std::vector<FileRecord>&& records) {
    WriteGuard lock(*this);

    const size_t n = records.size();

    // Build SoA columns from incoming records
    types_.resize(n);
    sizes_.resize(n);
    modTimes_.resize(n);
    inodes_.resize(n);
    devIds_.resize(n);
    for (size_t i = 0; i < n; i++) {
        types_[i] = records[i].type;
        sizes_[i] = records[i].size;
        modTimes_[i] = static_cast<int64_t>(records[i].modTime);
        inodes_[i] = records[i].inode;
        devIds_[i] = records[i].devId;
    }

    // Build origNamePool_ from original-case names (for v6 persistence)
    {
        std::vector<std::string> origNames(n);
        for (size_t i = 0; i < n; i++) {
            origNames[i] = records[i].name;
        }
        origNamePool_.loadBulk(origNames);
    }

    // Pre-compute lowercase names into a temporary vector (parallel),
    // then bulk-load into namePool_ (single-threaded append)
    std::vector<std::string> tempLowerNames(n);
    {
        unsigned numThreads = std::thread::hardware_concurrency();
        if (numThreads < 1) numThreads = 1;
        if (numThreads > 32) numThreads = 32;
        size_t chunkSize = (n + numThreads - 1) / numThreads;
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        for (unsigned t = 0; t < numThreads; t++) {
            size_t start = t * chunkSize;
            size_t end = std::min(start + chunkSize, n);
            if (start >= end) break;
            threads.emplace_back([&records, &tempLowerNames, start, end] {
                for (size_t i = start; i < end; i++) {
                    tempLowerNames[i] = me::toLower(records[i].name);
                }
            });
        }
        for (auto& th : threads) th.join();
    }
    namePool_.loadBulk(tempLowerNames);

    // Path deduplication: intern paths into pathPool_ + lowerPathPool_ + pathLookup_
    pathPool_.clear();
    lowerPathPool_.clear();
    pathLookup_.clear();
    lowerPathLookup_.clear();
    pathIndices_.resize(n);
    for (size_t i = 0; i < n; i++) {
        pathIndices_[i] = internPath(records[i].path);
    }

    uint32_t actualLive = rebuildPathIndexUnlocked();

    // Build trigram index for fast filename search
    buildTrigramIndex();
    // Build path trigram index for fast path-only search
    buildPathTrigramIndex();
    rebuildPathIdxToRecords();
    // Build extension index for fast ext: filter queries
    buildExtensionIndex();
    rebuildRecentCache();

    liveCount_.store(actualLive, std::memory_order_relaxed);

    // Initialize dirty page bitmap (no pages dirty after initial load)
    uint32_t pageCount = (static_cast<uint32_t>(n) + kRecordsPerPage - 1) / kRecordsPerPage;
    dirtyPages_.assign(pageCount, false);
    fullRewriteNeeded_.store(false, std::memory_order_relaxed);
}

void SearchEngine::loadRecordsV5(std::vector<FileRecord>&& records,
                                  std::vector<std::string>&& lowerNames,
                                  std::vector<uint32_t>&& pathIndices,
                                  StringPool&& pathDict,
                                  StringPool&& lowerPathDict) {
    WriteGuard lock(*this);

    const size_t n = records.size();

    // Build SoA columns from incoming records
    types_.resize(n);
    sizes_.resize(n);
    modTimes_.resize(n);
    inodes_.resize(n);
    devIds_.resize(n);
    for (size_t i = 0; i < n; i++) {
        types_[i] = records[i].type;
        sizes_[i] = records[i].size;
        modTimes_[i] = static_cast<int64_t>(records[i].modTime);
        inodes_[i] = records[i].inode;
        devIds_[i] = records[i].devId;
    }

    // Build origNamePool_ from original-case names (for v6 persistence)
    {
        std::vector<std::string> origNames(n);
        for (size_t i = 0; i < n; i++) {
            origNames[i] = records[i].name;
        }
        origNamePool_.loadBulk(origNames);
    }

    // Install pre-lowered names directly into namePool_ (skip parallel toLower)
    namePool_.loadBulk(lowerNames);

    // Install pre-built path dictionaries (skip path dedup + internPath loop)
    pathPool_ = std::move(pathDict);
    lowerPathPool_ = std::move(lowerPathDict);
    pathIndices_.assign(pathIndices.begin(), pathIndices.end());

    rebuildPathLookupsUnlocked();
    uint32_t actualLive = rebuildPathIndexUnlocked();

    // Build trigram index for fast filename search
    buildTrigramIndex();
    buildPathTrigramIndex();
    rebuildPathIdxToRecords();
    buildExtensionIndex();
    rebuildRecentCache();

    liveCount_.store(actualLive, std::memory_order_relaxed);

    // Initialize dirty page bitmap
    uint32_t pageCount = (static_cast<uint32_t>(n) + kRecordsPerPage - 1) / kRecordsPerPage;
    dirtyPages_.assign(pageCount, false);
    fullRewriteNeeded_.store(false, std::memory_order_relaxed);
}

FileRecord SearchEngine::getRecord(uint32_t index) const {
    std::shared_lock lock(mutex_);
    if (index >= types_.size()) return {};
    FileRecord r;
    r.type = types_[index];
    if (r.type == 0) return r;
    r.name = origNamePool_.str(index);
    r.path = pathPool_.str(pathIndices_[index]);
    r.size = sizes_[index];
    r.modTime = static_cast<time_t>(modTimes_[index]);
    r.inode = inodes_[index];
    r.devId = devIds_[index];
    return r;
}

uint32_t SearchEngine::recordCount() const {
    std::shared_lock lock(mutex_);
    return static_cast<uint32_t>(types_.size());
}

uint32_t SearchEngine::addRecord(FileRecord&& record) {
    WriteGuard lock(*this);

    std::string fullPath = makeFullPath(record.path, record.name);
    if (wal_) wal_->append(WALOp::Add, fullPath, record);

    // Tombstone existing record at same path to prevent orphaned duplicates
    uint32_t oldIdx = findRecordUnlocked(me::toLower(fullPath));
    if (oldIdx != UINT32_MAX) removeRecordUnlocked(oldIdx, false);

    return appendRecordUnlocked(std::move(record));
}

uint32_t SearchEngine::appendRecordUnlocked(FileRecord&& record) {
    uint32_t idx = static_cast<uint32_t>(types_.size());
    std::string lower = me::toLower(record.name);

    uint32_t pIdx = internPath(record.path);
    if (pIdx >= pathIdxToRecords_.size()) {
        ensurePathTrigramsForPathIdx(pIdx);
    }
    record.path.clear();
    record.path.shrink_to_fit();

    origNamePool_.append(record.name);
    pushRecord(std::move(record));
    namePool_.append(lower);
    pathIndices_.push_back(pIdx);
    pathIndex_.assignSelf(idx, recordKeyOf());

    addTrigramsForRecord(idx, namePool_.data(idx), namePool_.length(idx));
    addPathTrigramsForRecord(idx);
    addExtensionForRecord(idx);

    if (idx / kRecordsPerPage >= dirtyPages_.size()) {
        dirtyPages_.resize(idx / kRecordsPerPage + 1, false);
    }
    markPageDirty(idx);
    liveCount_.fetch_add(1, std::memory_order_relaxed);
    addToRecentCache(idx, static_cast<time_t>(modTimes_[idx]));
    return idx;
}

void SearchEngine::removeRecordUnlocked(uint32_t idx, bool writeWal) {
    if (idx >= types_.size() || types_[idx] == 0) return;
    if (writeWal && wal_) {
        wal_->append(WALOp::Remove, makeFullPath(pathPool_.view(pathIndices_[idx]), origNamePool_.view(idx)));
    }
    // Index cleanup must happen while the record's strings are still live.
    pathIndex_.eraseId(idx, recordKeyOf());
    removeTrigramsForRecord(idx);
    removePathTrigramsForRecord(idx);
    removeExtensionForRecord(idx);
    removeFromRecentCache(idx, static_cast<time_t>(modTimes_[idx]));
    tombstoneAt(idx);
    liveCount_.fetch_sub(1, std::memory_order_relaxed);
}

void SearchEngine::upsertUnlocked(const std::string& fullPath, FileRecord&& updated) {
    uint32_t idx = findRecordUnlocked(me::toLower(fullPath));
    if (idx != UINT32_MAX) {
        if (types_[idx] == updated.type &&
            origNamePool_.view(idx) == updated.name &&
            pathPool_.view(pathIndices_[idx]) == updated.path) {
            // Same entry, new metadata: update columns in place.
            time_t oldModTime = static_cast<time_t>(modTimes_[idx]);
            sizes_[idx] = updated.size;
            modTimes_[idx] = static_cast<int64_t>(updated.modTime);
            inodes_[idx] = updated.inode;
            devIds_[idx] = updated.devId;
            markPageDirty(idx);
            if (oldModTime != updated.modTime) {
                removeFromRecentCache(idx, oldModTime);
                addToRecentCache(idx, updated.modTime);
            }
            return;
        }
        removeRecordUnlocked(idx, false);
    }
    appendRecordUnlocked(std::move(updated));
}

uint32_t SearchEngine::rebuildPathIndexUnlocked() {
    const uint32_t n = static_cast<uint32_t>(types_.size());
    auto keyOf = recordKeyOf();

    // Hash keys in parallel, insert sequentially (last record wins on duplicates).
    Column<uint64_t> hashes(n);
    {
        unsigned numThreads = std::max(1u, std::min(32u, std::thread::hardware_concurrency()));
        uint32_t chunk = (n + numThreads - 1) / numThreads;
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < numThreads; t++) {
            uint32_t start = t * chunk;
            uint32_t end = std::min(start + chunk, n);
            if (start >= end) break;
            threads.emplace_back([&, start, end] {
                for (uint32_t i = start; i < end; i++) {
                    if (types_[i] != 0) hashes[i] = FlatKeyIndex::hashParts(keyOf(i));
                }
            });
        }
        for (auto& th : threads) th.join();
    }

    uint32_t candidates = 0;
    for (uint32_t i = 0; i < n; i++) candidates += types_[i] != 0;
    pathIndex_.clear();
    pathIndex_.reserve(candidates);
    for (uint32_t i = 0; i < n; i++) {
        if (types_[i] == 0) continue;
        pathIndex_.assignSelfHashed(hashes[i], i, keyOf);
    }

    // Tombstone orphaned duplicates: live records that lost to a later duplicate
    std::vector<bool> isWinner(n, false);
    pathIndex_.forEachValue([&](uint32_t idx) { isWinner[idx] = true; });
    uint32_t actualLive = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (types_[i] == 0) continue;
        if (isWinner[i]) {
            actualLive++;
        } else {
            tombstoneAt(i);
        }
    }
    return actualLive;
}

void SearchEngine::rebuildPathLookupsUnlocked() {
    pathLookup_.clear();
    lowerPathLookup_.clear();
    uint32_t n = pathPool_.entryCount();
    pathLookup_.reserve(n);
    lowerPathLookup_.reserve(n);
    auto origKey = poolKeyOf(pathPool_);
    auto lowerKey = poolKeyOf(lowerPathPool_);
    for (uint32_t i = 0; i < n; i++) {
        if (!pathPool_.isLive(i)) continue;
        pathLookup_.assignSelf(i, origKey);
        lowerPathLookup_.assignSelf(i, lowerKey);
    }
}

namespace {
// Order where '/' sorts before every other byte, so a directory's
// descendants form one contiguous run right after the directory itself.
int comparePathOrder(std::string_view a, std::string_view b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = a[i] == '/' ? 0 : static_cast<unsigned char>(a[i]);
        unsigned char cb = b[i] == '/' ? 0 : static_cast<unsigned char>(b[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

bool isSameOrUnder(std::string_view path, std::string_view prefix) {
    return path.size() >= prefix.size() &&
           path.compare(0, prefix.size(), prefix) == 0 &&
           (path.size() == prefix.size() || path[prefix.size()] == '/');
}
} // namespace

std::vector<uint32_t> SearchEngine::collectSubtreeUnlocked(const std::vector<std::string>& lowerPrefixes) const {
    // Normalize, sort, and drop prefixes nested inside another prefix.
    std::vector<std::string> sorted;
    for (std::string p : lowerPrefixes) {
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        if (p.empty() || p == "/") continue; // never wipe the whole index
        sorted.push_back(std::move(p));
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const std::string& a, const std::string& b) { return comparePathOrder(a, b) < 0; });
    std::vector<std::string> prefixes;
    for (auto& p : sorted) {
        if (!prefixes.empty() && isSameOrUnder(p, prefixes.back())) continue;
        prefixes.push_back(std::move(p));
    }
    std::vector<uint32_t> result;
    if (prefixes.empty()) return result;

    // Records whose own full path is one of the prefixes (the directories themselves).
    for (const auto& p : prefixes) {
        uint32_t idx = findRecordUnlocked(p);
        if (idx != UINT32_MAX) result.push_back(idx);
    }

    // Directory paths at or below a prefix.
    uint32_t pathCount = lowerPathPool_.entryCount();
    std::vector<bool> matched(pathCount, false);
    bool anyMatched = false;
    for (uint32_t pi = 0; pi < pathCount; pi++) {
        std::string_view s = lowerPathPool_.view(pi);
        if (s.empty()) continue;
        auto it = std::upper_bound(prefixes.begin(), prefixes.end(), s,
            [](std::string_view v, const std::string& p) { return comparePathOrder(v, p) < 0; });
        if (it == prefixes.begin()) continue;
        if (isSameOrUnder(s, *std::prev(it))) {
            matched[pi] = true;
            anyMatched = true;
        }
    }
    if (!anyMatched) return result;

    bool haveChildLists = dirRecordsBuilt_ && pathIdxToRecords_.size() >= pathCount;
    if (haveChildLists) {
        for (uint32_t pi = 0; pi < pathCount; pi++) {
            if (!matched[pi]) continue;
            for (uint32_t idx : pathIdxToRecords_[pi]) {
                if (idx < types_.size() && types_[idx] != 0) result.push_back(idx);
            }
        }
    } else {
        for (uint32_t idx = 0; idx < types_.size(); idx++) {
            if (types_[idx] != 0 && pathIndices_[idx] < pathCount && matched[pathIndices_[idx]]) {
                result.push_back(idx);
            }
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool SearchEngine::removeByPathUnlocked(const std::string& fullPath) {
    uint32_t idx = findRecordUnlocked(me::toLower(fullPath));
    if (idx == UINT32_MAX) return false;
    if (wal_) wal_->append(WALOp::Remove, fullPath);
    removeRecordUnlocked(idx, false);
    return true;
}

bool SearchEngine::removeByPath(const std::string& fullPath) {
    WriteGuard lock(*this);
    return removeByPathUnlocked(fullPath);
}

uint32_t SearchEngine::removeByPathPrefix(const std::string& pathPrefix) {
    WriteGuard lock(*this);
    auto victims = collectSubtreeUnlocked({me::toLower(pathPrefix)});
    for (uint32_t idx : victims) removeRecordUnlocked(idx, true);
    return static_cast<uint32_t>(victims.size());
}

uint32_t SearchEngine::batchRescanPrefix(const std::string& pathPrefix,
                                         std::vector<FileRecord>&& freshRecords) {
    WriteGuard lock(*this);

    // ── Phase 1: Tombstone old records beneath prefix ──
    // The directory's own record is kept: scans return only its contents.
    std::string lowerPrefix = me::toLower(pathPrefix);
    uint32_t self = findRecordUnlocked(lowerPrefix);
    uint32_t removed = 0;
    for (uint32_t idx : collectSubtreeUnlocked({lowerPrefix})) {
        if (idx == self) continue;
        removeRecordUnlocked(idx, true);
        removed++;
    }

    // ── Phase 2: Add fresh records ──
    for (auto& record : freshRecords) {
        std::string fullPath = makeFullPath(record.path, record.name);
        if (wal_) wal_->append(WALOp::Update, fullPath, record);
        upsertUnlocked(fullPath, std::move(record));
    }

    return removed;
}

void SearchEngine::updateByPathUnlocked(const std::string& fullPath, FileRecord&& updated) {
    if (wal_) wal_->append(WALOp::Update, fullPath, updated);
    upsertUnlocked(fullPath, std::move(updated));
}

void SearchEngine::updateByPath(const std::string& fullPath, FileRecord&& updated) {
    WriteGuard lock(*this);
    updateByPathUnlocked(fullPath, std::move(updated));
}

void SearchEngine::batchMutate(std::vector<MutationOp>&& ops) {
    if (ops.empty()) return;
    WriteGuard lock(*this);
    // Removed directories: FSEvents does not reliably report every descendant
    // (e.g. rm -rf or a move coalesced into one batch), so drop whole subtrees.
    std::vector<std::string> removedDirs;
    auto lowerKey = poolKeyOf(lowerPathPool_);
    for (auto& op : ops) {
        if (op.type == MutationOp::REMOVE) {
            removeByPathUnlocked(op.path);
            std::string lower = me::toLower(op.path);
            if (lowerPathLookup_.find(lower, lowerKey) != FlatKeyIndex::npos) {
                removedDirs.push_back(std::move(lower));
            }
        } else {
            updateByPathUnlocked(op.path, std::move(op.record));
        }
    }
    if (!removedDirs.empty()) {
        for (uint32_t idx : collectSubtreeUnlocked(removedDirs)) removeRecordUnlocked(idx, true);
    }
}

std::unordered_map<uint32_t, uint32_t> SearchEngine::compactRecords() {
    // ── Phase 1: Snapshot under shared_lock ──
    // Queries continue unblocked during snapshot copy.
    Column<uint8_t> snapTypes;
    Column<uint64_t> snapSizes;
    Column<int64_t> snapModTimes;
    Column<uint64_t> snapInodes;
    Column<int32_t> snapDevIds;
    StringPool snapNamePool;
    StringPool snapOrigNamePool;
    Column<uint32_t> snapPathIndices;
    StringPool snapPathPool;
    std::vector<bool> snapIndexed;
    uint32_t snapSize;
    {
        std::shared_lock lock(mutex_);
        uint32_t live = liveCount_.load(std::memory_order_relaxed);
        if (live == types_.size()) return {}; // nothing to compact
        snapTypes = types_;
        snapSizes = sizes_;
        snapModTimes = modTimes_;
        snapInodes = inodes_;
        snapDevIds = devIds_;
        snapNamePool = namePool_;
        snapOrigNamePool = origNamePool_;
        snapPathIndices = pathIndices_;
        snapPathPool = pathPool_;
        snapSize = static_cast<uint32_t>(types_.size());
        snapIndexed.assign(snapSize, false);
        pathIndex_.forEachValue([&](uint32_t idx) { if (idx < snapSize) snapIndexed[idx] = true; });
    }

    LOG_INFO("SearchEngine", "COW compaction Phase 2: building compacted data from "
             << snapSize << " records");

    // ── Phase 2: Build compacted data (no lock held) ──
    // Mutations (addRecord/removeByPath/updateByPath/batchRescanPrefix) continue
    // on the live data; they will be replayed in Phase 3.
    std::unordered_map<uint32_t, uint32_t> remap;
    remap.reserve(snapSize);

    Column<uint8_t> cdTypes;
    Column<uint64_t> cdSizes;
    Column<int64_t> cdModTimes;
    Column<uint64_t> cdInodes;
    Column<int32_t> cdDevIds;
    cdTypes.reserve(snapSize);
    cdSizes.reserve(snapSize);
    cdModTimes.reserve(snapSize);
    cdInodes.reserve(snapSize);
    cdDevIds.reserve(snapSize);
    StringPool cdNamePool;
    StringPool cdOrigNamePool;
    Column<uint32_t> cdPathIndices;
    cdPathIndices.reserve(snapSize);
    StringPool cdPathPool;
    StringPool cdLowerPathPool;
    FlatKeyIndex cdPathLookup;
    FlatKeyIndex cdLowerPathLookup;
    FlatKeyIndex cdPathIndex;
    auto cdOrigPathKey = poolKeyOf(cdPathPool);
    auto cdLowerPathKey = poolKeyOf(cdLowerPathPool);
    auto cdRecordKey = [&](uint32_t i) {
        return recordKeyParts(cdLowerPathPool.view(cdPathIndices[i]), cdNamePool.view(i));
    };
    cdPathIndex.reserve(snapSize);

    for (size_t i = 0; i < snapSize; i++) {
        if (snapTypes[i] == 0) continue;
        // Skip orphaned duplicates: live records not referenced by pathIndex_
        if (!snapIndexed[i]) continue;
        uint32_t newIdx = static_cast<uint32_t>(cdTypes.size());
        remap[static_cast<uint32_t>(i)] = newIdx;
        // Intern path into compacted pool (both original and lowered)
        std::string_view origPath = snapPathPool.view(snapPathIndices[i]);
        uint32_t newPIdx = cdPathLookup.get(origPath, cdOrigPathKey);
        if (newPIdx == UINT32_MAX) {
            newPIdx = cdPathPool.append(origPath);
            cdLowerPathPool.append(me::toLower(std::string(origPath)));
            cdPathLookup.assignSelf(newPIdx, cdOrigPathKey);
            cdLowerPathLookup.assignSelf(newPIdx, cdLowerPathKey);
        }
        // Copy name from snapshot pool into compacted pool
        cdOrigNamePool.append(snapOrigNamePool.data(i), snapOrigNamePool.length(i));
        cdNamePool.append(snapNamePool.data(i), snapNamePool.length(i));
        cdPathIndices.push_back(newPIdx);
        cdPathIndex.assignSelf(newIdx, cdRecordKey);
        cdTypes.push_back(snapTypes[i]);
        cdSizes.push_back(snapSizes[i]);
        cdModTimes.push_back(snapModTimes[i]);
        cdInodes.push_back(snapInodes[i]);
        cdDevIds.push_back(snapDevIds[i]);
    }
    uint32_t cdLiveCount = static_cast<uint32_t>(cdTypes.size());

    // Build trigram index, path trigram index, extension index, and recent cache outside any lock
    decltype(nameTrigramIndex_) cdTrigramIndex;
    decltype(pathTrigramIndex_) cdPathTrigramIndex;
    if (trigramIndexEnabled_) {
        cdTrigramIndex = buildTrigramIndexFromData(cdTypes, cdNamePool);
        cdPathTrigramIndex = buildPathTrigramIndexFromData(cdLowerPathPool);
    }
    auto cdPathIdxToRecords = buildPathIdxToRecordsFromData(cdTypes, cdPathIndices, cdPathPool.entryCount());
    auto cdExtensionIndex = buildExtensionIndexFromData(cdTypes, cdNamePool);
    auto cdRecentCache = buildRecentCacheFromData(cdTypes, cdModTimes, kRecentCacheSize);

    LOG_INFO("SearchEngine", "COW compaction Phase 3: swapping data, compacted "
             << snapSize << " -> " << cdLiveCount << " records");

    // ── Phase 3: Swap + replay under unique_lock ──
    // This lock is held only long enough to move data and replay the small
    // number of mutations that occurred during Phase 2 (~milliseconds).
    {
        WriteGuard lock(*this);

        // Move out current (mutated-during-Phase2) state
        auto oldTypes = std::move(types_);
        auto oldSizes = std::move(sizes_);
        auto oldModTimes = std::move(modTimes_);
        auto oldInodes = std::move(inodes_);
        auto oldDevIds = std::move(devIds_);
        auto oldOrigNamePool = std::move(origNamePool_);
        auto oldPathIndices = std::move(pathIndices_);
        auto oldPathPool = std::move(pathPool_);

        // Install compacted data
        types_ = std::move(cdTypes);
        sizes_ = std::move(cdSizes);
        modTimes_ = std::move(cdModTimes);
        inodes_ = std::move(cdInodes);
        devIds_ = std::move(cdDevIds);
        origNamePool_ = std::move(cdOrigNamePool);
        namePool_ = std::move(cdNamePool);
        pathIndices_ = std::move(cdPathIndices);
        pathPool_ = std::move(cdPathPool);
        lowerPathPool_ = std::move(cdLowerPathPool);
        pathLookup_ = std::move(cdPathLookup);
        lowerPathLookup_ = std::move(cdLowerPathLookup);
        pathIndex_ = std::move(cdPathIndex);
        nameTrigramIndex_ = std::move(cdTrigramIndex);
        pathTrigramIndex_ = std::move(cdPathTrigramIndex);
        pathIdxToRecords_ = std::move(cdPathIdxToRecords);
        dirRecordsBuilt_ = true;
        extensionIndex_ = std::move(cdExtensionIndex);
        recentCache_ = std::move(cdRecentCache);
        liveCount_.store(cdLiveCount, std::memory_order_relaxed);

        // Replay changes to snapshotted records made during Phase 2:
        // deletions, and in-place metadata updates.
        uint32_t replayedDeletes = 0;
        for (const auto& [oldIdx, newIdx] : remap) {
            if (oldIdx >= oldTypes.size()) continue;
            if (oldTypes[oldIdx] == 0) {
                removeRecordUnlocked(newIdx, false);
                replayedDeletes++;
            } else if (oldModTimes[oldIdx] != modTimes_[newIdx] || oldSizes[oldIdx] != sizes_[newIdx]) {
                sizes_[newIdx] = oldSizes[oldIdx];
                modTimes_[newIdx] = oldModTimes[oldIdx];
                inodes_[newIdx] = oldInodes[oldIdx];
                devIds_[newIdx] = oldDevIds[oldIdx];
            }
        }

        // Replay new records appended during Phase 2
        uint32_t replayedAdds = 0;
        for (size_t i = snapSize; i < oldTypes.size(); i++) {
            if (oldTypes[i] == 0) continue;
            FileRecord rec;
            rec.name = oldOrigNamePool.str(i);
            rec.path = oldPathPool.str(oldPathIndices[i]);
            rec.type = oldTypes[i];
            rec.size = oldSizes[i];
            rec.modTime = static_cast<time_t>(oldModTimes[i]);
            rec.inode = oldInodes[i];
            rec.devId = oldDevIds[i];
            std::string fullPath = makeFullPath(rec.path, rec.name);
            upsertUnlocked(fullPath, std::move(rec));
            replayedAdds++;
        }

        compactionGen_.fetch_add(1, std::memory_order_relaxed);

        LOG_INFO("SearchEngine", "COW compaction done: replayed " << replayedAdds
                 << " adds, " << replayedDeletes << " deletes, live="
                 << liveCount_.load(std::memory_order_relaxed));
    }

    fullRewriteNeeded_.store(true, std::memory_order_relaxed);
    phase2Pending_.store(false, std::memory_order_release);

    return remap;
}

void SearchEngine::attachWAL(std::shared_ptr<IndexWAL> wal) {
    WriteGuard lock(*this);
    wal_ = std::move(wal);
}

void SearchEngine::detachWAL() {
    WriteGuard lock(*this);
    wal_.reset();
}

std::vector<uint32_t> SearchEngine::recentIndices(uint32_t count) const {
    std::shared_lock lock(mutex_);
    std::vector<uint32_t> result;
    uint32_t n = std::min(count, static_cast<uint32_t>(recentCache_.size()));
    result.reserve(n);
    auto it = recentCache_.begin();
    for (uint32_t i = 0; i < n; ++i, ++it) {
        result.push_back(it->index);
    }
    return result;
}

std::set<SearchEngine::RecentEntry>
SearchEngine::buildRecentCacheFromData(const Column<uint8_t>& types,
                                       const Column<int64_t>& modTimes,
                                       uint32_t cacheSize) {
    // Bounded min-heap of the newest entries: O(N log K) time, O(K) memory.
    struct TimePair { time_t modTime; uint32_t index; };
    auto newer = [](const TimePair& a, const TimePair& b) { return a.modTime > b.modTime; };
    std::vector<TimePair> top;
    top.reserve(cacheSize + 1);
    for (size_t i = 0; i < types.size() && cacheSize > 0; i++) {
        if (types[i] == 0) continue;
        TimePair p{static_cast<time_t>(modTimes[i]), static_cast<uint32_t>(i)};
        if (top.size() < cacheSize) {
            top.push_back(p);
            std::push_heap(top.begin(), top.end(), newer);
        } else if (p.modTime > top.front().modTime) {
            std::pop_heap(top.begin(), top.end(), newer);
            top.back() = p;
            std::push_heap(top.begin(), top.end(), newer);
        }
    }
    std::set<RecentEntry> cache;
    for (const auto& p : top) cache.insert({p.modTime, p.index});
    return cache;
}

void SearchEngine::rebuildRecentCache() {
    recentCache_ = buildRecentCacheFromData(types_, modTimes_, kRecentCacheSize);
}

void SearchEngine::addToRecentCache(uint32_t idx, time_t modTime) {
    recentCache_.insert({modTime, idx});
    if (recentCache_.size() > kRecentCacheSize) {
        recentCache_.erase(std::prev(recentCache_.end()));
    }
}

void SearchEngine::removeFromRecentCache(uint32_t idx, time_t modTime) {
    recentCache_.erase({modTime, idx});
}

uint32_t SearchEngine::indexForPath(const std::string& fullPath) const {
    std::shared_lock lock(mutex_);
    return findRecordUnlocked(me::toLower(fullPath));
}

std::vector<FileRecord> SearchEngine::exportRecords() const {
    std::shared_lock lock(mutex_);
    std::vector<FileRecord> result;
    result.reserve(liveCount_.load(std::memory_order_relaxed));
    for (size_t i = 0; i < types_.size(); i++) {
        if (types_[i] == 0) continue; // skip tombstones
        FileRecord r;
        r.name = origNamePool_.str(i);
        r.type = types_[i];
        r.size = sizes_[i];
        r.modTime = static_cast<time_t>(modTimes_[i]);
        r.inode = inodes_[i];
        r.devId = devIds_[i];
        r.path = pathPool_.str(pathIndices_[i]);
        result.push_back(std::move(r));
    }
    return result;
}

void SearchEngine::replayWALEntries(std::vector<WALEntry>&& entries) {
    if (entries.empty()) return;

    WriteGuard lock(*this);

    for (auto& e : entries) {
        switch (e.op) {
        case WALOp::Add:
        case WALOp::Update:
            upsertUnlocked(e.fullPath, std::move(e.record));
            break;
        case WALOp::Remove: {
            uint32_t idx = findRecordUnlocked(me::toLower(e.fullPath));
            if (idx != UINT32_MAX) removeRecordUnlocked(idx, false);
            break;
        }
        }
    }

    rebuildRecentCache();
}
