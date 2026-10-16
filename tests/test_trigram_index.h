#pragma once
// Part 8: Trigram Index for Filename Search

static void runTrigramIndexTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 8: Trigram Index for Filename Search\n";
    std::cout << "========================================\n\n";

    // -- Test 1: Basic trigram-accelerated search --
    std::cout << "  --- Basic trigram search ---\n";
    {
        SearchEngine engine;
        engine.setTrigramIndexEnabled(true);
        std::vector<FileRecord> records;
        records.push_back({"document.pdf", "/home/user", 1, 100, 1000});
        records.push_back({"readme.md", "/home/user", 1, 200, 2000});
        records.push_back({"config.json", "/etc", 1, 300, 3000});
        records.push_back({"dockerfile", "/app", 1, 400, 4000});
        records.push_back({"doc_helper.py", "/src", 1, 500, 5000});
        engine.loadRecords(std::move(records));

        // "doc" has 1 trigram (d,o,c) — should use trigram index
        auto res = engine.query("doc");
        check(res.size() == 3, "Trigram: 'doc' matches document.pdf, dockerfile, doc_helper.py");

        // "document" has 6 trigrams — strong filtering
        res = engine.query("document");
        check(res.size() == 1, "Trigram: 'document' matches 1 file");
        check(engine.getRecord(res[0]).name == "document.pdf", "Trigram: 'document' matches document.pdf");

        // "readme" — unique match
        res = engine.query("readme");
        check(res.size() == 1, "Trigram: 'readme' matches 1 file");

        // "config" — unique match
        res = engine.query("config");
        check(res.size() == 1, "Trigram: 'config' matches 1 file");
    }

    // -- Test 2: Trigram index maintained through mutations --
    std::cout << "\n  --- Trigram index with mutations ---\n";
    {
        SearchEngine engine;
        engine.setTrigramIndexEnabled(true);
        std::vector<FileRecord> records;
        records.push_back({"alpha.txt", "/tmp", 1, 100, 1000});
        records.push_back({"beta.txt", "/tmp", 1, 200, 2000});
        engine.loadRecords(std::move(records));

        // Add a new record
        engine.addRecord({"alpha_v2.txt", "/tmp", 1, 300, 3000});
        auto res = engine.query("alpha");
        check(res.size() == 2, "Trigram add: 'alpha' matches 2 after addRecord");

        // Remove original alpha
        engine.removeByPath("/tmp/alpha.txt");
        res = engine.query("alpha");
        check(res.size() == 1, "Trigram remove: 'alpha' matches 1 after removeByPath");
        check(engine.getRecord(res[0]).name == "alpha_v2.txt", "Trigram remove: correct file remains");

        // Update beta to gamma
        engine.updateByPath("/tmp/beta.txt", {"gamma.txt", "/tmp", 1, 200, 2000});
        res = engine.query("beta");
        check(res.empty(), "Trigram update: 'beta' no longer matches after update");
        res = engine.query("gamma");
        check(res.size() == 1, "Trigram update: 'gamma' matches after update");
    }

    // -- Test 3: Trigram index survives compaction --
    std::cout << "\n  --- Trigram index after compaction ---\n";
    {
        SearchEngine engine;
        engine.setTrigramIndexEnabled(true);
        std::vector<FileRecord> records;
        for (int i = 0; i < 20; i++) {
            records.push_back({"trigram_test_" + std::to_string(i) + ".txt", "/data", 1, (uint64_t)i, (time_t)i});
        }
        engine.loadRecords(std::move(records));

        // Remove half
        for (int i = 0; i < 10; i++) {
            engine.removeByPath("/data/trigram_test_" + std::to_string(i) + ".txt");
        }

        // Compact
        engine.compactRecords();
        check(engine.liveRecordCount() == 10, "Trigram compact: 10 live records after compaction");

        // Query should still work via trigram index
        auto res = engine.query("trigram_test_15");
        check(res.size() == 1, "Trigram compact: specific file still findable after compaction");

        auto res2 = engine.query("trigram_test_5");
        check(res2.empty(), "Trigram compact: removed file not findable after compaction");
    }

    // -- Test 4: Short keywords fallback to linear scan --
    std::cout << "\n  --- Short keyword fallback ---\n";
    {
        SearchEngine engine;
        engine.setTrigramIndexEnabled(true);
        std::vector<FileRecord> records;
        records.push_back({"ab.txt", "/tmp", 1, 100, 1000});
        records.push_back({"abc.txt", "/tmp", 1, 200, 2000});
        records.push_back({"abcd.txt", "/tmp", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // 2-char keyword: must fallback to linear scan
        auto res = engine.query("ab");
        check(res.size() == 3, "Short keyword 'ab': all 3 files match (linear scan fallback)");

        // Single char
        res = engine.query("a");
        check(res.size() == 3, "Single char 'a': all 3 files match");
    }

    // -- Test 5: Glob patterns still return correct results --
    std::cout << "\n  --- Glob pattern matching ---\n";
    {
        SearchEngine engine;
        engine.setTrigramIndexEnabled(true);
        std::vector<FileRecord> records;
        records.push_back({"hello.cpp", "/src", 1, 100, 1000});
        records.push_back({"hello.h", "/src", 1, 50, 2000});
        records.push_back({"world.cpp", "/src", 1, 200, 3000});
        engine.loadRecords(std::move(records));

        auto res = engine.query("*.cpp");
        check(res.size() == 2, "Glob '*.cpp': 2 matches");

        res = engine.query("hello.*");
        check(res.size() == 2, "Glob 'hello.*': 2 matches");

        // Glob with '?' wildcard
        res = engine.query("hello.?");
        check(res.size() == 1, "Glob 'hello.?': 1 match (hello.h)");

        // Glob with no match
        res = engine.query("*.xyz");
        check(res.empty(), "Glob '*.xyz': 0 matches");
    }

    // -- Test 6: No-match keyword returns empty --
    std::cout << "\n  --- No-match via trigram ---\n";
    {
        SearchEngine engine;
        engine.setTrigramIndexEnabled(true);
        std::vector<FileRecord> records;
        records.push_back({"myfile.txt", "/tmp", 1, 100, 1000});
        engine.loadRecords(std::move(records));

        auto res = engine.query("xyz_nowhere");
        check(res.empty(), "No-match 'xyz_nowhere': trigram index returns empty");
    }

    // -- Test 7: Trigram search performance comparison --
    std::cout << "\n  --- Trigram search performance ---\n";
    {
        SearchEngine engine;
        engine.setTrigramIndexEnabled(true);
        std::vector<FileRecord> records;
        // Build 100k records with varied names
        for (uint32_t i = 0; i < 100000; i++) {
            std::string name = "file_" + std::to_string(i) + "_data.txt";
            records.push_back({name, "/bench/dir" + std::to_string(i % 100), 1, i * 10, (time_t)i});
        }
        engine.loadRecords(std::move(records));

        // Search for a very specific string with maxResults=100 (typical UI usage)
        auto t0 = std::chrono::steady_clock::now();
        for (int run = 0; run < 100; run++) {
            auto res = engine.query("file_99999_data", 100);
            (void)res;
        }
        auto t1 = std::chrono::steady_clock::now();
        double trigramTime = std::chrono::duration<double>(t1 - t0).count() * 1000;

        // Same search but with a 2-char keyword (linear scan fallback)
        t0 = std::chrono::steady_clock::now();
        for (int run = 0; run < 100; run++) {
            auto res = engine.query("fi", 100);  // 2 chars, linear scan
            (void)res;
        }
        t1 = std::chrono::steady_clock::now();
        double linearTime = std::chrono::duration<double>(t1 - t0).count() * 1000;

        // Also test trigram search without maxResults limit (worst case)
        t0 = std::chrono::steady_clock::now();
        for (int run = 0; run < 100; run++) {
            auto res = engine.query("file_99999_data");
            (void)res;
        }
        t1 = std::chrono::steady_clock::now();
        double trigramUnlimitedTime = std::chrono::duration<double>(t1 - t0).count() * 1000;

        std::cout << "    100x trigram search (maxResults=100): " << std::fixed << std::setprecision(2)
                  << trigramTime << "ms (" << trigramTime / 100 << "ms/query)\n";
        std::cout << "    100x linear search (short key, maxResults=100): " << std::fixed << std::setprecision(2)
                  << linearTime << "ms (" << linearTime / 100 << "ms/query)\n";
        std::cout << "    100x trigram search (unlimited): " << std::fixed << std::setprecision(2)
                  << trigramUnlimitedTime << "ms (" << trigramUnlimitedTime / 100 << "ms/query)\n";

        if (trigramTime < linearTime) {
            double speedup = linearTime / trigramTime;
            std::cout << "    Trigram speedup (limited): " << std::fixed << std::setprecision(1) << speedup << "x\n";
        }
        check(trigramTime > 0 && linearTime > 0, "Trigram performance benchmark produced timing data");

        // partial_sort benchmark: compare maxResults=100 vs unlimited (full sort)
        // With partial_sort, limited queries should be faster on large result sets
        t0 = std::chrono::steady_clock::now();
        for (int run = 0; run < 100; run++) {
            auto res = engine.query("fi", 100);  // many matches, partial_sort
            (void)res;
        }
        t1 = std::chrono::steady_clock::now();
        double partialSortTime = std::chrono::duration<double>(t1 - t0).count() * 1000;

        t0 = std::chrono::steady_clock::now();
        for (int run = 0; run < 100; run++) {
            auto res = engine.query("fi");  // many matches, full sort
            (void)res;
        }
        t1 = std::chrono::steady_clock::now();
        double fullSortTime = std::chrono::duration<double>(t1 - t0).count() * 1000;

        std::cout << "    100x partial_sort (maxResults=100, many matches): " << std::fixed << std::setprecision(2)
                  << partialSortTime << "ms (" << partialSortTime / 100 << "ms/query)\n";
        std::cout << "    100x full_sort (unlimited, many matches): " << std::fixed << std::setprecision(2)
                  << fullSortTime << "ms (" << fullSortTime / 100 << "ms/query)\n";
        if (partialSortTime < fullSortTime) {
            double speedup = fullSortTime / partialSortTime;
            std::cout << "    partial_sort speedup: " << std::fixed << std::setprecision(1) << speedup << "x\n";
        }
        check(partialSortTime > 0 && fullSortTime > 0, "partial_sort benchmark produced timing data");
    }

    // -- Test 8: Path-only match still works with trigram --
    std::cout << "\n  --- Path-only match with trigram ---\n";
    {
        SearchEngine engine;
        engine.setTrigramIndexEnabled(true);
        std::vector<FileRecord> records;
        records.push_back({"main.cpp", "/usr/local/src", 1, 100, 1000});
        records.push_back({"readme.md", "/usr/local/docs", 1, 200, 2000});
        records.push_back({"local.conf", "/etc", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // "local" matches local.conf by name, and both others by path
        auto res = engine.query("local");
        check(res.size() == 3, "Path+Trigram: 'local' matches 3 (1 name + 2 path)");

        // Name match should come before path matches
        check(engine.getRecord(res[0]).name == "local.conf",
              "Path+Trigram: name match 'local.conf' ranked first");
    }

    // -- Test 9: removeByPathPrefix with trigram cleanup --
    std::cout << "\n  --- removeByPathPrefix with trigram ---\n";
    {
        SearchEngine engine;
        engine.setTrigramIndexEnabled(true);
        std::vector<FileRecord> records;
        records.push_back({"aaa.txt", "/prefix/sub", 1, 100, 1000});
        records.push_back({"bbb.txt", "/prefix/sub", 1, 200, 2000});
        records.push_back({"ccc.txt", "/other", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        engine.removeByPathPrefix("/prefix");
        auto res = engine.query("aaa");
        check(res.empty(), "removeByPathPrefix: 'aaa' no longer findable via trigram");
        res = engine.query("ccc");
        check(res.size() == 1, "removeByPathPrefix: 'ccc' still findable via trigram");
    }

    // -- Test 10: removeByPathPrefix benchmark (single-pass vs old double-lookup) --
    std::cout << "\n  --- removeByPathPrefix benchmark ---\n";
    {
        // Build a large dataset: 50K records under 10 different prefixes
        const int recordsPerPrefix = 5000;
        const int numPrefixes = 10;
        const int totalRecords = recordsPerPrefix * numPrefixes;

        auto buildRecords = [&]() {
            std::vector<FileRecord> records;
            records.reserve(totalRecords);
            for (int p = 0; p < numPrefixes; p++) {
                std::string prefix = "/volume/prefix" + std::to_string(p);
                for (int i = 0; i < recordsPerPrefix; i++) {
                    std::string name = "file_" + std::to_string(p) + "_" + std::to_string(i) + ".txt";
                    records.push_back({name, prefix + "/sub/deep", 1, static_cast<uint64_t>(i * 100), static_cast<time_t>(1000 + i)});
                }
            }
            return records;
        };

        // Benchmark: remove one prefix (5000 records out of 50000)
        SearchEngine engine;
        engine.setTrigramIndexEnabled(true);
        engine.loadRecords(buildRecords());
        auto t0 = std::chrono::steady_clock::now();
        uint32_t removed = engine.removeByPathPrefix("/volume/prefix0");
        auto t1 = std::chrono::steady_clock::now();
        double removeTime = std::chrono::duration<double>(t1 - t0).count() * 1000;

        check(removed == recordsPerPrefix, "removeByPathPrefix: removed correct count");
        check(engine.liveRecordCount() == totalRecords - recordsPerPrefix,
              "removeByPathPrefix: live count correct after removal");

        // Verify removed records are not queryable
        auto res = engine.query("file_0_0");
        check(res.empty(), "removeByPathPrefix: removed file not queryable");
        // Verify non-removed records still queryable
        res = engine.query("file_1_0");
        check(res.size() >= 1, "removeByPathPrefix: non-removed file still queryable");

        std::cout << "    Remove " << removed << " / " << totalRecords << " records: "
                  << std::fixed << std::setprecision(2) << removeTime << "ms\n";
        check(removed > 0, "removeByPathPrefix benchmark: records were removed");
    }

    // -- Test 11: Trigram candidate threshold fallback to linear scan --
    std::cout << "\n  --- Trigram candidate threshold fallback ---\n";
    {
        // Create a dataset where a common keyword produces candidates > 25% of total
        // Total = 10000 records, threshold = 10000/4 = 2500
        // "test" appears in 3000 filenames (30% > 25%) → should fallback to linear
        // "unique_xyz" appears in 5 filenames (0.05% < 25%) → should stay trigram
        const uint32_t totalRecords = 10000;
        const uint32_t commonCount = 3000; // 30% of total

        SearchEngine engine;

        engine.setTrigramIndexEnabled(true);
        std::vector<FileRecord> records;
        records.reserve(totalRecords);

        // First commonCount records contain "test" in the name
        for (uint32_t i = 0; i < commonCount; i++) {
            records.push_back({"test_file_" + std::to_string(i) + ".txt",
                              "/data/dir" + std::to_string(i % 50), 1, (uint64_t)i, (time_t)i});
        }
        // A few records with a rare keyword
        for (uint32_t i = 0; i < 5; i++) {
            records.push_back({"unique_xyz_" + std::to_string(i) + ".txt",
                              "/data/rare", 1, (uint64_t)(commonCount + i), (time_t)(commonCount + i)});
        }
        // Fill remaining with unrelated names
        for (uint32_t i = commonCount + 5; i < totalRecords; i++) {
            records.push_back({"item_" + std::to_string(i) + ".dat",
                              "/data/bulk" + std::to_string(i % 100), 1, (uint64_t)i, (time_t)i});
        }
        engine.loadRecords(std::move(records));

        // Common keyword "test" should exceed threshold and fallback to linear
        QueryTimingInfo timing1;
        auto res1 = engine.query("test", 0, true, timing1);
        check(res1.size() == commonCount,
              "Threshold fallback: 'test' finds all matches");
        check(timing1.searchPath == "advanced-linear-gcd",
              "Threshold fallback: 'test' uses linear scan");

        // Rare keyword "unique_xyz" should stay on trigram path
        QueryTimingInfo timing2;
        auto res2 = engine.query("unique_xyz", 0, true, timing2);
        check(res2.size() == 5, "Threshold fallback: 'unique_xyz' finds 5 matches");
        check(timing2.searchPath != "advanced-linear-gcd",
              "Threshold fallback: 'unique_xyz' stays on trigram");

        std::cout << "    'test': " << res1.size() << " results, path=" << timing1.searchPath
                  << ", candidates=" << timing1.candidates << "\n";
        std::cout << "    'unique_xyz': " << res2.size() << " results, path=" << timing2.searchPath
                  << ", candidates=" << timing2.candidates << "\n";
    }

    // -- Test 12: Compiled glob + trigram pre-filtering --
    std::cout << "\n  --- Compiled glob + trigram pre-filtering ---\n";
    {
        // Build a dataset large enough that trigram pre-filter is meaningful
        const uint32_t totalRecords = 10000;
        SearchEngine engine;
        engine.setTrigramIndexEnabled(true);
        std::vector<FileRecord> records;
        records.reserve(totalRecords);

        // 50 .cpp files
        for (uint32_t i = 0; i < 50; i++) {
            records.push_back({"source_" + std::to_string(i) + ".cpp",
                              "/project/src", 1, (uint64_t)i, (time_t)i});
        }
        // 30 .hpp files
        for (uint32_t i = 0; i < 30; i++) {
            records.push_back({"header_" + std::to_string(i) + ".hpp",
                              "/project/include", 1, (uint64_t)(50 + i), (time_t)(50 + i)});
        }
        // Fill rest with .txt
        for (uint32_t i = 80; i < totalRecords; i++) {
            records.push_back({"data_" + std::to_string(i) + ".txt",
                              "/data/dir" + std::to_string(i % 100), 1, (uint64_t)i, (time_t)i});
        }
        engine.loadRecords(std::move(records));

        // *.cpp — SUFFIX pattern, fixed=".cpp" has 4 chars >= 3 → trigram pre-filter
        QueryTimingInfo timing1;
        auto res1 = engine.query("*.cpp", 0, true, timing1);
        check(res1.size() == 50, "Glob trigram: '*.cpp' finds 50 .cpp files");
        // With 10000 records, .cpp candidates should be small enough for trigram
        std::cout << "    '*.cpp': " << res1.size() << " results, path=" << timing1.searchPath
                  << ", candidates=" << timing1.candidates << "\n";

        // *.hpp — SUFFIX pattern, fixed=".hpp" >= 3
        QueryTimingInfo timing2;
        auto res2 = engine.query("*.hpp", 0, true, timing2);
        check(res2.size() == 30, "Glob trigram: '*.hpp' finds 30 .hpp files");
        std::cout << "    '*.hpp': " << res2.size() << " results, path=" << timing2.searchPath
                  << ", candidates=" << timing2.candidates << "\n";

        // *.h — SUFFIX pattern, fixed=".h" only 2 chars < 3 → linear fallback
        QueryTimingInfo timing3;
        auto res3 = engine.query("*.h", 0, true, timing3);
        // .h matches .hpp files too via glob
        check(timing3.searchPath == "advanced-linear-gcd",
              "Glob linear: '*.h' falls back to linear (fixed part too short for trigram)");
        std::cout << "    '*.h': " << res3.size() << " results, path=" << timing3.searchPath << "\n";

        // source_* — PREFIX pattern, fixed="source_" >= 3 → trigram pre-filter
        QueryTimingInfo timing4;
        auto res4 = engine.query("source_*", 0, true, timing4);
        check(res4.size() == 50, "Glob trigram: 'source_*' finds 50 files");
        std::cout << "    'source_*': " << res4.size() << " results, path=" << timing4.searchPath
                  << ", candidates=" << timing4.candidates << "\n";

        // *data* — CONTAINS pattern, fixed="data" >= 3
        QueryTimingInfo timing5;
        auto res5 = engine.query("*data*", 0, true, timing5);
        // data_ prefix in 9920 .txt files — candidates will exceed threshold → linear
        std::cout << "    '*data*': " << res5.size() << " results, path=" << timing5.searchPath
                  << ", candidates=" << timing5.candidates << "\n";

        // Performance: glob with trigram should be much faster than without
        auto t0 = std::chrono::steady_clock::now();
        for (int run = 0; run < 100; run++) {
            auto res = engine.query("*.cpp", 100, true);
            (void)res;
        }
        auto t1 = std::chrono::steady_clock::now();
        double trigramGlobTime = std::chrono::duration<double>(t1 - t0).count() * 1000;

        t0 = std::chrono::steady_clock::now();
        for (int run = 0; run < 100; run++) {
            auto res = engine.query("*.cpp", 100, false);  // force no-trigram
            (void)res;
        }
        t1 = std::chrono::steady_clock::now();
        double linearGlobTime = std::chrono::duration<double>(t1 - t0).count() * 1000;

        std::cout << "    100x '*.cpp' (trigram glob): " << std::fixed << std::setprecision(2)
                  << trigramGlobTime << "ms (" << trigramGlobTime / 100 << "ms/query)\n";
        std::cout << "    100x '*.cpp' (linear glob):  " << std::fixed << std::setprecision(2)
                  << linearGlobTime << "ms (" << linearGlobTime / 100 << "ms/query)\n";
        if (trigramGlobTime < linearGlobTime) {
            std::cout << "    Glob trigram speedup: " << std::fixed << std::setprecision(1)
                      << linearGlobTime / trigramGlobTime << "x\n";
        }
        check(trigramGlobTime > 0 && linearGlobTime > 0, "Compiled glob + trigram benchmark produced timing data");
    }

    // -- Test 13: Multi-segment glob + trigram pre-filtering --
    std::cout << "\n  --- Multi-segment glob + trigram pre-filtering ---\n";
    {
        const uint32_t totalRecords = 10000;
        SearchEngine engine;
        engine.setTrigramIndexEnabled(true);
        std::vector<FileRecord> records;
        records.reserve(totalRecords);

        // 50 .cpp source files
        for (uint32_t i = 0; i < 50; i++) {
            records.push_back({"source_" + std::to_string(i) + ".cpp",
                              "/project/src", 1, (uint64_t)i, (time_t)i});
        }
        // 20 test .cpp files
        for (uint32_t i = 0; i < 20; i++) {
            records.push_back({"test_source_" + std::to_string(i) + ".cpp",
                              "/project/tests", 1, (uint64_t)(50 + i), (time_t)(50 + i)});
        }
        // 10 .cpp.bak files
        for (uint32_t i = 0; i < 10; i++) {
            records.push_back({"source_" + std::to_string(i) + ".cpp.bak",
                              "/project/backup", 1, (uint64_t)(70 + i), (time_t)(70 + i)});
        }
        // 30 .hpp files
        for (uint32_t i = 0; i < 30; i++) {
            records.push_back({"header_" + std::to_string(i) + ".hpp",
                              "/project/include", 1, (uint64_t)(80 + i), (time_t)(80 + i)});
        }
        // 15 files with .ext. pattern
        for (uint32_t i = 0; i < 15; i++) {
            records.push_back({"file_" + std::to_string(i) + ".ext.tmp",
                              "/project/temp", 1, (uint64_t)(110 + i), (time_t)(110 + i)});
        }
        // Fill rest with .txt
        for (uint32_t i = 125; i < totalRecords; i++) {
            records.push_back({"data_" + std::to_string(i) + ".txt",
                              "/data/dir" + std::to_string(i % 100), 1, (uint64_t)i, (time_t)i});
        }
        engine.loadRecords(std::move(records));

        // *source*.cpp — GENERIC, segments=["source",".cpp"], multi-segment trigram
        {
            QueryTimingInfo timing;
            auto res = engine.query("*source*.cpp", 0, true, timing);
            check(res.size() == 70, "Multi-seg glob: '*source*.cpp' finds 70 files (50 src + 20 test)");
            check(timing.searchPath == "advanced-trigram",
                  "Multi-seg glob: '*source*.cpp' uses advanced-trigram path");
            std::cout << "    '*source*.cpp': " << res.size() << " results, path=" << timing.searchPath
                      << ", candidates=" << timing.candidates << "\n";
        }

        // *.cpp.* — GENERIC, segments=[".cpp."], trigram on ".cpp."
        {
            QueryTimingInfo timing;
            auto res = engine.query("*.cpp.*", 0, true, timing);
            check(res.size() == 10, "Multi-seg glob: '*.cpp.*' finds 10 .cpp.bak files");
            check(timing.searchPath == "advanced-trigram",
                  "Multi-seg glob: '*.cpp.*' uses advanced-trigram path");
            std::cout << "    '*.cpp.*': " << res.size() << " results, path=" << timing.searchPath
                      << ", candidates=" << timing.candidates << "\n";
        }

        // test_*.cpp — GENERIC, segments=["test_",".cpp"], multi-segment trigram
        {
            QueryTimingInfo timing;
            auto res = engine.query("test_*.cpp", 0, true, timing);
            check(res.size() == 20, "Multi-seg glob: 'test_*.cpp' finds 20 test files");
            check(timing.searchPath == "advanced-trigram",
                  "Multi-seg glob: 'test_*.cpp' uses advanced-trigram path");
            std::cout << "    'test_*.cpp': " << res.size() << " results, path=" << timing.searchPath
                      << ", candidates=" << timing.candidates << "\n";
        }

        // *.ext.* — GENERIC, segments=[".ext."], trigram pre-filter
        {
            QueryTimingInfo timing;
            auto res = engine.query("*.ext.*", 0, true, timing);
            check(res.size() == 15, "Multi-seg glob: '*.ext.*' finds 15 .ext.tmp files");
            check(timing.searchPath == "advanced-trigram",
                  "Multi-seg glob: '*.ext.*' uses advanced-trigram path");
            std::cout << "    '*.ext.*': " << res.size() << " results, path=" << timing.searchPath
                      << ", candidates=" << timing.candidates << "\n";
        }

        // ?ource*.cpp — GENERIC, segments=["ource",".cpp"]
        {
            QueryTimingInfo timing;
            auto res = engine.query("?ource*.cpp", 0, true, timing);
            // ?ource matches "source" (? = 's'), so finds source_*.cpp = 50 files
            check(res.size() == 50, "Multi-seg glob: '?ource*.cpp' finds 50 source files");
            check(timing.searchPath == "advanced-trigram",
                  "Multi-seg glob: '?ource*.cpp' uses advanced-trigram path");
            std::cout << "    '?ource*.cpp': " << res.size() << " results, path=" << timing.searchPath
                      << ", candidates=" << timing.candidates << "\n";
        }

        // *a*b* — all segments < 3 chars → linear fallback
        {
            QueryTimingInfo timing;
            auto res = engine.query("*a*b*", 0, true, timing);
            check(timing.searchPath == "advanced-linear-gcd",
                  "Multi-seg glob: '*a*b*' falls back to linear (all segments < 3)");
            std::cout << "    '*a*b*': " << res.size() << " results, path=" << timing.searchPath << "\n";
        }

        // Correctness: trigram vs linear must produce identical results
        {
            QueryTimingInfo t1, t2;
            auto res1 = engine.query("*source*.cpp", 0, true, t1);
            auto res2 = engine.query("*source*.cpp", 0, false, t2);
            // Sort by idx for comparison
            auto sortByIdx = [](std::vector<uint32_t>& v) { std::sort(v.begin(), v.end()); };
            sortByIdx(res1);
            sortByIdx(res2);
            check(res1 == res2,
                  "Multi-seg glob: trigram vs linear produce identical results for '*source*.cpp'");
            std::cout << "    Correctness: trigram(" << res1.size() << ") == linear("
                      << res2.size() << ")\n";
        }

        // Performance: multi-segment trigram should be faster than linear for selective patterns
        {
            auto t0 = std::chrono::steady_clock::now();
            for (int run = 0; run < 100; run++) {
                auto res = engine.query("*source*.cpp", 100, true);
                (void)res;
            }
            auto t1 = std::chrono::steady_clock::now();
            double trigramTime = std::chrono::duration<double>(t1 - t0).count() * 1000;

            t0 = std::chrono::steady_clock::now();
            for (int run = 0; run < 100; run++) {
                auto res = engine.query("*source*.cpp", 100, false);
                (void)res;
            }
            t1 = std::chrono::steady_clock::now();
            double linearTime = std::chrono::duration<double>(t1 - t0).count() * 1000;

            std::cout << "    100x '*source*.cpp' (trigram): " << std::fixed << std::setprecision(2)
                      << trigramTime << "ms (" << trigramTime / 100 << "ms/query)\n";
            std::cout << "    100x '*source*.cpp' (linear):  " << std::fixed << std::setprecision(2)
                      << linearTime << "ms (" << linearTime / 100 << "ms/query)\n";
            if (trigramTime < linearTime) {
                std::cout << "    Multi-seg trigram speedup: " << std::fixed << std::setprecision(1)
                          << linearTime / trigramTime << "x\n";
            }
            check(trigramTime > 0 && linearTime > 0, "Multi-segment glob + trigram benchmark produced timing data");
        }
    }

    std::cout << "\n";
}
