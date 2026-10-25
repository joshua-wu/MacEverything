#pragma once
// ═══════════════════════════════════════════════════════
//  Part 22: batchRescanPrefix Tests
// ═══════════════════════════════════════════════════════

static void runBatchRescanTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 22: batchRescanPrefix Tests\n";
    std::cout << "========================================\n\n";

    // ── Test 1: Correctness ──
    std::cout << "  --- Correctness ---\n";
    {
        SearchEngine engine;

        // Add 100 records under /a/b/ and 50 under /c/d/
        std::vector<FileRecord> initial;
        for (uint32_t i = 0; i < 100; i++) {
            initial.push_back({"ab_file_" + std::to_string(i) + ".txt", "/a/b", 1,
                                static_cast<uint64_t>(i * 10), static_cast<time_t>(1000 + i)});
        }
        for (uint32_t i = 0; i < 50; i++) {
            initial.push_back({"cd_file_" + std::to_string(i) + ".txt", "/c/d", 1,
                                static_cast<uint64_t>(i * 20), static_cast<time_t>(2000 + i)});
        }
        engine.loadRecords(std::move(initial));

        check(engine.liveRecordCount() == 150, "initial: liveRecordCount == 150");

        // Prepare 30 fresh replacement records for /a/b
        std::vector<FileRecord> fresh;
        for (uint32_t i = 0; i < 30; i++) {
            fresh.push_back({"new_ab_" + std::to_string(i) + ".dat", "/a/b", 1,
                              static_cast<uint64_t>(i * 100), static_cast<time_t>(5000 + i)});
        }

        uint32_t removedCount = engine.batchRescanPrefix("/a/b", std::move(fresh));

        check(removedCount == 100, "batchRescanPrefix removed 100 old records");
        check(engine.liveRecordCount() == 80, "liveRecordCount == 50 + 30 = 80");

        // Old /a/b/ records should be gone
        auto res = engine.query("ab_file_0");
        check(res.empty(), "old /a/b/ records gone: ab_file_0 not found");
        res = engine.query("ab_file_99");
        check(res.empty(), "old /a/b/ records gone: ab_file_99 not found");

        // New records should be findable
        res = engine.query("new_ab_0");
        check(res.size() == 1, "new record new_ab_0 found");
        res = engine.query("new_ab_29");
        check(res.size() == 1, "new record new_ab_29 found");

        // /c/d/ records should still exist
        res = engine.query("cd_file_0");
        check(res.size() == 1, "/c/d/ records intact: cd_file_0 found");
        res = engine.query("cd_file_49");
        check(res.size() == 1, "/c/d/ records intact: cd_file_49 found");
    }

    // ── Test 2: Trigram integrity ──
    std::cout << "\n  --- Trigram integrity ---\n";
    {
        SearchEngine engine;

        std::vector<FileRecord> initial;
        for (uint32_t i = 0; i < 50; i++) {
            initial.push_back({"alpha_item_" + std::to_string(i) + ".txt", "/prefix/dir", 1,
                                static_cast<uint64_t>(i), static_cast<time_t>(1000 + i)});
        }
        for (uint32_t i = 0; i < 30; i++) {
            initial.push_back({"bravo_thing_" + std::to_string(i) + ".log", "/other/path", 1,
                                static_cast<uint64_t>(i), static_cast<time_t>(2000 + i)});
        }
        engine.loadRecords(std::move(initial));

        // Replace /prefix/dir records with new ones having different trigrams
        std::vector<FileRecord> fresh;
        for (uint32_t i = 0; i < 20; i++) {
            fresh.push_back({"charlie_replaced_" + std::to_string(i) + ".bin", "/prefix/dir", 1,
                              static_cast<uint64_t>(i * 50), static_cast<time_t>(5000 + i)});
        }
        engine.batchRescanPrefix("/prefix/dir", std::move(fresh));

        // Trigram search for new records (keyword >= 3 chars)
        auto res = engine.query("charlie");
        check(res.size() == 20, "trigram search for 'charlie' finds 20 new records");

        // Old trigrams should not match anymore
        res = engine.query("alpha_item");
        check(res.empty(), "trigram search for old 'alpha_item' returns empty");

        // Unaffected records should still be searchable via trigrams
        res = engine.query("bravo_thing");
        check(res.size() == 30, "trigram search for unaffected 'bravo_thing' finds 30 records");

        // Verify with a different trigram-eligible search
        res = engine.query("replaced");
        check(res.size() == 20, "trigram search for 'replaced' finds 20 new records");
    }

    // ── Test 3: Performance comparison ──
    std::cout << "\n  --- Performance comparison ---\n";
    {
        constexpr uint32_t N = 50000;
        constexpr uint32_t REPLACE_COUNT = 1000;

        // Helper lambda to build N records under /perf/prefix
        auto buildRecords = [&]() {
            std::vector<FileRecord> recs;
            recs.reserve(N);
            for (uint32_t i = 0; i < N; i++) {
                recs.push_back({"perffile_" + std::to_string(i) + ".txt", "/perf/prefix", 1,
                                 static_cast<uint64_t>(i * 10), static_cast<time_t>(i)});
            }
            return recs;
        };

        auto buildFreshRecords = [&]() {
            std::vector<FileRecord> recs;
            recs.reserve(REPLACE_COUNT);
            for (uint32_t i = 0; i < REPLACE_COUNT; i++) {
                recs.push_back({"freshfile_" + std::to_string(i) + ".txt", "/perf/prefix", 1,
                                 static_cast<uint64_t>(i * 100), static_cast<time_t>(100000 + i)});
            }
            return recs;
        };

        // ── Old way: removeByPathPrefix + per-record updateByPath ──
        SearchEngine oldEngine;
        oldEngine.loadRecords(buildRecords());

        auto t0 = std::chrono::steady_clock::now();
        oldEngine.removeByPathPrefix("/perf/prefix");
        auto freshOld = buildFreshRecords();
        for (auto& r : freshOld) {
            std::string fp = SearchEngine::makeFullPath(r.path, r.name);
            oldEngine.updateByPath(fp, std::move(r));
        }
        auto t1 = std::chrono::steady_clock::now();
        double oldWayMs = std::chrono::duration<double>(t1 - t0).count() * 1000;

        // ── New way: batchRescanPrefix ──
        SearchEngine newEngine;
        newEngine.loadRecords(buildRecords());

        t0 = std::chrono::steady_clock::now();
        newEngine.batchRescanPrefix("/perf/prefix", buildFreshRecords());
        t1 = std::chrono::steady_clock::now();
        double newWayMs = std::chrono::duration<double>(t1 - t0).count() * 1000;

        double speedup = oldWayMs / newWayMs;
        std::cout << "    Old way (remove + updateByPath loop): " << std::fixed << std::setprecision(2) << oldWayMs << " ms\n";
        std::cout << "    New way (batchRescanPrefix):           " << std::fixed << std::setprecision(2) << newWayMs << " ms\n";
        std::cout << "    Speedup: " << std::setprecision(1) << speedup << "x\n";

        // With incremental trigram updates, the batch advantage is single lock
        // acquisition vs N lock/unlock cycles. Expect modest speedup.
        check(speedup >= 0.8, "batchRescanPrefix is not slower than old way");

        // Verify correctness of both results
        check(oldEngine.liveRecordCount() == REPLACE_COUNT, "old way: liveRecordCount correct");
        check(newEngine.liveRecordCount() == REPLACE_COUNT, "new way: liveRecordCount correct");
    }

    // ── Test 4: Edge cases ──
    std::cout << "\n  --- Edge cases ---\n";
    {
        // 4a: Empty prefix match (no records removed)
        SearchEngine engine;
        std::vector<FileRecord> initial;
        for (uint32_t i = 0; i < 10; i++) {
            initial.push_back({"file_" + std::to_string(i) + ".txt", "/exists", 1,
                                static_cast<uint64_t>(i), static_cast<time_t>(i)});
        }
        engine.loadRecords(std::move(initial));

        std::vector<FileRecord> fresh;
        fresh.push_back({"replacement.txt", "/nonexistent", 1, 100, 9999});
        uint32_t removed = engine.batchRescanPrefix("/nonexistent", std::move(fresh));
        check(removed == 0, "edge: no records match prefix -> removed == 0");
        check(engine.liveRecordCount() == 11, "edge: no match -> liveRecordCount == 10 + 1 = 11");

        // 4b: Zero fresh replacement records (pure deletion)
        SearchEngine engine2;
        std::vector<FileRecord> initial2;
        for (uint32_t i = 0; i < 20; i++) {
            initial2.push_back({"del_" + std::to_string(i) + ".txt", "/to_delete", 1,
                                 static_cast<uint64_t>(i), static_cast<time_t>(i)});
        }
        for (uint32_t i = 0; i < 5; i++) {
            initial2.push_back({"keep_" + std::to_string(i) + ".txt", "/keep", 1,
                                 static_cast<uint64_t>(i), static_cast<time_t>(i)});
        }
        engine2.loadRecords(std::move(initial2));

        std::vector<FileRecord> emptyFresh;
        removed = engine2.batchRescanPrefix("/to_delete", std::move(emptyFresh));
        check(removed == 20, "edge: pure deletion removed 20 records");
        check(engine2.liveRecordCount() == 5, "edge: pure deletion liveRecordCount == 5");
        auto res = engine2.query("del_0");
        check(res.empty(), "edge: deleted records not queryable");
        res = engine2.query("keep_0");
        check(res.size() == 1, "edge: kept records still queryable");

        // 4c: Prefix matching ALL records
        SearchEngine engine3;
        std::vector<FileRecord> initial3;
        for (uint32_t i = 0; i < 15; i++) {
            initial3.push_back({"all_" + std::to_string(i) + ".txt", "/all/gone", 1,
                                 static_cast<uint64_t>(i), static_cast<time_t>(i)});
        }
        engine3.loadRecords(std::move(initial3));

        std::vector<FileRecord> replacements;
        replacements.push_back({"survivor.txt", "/all/gone", 1, 42, 9999});
        removed = engine3.batchRescanPrefix("/all/gone", std::move(replacements));
        check(removed == 15, "edge: all-match prefix removed 15 records");
        check(engine3.liveRecordCount() == 1, "edge: all-match liveRecordCount == 1");
        res = engine3.query("survivor");
        check(res.size() == 1, "edge: replacement record queryable after full prefix removal");
    }

    // ── Test 5: Empty prefix must be a no-op (regression: index wipe) ──
    // An empty prefix matches every absolute path; tombstoning against it
    // would delete the ENTIRE index. Seen in production as a 99.99% tombstone
    // ratio when an FSEvents MustScanSubDirs event carried an empty path.
    std::cout << "\n  --- Empty prefix regression ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> initial;
        for (uint32_t i = 0; i < 100; i++) {
            initial.push_back({"keep_" + std::to_string(i) + ".txt", "/data/dir", 1,
                                static_cast<uint64_t>(i), static_cast<time_t>(i)});
        }
        engine.loadRecords(std::move(initial));

        std::vector<FileRecord> emptyFresh;
        uint32_t removed = engine.batchRescanPrefix("", std::move(emptyFresh));
        check(removed == 0, "regression: empty prefix removes nothing");
        check(engine.liveRecordCount() == 100, "regression: all 100 records survive empty-prefix rescan");
        auto res = engine.query("keep_50");
        check(res.size() == 1, "regression: records still queryable after empty-prefix rescan");

        // Must also refuse when the caller supplies fresh records
        std::vector<FileRecord> fresh;
        fresh.push_back({"newfile.txt", "/data/dir", 1, 1, 9999});
        removed = engine.batchRescanPrefix("", std::move(fresh));
        check(removed == 0, "regression: empty prefix with fresh records removes nothing");
        check(engine.liveRecordCount() == 100, "regression: rejected op leaves record count unchanged");
    }

    std::cout << "\n";
}
