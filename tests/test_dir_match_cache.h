#pragma once
// ═══════════════════════════════════════════════════════
//  Part 3g: Per-directory match cache equivalence
// ═══════════════════════════════════════════════════════
#include <random>

// Original allocation-based implementation, kept as the reference.
static bool referencePathSegmentsMatch(std::string_view dirPath,
                                       const std::vector<PathSegment>& segments) {
    if (segments.empty()) return true;
    std::vector<std::string_view> components;
    size_t start = 0;
    for (size_t i = 0; i <= dirPath.size(); i++) {
        if (i == dirPath.size() || dirPath[i] == '/') {
            if (i > start) components.push_back(dirPath.substr(start, i - start));
            start = i + 1;
        }
    }
    int segIdx = static_cast<int>(segments.size()) - 1;
    int compIdx = static_cast<int>(components.size()) - 1;
    while (segIdx >= 0 && compIdx >= 0) {
        if (components[compIdx].find(segments[segIdx].text) != std::string_view::npos) {
            segIdx--;
            compIdx--;
        } else {
            if (segIdx < static_cast<int>(segments.size()) - 1 &&
                segments[segIdx + 1].adjacentToNext == false) {
                compIdx--;
            } else if (segIdx == static_cast<int>(segments.size()) - 1) {
                compIdx--;
            } else {
                return false;
            }
        }
    }
    return segIdx < 0;
}

static void runDirMatchCacheTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 3g: Per-directory Match Cache\n";
    std::cout << "========================================\n\n";

    std::mt19937 rng(12345);
    const char* words[] = {"alpha", "beta", "gamma", "Delta", "src", "docs", "report", "node_modules", "微信"};
    auto pick = [&] { return std::string(words[rng() % 9]); };

    // --- pathSegmentsMatchFrom(last) is identical to the reference ---
    {
        bool same = true;
        for (int i = 0; i < 20000 && same; i++) {
            std::string path;
            int depth = rng() % 6;
            for (int d = 0; d < depth; d++) path += "/" + pick() + (rng() % 3 ? "" : std::to_string(rng() % 3));
            std::vector<PathSegment> segs;
            int n = 1 + rng() % 3;
            for (int k = 0; k < n; k++) segs.push_back({me::toLower(pick()).substr(0, 1 + rng() % 4), (rng() % 2) == 0});
            std::string lower = me::toLower(path);
            same = SearchEngine::pathSegmentsMatch(lower, segs) == referencePathSegmentsMatch(lower, segs);
            // Splitting off the last component must reproduce the full-path result.
            if (same && !lower.empty()) {
                size_t slash = lower.rfind('/');
                std::string_view dir(lower.data(), slash);
                std::string_view name(lower.data() + slash + 1, lower.size() - slash - 1);
                if (!name.empty()) {
                    int last = static_cast<int>(segs.size()) - 1;
                    bool nameHit = name.find(segs.back().text) != std::string_view::npos;
                    bool split = nameHit ? SearchEngine::pathSegmentsMatchFrom(dir, segs, last - 1)
                                         : SearchEngine::pathSegmentsMatchFrom(dir, segs, last);
                    same = split == referencePathSegmentsMatch(lower, segs);
                }
            }
        }
        check(same, "pathSegmentsMatchFrom: matches reference on 20000 random paths");
    }

    // --- Query results with and without the cache are identical ---
    SearchEngine engine;
    std::vector<FileRecord> recs;
    for (int d = 0; d < 400; d++) {
        std::string dir = "/data/" + pick() + std::to_string(d % 7) + "/" + pick() + "_" + std::to_string(d);
        for (int f = 0; f < 60; f++) {
            std::string name = pick() + "-" + std::to_string(f) + (f % 3 ? ".txt" : ".PDF");
            recs.push_back({name, dir, 1, static_cast<uint64_t>(f * 100), 1000 + f});
        }
        recs.push_back({"sub" + std::to_string(d), dir, 2, 0, 1000});
    }
    engine.loadRecords(std::move(recs));
    check(engine.liveRecordCount() > 20000, "DirCache: test corpus above threshold");

    const char* queries[] = {
        "alpha", "ALPHA", "eta", "微信", "src report", "alpha | gamma", "!docs alpha",
        "case:Delta", "case:delta", "path:gamma", "nopath:gamma beta", "path:_12",
        "alpha/report", "gamma/ beta", "/data/src", "docs/*.txt", "beta/gamma/report",
        "regex:alpha_[0-9]+", "ww:beta", "node_modules", "_3", "pdf",
    };
    for (const char* q : queries) {
        engine.setDirCacheMinRecords(SIZE_MAX);
        auto plain = engine.query(q, 0);
        engine.setDirCacheMinRecords(0);
        auto cached = engine.query(q, 0);
        std::sort(plain.begin(), plain.end());
        std::sort(cached.begin(), cached.end());
        std::string label = std::string("DirCache: '") + q + "' same results (" + std::to_string(plain.size()) + ")";
        check(plain == cached, label.c_str());
    }
    engine.setDirCacheMinRecords(SearchEngine::kDirCacheMinRecords);
}
