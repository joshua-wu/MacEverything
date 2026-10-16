#include "SearchEngine.h"
#include "StringUtils.h"
#include "CompiledGlob.h"
#include "Logger.h"
#include "QueryTokenizer.h"
#include "QueryParser.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <unordered_set>
#include <dispatch/dispatch.h>

// ---------------------------------------------------------------------------
// Match priority
// ---------------------------------------------------------------------------

uint8_t SearchEngine::namePriority(const char* nameData, uint16_t nameLen,
                                   const char* keyData, size_t keyLen) {
    if (nameLen == keyLen && memcmp(nameData, keyData, nameLen) == 0)
        return 0; // exact match
    if (nameLen >= keyLen && memcmp(nameData, keyData, keyLen) == 0)
        return 1; // starts with
    return 2; // contains
}

// ---------------------------------------------------------------------------
// Full path buffer construction
// ---------------------------------------------------------------------------

size_t SearchEngine::buildFullPathBuf(std::vector<char>& buf,
                                      const char* pathData, uint16_t pathLen,
                                      const char* nameData, uint16_t nameLen) {
    size_t fullLen = static_cast<size_t>(pathLen) + 1 + nameLen;
    if (buf.size() < fullLen) buf.resize(fullLen * 2);
    memcpy(buf.data(), pathData, pathLen);
    buf[pathLen] = '/';
    memcpy(buf.data() + pathLen + 1, nameData, nameLen);
    return fullLen;
}

// ---------------------------------------------------------------------------
// Glob matching
// ---------------------------------------------------------------------------

bool SearchEngine::globMatch(const std::string& pattern, const std::string& text) {
    return globMatchImpl(pattern, text);
}

// ---------------------------------------------------------------------------
// queryDirList: DIR_LIST mode — find directory, return its children
// ---------------------------------------------------------------------------
void SearchEngine::queryDirList(const ParsedQuery& pq,
                                size_t totalSize, const QueryCancelCtx& cancel,
                                std::vector<Match>& merged) const {
    const auto& dirName = pq.namePattern;
    if (dirName.empty()) return;

    // Find directory records whose name exactly matches dirName
    std::vector<uint32_t> dirIndices;

    if (dirName.size() >= 3 && !nameTrigramIndex_.empty()) {
        bool allFound = false;
        auto candidates = intersectPostingLists(nameTrigramIndex_, dirName, allFound);
        if (allFound && candidates.size() <= totalSize / 4) {
            for (uint32_t idx : candidates) {
                if (types_[idx] != 2) continue; // must be directory
                const char* nd = namePool_.data(idx);
                uint16_t nl = namePool_.length(idx);
                if (nl != dirName.size()) continue;
                if (std::memcmp(nd, dirName.data(), nl) != 0) continue;
                // Check path constraints
                if (!pq.pathSegments.empty()) {
                    std::string dirPath = lowerPathPool_.str(pathIndices_[idx]);
                    if (!pathSegmentsMatch(dirPath, pq.pathSegments)) continue;
                }
                dirIndices.push_back(idx);
            }
        } else {
            // Fallback: linear scan for directories
            for (size_t i = 0; i < totalSize; i++) {
                if (types_[i] != 2) continue;
                const char* nd = namePool_.data(i);
                uint16_t nl = namePool_.length(i);
                if (nl != dirName.size()) continue;
                if (std::memcmp(nd, dirName.data(), nl) != 0) continue;
                if (!pq.pathSegments.empty()) {
                    std::string dirPath = lowerPathPool_.str(pathIndices_[i]);
                    if (!pathSegmentsMatch(dirPath, pq.pathSegments)) continue;
                }
                dirIndices.push_back(static_cast<uint32_t>(i));
            }
        }
    } else {
        // Short name or no trigram index — linear scan
        for (size_t i = 0; i < totalSize; i++) {
            if (types_[i] != 2) continue;
            const char* nd = namePool_.data(i);
            uint16_t nl = namePool_.length(i);
            if (nl != dirName.size()) continue;
            if (std::memcmp(nd, dirName.data(), nl) != 0) continue;
            if (!pq.pathSegments.empty()) {
                std::string dirPath = lowerPathPool_.str(pathIndices_[i]);
                if (!pathSegmentsMatch(dirPath, pq.pathSegments)) continue;
            }
            dirIndices.push_back(static_cast<uint32_t>(i));
        }
    }

    if (dirIndices.empty()) return;

    // For each matching directory, find its children via pathLookup_ + pathIdxToRecords_
    for (uint32_t dirIdx : dirIndices) {
        if (cancel.cancelled()) return;

        // Build the full directory path: parentPath + "/" + dirName
        std::string parentPath = pathPool_.str(pathIndices_[dirIdx]);
        std::string fullDirPath = parentPath;
        if (!fullDirPath.empty() && fullDirPath.back() != '/') fullDirPath += '/';
        fullDirPath += origNamePool_.view(dirIdx);

        // Look up this full path in pathLookup_ to find its pathPool index
        uint32_t childPathIdx = pathLookup_.get(fullDirPath, poolKeyOf(pathPool_));
        if (childPathIdx == UINT32_MAX) continue;
        if (childPathIdx >= pathIdxToRecords_.size()) continue;

        const auto& childRecords = pathIdxToRecords_[childPathIdx];
        for (uint32_t childIdx : childRecords) {
            if (types_[childIdx] == 0) continue; // skip tombstones
            const char* nd = namePool_.data(childIdx);
            uint16_t nl = namePool_.length(childIdx);
            uint8_t priority = 2; // children are all "contains" priority
            uint32_t pLen = static_cast<uint32_t>(pathPool_.length(pathIndices_[childIdx]) + 1 + nl);
            merged.push_back({childIdx, priority, pLen});
        }
    }
}

// ---------------------------------------------------------------------------
// Query preprocessing — normalise raw user input before routing.
// All transformations that should apply to every query path go here.
// Returns both original-case and pre-lowered text to avoid redundant lowering.
// ---------------------------------------------------------------------------

struct PreprocessedQuery {
    std::string original;  // after trim + tilde expansion (original case)
    std::string lower;     // me::toLower(original) — single canonical lowering
};

static PreprocessedQuery preprocessQuery(const std::string& raw) {
    // 0) Strip leading/trailing whitespace
    auto start = raw.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    auto end = raw.find_last_not_of(" \t\r\n");
    std::string result = raw.substr(start, end - start + 1);

    // 1) Expand leading ~ to the user's home directory so that patterns like
    //    ~/*/*.txt match absolute indexed paths (e.g. /Users/username/Downloads/f1.txt).
    if (!result.empty() && result[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) {
            if (result.size() == 1) {
                result = home;
            } else if (result[1] == '/') {
                result = std::string(home) + result.substr(1);
            }
        }
    }

    // 2) Compute canonical lowercase once — eliminates redundant me::toLower()
    //    calls in parseQuery(), transformSlashTerms(), and makeTerm().
    return { result, me::toLower(result) };
}

// ---------------------------------------------------------------------------
// Main query() entry points
// ---------------------------------------------------------------------------

std::vector<uint32_t> SearchEngine::query(const std::string& keyword, uint32_t maxResults,
                                          bool useTrigram, uint64_t sessionId) const {
    QueryTimingInfo unused;
    return query(keyword, maxResults, useTrigram, unused, sessionId);
}

std::vector<uint32_t> SearchEngine::query(const std::string& keyword, uint32_t maxResults,
                                          bool useTrigram, QueryTimingInfo& timing,
                                          uint64_t sessionId) const {
    // Acquire per-session generation so only same-session queries cancel each other.
    auto [genAtom, myGen] = acquireSessionGeneration(sessionId);

    if (keyword.empty()) return {};

    auto pq = preprocessQuery(keyword);
    if (pq.original.empty()) return {};

    // Check for DIR_LIST mode: /path/* queries list directory children directly
    auto parsedQuery = parseQuery(pq.original, pq.lower);
    if (parsedQuery.mode == QueryMode::DIR_LIST) {
        auto queryStart = std::chrono::steady_clock::now();
        auto beforeLock = std::chrono::steady_clock::now();
        std::shared_lock lock(mutex_);
        auto afterLock = std::chrono::steady_clock::now();

        if (types_.empty()) return {};
        size_t totalSize = types_.size();

        std::vector<Match> merged;
        QueryCancelCtx cancel{genAtom.get(), myGen};
        queryDirList(parsedQuery, totalSize, cancel, merged);

        if (genAtom->load(std::memory_order_relaxed) != myGen) return {};

        auto beforeUnlock = std::chrono::steady_clock::now();
        lock.unlock();

        // Sort by priority then path length
        auto beforeSort = std::chrono::steady_clock::now();
        auto cmp = [](const Match& a, const Match& b) {
            if (a.priority != b.priority) return a.priority < b.priority;
            return a.pathLen < b.pathLen;
        };
        size_t resultCount = merged.size();
        if (maxResults > 0 && resultCount > maxResults) resultCount = maxResults;
        if (resultCount < merged.size()) {
            std::partial_sort(merged.begin(), merged.begin() + resultCount, merged.end(), cmp);
        } else {
            std::sort(merged.begin(), merged.end(), cmp);
        }
        auto afterSort = std::chrono::steady_clock::now();

        std::vector<uint32_t> result;
        result.reserve(resultCount);
        for (size_t i = 0; i < resultCount; i++) {
            result.push_back(merged[i].idx);
        }

        // Populate timing
        auto toMs = [](auto dur) { return std::chrono::duration<double, std::milli>(dur).count(); };
        timing.totalMs = toMs(std::chrono::steady_clock::now() - queryStart);
        timing.lockWaitMs = toMs(afterLock - beforeLock);
        timing.lockHeldMs = toMs(beforeUnlock - afterLock);
        timing.sortMs = toMs(afterSort - beforeSort);
        timing.totalRecords = totalSize;
        timing.resultCount = result.size();
        timing.searchPath = "dir-list";
        return result;
    }

    // All non-DIR_LIST queries go through the unified Advanced path
    return queryAdvanced(pq.original, maxResults, useTrigram, timing, myGen, genAtom.get());
}

// ---------------------------------------------------------------------------
// Per-session generation management
// ---------------------------------------------------------------------------

std::pair<std::shared_ptr<std::atomic<uint64_t>>, uint64_t>
SearchEngine::acquireSessionGeneration(uint64_t sessionId) const {
    if (sessionId == 0) {
        // No cancellation: return a dummy atomic that nobody else will check
        auto dummy = std::make_shared<std::atomic<uint64_t>>(0);
        return {dummy, 0};
    }
    std::lock_guard<std::mutex> lock(sessionGenMutex_);
    auto& ptr = sessionGenerations_[sessionId];
    if (!ptr) ptr = std::make_shared<std::atomic<uint64_t>>(0);
    uint64_t gen = ptr->fetch_add(1, std::memory_order_relaxed) + 1;
    return {ptr, gen};
}

void SearchEngine::cancelSession(uint64_t sessionId) const {
    if (sessionId == 0) return;
    std::lock_guard<std::mutex> lock(sessionGenMutex_);
    auto it = sessionGenerations_.find(sessionId);
    if (it != sessionGenerations_.end()) {
        it->second->fetch_add(1, std::memory_order_relaxed);
    }
}
