#include "SearchEngine.h"
#include "StringUtils.h"
#include <algorithm>
#include <unordered_set>

// Insert into a sorted posting list. Grows capacity by ~12.5% instead of
// doubling: most lists are large, long-lived and receive few inserts.
static void insertSorted(std::vector<uint32_t>& list, uint32_t idx) {
    auto pos = std::lower_bound(list.begin(), list.end(), idx);
    if (pos != list.end() && *pos == idx) return;
    if (list.size() == list.capacity()) {
        size_t off = static_cast<size_t>(pos - list.begin());
        list.reserve(list.size() + list.size() / 8 + 4);
        pos = list.begin() + static_cast<std::ptrdiff_t>(off);
    }
    list.insert(pos, idx);
}

// ---------------------------------------------------------------------------
// Trigram posting list intersection utilities
// ---------------------------------------------------------------------------

std::vector<uint32_t> SearchEngine::intersectPostingLists(
    const std::unordered_map<Trigram, std::vector<uint32_t>>& index,
    const std::string& keyword,
    bool& allFound) {
    allFound = true;
    auto keyTrigrams = ContentIndex::extractTrigrams(keyword);
    std::unordered_set<Trigram> unique(keyTrigrams.begin(), keyTrigrams.end());
    if (unique.empty()) { allFound = false; return {}; }

    std::vector<const std::vector<uint32_t>*> postings;
    for (Trigram t : unique) {
        auto it = index.find(t);
        if (it == index.end()) { allFound = false; return {}; }
        postings.push_back(&it->second);
    }

    std::sort(postings.begin(), postings.end(),
        [](const auto* a, const auto* b) { return a->size() < b->size(); });

    std::vector<uint32_t> result;
    result.reserve(postings[0]->size());
    result.assign(postings[0]->begin(), postings[0]->end());

    for (size_t i = 1; i < postings.size() && !result.empty(); i++) {
        const auto& other = *postings[i];
        std::vector<uint32_t> isect;
        isect.reserve(std::min(result.size(), other.size()));
        std::set_intersection(result.begin(), result.end(),
                              other.begin(), other.end(),
                              std::back_inserter(isect));
        result = std::move(isect);
    }
    return result;
}

std::vector<uint32_t> SearchEngine::intersectPostingListsMulti(
    const std::unordered_map<Trigram, std::vector<uint32_t>>& index,
    const std::vector<std::string>& segments,
    bool& allFound) {
    allFound = true;
    // Collect trigrams from all segments and deduplicate
    std::unordered_set<Trigram> unique;
    for (const auto& seg : segments) {
        auto segTrigrams = ContentIndex::extractTrigrams(seg);
        unique.insert(segTrigrams.begin(), segTrigrams.end());
    }
    if (unique.empty()) { allFound = false; return {}; }

    std::vector<const std::vector<uint32_t>*> postings;
    for (Trigram t : unique) {
        auto it = index.find(t);
        if (it == index.end()) { allFound = false; return {}; }
        postings.push_back(&it->second);
    }

    std::sort(postings.begin(), postings.end(),
        [](const auto* a, const auto* b) { return a->size() < b->size(); });

    std::vector<uint32_t> result;
    result.reserve(postings[0]->size());
    result.assign(postings[0]->begin(), postings[0]->end());

    for (size_t i = 1; i < postings.size() && !result.empty(); i++) {
        const auto& other = *postings[i];
        std::vector<uint32_t> isect;
        isect.reserve(std::min(result.size(), other.size()));
        std::set_intersection(result.begin(), result.end(),
                              other.begin(), other.end(),
                              std::back_inserter(isect));
        result = std::move(isect);
    }
    return result;
}

std::vector<uint32_t> SearchEngine::unionPostingListsMulti(
    const std::unordered_map<Trigram, std::vector<uint32_t>>& index,
    const std::vector<std::string>& segments) {
    // For each segment, intersect its trigrams to get that segment's candidates.
    // Then UNION across all segments. This is safe for both AND and OR semantics
    // from FilteredRE2 — wider than pure intersection but never drops results.
    std::vector<uint32_t> result;
    for (const auto& seg : segments) {
        bool allFound = false;
        auto segCands = intersectPostingLists(index, seg, allFound);
        if (!allFound) continue;  // skip atoms with no trigram coverage
        if (result.empty()) {
            result = std::move(segCands);
        } else {
            std::vector<uint32_t> merged;
            merged.reserve(result.size() + segCands.size());
            std::set_union(result.begin(), result.end(),
                           segCands.begin(), segCands.end(),
                           std::back_inserter(merged));
            result = std::move(merged);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Name trigram index build / add / remove
// ---------------------------------------------------------------------------

std::unordered_map<Trigram, std::vector<uint32_t>>
SearchEngine::buildTrigramIndexFromData(const Column<uint8_t>& types,
                                        const StringPool& namePool) {
    std::unordered_map<Trigram, std::vector<uint32_t>> index;
    for (uint32_t i = 0; i < namePool.entryCount(); i++) {
        if (types[i] == 0 || !namePool.isLive(i)) continue;
        std::string_view sv = namePool.view(i);
        auto trigrams = ContentIndex::extractTrigrams(std::string(sv));
        std::sort(trigrams.begin(), trigrams.end());
        trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());
        for (Trigram t : trigrams) {
            index[t].push_back(i);
        }
    }
    for (auto& [trigram, list] : index) {
        std::sort(list.begin(), list.end());
        list.shrink_to_fit();
    }
    return index;
}

void SearchEngine::buildTrigramIndex() {
    if (!trigramIndexEnabled_) { nameTrigramIndex_.clear(); return; }
    nameTrigramIndex_ = buildTrigramIndexFromData(types_, namePool_);
}

void SearchEngine::addTrigramsForRecord(uint32_t idx, const char* data, uint16_t len) {
    if (!trigramIndexEnabled_) return;
    auto trigrams = ContentIndex::extractTrigrams(std::string(data, len));
    // P-2 fix: sort+unique instead of unordered_set
    std::sort(trigrams.begin(), trigrams.end());
    trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());

    for (Trigram t : trigrams) {
        insertSorted(nameTrigramIndex_[t], idx);
    }
}

void SearchEngine::removeTrigramsForRecord(uint32_t idx) {
    if (idx >= namePool_.entryCount() || !namePool_.isLive(idx)) return;
    // Recompute trigrams from namePool_ instead of storing per-record lists
    removeTrigramsForName(idx, namePool_.view(idx));
}

void SearchEngine::removeTrigramsForName(uint32_t idx, std::string_view lowerName) {
    if (!trigramIndexEnabled_ || lowerName.empty()) return;
    auto trigrams = ContentIndex::extractTrigrams(std::string(lowerName));
    std::sort(trigrams.begin(), trigrams.end());
    trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());
    for (Trigram t : trigrams) {
        auto it = nameTrigramIndex_.find(t);
        if (it != nameTrigramIndex_.end()) {
            auto& list = it->second;
            // Binary search in sorted list
            auto pos = std::lower_bound(list.begin(), list.end(), idx);
            if (pos != list.end() && *pos == idx) {
                list.erase(pos);
            }
            if (list.empty()) {
                nameTrigramIndex_.erase(it);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Extension index build / add / remove
// ---------------------------------------------------------------------------

/// Extract lowercase extension from name data. Returns empty string if no dot.
static std::string extractExtFromName(const char* data, uint16_t len) {
    int dotPos = -1;
    for (int i = static_cast<int>(len) - 1; i >= 0; --i) {
        if (data[i] == '.') { dotPos = i; break; }
    }
    if (dotPos < 0 || dotPos == static_cast<int>(len) - 1) return {};
    // namePool_ stores lowercase names already
    return std::string(data + dotPos + 1, len - dotPos - 1);
}

std::unordered_map<std::string, std::vector<uint32_t>>
SearchEngine::buildExtensionIndexFromData(const Column<uint8_t>& types,
                                           const StringPool& namePool) {
    std::unordered_map<std::string, std::vector<uint32_t>> index;
    for (uint32_t i = 0; i < namePool.entryCount(); i++) {
        if (types[i] == 0 || !namePool.isLive(i)) continue;
        std::string ext = extractExtFromName(namePool.data(i), namePool.length(i));
        if (!ext.empty()) {
            index[ext].push_back(i);
        }
    }
    // Posting lists are already in order since we iterate i = 0..N
    for (auto& [_, list] : index) list.shrink_to_fit();
    return index;
}

void SearchEngine::buildExtensionIndex() {
    if (!extensionIndexEnabled_) { extensionIndex_.clear(); return; }
    extensionIndex_ = buildExtensionIndexFromData(types_, namePool_);
}

void SearchEngine::addExtensionForRecord(uint32_t idx) {
    if (!extensionIndexEnabled_) return;
    if (idx >= namePool_.entryCount() || !namePool_.isLive(idx)) return;
    std::string ext = extractExtFromName(namePool_.data(idx), namePool_.length(idx));
    if (ext.empty()) return;
    insertSorted(extensionIndex_[ext], idx);
}

void SearchEngine::removeExtensionForRecord(uint32_t idx) {
    if (!extensionIndexEnabled_) return;
    if (idx >= namePool_.entryCount() || !namePool_.isLive(idx)) return;
    removeExtensionForName(idx, namePool_.view(idx));
}

void SearchEngine::removeExtensionForName(uint32_t idx, std::string_view lowerName) {
    if (extensionIndex_.empty()) return;
    std::string ext = extractExtFromName(lowerName.data(), static_cast<uint16_t>(lowerName.size()));
    if (ext.empty()) return;
    auto it = extensionIndex_.find(ext);
    if (it != extensionIndex_.end()) {
        auto& list = it->second;
        auto pos = std::lower_bound(list.begin(), list.end(), idx);
        if (pos != list.end() && *pos == idx) {
            list.erase(pos);
        }
        if (list.empty()) {
            extensionIndex_.erase(it);
        }
    }
}

// ---------------------------------------------------------------------------
// Path trigram index build / add / remove
// ---------------------------------------------------------------------------

std::unordered_map<Trigram, std::vector<uint32_t>>
SearchEngine::buildPathTrigramIndexFromData(const StringPool& lowerPathPool) {
    std::unordered_map<Trigram, std::vector<uint32_t>> index;
    for (uint32_t pi = 0; pi < lowerPathPool.entryCount(); pi++) {
        if (!lowerPathPool.isLive(pi)) continue;
        std::string_view pathView(lowerPathPool.data(pi), lowerPathPool.length(pi));
        auto trigrams = ContentIndex::extractTrigrams(std::string(pathView));
        std::sort(trigrams.begin(), trigrams.end());
        trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());
        for (Trigram t : trigrams) {
            index[t].push_back(pi);
        }
    }
    for (auto& [_, list] : index) {
        std::sort(list.begin(), list.end());
        list.shrink_to_fit();
    }
    return index;
}

DirPostings
SearchEngine::buildPathIdxToRecordsFromData(const Column<uint8_t>& types,
                                            const Column<uint32_t>& pathIndices,
                                            uint32_t pathPoolSize) {
    return DirPostings::build(types, pathIndices, pathPoolSize);
}

void SearchEngine::buildPathTrigramIndex() {
    if (!trigramIndexEnabled_) { pathTrigramIndex_.clear(); return; }
    pathTrigramIndex_ = buildPathTrigramIndexFromData(lowerPathPool_);
}

void SearchEngine::rebuildPathIdxToRecords() {
    pathIdxToRecords_ = buildPathIdxToRecordsFromData(types_, pathIndices_, pathPool_.entryCount());
    dirRecordsBuilt_ = true;
}

void SearchEngine::addPathTrigramsForRecord(uint32_t idx) {
    if (idx >= pathIndices_.size()) return;
    uint32_t pi = pathIndices_[idx];
    pathIdxToRecords_.insertSorted(pi, idx);
}

void SearchEngine::removePathTrigramsForRecord(uint32_t idx) {
    if (idx >= pathIndices_.size()) return;
    uint32_t pi = pathIndices_[idx];
    pathIdxToRecords_.eraseSorted(pi, idx);
}

void SearchEngine::ensurePathTrigramsForPathIdx(uint32_t pathIdx) {
    if (!trigramIndexEnabled_) return;
    // Check if this pathIdx already has entries in pathTrigramIndex_
    std::string lowerPath = lowerPathPool_.str(pathIdx);
    auto trigrams = ContentIndex::extractTrigrams(lowerPath);
    std::sort(trigrams.begin(), trigrams.end());
    trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());
    for (Trigram t : trigrams) {
        insertSorted(pathTrigramIndex_[t], pathIdx);
    }
}

// ---------------------------------------------------------------------------
// Dirty page tracking
// ---------------------------------------------------------------------------

void SearchEngine::markPageDirty(uint32_t recordIndex) {
    uint32_t page = recordIndex / kRecordsPerPage;
    if (page < dirtyPages_.size()) {
        dirtyPages_[page] = true;
    }
}

std::vector<uint32_t> SearchEngine::getDirtyPageNumbers() const {
    std::shared_lock lock(mutex_);
    std::vector<uint32_t> result;
    for (size_t i = 0; i < dirtyPages_.size(); i++) {
        if (dirtyPages_[i]) result.push_back(static_cast<uint32_t>(i));
    }
    return result;
}

void SearchEngine::clearDirtyPages() {
    WriteGuard lock(*this);
    std::fill(dirtyPages_.begin(), dirtyPages_.end(), false);
}
