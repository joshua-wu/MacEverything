#pragma once
#include "FileRecord.h"
#include "ContentIndex.h" // for Trigram type and extractTrigrams/makeTrigram
#include "StringPool.h"
#include "SIMDSearch.h"
#include "FlatKeyIndex.h"
#include "DirPostings.h"
#include "Column.h"
#include <functional>
#include <vector>
#include <string>
#include <string_view>
#include <cstdint>
#include <cstdio>
#include <shared_mutex>
#include <unordered_map>
#include <atomic>
#include <memory>
#include <map>
#include <set>
#include <mutex>
#include <utility>

#include "IndexWAL.h"
#include "QueryAST.h"
#include "StructuredQueryParser.h"

/// Deduplication table for directory path strings.
/// Interns unique paths and returns a compact uint32_t index.
class PathTable {
public:
    /// Look up or insert a path, returning its index.
    uint32_t intern(const std::string& path) {
        auto it = lookup_.find(path);
        if (it != lookup_.end()) return it->second;
        uint32_t idx = static_cast<uint32_t>(paths_.size());
        paths_.push_back(path);
        lookup_[path] = idx;
        return idx;
    }

    /// Resolve an index back to its path string.
    /// Returns empty string if index is out of range.
    const std::string& resolve(uint32_t index) const {
        static const std::string empty;
        if (index >= paths_.size()) return empty;
        return paths_[index];
    }

    /// Number of unique paths stored.
    uint32_t size() const { return static_cast<uint32_t>(paths_.size()); }

    /// Clear all entries.
    void clear() { paths_.clear(); lookup_.clear(); }

    PathTable() = default;
    PathTable(PathTable&&) = default;
    PathTable& operator=(PathTable&&) = default;
    PathTable(const PathTable&) = default;
    PathTable& operator=(const PathTable&) = default;

private:
    std::vector<std::string> paths_;                    // index -> path
    std::unordered_map<std::string, uint32_t> lookup_;  // path -> index
};

/// Metadata stored in the index header (v3+). Extensible key-value pairs.
struct IndexMetadata {
    uint32_t formatVersion = 0; // populated on load
    int64_t  timestamp = 0;     // creation timestamp (seconds since epoch)
    uint64_t lastEventId = 0;   // FSEvents stream ID for incremental replay
    std::map<std::string, std::string> extra; // extensible key-value pairs

    // Well-known metadata keys
    static constexpr const char* kScanRoot     = "scan_root";      // e.g. "/"
    static constexpr const char* kAppVersion   = "app_version";    // e.g. "1.0.0"
    static constexpr const char* kOSVersion    = "os_version";     // e.g. "14.5"
    static constexpr const char* kRecordFormat = "record_format";  // e.g. "v2_inode"
};

/// Detailed timing breakdown from a search query.
struct QueryTimingInfo {
    double totalMs = 0;
    double lockWaitMs = 0;
    double trigramMs = 0;     // trigram index lookup (trigram path only)
    double phase1Ms = 0;      // name verification (trigram path only)
    double phase2Ms = 0;      // path-only match / NEON full scan
    double sortMs = 0;
    double lockHeldMs = 0;
    size_t totalRecords = 0;
    size_t candidates = 0;    // trigram candidates count
    size_t nameMatches = 0;   // phase1 results (name matches)
    size_t pathMatches = 0;   // phase2 results (path-only matches)
    size_t resultCount = 0;
    bool usedTrigram = false;
    std::string searchPath;   // "trigram", "trigram-split", or "linear"
};

/// Bundled cancellation context passed through query call chain.
struct QueryCancelCtx {
    const std::atomic<uint64_t>* gen;
    uint64_t myGen;
    bool cancelled() const {
        return gen->load(std::memory_order_relaxed) != myGen;
    }
};

class SearchEngine {
public:
    /// Takes ownership of scanned records and pre-computes lowercase names.
    void loadRecords(std::vector<FileRecord>&& records);

    /// v5 fast-path load: accepts pre-separated data from on-disk path dictionary.
    /// Skips toLower of names and path dedup since they are pre-computed.
    void loadRecordsV5(std::vector<FileRecord>&& records,
                       std::vector<std::string>&& lowerNames,
                       std::vector<uint32_t>&& pathIndices,
                       StringPool&& pathDict,
                       StringPool&& lowerPathDict);

    /// All per-record storage, in the shape it is persisted in the flat index
    /// file. Lookup tables and directory lists are optional: when absent they
    /// are rebuilt from the columns.
    struct Storage {
        StringPool origNamePool;   // original-case names
        StringPool namePool;       // lowercase names
        Column<uint32_t> pathIndices;
        StringPool pathPool;
        StringPool lowerPathPool;
        Column<uint8_t> types;
        Column<uint64_t> sizes;
        Column<int64_t> modTimes;
        Column<uint64_t> inodes;
        Column<int32_t> devIds;
        FlatKeyIndex pathLookup;
        FlatKeyIndex lowerPathLookup;
        FlatKeyIndex pathIndex;
        DirPostings dirRecords;
        bool hasLookups = false;     // pathLookup/lowerPathLookup/pathIndex valid
        bool hasDirRecords = false;  // dirRecords valid
        uint32_t liveCount = 0;      // required with hasLookups
    };

    /// Read-only view of the engine's storage, for writing it out.
    struct StorageView {
        const StringPool& origNamePool;
        const StringPool& namePool;
        const Column<uint32_t>& pathIndices;
        const StringPool& pathPool;
        const StringPool& lowerPathPool;
        const Column<uint8_t>& types;
        const Column<uint64_t>& sizes;
        const Column<int64_t>& modTimes;
        const Column<uint64_t>& inodes;
        const Column<int32_t>& devIds;
        const FlatKeyIndex& pathLookup;
        const FlatKeyIndex& lowerPathLookup;
        const FlatKeyIndex& pathIndex;
        const DirPostings& dirRecords;
        bool dirRecordsComplete;     // false while Phase 2 is pending
        uint32_t liveCount;
    };

    /// Install loaded storage (e.g. columns mapped from the flat index file).
    /// Only builds what the storage lacks; the rest is deferred to Phase 2.
    void loadStorage(Storage&& storage);

    /// Write the storage out and switch to the copy that was written.
    /// `write` runs under a shared lock with mutations held off; if it
    /// succeeds, `reload` reads the written copy back (normally as private
    /// file mappings) and it replaces the in-memory storage, which drops pages
    /// dirtied or reallocated since the last load. Returns write's result.
    bool persistStorage(const std::function<bool(const StorageView&)>& write,
                        const std::function<bool(Storage&)>& reload);

    /// Complete Phase 2 of two-stage startup: build trigram indices in background,
    /// then swap under lock and replay mutations that occurred during build.
    void completePhase2();

    /// Trigram indices trade ~13 bytes per record (plus path postings) for
    /// faster substring candidate lookup. Disabled by default: a parallel scan
    /// of the name pool plus per-directory match caches is fast enough and
    /// keeps memory close to the raw data size. Set before loading records.
    void setTrigramIndexEnabled(bool enabled) { trigramIndexEnabled_ = enabled; }
    bool trigramIndexEnabled() const { return trigramIndexEnabled_; }

    /// The extension index speeds up ext: filters (~4 bytes per file). Disabled
    /// by default: the linear scan evaluates ext: filters in a few ms.
    void setExtensionIndexEnabled(bool enabled) { extensionIndexEnabled_ = enabled; }
    bool extensionIndexEnabled() const { return extensionIndexEnabled_; }

    /// Whether Phase 2 is pending (trigram indices not yet built).
    bool isPhase2Pending() const { return phase2Pending_.load(std::memory_order_acquire); }

    /// Case-insensitive substring search. Returns indices into the records array.
    /// If maxResults > 0, stops early once enough matches are found.
    /// If useTrigram is false, bypasses trigram index and does NEON full scan.
    std::vector<uint32_t> query(const std::string& keyword, uint32_t maxResults = 0,
                                bool useTrigram = true, uint64_t sessionId = 0) const;

    /// Same as query() but also populates timing breakdown.
    std::vector<uint32_t> query(const std::string& keyword, uint32_t maxResults,
                                bool useTrigram, QueryTimingInfo& timing,
                                uint64_t sessionId = 0) const;

    /// Advanced query: evaluate an AST with boolean operators, quoted phrases,
    /// and filter nodes. Uses trigram index for TERM nodes, then applies
    /// AND/OR/NOT logic. Called automatically by query() when advanced syntax
    /// is detected.
    std::vector<uint32_t> queryAdvanced(const std::string& input, uint32_t maxResults,
                                         bool useTrigram, QueryTimingInfo& timing,
                                         uint64_t myGen = 0,
                                         const std::atomic<uint64_t>* genPtr = nullptr) const;

    /// Cancel in-flight queries for the given session. Lock-free on the generation atomic.
    void cancelSession(uint64_t sessionId) const;

    /// Get or create the generation atomic for a session. Returns {atomicPtr, currentGenValue}.
    std::pair<std::shared_ptr<std::atomic<uint64_t>>, uint64_t>
    acquireSessionGeneration(uint64_t sessionId) const;

    FileRecord getRecord(uint32_t index) const;
    uint32_t recordCount() const;

    // --- Incremental mutation (for FSEvents) ---

    /// Batch mutation operation descriptor.
    struct MutationOp {
        enum Type { REMOVE, UPDATE };
        Type type;
        std::string path;
        FileRecord record;  // only used for UPDATE
    };

    /// Append a new record. Returns the new index. Thread-safe.
    uint32_t addRecord(FileRecord&& record);

    /// Mark record at fullPath as deleted (tombstone). Thread-safe.
    /// Returns true if found and removed.
    bool removeByPath(const std::string& fullPath);

    /// Remove old record at fullPath (if any) and add updated record. Thread-safe.
    void updateByPath(const std::string& fullPath, FileRecord&& updated);

    /// Apply multiple mutations under a single write lock. Thread-safe.
    /// Reduces lock contention compared to calling removeByPath/updateByPath individually.
    void batchMutate(std::vector<MutationOp>&& ops);

    /// Remove all records whose full path starts with the given prefix. Thread-safe.
    /// Returns the number of records removed.
    uint32_t removeByPathPrefix(const std::string& pathPrefix);

    /// Batch-replace all records under pathPrefix with freshRecords.
    /// Tombstones old records, adds fresh records, and rebuilds trigram index once.
    /// Returns the number of old records removed. Thread-safe.
    uint32_t batchRescanPrefix(const std::string& pathPrefix,
                               std::vector<FileRecord>&& freshRecords);

    /// Replay WAL entries in-place using pathIndex_ for O(1) lookups.
    /// Skips WAL writes (entries came FROM the WAL). Thread-safe.
    void replayWALEntries(std::vector<WALEntry>&& entries);

    /// Number of live (non-tombstoned) records.
    uint32_t liveRecordCount() const { return liveCount_.load(std::memory_order_relaxed); }

    /// Compact in-memory records by removing tombstoned entries.
    /// Rebuilds pathIndex_ and namePool_. Thread-safe.
    /// Returns a mapping from old index → new index for live records.
    /// Returns empty map if nothing was compacted.
    std::unordered_map<uint32_t, uint32_t> compactRecords();

    /// Save live records to a binary file (format v3). Thread-safe.
    /// metadata.extra can contain arbitrary key-value pairs for forward compatibility.
    bool saveToFile(const std::string& filePath, const IndexMetadata& metadata) const;

    /// Legacy overload for backward compatibility.
    bool saveToFile(const std::string& filePath, uint64_t lastEventId = 0) const;

    /// Load records from a binary file (v1, v2, or v3), rebuilding indices. Thread-safe.
    /// outMetadata receives all metadata including formatVersion, timestamp, lastEventId, and extra pairs.
    bool loadFromFile(const std::string& filePath, IndexMetadata* outMetadata = nullptr);

    /// Legacy overload for backward compatibility.
    bool loadFromFile(const std::string& filePath, uint64_t* outLastEventId);

    /// Attach a WAL for logging mutations. Must be already opened.
    void attachWAL(std::shared_ptr<IndexWAL> wal);

    /// Detach the WAL (e.g. before compaction).
    void detachWAL();

    /// Records per page for paged persistence
    static constexpr uint32_t kRecordsPerPage = 1024;

    /// Linear scans build per-directory match caches from this many records up.
    static constexpr size_t kDirCacheMinRecords = 20000;
    /// Test hook: override the record threshold for per-directory caches.
    void setDirCacheMinRecords(size_t n) { dirCacheMinRecords_ = n; }

    /// Get the list of dirty page numbers (pages modified since last clearDirtyPages). Thread-safe.
    std::vector<uint32_t> getDirtyPageNumbers() const;

    /// Clear the dirty page bitmap. Thread-safe.
    void clearDirtyPages();

    /// Whether a full rewrite is needed (set after compactRecords renumbers indices).
    bool needsFullRewrite() const { return fullRewriteNeeded_.load(std::memory_order_relaxed); }
    void clearFullRewriteNeeded() { fullRewriteNeeded_.store(false, std::memory_order_relaxed); }

    /// Monotonically increasing generation counter, incremented on each compaction.
    uint64_t compactionGeneration() const { return compactionGen_.load(std::memory_order_relaxed); }

    /// Return indices of the most recently modified live records, sorted by modTime descending.
    /// Accesses recentCache_ directly under the lock to avoid per-record copy overhead.
    std::vector<uint32_t> recentIndices(uint32_t count) const;

    /// Look up the record index for a given full path. Returns UINT32_MAX if not found.
    uint32_t indexForPath(const std::string& fullPath) const;

    /// Export a copy of all live records with their paths restored. Thread-safe.
    std::vector<FileRecord> exportRecords() const;

    /// Build the full path from a record's path and name components.
    static std::string makeFullPath(std::string_view path, std::string_view name);

    /// Resolve a record's path via pathPool_. Thread-safe (acquires shared_lock).
    /// Returns by value to prevent dangling references during concurrent compaction.
    std::string resolveRecordPath(uint32_t index) const {
        std::shared_lock lock(mutex_);
        if (index >= pathIndices_.size()) return {};
        return pathPool_.str(pathIndices_[index]);
    }

    /// Batch callback access under a single shared_lock. Reconstructs FileRecord from SoA.
    /// Callback signature: void(uint32_t idx, const FileRecord& record, const std::string& path)
    template<typename Func>
    void forEachRecordWithPath(const std::vector<uint32_t>& indices, Func&& func) const {
        std::shared_lock lock(mutex_);
        for (uint32_t idx : indices) {
            if (idx >= types_.size() || types_[idx] == 0) continue;
            std::string path = pathPool_.str(pathIndices_[idx]);
            FileRecord rec;
            rec.name = origNamePool_.str(idx);
            rec.path = path;
            rec.type = types_[idx];
            rec.size = sizes_[idx];
            rec.modTime = static_cast<time_t>(modTimes_[idx]);
            rec.inode = inodes_[idx];
            rec.devId = devIds_[idx];
            func(idx, rec, path);
        }
    }

    /// Batch callback for a contiguous range of records, including tombstones.
    /// Used by PagedIndexWriter to serialize pages with stable index positions.
    /// Callback signature: void(uint32_t idx, const FileRecord& record, const std::string& path)
    template<typename Func>
    void forEachRecordInRange(uint32_t startIdx, uint32_t count, Func&& func) const {
        std::shared_lock lock(mutex_);
        uint32_t end = std::min(startIdx + count, static_cast<uint32_t>(types_.size()));
        for (uint32_t i = startIdx; i < end; i++) {
            std::string path = pathPool_.str(pathIndices_[i]);
            FileRecord rec;
            rec.name = origNamePool_.str(i);
            rec.path = path;
            rec.type = types_[i];
            rec.size = sizes_[i];
            rec.modTime = static_cast<time_t>(modTimes_[i]);
            rec.inode = inodes_[i];
            rec.devId = devIds_[i];
            func(i, rec, path);
        }
    }

    /// Batch callback for v5 serialization. Provides pathIdx and lower name data directly.
    /// Callback signature: void(uint32_t idx, const FileRecord& rec, uint32_t pathIdx,
    ///                          const char* lowerName, uint16_t lowerNameLen)
    template<typename Func>
    void forEachRecordInRangeV5(uint32_t startIdx, uint32_t count, Func&& func) const {
        std::shared_lock lock(mutex_);
        uint32_t end = std::min(startIdx + count, static_cast<uint32_t>(types_.size()));
        for (uint32_t i = startIdx; i < end; i++) {
            FileRecord rec;
            rec.name = origNamePool_.str(i);
            rec.type = types_[i];
            rec.size = sizes_[i];
            rec.modTime = static_cast<time_t>(modTimes_[i]);
            rec.inode = inodes_[i];
            rec.devId = devIds_[i];
            func(i, rec, pathIndices_[i],
                 namePool_.data(i), namePool_.length(i));
        }
    }

    /// Thread-safe copy of pathPool_ for serialization.
    StringPool pathPoolSnapshot() const {
        std::shared_lock lock(mutex_);
        return pathPool_;
    }

    /// Thread-safe copy of lowerPathPool_ for serialization.
    StringPool lowerPathPoolSnapshot() const {
        std::shared_lock lock(mutex_);
        return lowerPathPool_;
    }

    /// Thread-safe snapshot of PathTable. Reconstructs from pathPool_ for compatibility.
    PathTable pathTableSnapshot() const {
        std::shared_lock lock(mutex_);
        PathTable pt;
        for (uint32_t i = 0; i < pathPool_.entryCount(); i++) {
            if (pathPool_.isLive(i)) {
                pt.intern(pathPool_.str(i));
            }
        }
        return pt;
    }

private:
    StringPool origNamePool_;              // contiguous original-case filenames (for v6 persistence)
    StringPool namePool_;                  // contiguous lowercase filenames
    Column<uint32_t> pathIndices_;         // per-record index into pathPool_
    StringPool pathPool_;                  // contiguous directory paths (deduplicated)
    StringPool lowerPathPool_;             // parallel to pathPool_, stores pre-lowered paths

    // SoA columnar arrays — the canonical per-record storage
    Column<uint8_t>  types_;               // 1=file, 2=directory, 0=tombstone
    Column<uint64_t> sizes_;               // file size in bytes
    Column<int64_t>  modTimes_;            // modification time (Unix epoch)
    Column<uint64_t> inodes_;              // inode number
    Column<int32_t>  devIds_;              // device ID
    // Key-less hash indices: keys are re-derived from the pools above (see FlatKeyIndex).
    FlatKeyIndex pathLookup_;      // path string -> pathPool_ index
    FlatKeyIndex lowerPathLookup_; // lowered path -> pathPool_ index
    FlatKeyIndex pathIndex_;       // lowercase fullPath -> live record index
    std::atomic<uint32_t> liveCount_{0};
    mutable std::shared_mutex mutex_;
    // Taken before mutex_ by every mutation, so persistStorage can write under
    // a shared lock and then swap storage without mutations in between.
    mutable std::mutex writeGate_;
    struct WriteGuard {
        std::lock_guard<std::mutex> gate;
        std::unique_lock<std::shared_mutex> lock;
        explicit WriteGuard(const SearchEngine& e) : gate(e.writeGate_), lock(e.mutex_) {}
    };

    // Trigram inverted index for fast filename search
    std::unordered_map<Trigram, std::vector<uint32_t>> nameTrigramIndex_; // trigram -> record indices

    // Path trigram index: two-level lookup for fast path-only matching
    std::unordered_map<Trigram, std::vector<uint32_t>> pathTrigramIndex_; // trigram -> sorted pathIdx
    DirPostings pathIdxToRecords_; // pathIdx -> sorted record indices
    bool dirRecordsBuilt_ = false; // pathIdxToRecords_ covers every record

    // Extension index: extension string -> sorted record indices (files only)
    std::unordered_map<std::string, std::vector<uint32_t>> extensionIndex_;

    /// Key accessors for the FlatKeyIndex members.
    static FlatKeyIndex::KeyParts recordKeyParts(std::string_view lowerDir, std::string_view lowerName) {
        return {lowerDir, lowerName, !(lowerDir.empty() || lowerDir.back() == '/')};
    }
    auto recordKeyOf() const {
        return [this](uint32_t i) {
            return recordKeyParts(lowerPathPool_.view(pathIndices_[i]), namePool_.view(i));
        };
    }
    static auto poolKeyOf(const StringPool& pool) {
        return [&pool](uint32_t i) { return FlatKeyIndex::KeyParts{pool.view(i), {}, false}; };
    }
    /// Live record index for a lowercase full path, or UINT32_MAX.
    uint32_t findRecordUnlocked(std::string_view lowerFull) const {
        return pathIndex_.get(lowerFull, recordKeyOf());
    }
    /// Rebuild pathIndex_ from the live records; tombstones losing duplicates.
    /// Returns the live count.
    uint32_t rebuildPathIndexUnlocked();
    /// Rebuild pathLookup_/lowerPathLookup_ from the path pools.
    void rebuildPathLookupsUnlocked();

    /// Remove a live record from every index and tombstone it.
    void removeRecordUnlocked(uint32_t idx, bool writeWal);

    /// Append a new live record and index it (no WAL write). Returns its index.
    uint32_t appendRecordUnlocked(FileRecord&& rec);
    /// Insert or update the record at fullPath (no WAL write). Updates of an
    /// existing record with the same name and type are applied in place, so
    /// content changes do not create tombstones.
    void upsertUnlocked(const std::string& fullPath, FileRecord&& rec);
    /// Remove idx from the name trigram / extension indices given its lowercase name.
    void removeTrigramsForName(uint32_t idx, std::string_view lowerName);
    void removeExtensionForName(uint32_t idx, std::string_view lowerName);

    /// Indices of live records whose lowercase full path equals one of the
    /// prefixes or lies beneath it.
    std::vector<uint32_t> collectSubtreeUnlocked(const std::vector<std::string>& lowerPrefixes) const;

    /// Intern a directory path into pathPool_ and lowerPathPool_. Returns pathPool_ index.
    /// Deduplicates via pathLookup_. Must be called under unique_lock.
    uint32_t internPath(const std::string& path);

    /// Tombstone a record at idx: clear SoA columns. Must be called under unique_lock.
    void tombstoneAt(uint32_t idx);

    /// Append a record to SoA columns. Must be called under unique_lock.
    void pushRecord(FileRecord&& rec);

    /// Unlocked variants — caller must hold unique_lock on mutex_.
    bool removeByPathUnlocked(const std::string& fullPath);
    void updateByPathUnlocked(const std::string& fullPath, FileRecord&& updated);

    /// Build trigram index from namePool_ (called inside loadRecords/compactRecords under lock)
    void buildTrigramIndex();
    /// Add trigrams for a single record to the index
    void addTrigramsForRecord(uint32_t idx, const char* data, uint16_t len);
    /// Remove trigrams for a single record from the index
    void removeTrigramsForRecord(uint32_t idx);

    /// Build extension index from namePool_ (maps extension -> sorted record indices)
    void buildExtensionIndex();
    /// Add a record to the extension index (sorted insert)
    void addExtensionForRecord(uint32_t idx);
    /// Remove a record from the extension index
    void removeExtensionForRecord(uint32_t idx);

    /// Build path trigram index from pathPool_
    void buildPathTrigramIndex();
    /// Rebuild pathIdx -> record indices mapping
    void rebuildPathIdxToRecords();
    /// Add a record to pathIdxToRecords_ (sorted insert)
    void addPathTrigramsForRecord(uint32_t idx);
    /// Remove a record from pathIdxToRecords_ (sorted remove)
    void removePathTrigramsForRecord(uint32_t idx);
    /// Ensure pathTrigramIndex_ has entries for a given pathIdx (called when PathTable grows)
    void ensurePathTrigramsForPathIdx(uint32_t pathIdx);

    static bool globMatch(const std::string& pattern, const std::string& text);

    /// Match result from search: (record index, priority, full path length).
    /// Priority: 0=exact, 1=starts-with, 2=contains, 3=path-only match.
    struct Match { uint32_t idx; uint8_t priority; uint32_t pathLen; };

    /// Node-centric structured query: name trigram → name verify → path constraint verify.
    /// Handles SEGMENTS and DIR_EXACT modes.
    void queryStructured(const ParsedQuery& pq,
                         size_t totalSize, const QueryCancelCtx& cancel,
                         std::vector<Match>& merged) const;

    /// DIR_LIST mode: find matching directories, return their direct children.
    void queryDirList(const ParsedQuery& pq,
                      size_t totalSize, const QueryCancelCtx& cancel,
                      std::vector<Match>& merged) const;

public:
    /// Check if a directory path matches the path segment constraints.
    static bool pathSegmentsMatch(std::string_view dirPath,
                                  const std::vector<PathSegment>& segments);
    /// pathSegmentsMatch starting with segments[0..segIdx] still unmatched,
    /// as if segments after segIdx were already matched further right.
    static bool pathSegmentsMatchFrom(std::string_view dirPath,
                                      const std::vector<PathSegment>& segments,
                                      int segIdx);
private:

    /// Estimate trigram candidate count for a keyword (min posting list size).
    /// Returns SIZE_MAX if keyword is < 3 chars, 0 if any trigram not in index.
    size_t estimateTrigramCost(const std::string& keyword) const;

    /// Tree-walk from anchor directory down to target segment, collecting matches.
    void treeWalkDown(uint32_t dirIdx, const ParsedQuery& pq,
                      int fromSegIdx, int toSegIdx,
                      size_t totalSize, const QueryCancelCtx& cancel,
                      std::vector<Match>& merged) const;

    /// Anchor on namePattern: trigram intersect + path verify. Returns false to fall back.
    bool queryStructuredNameAnchor(const ParsedQuery& pq,
                                   size_t totalSize, const QueryCancelCtx& cancel,
                                   std::vector<Match>& merged) const;

    /// Anchor on path segment: trigram intersect + tree-walk. Returns false to fall back.
    bool queryStructuredPathAnchor(const ParsedQuery& pq,
                                   size_t anchorIdx,
                                   size_t totalSize, const QueryCancelCtx& cancel,
                                   std::vector<Match>& merged) const;

    std::vector<bool> dirtyPages_;                // dirty page bitmap
    std::atomic<bool> fullRewriteNeeded_{false};   // set by compactRecords()

    /// Mark the page containing the given record index as dirty. Must be called under lock.
    void markPageDirty(uint32_t recordIndex);

    std::shared_ptr<IndexWAL> wal_;
    std::atomic<uint64_t> compactionGen_{0};
    bool trigramIndexEnabled_ = false;
    bool extensionIndexEnabled_ = false;
    size_t dirCacheMinRecords_ = kDirCacheMinRecords;

    // Phase 2 two-stage startup: trigram indices built in background
    std::atomic<bool> phase2Pending_{false};
    uint32_t phase2StartRecordCount_{0};

    // Per-session query generation for cancellation.
    // Key: sessionId (0 = no cancellation). Value: shared atomic counter.
    mutable std::unordered_map<uint64_t, std::shared_ptr<std::atomic<uint64_t>>> sessionGenerations_;
    mutable std::mutex sessionGenMutex_;  // protects sessionGenerations_ map only

    // Recent files cache: top-K records by modTime, maintained incrementally
    static constexpr uint32_t kRecentCacheSize = 200;
    struct RecentEntry {
        time_t modTime;
        uint32_t index;
        bool operator<(const RecentEntry& o) const {
            return modTime > o.modTime || (modTime == o.modTime && index > o.index);
        }
    };
    std::set<RecentEntry> recentCache_;
    void rebuildRecentCache();
    void addToRecentCache(uint32_t idx, time_t modTime);
    void removeFromRecentCache(uint32_t idx, time_t modTime);

    /// Compute match priority: 0=exact, 1=starts-with, 2=contains.
    static uint8_t namePriority(const char* nameData, uint16_t nameLen,
                                const char* keyData, size_t keyLen);

    /// Intersect posting lists from a trigram index for a given keyword.
    /// Returns sorted candidate indices, or empty if any trigram is missing.
    /// Sets allFound to false if any trigram is not in the index.
    static std::vector<uint32_t> intersectPostingLists(
        const std::unordered_map<Trigram, std::vector<uint32_t>>& index,
        const std::string& keyword,
        bool& allFound);

    /// Intersect trigrams from multiple literal segments for complex glob pre-filtering.
    static std::vector<uint32_t> intersectPostingListsMulti(
        const std::unordered_map<Trigram, std::vector<uint32_t>>& index,
        const std::vector<std::string>& segments,
        bool& allFound);

    /// Union posting lists across multiple atoms for regex pre-filtering.
    /// Each atom's trigrams are intersected internally, then results are unioned.
    /// Safe for both AND and OR semantics from FilteredRE2.
    static std::vector<uint32_t> unionPostingListsMulti(
        const std::unordered_map<Trigram, std::vector<uint32_t>>& index,
        const std::vector<std::string>& segments);

    /// Build full path in a reusable buffer: path + '/' + name.
    /// Lowercases the path portion via SIMD. Returns the full path length.
    /// Buffer is resized if needed (with 2x growth).
    static size_t buildFullPathBuf(std::vector<char>& buf,
                                   const char* pathData, uint16_t pathLen,
                                   const char* nameData, uint16_t nameLen);

    /// Build trigram index from standalone data (no member access, used by COW compaction)
    static std::unordered_map<Trigram, std::vector<uint32_t>>
        buildTrigramIndexFromData(const Column<uint8_t>& types,
                                  const StringPool& namePool);

    /// Build extension index from standalone data (no member access, used by COW compaction)
    static std::unordered_map<std::string, std::vector<uint32_t>>
        buildExtensionIndexFromData(const Column<uint8_t>& types,
                                    const StringPool& namePool);

    /// Build path trigram index from pre-lowered StringPool (used by COW compaction)
    static std::unordered_map<Trigram, std::vector<uint32_t>>
        buildPathTrigramIndexFromData(const StringPool& lowerPathPool);

    /// Build pathIdx -> record indices mapping from standalone data (used by COW compaction)
    static DirPostings
        buildPathIdxToRecordsFromData(const Column<uint8_t>& types,
                                      const Column<uint32_t>& pathIndices,
                                      uint32_t pathPoolSize);

    /// Build recent cache from standalone data (no member access, used by COW compaction)
    static std::set<RecentEntry>
        buildRecentCacheFromData(const Column<uint8_t>& types,
                                 const Column<int64_t>& modTimes,
                                 uint32_t cacheSize);
};
