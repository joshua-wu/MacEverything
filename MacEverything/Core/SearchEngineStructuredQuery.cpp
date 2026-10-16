#include "SearchEngine.h"
#include "SIMDSearch.h"
#include "StringUtils.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <unordered_set>
#include <dispatch/dispatch.h>

// ---------------------------------------------------------------------------
// pathSegmentsMatch: check if dirPath satisfies path segment constraints
// ---------------------------------------------------------------------------
bool SearchEngine::pathSegmentsMatch(std::string_view dirPath,
                                     const std::vector<PathSegment>& segments) {
    return pathSegmentsMatchFrom(dirPath, segments, static_cast<int>(segments.size()) - 1);
}

bool SearchEngine::pathSegmentsMatchFrom(std::string_view dirPath,
                                         const std::vector<PathSegment>& segments,
                                         int segIdx) {
    if (segIdx < 0) return true;
    const int lastSeg = static_cast<int>(segments.size()) - 1;

    // Walk '/'-separated components right-to-left without allocating.
    // dirPath comes from lowerPathPool_ which is already lowercase.
    size_t end = dirPath.size();
    while (segIdx >= 0) {
        while (end > 0 && dirPath[end - 1] == '/') end--;
        if (end == 0) break; // no components left
        size_t start = dirPath.rfind('/', end - 1);
        start = (start == std::string_view::npos) ? 0 : start + 1;
        std::string_view component = dirPath.substr(start, end - start);
        end = start;

        if (component.find(segments[segIdx].text) != std::string_view::npos) {
            segIdx--;
        } else if (segIdx < lastSeg && !segments[segIdx + 1].adjacentToNext) {
            // gap allowed before the already-matched segment: skip component
        } else if (segIdx == lastSeg) {
            // last segment may float: skip trailing components
        } else {
            return false;
        }
    }
    return segIdx < 0;
}

// ---------------------------------------------------------------------------
// estimateTrigramCost: cheap upper-bound for trigram candidate count
// ---------------------------------------------------------------------------
size_t SearchEngine::estimateTrigramCost(const std::string& keyword) const {
    if (keyword.size() < 3 || nameTrigramIndex_.empty()) return SIZE_MAX;
    auto trigrams = ContentIndex::extractTrigrams(keyword);
    std::unordered_set<Trigram> unique(trigrams.begin(), trigrams.end());
    if (unique.empty()) return SIZE_MAX;

    size_t minSize = SIZE_MAX;
    for (Trigram t : unique) {
        auto it = nameTrigramIndex_.find(t);
        if (it == nameTrigramIndex_.end()) return 0; // trigram absent → 0 candidates
        minSize = std::min(minSize, it->second.size());
    }
    return minSize;
}

// ---------------------------------------------------------------------------
// treeWalkDown: walk children from anchor dir toward namePattern
// ---------------------------------------------------------------------------
void SearchEngine::treeWalkDown(uint32_t dirIdx, const ParsedQuery& pq,
                                int fromSegIdx, int toSegIdx,
                                size_t totalSize, const QueryCancelCtx& cancel,
                                std::vector<Match>& merged) const {
    // Build the full lowered directory path for this dir record
    auto lpView = lowerPathPool_.view(pathIndices_[dirIdx]);
    std::string fullDirPath(lpView);
    if (!fullDirPath.empty() && fullDirPath.back() != '/') fullDirPath += '/';
    fullDirPath += std::string(namePool_.data(dirIdx), namePool_.length(dirIdx));

    // Look up children of this directory
    uint32_t childPathIdx = lowerPathLookup_.get(fullDirPath, poolKeyOf(lowerPathPool_));
    if (childPathIdx == UINT32_MAX) return;
    if (childPathIdx >= pathIdxToRecords_.size()) return;
    const auto& childRecords = pathIdxToRecords_[childPathIdx];

    if (fromSegIdx > toSegIdx) {
        // We've walked through all intermediate segments.
        // Now match children against namePattern.
        const auto& namePattern = pq.namePattern;
        for (uint32_t childIdx : childRecords) {
            if (types_[childIdx] == 0) continue;
            const char* nd = namePool_.data(childIdx);
            uint16_t nl = namePool_.length(childIdx);

            if (pq.mode == QueryMode::DIR_EXACT) {
                if (types_[childIdx] != 2) continue;
                if (nl != namePattern.size()) continue;
                if (std::memcmp(nd, namePattern.data(), nl) != 0) continue;
            } else {
                if (!me::simdContains(nd, nl, namePattern.data(), namePattern.size())) continue;
            }

            uint8_t priority = namePriority(nd, nl, namePattern.data(), namePattern.size());
            uint32_t pLen = static_cast<uint32_t>(pathPool_.length(pathIndices_[childIdx]) + 1 + nl);
            merged.push_back({childIdx, priority, pLen});
        }
        return;
    }

    // Walk through intermediate segments: find children whose name matches
    // pathSegments[fromSegIdx], then recurse down
    const auto& segText = pq.pathSegments[fromSegIdx].text;
    for (uint32_t childIdx : childRecords) {
        if (cancel.cancelled()) return;
        if (types_[childIdx] != 2) continue; // must be directory
        const char* nd = namePool_.data(childIdx);
        uint16_t nl = namePool_.length(childIdx);
        // Segment match: name must contain segment text
        if (!me::simdContains(nd, nl, segText.data(), segText.size())) continue;

        // Recurse to next level
        treeWalkDown(childIdx, pq, fromSegIdx + 1, toSegIdx, totalSize, cancel, merged);
    }
}

// ---------------------------------------------------------------------------
// queryStructured: SEGMENTS and DIR_EXACT modes (with anchor-selection)
// ---------------------------------------------------------------------------
void SearchEngine::queryStructured(const ParsedQuery& pq,
                                   size_t totalSize, const QueryCancelCtx& cancel,
                                   std::vector<Match>& merged) const {
    const auto& namePattern = pq.namePattern;
    if (namePattern.empty()) return;

    // ------------------------------------------------------------------
    // Step 1: Estimate trigram cost for each segment + namePattern
    // ------------------------------------------------------------------
    size_t numPathSegs = pq.pathSegments.size();
    size_t nameCost = estimateTrigramCost(namePattern);

    size_t bestIdx = numPathSegs; // index into [pathSegs..., namePattern]
    size_t bestCost = nameCost;
    for (size_t i = 0; i < numPathSegs; i++) {
        size_t c = estimateTrigramCost(pq.pathSegments[i].text);
        // cost==0 for a path segment means its trigrams aren't in nameTrigramIndex_,
        // but that doesn't mean zero results — it just can't be used as an anchor.
        if (c > 0 && c < bestCost) {
            bestCost = c;
            bestIdx = i;
        }
    }

    size_t trigramThreshold = totalSize / 4;

    // ------------------------------------------------------------------
    // Step 2: Try anchor-based strategies before falling back to linear
    // ------------------------------------------------------------------
    // Only nameCost==0 guarantees zero results (namePattern must match a file name).
    // Path segment cost==0 just means that segment text isn't indexed as a name.
    if (nameCost == 0) return;

    if (bestCost <= trigramThreshold) {
        if (bestIdx == numPathSegs) {
            // Anchor is namePattern — original trigram path
            if (queryStructuredNameAnchor(pq, totalSize, cancel, merged))
                return;
        } else {
            // Anchor is a path segment — tree-walk strategy
            if (queryStructuredPathAnchor(pq, bestIdx, totalSize, cancel, merged))
                return;
        }
    }

    // ------------------------------------------------------------------
    // Fallback strategy selection:
    // - With path segments: path-first scan (iterate only records under
    //   matching paths via pathIdxToRecords_, then check names)
    // - Without path segments: buffer-scan (SIMD scan namePool_.buffer_)
    // ------------------------------------------------------------------
    bool hasPathSegs = !pq.pathSegments.empty();

    if (hasPathSegs) {
        // Path-first: narrow candidate paths, then iterate only their records.
        // Try path trigram index first (O(candidates) vs O(all_paths)).
        // Find the best segment (longest, >= 3 chars) for trigram lookup.
        int bestSegIdx = -1;
        size_t bestSegLen = 0;
        for (size_t i = 0; i < pq.pathSegments.size(); i++) {
            if (pq.pathSegments[i].text.size() >= 3 &&
                pq.pathSegments[i].text.size() > bestSegLen) {
                bestSegLen = pq.pathSegments[i].text.size();
                bestSegIdx = static_cast<int>(i);
            }
        }

        // Lambda: check name match and emit result for records under pathIdx pi
        auto emitRecordsForPath = [&](uint32_t pi) {
            if (pi >= pathIdxToRecords_.size()) return;
            const auto& recIds = pathIdxToRecords_[pi];
            for (uint32_t idx : recIds) {
                if (cancel.cancelled()) return;
                if (types_[idx] == 0) continue;

                const char* nd = namePool_.data(idx);
                uint16_t nl = namePool_.length(idx);

                if (pq.mode == QueryMode::DIR_EXACT) {
                    if (types_[idx] != 2) continue;
                    if (nl != namePattern.size()) continue;
                    if (std::memcmp(nd, namePattern.data(), nl) != 0) continue;
                } else {
                    if (!me::simdContains(nd, nl, namePattern.data(), namePattern.size())) continue;
                }

                uint8_t priority = namePriority(nd, nl, namePattern.data(), namePattern.size());
                uint32_t pLen = static_cast<uint32_t>(pathPool_.length(pathIndices_[idx]) + 1 + nl);
                merged.push_back({idx, priority, pLen});
            }
        };

        bool usedTrigram = false;
        if (bestSegIdx >= 0 && !pathTrigramIndex_.empty()) {
            bool allFound = false;
            auto candidatePaths = intersectPostingLists(
                pathTrigramIndex_, pq.pathSegments[bestSegIdx].text, allFound);

            if (allFound) {
                usedTrigram = true;
                for (uint32_t pi : candidatePaths) {
                    if (cancel.cancelled()) return;
                    if (!lowerPathPool_.isLive(pi)) continue;
                    std::string_view lpv(lowerPathPool_.data(pi), lowerPathPool_.length(pi));
                    if (!pathSegmentsMatch(lpv, pq.pathSegments)) continue;
                    emitRecordsForPath(pi);
                }
            }
        }

        if (!usedTrigram) {
            // Linear scan all paths (no usable trigrams)
            uint32_t pathCount = lowerPathPool_.entryCount();
            for (uint32_t pi = 0; pi < pathCount; pi++) {
                if (cancel.cancelled()) return;
                if (!lowerPathPool_.isLive(pi)) continue;
                std::string_view lpv(lowerPathPool_.data(pi), lowerPathPool_.length(pi));
                if (!pathSegmentsMatch(lpv, pq.pathSegments)) continue;
                emitRecordsForPath(pi);
            }
        }
    } else {
        // No path segments: buffer-scan the contiguous namePool_ buffer
        // with SIMD, then resolve byte offsets → record indices.
        std::vector<size_t> hitOffsets;
        me::simdFindAll(namePool_.rawBuffer(), namePool_.rawSize(),
                        namePattern.data(), namePattern.size(), hitOffsets);

        if (cancel.cancelled()) return;

        const auto* entries = namePool_.entries();
        uint32_t entryCount = namePool_.entryCount();

        // hitOffsets are in ascending order. Walk cursor in sync — O(hits + entries).
        uint32_t cursor = 0;
        uint32_t lastIdx = UINT32_MAX;

        for (size_t hitOff : hitOffsets) {
            while (cursor + 1 < entryCount &&
                   entries[cursor + 1].offset <= static_cast<uint32_t>(hitOff)) {
                cursor++;
            }

            uint32_t entryEnd = entries[cursor].offset + entries[cursor].length;
            if (hitOff < entries[cursor].offset ||
                hitOff + namePattern.size() > entryEnd)
                continue;

            if (cursor == lastIdx) continue;
            lastIdx = cursor;

            if (types_[cursor] == 0) continue;

            if (pq.mode == QueryMode::DIR_EXACT) {
                if (types_[cursor] != 2) continue;
                if (entries[cursor].length != namePattern.size()) continue;
            }

            uint8_t priority = namePriority(namePool_.data(cursor), namePool_.length(cursor),
                                             namePattern.data(), namePattern.size());
            uint32_t pLen = static_cast<uint32_t>(pathPool_.length(pathIndices_[cursor]) + 1 + entries[cursor].length);
            merged.push_back({cursor, priority, pLen});
        }
    }
}

// ---------------------------------------------------------------------------
// queryStructuredNameAnchor: trigram on namePattern, verify path constraints
// Returns true if handled (even if 0 results), false to fall back to linear.
// ---------------------------------------------------------------------------
bool SearchEngine::queryStructuredNameAnchor(const ParsedQuery& pq,
                                             size_t /*totalSize*/, const QueryCancelCtx& cancel,
                                             std::vector<Match>& merged) const {
    const auto& namePattern = pq.namePattern;
    bool allFound = false;
    auto candidates = intersectPostingLists(nameTrigramIndex_, namePattern, allFound);
    if (!allFound) return false;

    for (size_t ci = 0; ci < candidates.size(); ci++) {
        if ((ci & 1023) == 0 && cancel.cancelled()) return true;
        uint32_t idx = candidates[ci];
        if (types_[idx] == 0) continue;

        const char* nameData = namePool_.data(idx);
        uint16_t nameLen = namePool_.length(idx);

        if (pq.mode == QueryMode::DIR_EXACT) {
            if (types_[idx] != 2) continue;
            if (nameLen != namePattern.size()) continue;
            if (std::memcmp(nameData, namePattern.data(), nameLen) != 0) continue;
        } else {
            if (!me::simdContains(nameData, nameLen, namePattern.data(), namePattern.size())) continue;
        }

        if (!pq.pathSegments.empty()) {
            if (!pathSegmentsMatch(lowerPathPool_.view(pathIndices_[idx]), pq.pathSegments)) continue;
        }

        uint8_t priority = namePriority(nameData, nameLen, namePattern.data(), namePattern.size());
        uint32_t pLen = static_cast<uint32_t>(pathPool_.length(pathIndices_[idx]) + 1 + nameLen);
        merged.push_back({idx, priority, pLen});
    }
    return true;
}

// ---------------------------------------------------------------------------
// queryStructuredPathAnchor: trigram on a path segment, tree-walk to name
// Returns true if handled, false to fall back to linear.
// ---------------------------------------------------------------------------
bool SearchEngine::queryStructuredPathAnchor(const ParsedQuery& pq,
                                             size_t anchorIdx,
                                             size_t totalSize, const QueryCancelCtx& cancel,
                                             std::vector<Match>& merged) const {
    size_t numPathSegs = pq.pathSegments.size();

    // Check that all segments from anchor to namePattern are adjacent.
    // If any gap is non-adjacent, we can't tree-walk → fall back.
    if (!pq.pathSegments[anchorIdx].adjacentToNext) return false;
    for (size_t s = anchorIdx + 1; s < numPathSegs; s++) {
        if (!pq.pathSegments[s].adjacentToNext) return false;
    }

    const auto& anchorText = pq.pathSegments[anchorIdx].text;
    bool allFound = false;
    auto candidates = intersectPostingLists(nameTrigramIndex_, anchorText, allFound);
    if (!allFound) return false;

    int walkFrom = static_cast<int>(anchorIdx) + 1;
    int walkTo = static_cast<int>(numPathSegs) - 1;

    for (size_t ci = 0; ci < candidates.size(); ci++) {
        if ((ci & 1023) == 0 && cancel.cancelled()) return true;
        uint32_t idx = candidates[ci];
        if (types_[idx] == 0) continue;
        if (types_[idx] != 2) continue; // anchor must be a directory

        const char* nd = namePool_.data(idx);
        uint16_t nl = namePool_.length(idx);
        if (!me::simdContains(nd, nl, anchorText.data(), anchorText.size())) continue;

        // Verify ancestor segments [0..anchorIdx-1] against this record's path
        if (anchorIdx > 0) {
            std::string_view dirPath = lowerPathPool_.view(pathIndices_[idx]);
            std::vector<PathSegment> ancestorSegs(
                pq.pathSegments.begin(),
                pq.pathSegments.begin() + static_cast<int>(anchorIdx));
            if (!pathSegmentsMatch(dirPath, ancestorSegs)) continue;
        }

        // Tree-walk down through remaining path segments to namePattern
        treeWalkDown(idx, pq, walkFrom, walkTo, totalSize, cancel, merged);
    }
    return true;
}
