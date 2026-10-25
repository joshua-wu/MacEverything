#include "SearchEngine.h"
#include "StringUtils.h"
#include "QueryParser.h"
#include "QueryTokenizer.h"
#include "QueryFilterParser.h"
#include "QueryNeedsAnalysis.h"
#include "ASTStructuredTransform.h"
#include "ASTGlobTransform.h"
#include "CompiledGlob.h"
#include "SIMDSearch.h"
#include "Logger.h"
#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <unordered_map>
#include <chrono>
#include <re2/re2.h>
#include <re2/filtered_re2.h>
#include <functional>
#include <thread>
#include <dispatch/dispatch.h>

// ---------------------------------------------------------------------------
// queryAdvanced: evaluate a parsed AST against the record set.
//
// Strategy:
// 1. Parse the query into an AST.
// 2. Extract best TERM for trigram pre-filtering.
// 3. Evaluate AST per-record: TERM=substring/glob, FILTER=structured filters,
//    AND/OR/NOT=boolean logic.
// 4. Score, sort, and return results.
//
// Supported FILTER types: ext:, size:, file:, folder:, type:, path:, nopath:,
// parent:, depth:, len:
// ---------------------------------------------------------------------------

namespace {

/// Compare a numeric value against a filter's parsed operator and thresholds.
static bool compareNumeric(uint64_t val, CompareOp op, uint64_t v1, uint64_t v2) {
    switch (op) {
        case CompareOp::EQ:    return val == v1;
        case CompareOp::LT:    return val < v1;
        case CompareOp::LE:    return val <= v1;
        case CompareOp::GT:    return val > v1;
        case CompareOp::GE:    return val >= v1;
        case CompareOp::RANGE: return val >= v1 && val <= v2;
    }
    return false;
}

/// Extract lowercase file extension from name data.
/// Returns empty string_view if no extension found.
static std::string getExtLower(const char* nameData, uint16_t nameLen) {
    // Find last '.'
    int dotPos = -1;
    for (int i = static_cast<int>(nameLen) - 1; i >= 0; --i) {
        if (nameData[i] == '.') { dotPos = i; break; }
    }
    if (dotPos < 0 || dotPos == static_cast<int>(nameLen) - 1) return {};
    // nameData is already lowercase
    return std::string(nameData + dotPos + 1, nameLen - dotPos - 1);
}

/// Count '/' in path to determine depth.
static uint64_t computeDepth(const char* pathData, uint16_t pathLen) {
    uint64_t depth = 0;
    for (uint16_t i = 0; i < pathLen; ++i) {
        if (pathData[i] == '/') depth++;
    }
    return depth;
}

// globMatchImpl is now in CompiledGlob.h

/// Check if a character is a word boundary character (not alphanumeric).
/// Note: underscore '_' IS a boundary (consistent with Everything behavior).
static bool isWordBoundaryChar(char c) {
    return !std::isalnum(static_cast<unsigned char>(c));
}

/// Check if a substring match at position `pos` of length `len` within `text`
/// (total length `textLen`) is a whole-word match.
static bool isWholeWordMatch(const char* text, size_t textLen,
                             size_t pos, size_t len) {
    // Check left boundary: must be start of string or preceded by non-word char
    if (pos > 0 && !isWordBoundaryChar(text[pos - 1])) return false;
    // Check right boundary: must be end of string or followed by non-word char
    size_t end = pos + len;
    if (end < textLen && !isWordBoundaryChar(text[end])) return false;
    return true;
}

/// Pre-compiled regex cache, keyed by QueryNode pointer.
/// Uses RE2 for guaranteed linear-time matching (no backtracking).
using RegexCache = std::unordered_map<const QueryNode*, std::unique_ptr<re2::RE2>>;

/// Per-query, per-directory match results. A substring without '/' occurs in
/// "dir/name" iff it occurs in dir or in name, so the directory half can be
/// evaluated once per directory (~10x fewer than records) instead of once per
/// record, and records only need the name check plus a bitmap lookup.
/// Path-segment filters are cached the same way for both possible matcher
/// states after the name component (see pathSegmentsMatchFrom).
struct DirMatchCache {
    struct Entry {
        const QueryNode* node;
        std::vector<uint8_t> hit;     // term/path: dir contains text; __pathseg: name skipped
        std::vector<uint8_t> hitAlt;  // __pathseg only: name matched the last segment
    };
    std::vector<Entry> entries;

    const Entry* find(const QueryNode* n) const {
        for (const auto& e : entries) {
            if (e.node == n) return &e;
        }
        return nullptr;
    }
};

/// Collect all REGEX TERM nodes from the AST for pre-compilation.
static void collectRegexNodes(const QueryNode& node, std::vector<const QueryNode*>& out) {
    if (node.type == QueryNodeType::TERM && node.mode == MatchMode::REGEX) {
        out.push_back(&node);
    }
    for (auto& child : node.children) {
        collectRegexNodes(*child, out);
    }
}

/// Clone RegexCache for each thread to avoid RE2 DFA mutex contention.
/// RE2's lazy DFA construction uses internal mutexes; sharing one RE2 object
/// across threads causes severe contention (measured: 12T 6x slower than 1T).
/// Per-thread clones eliminate this, yielding ~3.8x MT speedup.
static std::vector<RegexCache> cloneRegexCachePerThread(
    const RegexCache& src, unsigned numThreads) {
    std::vector<RegexCache> perThread(numThreads);
    if (src.empty()) return perThread;
    for (unsigned t = 0; t < numThreads; t++) {
        for (const auto& [nodePtr, re] : src) {
            re2::RE2::Options opts;
            opts.set_case_sensitive(re->options().case_sensitive());
            opts.set_log_errors(false);
            perThread[t].emplace(nodePtr,
                std::make_unique<re2::RE2>(re->pattern(), opts));
        }
    }
    return perThread;
}

/// Evaluate a FILTER node against a single record (SoA fields).
/// When called from the pure-filter SoA fast path, nameData and pathData are nullptr —
/// string-based filters short-circuit (return true) since isPureFilter() guarantees
/// the query contains no string filters.
static bool evalFilter(const QueryNode& node,
                       uint8_t recType, uint64_t recSize, time_t recModTime,
                       const char* nameData, uint16_t nameLen,
                       const char* pathData, uint16_t pathLen,
                       std::vector<char>& pathBuf,
                       const DirMatchCache* dirCache = nullptr, uint32_t pathIdx = 0) {
    const auto& name = node.filterName;

    // __pathseg: internal filter — structured path segment matching
    if (name == "__pathseg") {
        if (!nameData || !pathData) return true;
        if (const auto* e = dirCache ? dirCache->find(&node) : nullptr) {
            // The name is the last path component; the matcher consumes it
            // first (right-to-left), then continues on the directory.
            const auto& last = node.pathSegments.back().text;
            bool nameHit = std::string_view(nameData, nameLen).find(last) != std::string_view::npos;
            return (nameHit ? e->hitAlt : e->hit)[pathIdx] != 0;
        }
        // Build full path: pathData/nameData for component-level matching
        size_t fullLen = static_cast<size_t>(pathLen) + 1 + nameLen;
        if (pathBuf.size() < fullLen) pathBuf.resize(fullLen * 2);
        memcpy(pathBuf.data(), pathData, pathLen);
        pathBuf[pathLen] = '/';
        memcpy(pathBuf.data() + pathLen + 1, nameData, nameLen);
        std::string_view fullPath(pathBuf.data(), fullLen);
        return SearchEngine::pathSegmentsMatch(fullPath, node.pathSegments);
    }

    // ext: — match file extension (requires name data)
    if (name == "ext") {
        if (!nameData) return true;
        std::string ext = getExtLower(nameData, nameLen);
        if (ext.empty()) return false;
        for (auto& e : node.extList) {
            if (ext == e) return true;
        }
        return false;
    }

    // size: — compare file size
    if (name == "size") {
        return compareNumeric(recSize, node.op, node.numVal1, node.numVal2);
    }

    // file: — match files only
    if (name == "file") {
        return recType == 1; // 1=file
    }

    // folder: — match directories only
    if (name == "folder") {
        return recType == 2; // 2=dir
    }

    // type: — shorthand for file/folder
    if (name == "type") {
        if (node.filterArg == "file") return recType == 1;
        if (node.filterArg == "folder" || node.filterArg == "dir") return recType == 2;
        return false;
    }

    // path: — path must contain substring (case insensitive, requires path data)
    if (name == "path") {
        if (!nameData || !pathData) return true;
        if (const auto* e = dirCache ? dirCache->find(&node) : nullptr) {
            return e->hit[pathIdx] || me::simdContains(nameData, nameLen,
                node.filterArg.data(), node.filterArg.size());
        }
        // Build full path: pathData + '/' + nameData
        size_t fullLen = static_cast<size_t>(pathLen) + 1 + nameLen;
        if (pathBuf.size() < fullLen) pathBuf.resize(fullLen * 2);
        memcpy(pathBuf.data(), pathData, pathLen);
        pathBuf[pathLen] = '/';
        memcpy(pathBuf.data() + pathLen + 1, nameData, nameLen);
        return me::simdContains(pathBuf.data(), fullLen,
                                node.filterArg.data(), node.filterArg.size());
    }

    // nopath: — path must NOT contain substring (requires path data)
    if (name == "nopath") {
        if (!nameData || !pathData) return true;
        if (const auto* e = dirCache ? dirCache->find(&node) : nullptr) {
            return !(e->hit[pathIdx] || me::simdContains(nameData, nameLen,
                node.filterArg.data(), node.filterArg.size()));
        }
        size_t fullLen = static_cast<size_t>(pathLen) + 1 + nameLen;
        if (pathBuf.size() < fullLen) pathBuf.resize(fullLen * 2);
        memcpy(pathBuf.data(), pathData, pathLen);
        pathBuf[pathLen] = '/';
        memcpy(pathBuf.data() + pathLen + 1, nameData, nameLen);
        return !me::simdContains(pathBuf.data(), fullLen,
                                 node.filterArg.data(), node.filterArg.size());
    }

    // parent: — direct parent path must match exactly (requires path data)
    if (name == "parent") {
        if (!pathData) return true;
        // filterArg is lowercased; pathData is already lowercased
        std::string pathStr(pathData, pathLen);
        return pathStr == node.filterArg;
    }

    // depth: — directory depth (count of '/' in full path, requires path data)
    if (name == "depth") {
        if (!pathData) return true;
        uint64_t depth = computeDepth(pathData, pathLen);
        return compareNumeric(depth, node.op, node.numVal1, node.numVal2);
    }

    // len: — filename length (requires name data)
    if (name == "len") {
        if (!nameData) return true;
        return compareNumeric(static_cast<uint64_t>(nameLen), node.op,
                              node.numVal1, node.numVal2);
    }

    // dm: / datemodified: — modification date filter
    if (name == "dm" || name == "datemodified") {
        return compareNumeric(static_cast<uint64_t>(recModTime), node.op,
                              node.numVal1, node.numVal2);
    }

    // dc: / datecreated: — creation date (uses modTime as fallback since
    // FileRecord doesn't store birthtime yet)
    if (name == "dc" || name == "datecreated") {
        return compareNumeric(static_cast<uint64_t>(recModTime), node.op,
                              node.numVal1, node.numVal2);
    }

    // da: / dateaccessed: — access date (uses modTime as fallback)
    if (name == "da" || name == "dateaccessed") {
        return compareNumeric(static_cast<uint64_t>(recModTime), node.op,
                              node.numVal1, node.numVal2);
    }

    // Unknown filter — pass through
    return true;
}

/// Evaluate a TERM node against a single record.
/// Returns true if the record's name or full path matches the term text.
/// nameData is lowercase (from namePool_), origNameData has original case (from origNamePool_).
/// pathData is lowercase, origPathData has original case (from pathPool_).
/// Returns false when nameData is nullptr (pure-filter SoA fast path).
static bool evalTerm(const QueryNode& node,
                     const char* nameData, uint16_t nameLen,
                     const char* pathData, uint16_t pathLen,
                     const char* origNameData, uint16_t origNameLen,
                     const char* origPathData, uint16_t origPathLen,
                     std::vector<char>& pathBuf,
                     const RegexCache& regexCache,
                     const DirMatchCache* dirCache = nullptr, uint32_t pathIdx = 0) {
    if (!nameData) return false; // SoA pure-filter path — no string data available
    switch (node.mode) {

    case MatchMode::SUBSTRING: {
        if (node.caseSensitive) {
            // Case-sensitive: compare against original-case name
            const auto& term = node.text;
            // Check name
            if (origNameLen >= term.size()) {
                for (size_t i = 0; i + term.size() <= origNameLen; ++i) {
                    if (memcmp(origNameData + i, term.data(), term.size()) == 0)
                        return true;
                }
            }
            // Check full path (original case: origPath + "/" + origName)
            if (node.nameOnly) return false;
            if (const auto* e = dirCache ? dirCache->find(&node) : nullptr) return e->hit[pathIdx] != 0;
            size_t fullLen = static_cast<size_t>(origPathLen) + 1 + origNameLen;
            if (pathBuf.size() < fullLen) pathBuf.resize(fullLen * 2);
            memcpy(pathBuf.data(), origPathData, origPathLen);
            pathBuf[origPathLen] = '/';
            memcpy(pathBuf.data() + origPathLen + 1, origNameData, origNameLen);
            if (fullLen >= term.size()) {
                for (size_t i = 0; i + term.size() <= fullLen; ++i) {
                    if (memcmp(pathBuf.data() + i, term.data(), term.size()) == 0)
                        return true;
                }
            }
            return false;
        }
        // Case-insensitive (default): use lowercase nameData
        const auto& lower = node.textLower;
        if (me::simdContains(nameData, nameLen, lower.data(), lower.size())) {
            return true;
        }
        // Skip full-path matching when nameOnly is set
        // (transformSlashTerms uses this for name-component terms)
        if (node.nameOnly) return false;
        if (const auto* e = dirCache ? dirCache->find(&node) : nullptr) return e->hit[pathIdx] != 0;
        size_t fullLen = static_cast<size_t>(pathLen) + 1 + nameLen;
        if (pathBuf.size() < fullLen) pathBuf.resize(fullLen * 2);
        memcpy(pathBuf.data(), pathData, pathLen);
        pathBuf[pathLen] = '/';
        memcpy(pathBuf.data() + pathLen + 1, nameData, nameLen);
        return me::simdContains(pathBuf.data(), fullLen, lower.data(), lower.size());
    }

    case MatchMode::GLOB: {
        const auto& pattern = node.textLower;
        // If pattern contains '/', match against full path; otherwise name only
        if (pattern.find('/') != std::string::npos) {
            size_t fullLen = static_cast<size_t>(pathLen) + 1 + nameLen;
            if (pathBuf.size() < fullLen) pathBuf.resize(fullLen * 2);
            memcpy(pathBuf.data(), pathData, pathLen);
            pathBuf[pathLen] = '/';
            memcpy(pathBuf.data() + pathLen + 1, nameData, nameLen);
            if (node.compiledGlob) {
                return compiledGlobMatch(*node.compiledGlob, pathBuf.data(), fullLen);
            }
            return globMatchImpl(pattern, pathBuf.data(), fullLen);
        }
        // Name-only glob: use pre-compiled fast path (zero-alloc)
        if (node.compiledGlob) {
            return compiledGlobMatch(*node.compiledGlob, nameData, nameLen);
        }
        return globMatchImpl(pattern, nameData, nameLen);
    }

    case MatchMode::REGEX: {
        auto it = regexCache.find(&node);
        if (it == regexCache.end()) return false;
        // Match against name using RE2 zero-copy StringPiece (no per-record allocation)
        re2::StringPiece sp(nameData, nameLen);
        if (RE2::PartialMatch(sp, *it->second)) return true;
        // If not nameOnly, also try full path match
        if (node.nameOnly) return false;
        size_t fullLen = static_cast<size_t>(pathLen) + 1 + nameLen;
        if (pathBuf.size() < fullLen) pathBuf.resize(fullLen * 2);
        memcpy(pathBuf.data(), pathData, pathLen);
        pathBuf[pathLen] = '/';
        memcpy(pathBuf.data() + pathLen + 1, nameData, nameLen);
        re2::StringPiece fullSp(pathBuf.data(), fullLen);
        return RE2::PartialMatch(fullSp, *it->second);
    }

    case MatchMode::WHOLEWORD: {
        const auto& lower = node.textLower;
        // Check in lowercase name
        for (size_t i = 0; i + lower.size() <= nameLen; ++i) {
            if (memcmp(nameData + i, lower.data(), lower.size()) == 0) {
                if (isWholeWordMatch(nameData, nameLen, i, lower.size()))
                    return true;
            }
        }
        return false;
    }

    case MatchMode::WHOLEFILENAME: {
        const auto& lower = node.textLower;
        // Entire filename must match
        if (nameLen == lower.size() &&
            memcmp(nameData, lower.data(), nameLen) == 0)
            return true;
        return false;
    }

    } // switch
    return false;
}

/// Recursively evaluate AST node against a single record.
static bool evalNode(const QueryNode& node,
                     uint8_t recType, uint64_t recSize, time_t recModTime,
                     const char* nameData, uint16_t nameLen,
                     const char* pathData, uint16_t pathLen,
                     const char* origNameData, uint16_t origNameLen,
                     const char* origPathData, uint16_t origPathLen,
                     std::vector<char>& pathBuf,
                     const RegexCache& regexCache,
                     const DirMatchCache* dirCache = nullptr, uint32_t pathIdx = 0) {
    switch (node.type) {
        case QueryNodeType::TERM:
            return evalTerm(node, nameData, nameLen, pathData, pathLen,
                            origNameData, origNameLen, origPathData, origPathLen,
                            pathBuf, regexCache, dirCache, pathIdx);

        case QueryNodeType::AND:
            for (auto& child : node.children) {
                if (!evalNode(*child, recType, recSize, recModTime,
                              nameData, nameLen, pathData, pathLen,
                              origNameData, origNameLen, origPathData, origPathLen,
                              pathBuf, regexCache, dirCache, pathIdx))
                    return false;
            }
            return true;

        case QueryNodeType::OR:
            for (auto& child : node.children) {
                if (evalNode(*child, recType, recSize, recModTime,
                             nameData, nameLen, pathData, pathLen,
                             origNameData, origNameLen, origPathData, origPathLen,
                             pathBuf, regexCache, dirCache, pathIdx))
                    return true;
            }
            return false;

        case QueryNodeType::NOT:
            if (node.children.empty()) return true;
            return !evalNode(*node.children[0], recType, recSize, recModTime,
                             nameData, nameLen, pathData, pathLen,
                             origNameData, origNameLen, origPathData, origPathLen,
                             pathBuf, regexCache, dirCache, pathIdx);

        case QueryNodeType::FILTER:
            return evalFilter(node, recType, recSize, recModTime,
                              nameData, nameLen, pathData, pathLen, pathBuf,
                              dirCache, pathIdx);
    }
    return false;
}

/// Collect AST nodes whose directory half can be cached (see DirMatchCache).
static void collectDirCacheNodes(const QueryNode& node, std::vector<const QueryNode*>& out) {
    if (node.type == QueryNodeType::TERM) {
        if (node.mode == MatchMode::SUBSTRING && !node.nameOnly &&
            node.text.find('/') == std::string::npos && !node.textLower.empty()) {
            out.push_back(&node);
        }
    } else if (node.type == QueryNodeType::FILTER) {
        const auto& fn = node.filterName;
        if ((fn == "path" || fn == "nopath") && !node.filterArg.empty() &&
            node.filterArg.find('/') == std::string::npos) {
            out.push_back(&node);
        } else if (fn == "__pathseg" && !node.pathSegments.empty()) {
            out.push_back(&node);
        }
    }
    for (const auto& child : node.children) collectDirCacheNodes(*child, out);
}

/// Evaluate cacheable nodes once per directory, in parallel.
static DirMatchCache buildDirMatchCache(const QueryNode& root,
                                        const StringPool& lowerPathPool,
                                        const StringPool& pathPool) {
    DirMatchCache cache;
    std::vector<const QueryNode*> nodes;
    collectDirCacheNodes(root, nodes);
    if (nodes.empty()) return cache;

    const uint32_t pathCount = lowerPathPool.entryCount();
    for (const auto* n : nodes) {
        DirMatchCache::Entry e{n, std::vector<uint8_t>(pathCount, 0), {}};
        if (n->type == QueryNodeType::FILTER && n->filterName == "__pathseg") {
            e.hitAlt.assign(pathCount, 0);
        }
        cache.entries.push_back(std::move(e));
    }

    auto* entries = cache.entries.data();
    const size_t entryCount = cache.entries.size();
    constexpr uint32_t kChunk = 8192;
    const size_t chunks = (pathCount + kChunk - 1) / kChunk;
    dispatch_apply(chunks, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t c) {
        uint32_t begin = static_cast<uint32_t>(c * kChunk);
        uint32_t end = std::min(pathCount, begin + kChunk);
        for (size_t k = 0; k < entryCount; k++) {
            auto& e = entries[k];
            const QueryNode& n = *e.node;
            if (n.type == QueryNodeType::TERM) {
                bool cs = n.caseSensitive;
                const StringPool& pool = cs ? pathPool : lowerPathPool;
                const std::string& t = cs ? n.text : n.textLower;
                for (uint32_t pi = begin; pi < end; pi++) {
                    e.hit[pi] = me::simdContains(pool.data(pi), pool.length(pi), t.data(), t.size());
                }
            } else if (n.filterName == "__pathseg") {
                const int last = static_cast<int>(n.pathSegments.size()) - 1;
                for (uint32_t pi = begin; pi < end; pi++) {
                    std::string_view dir = lowerPathPool.view(pi);
                    e.hit[pi] = SearchEngine::pathSegmentsMatchFrom(dir, n.pathSegments, last);
                    e.hitAlt[pi] = SearchEngine::pathSegmentsMatchFrom(dir, n.pathSegments, last - 1);
                }
            } else { // path / nopath
                const std::string& t = n.filterArg;
                for (uint32_t pi = begin; pi < end; pi++) {
                    e.hit[pi] = me::simdContains(lowerPathPool.data(pi), lowerPathPool.length(pi),
                                                 t.data(), t.size());
                }
            }
        }
    });
    return cache;
}

/// Extract all TERM texts from the AST (for trigram pre-filtering).
static void collectTerms(const QueryNode& node, std::vector<std::string>& terms) {
    switch (node.type) {
        case QueryNodeType::TERM:
            terms.push_back(node.textLower);
            break;
        case QueryNodeType::AND:
        case QueryNodeType::OR:
            for (auto& child : node.children) collectTerms(*child, terms);
            break;
        case QueryNodeType::NOT:
            if (!node.children.empty()) collectTerms(*node.children[0], terms);
            break;
        case QueryNodeType::FILTER:
            break;
    }
}

/// Find the single best (longest) SUBSTRING TERM from the AND-level for trigram pre-filtering.
/// For OR queries we can't pre-filter (any branch might match), so return empty.
/// Skips __pathseg filter nodes and recurses into AND children.
/// Extract the best trigram key from a TERM node (SUBSTRING or GLOB mode).
static std::string termTrigramKey(const QueryNode& node) {
    if (node.mode == MatchMode::SUBSTRING) return node.textLower;
    if (node.mode == MatchMode::GLOB) {
        auto segs = extractLiteralSegments(node.textLower);
        std::string best;
        for (const auto& s : segs)
            if (s.size() >= 3 && s.size() > best.size()) best = s;
        return best;
    }
    return {};
}

static std::string bestTrigramTerm(const QueryNode& node) {
    if (node.type == QueryNodeType::TERM) {
        return termTrigramKey(node);
    }
    if (node.type == QueryNodeType::AND) {
        std::string best;
        for (auto& child : node.children) {
            if (child->type == QueryNodeType::TERM) {
                std::string t = termTrigramKey(*child);
                if (t.size() > best.size()) best = t;
            } else if (child->type == QueryNodeType::AND) {
                std::string t = bestTrigramTerm(*child);
                if (t.size() > best.size()) best = t;
            }
        }
        return best;
    }
    // OR, NOT, FILTER — can't reliably pre-filter
    return {};
}

/// Extract literal atoms from a regex pattern using RE2's FilteredRE2.
/// Returns lowercased, deduplicated atoms (min length 3) that RE2's own
/// AST parser determines are useful for pre-filtering.
/// Correctly handles alternation, character classes, and all regex constructs.
static std::vector<std::string> extractRegexLiteralsImpl(const std::string& pattern) {
    re2::FilteredRE2 fre2(3);  // min_atom_len = 3 (matches trigram minimum)
    re2::RE2::Options opts;
    opts.set_case_sensitive(false);
    opts.set_log_errors(false);
    int id;
    if (fre2.Add(pattern, opts, &id) != re2::RE2::NoError) {
        return {};
    }
    std::vector<std::string> atoms;
    fre2.Compile(&atoms);
    return atoms;
}

/// Collect regex literal substrings from all REGEX TERM nodes in the AST.
static std::vector<std::string> extractRegexLiteralsFromAST(const QueryNode& node) {
    std::vector<std::string> allLiterals;
    std::function<void(const QueryNode&)> collect = [&](const QueryNode& n) {
        if (n.type == QueryNodeType::TERM && n.mode == MatchMode::REGEX) {
            auto lits = extractRegexLiteralsImpl(n.text);
            allLiterals.insert(allLiterals.end(), lits.begin(), lits.end());
        }
        for (auto& child : n.children) collect(*child);
    };
    collect(node);
    return allLiterals;
}


/// Extract the first SUBSTRING TERM text from the AST for result scoring.
/// Returns empty string if no suitable term is found (pure filter queries).
/// Skips __pathseg filter nodes (they are path constraints, not scoring terms).
static std::string extractScoringTerm(const QueryNode& node) {
    if (node.type == QueryNodeType::TERM && node.mode == MatchMode::SUBSTRING) {
        return node.textLower;
    }
    if (node.type == QueryNodeType::FILTER && node.filterName == "__pathseg") {
        return {}; // path segment constraint, not a scoring term
    }
    if (node.type == QueryNodeType::AND || node.type == QueryNodeType::OR) {
        for (auto& child : node.children) {
            auto t = extractScoringTerm(*child);
            if (!t.empty()) return t;
        }
    }
    return {};
}

/// Extract the best (longest, >= 3 chars) path segment text from __pathseg filters in the AST.
/// Only looks in AND-level nodes (can't pre-filter OR branches).
static std::string bestPathSegTerm(const QueryNode& node) {
    if (node.type == QueryNodeType::FILTER && node.filterName == "__pathseg") {
        std::string best;
        for (auto& seg : node.pathSegments) {
            if (seg.text.size() >= 3 && seg.text.size() > best.size()) {
                best = seg.text;
            }
        }
        return best;
    }
    if (node.type == QueryNodeType::AND) {
        std::string best;
        for (auto& child : node.children) {
            auto t = bestPathSegTerm(*child);
            if (t.size() > best.size()) best = t;
        }
        return best;
    }
    return {};
}

/// Extract extension filter values from AND-level ext: FILTER nodes in the AST.
/// Returns the collected extList values for the first ext: filter found at AND level.
/// For OR queries or deeply nested ext:, returns empty (can't pre-filter safely).
static std::vector<std::string> extractExtFilterValues(const QueryNode& node) {
    if (node.type == QueryNodeType::FILTER && node.filterName == "ext") {
        return node.extList;
    }
    if (node.type == QueryNodeType::AND) {
        for (auto& child : node.children) {
            if (child->type == QueryNodeType::FILTER && child->filterName == "ext") {
                return child->extList;
            }
        }
    }
    return {};
}

} // anonymous namespace

// Expose extractRegexLiterals for unit testing
namespace me_test {
    std::vector<std::string> extractRegexLiterals(const std::string& pattern) {
        return extractRegexLiteralsImpl(pattern);
    }
}

std::vector<uint32_t> SearchEngine::queryAdvanced(const std::string& input,
                                                   uint32_t maxResults,
                                                   bool useTrigram,
                                                   QueryTimingInfo& timing,
                                                   uint64_t myGen,
                                                   const std::atomic<uint64_t>* genPtr) const {
    // If no external generation pointer, create a local dummy (no cancellation)
    std::atomic<uint64_t> dummyGen{myGen};
    if (!genPtr) genPtr = &dummyGen;

    auto queryStart = std::chrono::steady_clock::now();

    // Parse the AST
    auto ast = QueryParser::parse(input);
    if (!ast) return {};

    // Transform TERM nodes containing '/' into structured path-segment constraints.
    // e.g. TERM("/usr/local/test") → AND(FILTER("__pathseg",[usr,local]), TERM("test"))
    ast = transformSlashTerms(std::move(ast));

    // Transform TERM nodes containing '*' or '?' from SUBSTRING to GLOB mode.
    ast = transformGlobTerms(std::move(ast));

    // Analyze which record fields the query actually needs
    QueryNeeds needs = analyzeQueryNeeds(*ast);

    // Pre-compile all regex patterns before acquiring lock
    RegexCache regexCache;
    {
        std::vector<const QueryNode*> regexNodes;
        collectRegexNodes(*ast, regexNodes);
        for (auto* rn : regexNodes) {
            re2::RE2::Options opts;
            opts.set_case_sensitive(rn->caseSensitive);
            opts.set_log_errors(false);
            auto re = std::make_unique<re2::RE2>(rn->text, opts);
            if (re->ok()) {
                regexCache.emplace(rn, std::move(re));
            }
            // Invalid regex — evalTerm will return false for this node
        }
    }

    auto beforeLock = std::chrono::steady_clock::now();
    std::shared_lock lock(mutex_);
    auto afterLock = std::chrono::steady_clock::now();

    if (types_.empty()) return {};
    size_t totalSize = types_.size();
    auto beforeTrigram = afterLock, afterTrigram = afterLock;

    // --- Trigram pre-filtering: competitive selection among name, regex, and path trigrams ---
    std::string trigramKey = bestTrigramTerm(*ast);
    std::vector<uint32_t> candidates;
    bool useTrigramIndex = false;

    // Stage 1: Name trigram — stash result, don't commit yet
    std::vector<uint32_t> nameCands;
    bool nameOk = false;
    bool stage1AllFound = false;
    size_t stage1RawCandCount = 0;
    if (useTrigram && !trigramKey.empty() && trigramKey.size() >= 3 && !nameTrigramIndex_.empty()) {
        beforeTrigram = std::chrono::steady_clock::now();
        nameCands = intersectPostingLists(nameTrigramIndex_, trigramKey, stage1AllFound);
        stage1RawCandCount = nameCands.size();
        nameOk = stage1AllFound && nameCands.size() <= totalSize / 10;
        if (!nameOk) nameCands.clear();
        afterTrigram = std::chrono::steady_clock::now();
    }

    // Diagnostic: log why trigram Stage 1 was skipped or failed (for 3+ char queries)
    if (trigramIndexEnabled_ && trigramKey.size() >= 3 && !nameOk) {
        bool p2 = phase2Pending_.load(std::memory_order_acquire);
        bool idxEmpty = nameTrigramIndex_.empty();
        LOG_INFO("SearchEngine", "DIAG-TRIGRAM key=\"" << trigramKey
            << "\" SKIP reason="
            << (!useTrigram ? "useTrigram=false" :
                idxEmpty ? "indexEmpty" :
                !stage1AllFound ? "allFound=false" :
                stage1RawCandCount > totalSize / 10 ? "tooManyCands" : "unknown")
            << " phase2Pending=" << p2
            << " indexEmpty=" << idxEmpty
            << " indexBuckets=" << nameTrigramIndex_.size()
            << " allFound=" << stage1AllFound
            << " rawCands=" << stage1RawCandCount
            << " threshold=" << totalSize / 10
            << " totalSize=" << totalSize);
    }

    // Stage 2: Regex trigram (only if name failed)
    // Uses UNION (not AND) across atoms because FilteredRE2 atoms may have
    // OR semantics (e.g. alternation `foo|bar` → atoms are OR-related).
    if (useTrigram && !nameOk && !nameTrigramIndex_.empty()) {
        auto regexLiterals = extractRegexLiteralsFromAST(*ast);
        if (!regexLiterals.empty()) {
            beforeTrigram = std::chrono::steady_clock::now();
            nameCands = unionPostingListsMulti(nameTrigramIndex_, regexLiterals);
            nameOk = !nameCands.empty() && nameCands.size() <= totalSize / 10;
            if (!nameOk) nameCands.clear();
            afterTrigram = std::chrono::steady_clock::now();
        }
    }

    // Stage 3: Path trigram — always attempt (compete with name trigram)
    std::vector<uint32_t> pathCands;
    bool pathOk = false;
    if (useTrigram && !pathTrigramIndex_.empty()) {
        std::string pathSegKey = bestPathSegTerm(*ast);
        if (pathSegKey.size() >= 3) {
            auto pt0 = std::chrono::steady_clock::now();
            bool allFound = false;
            auto pathIdxCands = intersectPostingLists(pathTrigramIndex_, pathSegKey, allFound);
            if (allFound) {
                // Early-exit: if name trigram already succeeded and path index count
                // is >= nameCands (each path index expands to >=1 record), expansion
                // cannot produce fewer candidates — skip the expensive expansion.
                bool skipExpansion = nameOk && pathIdxCands.size() >= nameCands.size();
                if (!skipExpansion) {
                    for (uint32_t pi : pathIdxCands) {
                        if (genPtr->load(std::memory_order_relaxed) != myGen) return {};
                        if (pi >= pathIdxToRecords_.size()) continue;
                        const auto& recIds = pathIdxToRecords_[pi];
                        pathCands.insert(pathCands.end(), recIds.begin(), recIds.end());
                    }
                    std::sort(pathCands.begin(), pathCands.end());
                    pathCands.erase(std::unique(pathCands.begin(), pathCands.end()), pathCands.end());
                    pathOk = pathCands.size() <= totalSize / 4;
                    if (!pathOk) pathCands.clear();
                }
            }
            afterTrigram = std::chrono::steady_clock::now();
        }
    }

    // Stage 4: Extension index pre-filtering
    std::vector<uint32_t> extCands;
    bool extOk = false;
    {
        auto extValues = extensionIndexEnabled_ && !phase2Pending_.load(std::memory_order_acquire)
                             ? extractExtFilterValues(*ast) : decltype(extractExtFilterValues(*ast)){};
        if (!extValues.empty()) {
            // Union posting lists for all requested extensions
            for (const auto& ext : extValues) {
                auto it = extensionIndex_.find(ext);
                if (it != extensionIndex_.end()) {
                    extCands.insert(extCands.end(), it->second.begin(), it->second.end());
                }
            }
            if (extValues.size() > 1) {
                std::sort(extCands.begin(), extCands.end());
                extCands.erase(std::unique(extCands.begin(), extCands.end()), extCands.end());
            }
            extOk = true; // Extension index is precise — empty means no matches
        }
    }

    // Stage 5: Pick best — fewer candidates wins
    // Collect all viable candidate sets and pick the smallest
    struct CandidateSet {
        std::vector<uint32_t>* cands;
        const char* label;
    };
    std::vector<CandidateSet> viable;
    if (nameOk) viable.push_back({&nameCands, "name"});
    if (pathOk) viable.push_back({&pathCands, "path"});
    if (extOk)  viable.push_back({&extCands,  "ext"});

    if (!viable.empty()) {
        // Find smallest
        auto best = &viable[0];
        for (size_t i = 1; i < viable.size(); i++) {
            if (viable[i].cands->size() < best->cands->size()) {
                best = &viable[i];
            }
        }
        candidates = std::move(*best->cands);
        // If the winner is not name trigram, clear trigramKey for label purposes
        if (std::string(best->label) != "name") {
            trigramKey.clear();
        }
        useTrigramIndex = true;
    }

    // Evaluate AST against each candidate (or all records for linear scan)
    // Uses SearchEngine::Match (declared in SearchEngine.h)
    std::vector<Match> merged;
    std::vector<char> pathBuf;

    // Extract scoring term from AST (not raw input) — hoisted out of per-record loop
    std::string scoringTerm = extractScoringTerm(*ast);

    auto beforePhase = std::chrono::steady_clock::now();

    if (useTrigramIndex) {
        // Trigram-filtered path with parallel dispatch + software prefetch
        const size_t candidateCount = candidates.size();
        constexpr size_t PARALLEL_THRESHOLD = 10000;
        constexpr size_t PREFETCH_DIST = 8;

        if (candidateCount >= PARALLEL_THRESHOLD) {
            // Large candidate set — parallel dispatch_apply with prefetch
            unsigned numThreads = std::thread::hardware_concurrency();
            if (numThreads < 1) numThreads = 1;
            if (numThreads > 32) numThreads = 32;
            size_t chunkSize = (candidateCount + numThreads - 1) / numThreads;

            __block std::vector<std::vector<Match>> threadResults(numThreads);
            uint64_t capturedGen = myGen;
            const uint32_t* candidatesData = candidates.data();
            const auto* astPtr = ast.get();
            const auto* typesPtr = types_.data();
            const auto* sizesPtr = sizes_.data();
            const auto* modTimesPtr = modTimes_.data();
            const auto& namePool = namePool_;
            const auto& origNamePool = origNamePool_;
            const auto& lowerPathPool = lowerPathPool_;
            const auto& pathPool = pathPool_;
            const auto& pIndices = pathIndices_;
            // Per-thread RE2 clones to avoid DFA mutex contention
            auto perThreadCaches = cloneRegexCachePerThread(regexCache, numThreads);
            auto* ptcPtr = perThreadCaches.data();
            const auto& sTerm = scoringTerm;

            dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
            dispatch_apply(numThreads, queue, ^(size_t t) {
                size_t start = t * chunkSize;
                size_t end = std::min(start + chunkSize, candidateCount);
                if (start >= end) return;
                auto& local = threadResults[t];
                std::vector<char> localPathBuf;
                const auto& regCache = ptcPtr[t];
                for (size_t ci = start; ci < end; ci++) {
                    if ((ci & 1023) == 0 && genPtr->load(std::memory_order_relaxed) != capturedGen) return;
                    if (ci + PREFETCH_DIST < end) {
                        uint32_t futureIdx = candidatesData[ci + PREFETCH_DIST];
                        __builtin_prefetch(typesPtr + futureIdx, 0, 0);
                        __builtin_prefetch(namePool.entries() + futureIdx, 0, 0);
                    }
                    uint32_t idx = candidatesData[ci];
                    if (typesPtr[idx] == 0) continue;
                    const char* nd = namePool.data(idx);
                    uint16_t nl = namePool.length(idx);
                    uint32_t pi = pIndices[idx];
                    const char* pd = lowerPathPool.data(pi);
                    uint16_t pl = lowerPathPool.length(pi);
                    const char* ond = origNamePool.data(idx);
                    uint16_t onl = origNamePool.length(idx);
                    const char* opd = pathPool.data(pi);
                    uint16_t opl = pathPool.length(pi);
                    if (!evalNode(*astPtr, typesPtr[idx], sizesPtr[idx],
                                  static_cast<time_t>(modTimesPtr[idx]),
                                  nd, nl, pd, pl, ond, onl, opd, opl,
                                  localPathBuf, regCache)) continue;
                    uint8_t priority = 2;
                    if (!sTerm.empty()) {
                        if (me::simdContains(nd, nl, sTerm.data(), sTerm.size())) {
                            priority = namePriority(nd, nl, sTerm.data(), sTerm.size());
                        } else {
                            priority = 3;
                        }
                    }
                    local.push_back({idx, priority, static_cast<uint32_t>(pl + 1 + nl)});
                }
            });

            if (genPtr->load(std::memory_order_relaxed) != myGen) return {};
            for (auto& v : threadResults) {
                merged.insert(merged.end(), v.begin(), v.end());
            }
        } else {
            // Small candidate set — single-threaded with prefetch
            const uint32_t* candidatesData = candidates.data();
            const auto* smallTypesPtr = types_.data();
            const auto* smallSizesPtr = sizes_.data();
            const auto* smallModTimesPtr = modTimes_.data();
            for (size_t ci = 0; ci < candidateCount; ci++) {
                if ((ci & 1023) == 0 && genPtr->load(std::memory_order_relaxed) != myGen) return {};
                if (ci + PREFETCH_DIST < candidateCount) {
                    uint32_t futureIdx = candidatesData[ci + PREFETCH_DIST];
                    __builtin_prefetch(smallTypesPtr + futureIdx, 0, 0);
                    __builtin_prefetch(namePool_.entries() + futureIdx, 0, 0);
                }
                uint32_t idx = candidatesData[ci];
                if (smallTypesPtr[idx] == 0) continue;
                const char* nd = namePool_.data(idx);
                uint16_t nl = namePool_.length(idx);
                uint32_t pi = pathIndices_[idx];
                const char* pd = lowerPathPool_.data(pi);
                uint16_t pl = lowerPathPool_.length(pi);
                const char* ond = origNamePool_.data(idx);
                uint16_t onl = origNamePool_.length(idx);
                const char* opd = pathPool_.data(pi);
                uint16_t opl = pathPool_.length(pi);
                if (!evalNode(*ast, smallTypesPtr[idx], smallSizesPtr[idx],
                              static_cast<time_t>(smallModTimesPtr[idx]),
                              nd, nl, pd, pl, ond, onl, opd, opl,
                              pathBuf, regexCache)) continue;
                uint8_t priority = 2;
                if (!scoringTerm.empty()) {
                    if (me::simdContains(nd, nl, scoringTerm.data(), scoringTerm.size())) {
                        priority = namePriority(nd, nl, scoringTerm.data(), scoringTerm.size());
                    } else {
                        priority = 3;
                    }
                }
                merged.push_back({idx, priority, static_cast<uint32_t>(pl + 1 + nl)});
            }
        }
    } else {
        // Linear scan — parallelize with GCD dispatch_apply
        unsigned numThreads = std::thread::hardware_concurrency();
        if (numThreads < 1) numThreads = 1;
        if (numThreads > 32) numThreads = 32;
        // For small record sets, don't over-parallelize
        if (totalSize < 10000) numThreads = 1;

        size_t chunkSize = (totalSize + numThreads - 1) / numThreads;
        __block std::vector<std::vector<Match>> threadResults(numThreads);

        uint64_t capturedGen = myGen;
        bool pureFilter = needs.isPureFilter();

        // Capture references for block
        const auto* astPtr = ast.get();
        const auto* typesPtr = types_.data();
        const auto* sizesPtr = sizes_.data();
        const auto* modTimesPtr = modTimes_.data();
        const auto& namePool = namePool_;
        const auto& origNamePool = origNamePool_;
        const auto& lowerPathPool = lowerPathPool_;
        const auto& pathPool = pathPool_;
        const auto& pIndices = pathIndices_;
        // Per-thread RE2 clones to avoid DFA mutex contention
        auto perThreadCaches = cloneRegexCachePerThread(regexCache, numThreads);
        auto* ptcPtr = perThreadCaches.data();
        const auto& sTerm = scoringTerm;

        // Per-directory match cache: worthwhile once records clearly outnumber
        // directories (always true for a real disk index).
        DirMatchCache dirCacheStorage;
        if (!pureFilter && totalSize >= dirCacheMinRecords_ &&
            (dirCacheMinRecords_ == 0 || lowerPathPool_.entryCount() <= totalSize / 2)) {
            dirCacheStorage = buildDirMatchCache(*ast, lowerPathPool_, pathPool_);
        }
        const DirMatchCache* dirCache = dirCacheStorage.entries.empty() ? nullptr : &dirCacheStorage;

        dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
        dispatch_apply(numThreads, queue, ^(size_t t) {
            size_t start = t * chunkSize;
            size_t end = std::min(start + chunkSize, totalSize);
            if (start >= end) return;

            auto& local = threadResults[t];
            std::vector<char> localPathBuf;
            const auto& regCache = ptcPtr[t];

            if (pureFilter) {
                // ── SIMD-batched pure-filter fast path ──
                size_t idx = start;

                size_t alignedStart = (start + 15) & ~size_t(15);
                if (alignedStart > end) alignedStart = end;

                // Scalar pre-amble
                for (; idx < alignedStart; idx++) {
                    if (typesPtr[idx] == 0) continue;
                    if (!evalNode(*astPtr, typesPtr[idx], sizesPtr[idx],
                                  static_cast<time_t>(modTimesPtr[idx]),
                                  nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0,
                                  localPathBuf, regCache)) continue;
                    local.push_back({static_cast<uint32_t>(idx), 2, 0});
                }

                // SIMD main loop: 16 records per iteration
                for (; idx + 16 <= end; idx += 16) {
                    if ((idx & 4095) == 0 && genPtr->load(std::memory_order_relaxed) != capturedGen) return;

                    uint16_t liveMask = me::simdTypeLive16(typesPtr + idx);
                    if (liveMask == 0) continue;

                    while (liveMask) {
                        int bit = __builtin_ctz(liveMask);
                        size_t ri = idx + bit;
                        if (evalNode(*astPtr, typesPtr[ri], sizesPtr[ri],
                                     static_cast<time_t>(modTimesPtr[ri]),
                                     nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0,
                                     localPathBuf, regCache)) {
                            local.push_back({static_cast<uint32_t>(ri), 2, 0});
                        }
                        liveMask &= liveMask - 1;
                    }
                }

                // Scalar tail
                for (; idx < end; idx++) {
                    if (typesPtr[idx] == 0) continue;
                    if (!evalNode(*astPtr, typesPtr[idx], sizesPtr[idx],
                                  static_cast<time_t>(modTimesPtr[idx]),
                                  nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0,
                                  localPathBuf, regCache)) continue;
                    local.push_back({static_cast<uint32_t>(idx), 2, 0});
                }
            } else {
                // ── Full evaluation path (needs string access) ──
                for (size_t idx = start; idx < end; idx++) {
                    if ((idx & 4095) == 0 && genPtr->load(std::memory_order_relaxed) != capturedGen) return;

                    if (typesPtr[idx] == 0) continue;
                    const char* nd = namePool.data(static_cast<uint32_t>(idx));
                    uint16_t nl = namePool.length(static_cast<uint32_t>(idx));
                    uint32_t pi = pIndices[idx];
                    const char* pd = lowerPathPool.data(pi);
                    uint16_t pl = lowerPathPool.length(pi);
                    const char* ond = origNamePool.data(static_cast<uint32_t>(idx));
                    uint16_t onl = origNamePool.length(static_cast<uint32_t>(idx));
                    const char* opd = pathPool.data(pi);
                    uint16_t opl = pathPool.length(pi);

                    if (!evalNode(*astPtr, typesPtr[idx], sizesPtr[idx],
                                  static_cast<time_t>(modTimesPtr[idx]),
                                  nd, nl, pd, pl, ond, onl, opd, opl,
                                  localPathBuf, regCache, dirCache, pi)) continue;

                    uint8_t priority = 2;
                    if (!sTerm.empty()) {
                        if (me::simdContains(nd, nl, sTerm.data(), sTerm.size())) {
                            priority = namePriority(nd, nl, sTerm.data(), sTerm.size());
                        } else {
                            priority = 3;
                        }
                    }
                    uint32_t pLen = static_cast<uint32_t>(pl + 1 + nl);
                    local.push_back({static_cast<uint32_t>(idx), priority, pLen});
                }
            }
        });

        // Check if superseded after dispatch_apply
        if (genPtr->load(std::memory_order_relaxed) != myGen) return {};

        // Merge thread-local results
        for (auto& v : threadResults) {
            merged.insert(merged.end(), v.begin(), v.end());
        }
    }

    auto afterPhase = std::chrono::steady_clock::now();

    // Release lock before sorting
    auto beforeUnlock = std::chrono::steady_clock::now();
    lock.unlock();

    // Sort by priority, then path length
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
    timing.trigramMs = toMs(afterTrigram - beforeTrigram);
    timing.phase1Ms = 0;
    timing.phase2Ms = toMs(afterPhase - beforePhase);
    timing.totalRecords = totalSize;
    timing.candidates = candidates.size();
    timing.nameMatches = 0;
    timing.pathMatches = 0;
    timing.resultCount = result.size();
    timing.usedTrigram = useTrigramIndex;
    if (useTrigramIndex) {
        if (!trigramKey.empty()) {
            timing.searchPath = "advanced-trigram";
        } else if (extOk && !extractExtFilterValues(*ast).empty()) {
            timing.searchPath = "advanced-ext-index";
        } else if (!bestPathSegTerm(*ast).empty()) {
            timing.searchPath = "advanced-path-trigram";
        } else {
            timing.searchPath = "advanced-regex-trigram";
        }
    } else if (needs.isPureFilter()) {
        timing.searchPath = "advanced-pure-filter-soa-gcd";
    } else {
        timing.searchPath = "advanced-linear-gcd";
    }

    auto ms = static_cast<long long>(timing.totalMs);
    if (ms > 100) {
        LOG_INFO("SearchEngine", "QueryAdvanced \"" << input << "\" total=" << ms
            << "ms | path=" << timing.searchPath
            << " candidates=" << timing.candidates
            << " results=" << timing.resultCount
            << " totalRecords=" << timing.totalRecords);
    }

    return result;
}
