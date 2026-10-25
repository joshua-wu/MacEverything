#pragma once
// Part 48: Slash Query — Node-Centric (Structured Query)
// Tests that slash queries use node-centric semantics:
//   /abc/def → name contains "def", parent dir contains "abc"

static void runSlashQueryTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 48: Slash Query (Node-Centric)\n";
    std::cout << "========================================\n\n";

    // -- Test 1: Basic slash query — name + path constraint --
    std::cout << "  --- Test 1: Basic slash query ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"file.txt", "/path/to/dir", 1, 100, 1000});
        records.push_back({"other.txt", "/path/to/dir", 1, 200, 2000});
        records.push_back({"file.txt", "/unrelated/place", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // "dir/file" → name contains "file", path must contain "dir"
        auto res = engine.query("dir/file");
        check(res.size() == 1, "Slash query: 'dir/file' matches 1 result (name='file', path has 'dir')");
        auto rec = engine.getRecord(res[0]);
        check(std::string(rec.name) == "file.txt", "Slash query: matched file is file.txt");
    }

    // -- Test 2: Name must match — path-only keywords don't match node name --
    std::cout << "\n  --- Test 2: Name must match node name ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"gcc", "/usr/local/bin", 1, 100, 1000});
        records.push_back({"lib.a", "/usr/local/lib", 1, 200, 2000});
        records.push_back({"readme.txt", "/usr/share/doc", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // "local/gcc" → name contains "gcc", path contains "local"
        auto res = engine.query("local/gcc");
        check(res.size() == 1, "Slash query: 'local/gcc' matches gcc in /usr/local/bin");

        // "local/lib" → name contains "lib", path contains "local"
        // lib.a → name "lib.a" contains "lib" ✓, path "/usr/local/lib" contains "local" ✓
        res = engine.query("local/lib");
        check(res.size() == 1, "Slash query: 'local/lib' matches lib.a");
    }

    // -- Test 3: Long name slash query --
    std::cout << "\n  --- Test 3: Long name slash query ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"test_query_perf_10m.h", "/project/tests", 1, 100, 1000});
        records.push_back({"test_query_basic.h", "/project/tests", 1, 200, 2000});
        records.push_back({"test_query_perf_10m.h", "/other/tests", 1, 300, 3000});
        records.push_back({"unrelated.cpp", "/project/src", 1, 400, 4000});
        engine.loadRecords(std::move(records));

        // "tests/test_query_perf" → name contains "test_query_perf", path contains "tests"
        auto res = engine.query("tests/test_query_perf");
        check(res.size() == 2, "Slash query: 'tests/test_query_perf' matches 2 results");
    }

    // -- Test 4: No match --
    std::cout << "\n  --- Test 4: Slash query no match ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"file.txt", "/existing/path", 1, 100, 1000});
        engine.loadRecords(std::move(records));

        // "nonexist/dir" → name contains "dir", path contains "nonexist" → 0
        auto res = engine.query("nonexist/dir");
        check(res.size() == 0, "Slash query: 'nonexist/dir' returns empty");
    }

    // -- Test 5: Short parts fallback to linear scan --
    std::cout << "\n  --- Test 5: Short parts fallback ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"b.c", "/a", 1, 100, 1000});
        records.push_back({"d.c", "/a", 1, 200, 2000});
        records.push_back({"b.c", "/x", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // "a/b" → name contains "b", path contains "a"
        // b.c in /a → name "b.c" contains "b" ✓, path "/a" contains "a" ✓
        // b.c in /x → path "/x" doesn't contain "a" ✗
        auto res = engine.query("a/b");
        check(res.size() == 1, "Slash query: 'a/b' with short parts still finds correct result");
        auto rec = engine.getRecord(res[0]);
        check(std::string(rec.name) == "b.c", "Slash query: short parts matched b.c in /a");
    }

    // -- Test 6: No double-counting with path constraint --
    std::cout << "\n  --- Test 6: No double-counting ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"tests_helper.cpp", "/project/src", 1, 100, 1000});
        records.push_back({"tests_helper.cpp", "/project/lib", 1, 200, 2000});
        records.push_back({"main.cpp", "/project/src", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // "src/tests" → name contains "tests", path contains "src"
        auto res = engine.query("src/tests");
        check(res.size() == 1, "Slash query: 'src/tests' matches only the file under /project/src");
        auto rec = engine.getRecord(res[0]);
        check(std::string(rec.name) == "tests_helper.cpp", "Slash query: correct file matched");
    }

    // -- Test 7: Absolute path single segment --
    std::cout << "\n  --- Test 7: Absolute path single segment ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"brew", "/usr/local/bin", 1, 100, 1000});
        records.push_back({"lib.a", "/usr/local/lib", 1, 200, 2000});
        records.push_back({"readme.txt", "/usr/share/doc", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // "/brew" → single segment: name contains "brew", no path constraint
        auto res = engine.query("/brew");
        check(res.size() == 1, "AbsPath: '/brew' matches brew");
    }

    // -- Test 8: Multi-segment adjacency --
    std::cout << "\n  --- Test 8: Multi-segment adjacency ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"hosts", "/etc", 1, 100, 1000});
        records.push_back({"passwd", "/etc", 1, 200, 2000});
        records.push_back({"config.txt", "/home/user", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // "/etc/hosts" → name contains "hosts", path contains "etc"
        auto res = engine.query("/etc/hosts");
        check(res.size() == 1, "AbsPath: '/etc/hosts' matches hosts in /etc");
    }

    // -- Test 9: Non-adjacent match with * --
    std::cout << "\n  --- Test 9: Non-adjacent wildcard ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"target.txt", "/project/src/core", 1, 100, 1000});
        records.push_back({"target.txt", "/project/docs", 1, 200, 2000});
        records.push_back({"target.txt", "/other/src/core", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // "/project/*/target" → name contains "target", ancestor contains "project" (non-adj)
        auto res = engine.query("/project/*/target");
        check(res.size() == 2, "Non-adj wildcard: '/project/*/target' matches 2 under /project");
    }

    // -- Test 10: Structured search path label --
    std::cout << "\n  --- Test 10: Structured search path label ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"brew", "/usr/local/bin", 1, 100, 1000});
        records.push_back({"python3", "/usr/local/bin", 1, 200, 2000});
        records.push_back({"gcc", "/usr/bin", 1, 300, 3000});
        records.push_back({"lib.a", "/usr/local/lib", 1, 400, 4000});
        engine.loadRecords(std::move(records));

        // "local/brew" → SEGMENTS mode, should use "structured" path
        QueryTimingInfo timing;
        auto res = engine.query("local/brew", 0, true, timing);
        check(res.size() == 1, "Structured path: 'local/brew' finds brew");
        // All queries now route through unified Advanced path
        check(!timing.searchPath.empty(),
              ("Slash query has searchPath set (got: " + timing.searchPath + ")").c_str());
    }

    // -- Test 11: Timing non-negative --
    std::cout << "\n  --- Test 11: Timing non-negative ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"brew", "/usr/local/bin", 1, 100, 1000});
        records.push_back({"python3", "/usr/local/bin", 1, 200, 2000});
        records.push_back({"gcc", "/usr/bin", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        QueryTimingInfo timing;
        auto res = engine.query("local/brew", 0, true, timing);
        check(timing.phase1Ms >= 0.0,
              "phase1Ms must be non-negative for structured queries");
        check(timing.phase2Ms >= 0.0,
              "phase2Ms must be non-negative for structured queries");

        std::cout << "    timing: phase1=" << timing.phase1Ms
                  << "ms phase2=" << timing.phase2Ms << "ms\n";
    }

    // -- Test 12: DIR_LIST via /*  --
    std::cout << "\n  --- Test 12: DIR_LIST via /* ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"bin", "/usr/local", 2, 0, 1000});
        records.push_back({"gcc", "/usr/local/bin", 1, 100, 2000});
        records.push_back({"ls", "/usr/local/bin", 1, 200, 3000});
        records.push_back({"cat", "/usr/bin", 1, 300, 4000});
        engine.loadRecords(std::move(records));

        // "/local/bin/*" → DIR_LIST: list children of "bin" where parent has "local"
        auto res = engine.query("/local/bin/*");
        std::set<std::string> names;
        for (uint32_t idx : res) {
            names.insert(engine.getRecord(idx).name);
        }
        check(names.count("gcc") == 1, "DIR_LIST: gcc found under /usr/local/bin");
        check(names.count("ls") == 1, "DIR_LIST: ls found under /usr/local/bin");
        check(names.count("cat") == 0, "DIR_LIST: cat not under /usr/local/bin");
    }

    // -- Test 13: Trigram-degrade path phase1Ms non-negative --
    std::cout << "\n  --- Test 13: Trigram-degrade phase1Ms non-negative ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        // Create enough records with "test" in the name to exceed the trigram threshold.
        // Threshold is totalSize / 4 (25%).  With 200 records, threshold = 50.
        // All 200 names contain "test", so trigram candidates (200) > 50 → degrades to linear.
        for (int i = 0; i < 200; i++) {
            std::string name = "test_file_" + std::to_string(i) + ".cpp";
            records.push_back({name.c_str(), "/some/path", 1, (uint64_t)i, (time_t)(i * 100)});
        }
        engine.loadRecords(std::move(records));

        QueryTimingInfo timing;
        auto res = engine.query("test", 0, true, timing);
        check(res.size() > 0, "Trigram-degrade query 'test' finds results");
        // All queries now route through unified Advanced path
        check(!timing.searchPath.empty(),
              ("Trigram-degrade query has searchPath set (got: " + timing.searchPath + ")").c_str());
        check(timing.phase1Ms >= 0.0,
              ("phase1Ms must be non-negative after trigram degrade (got: " + std::to_string(timing.phase1Ms) + ")").c_str());
        check(timing.trigramMs >= 0.0,
              ("trigramMs must be non-negative after trigram degrade (got: " + std::to_string(timing.trigramMs) + ")").c_str());

        std::cout << "    timing: trigram=" << timing.trigramMs
                  << "ms phase1=" << timing.phase1Ms << "ms\n";
    }

    // -- Test 14: Glob-trigram path phase1Ms non-negative --
    std::cout << "\n  --- Test 14: Glob-trigram phase1Ms non-negative ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"main.cpp", "/project/src", 1, 100, 1000});
        records.push_back({"util.cpp", "/project/src", 1, 200, 2000});
        records.push_back({"test.h", "/project/tests", 1, 300, 3000});
        records.push_back({"readme.md", "/project", 1, 400, 4000});
        engine.loadRecords(std::move(records));

        QueryTimingInfo timing;
        auto res = engine.query("*.cpp", 0, true, timing);
        check(res.size() == 2, "Glob query '*.cpp' finds 2 results");
        check(timing.phase1Ms >= 0.0,
              ("phase1Ms must be non-negative for glob-trigram (got: " + std::to_string(timing.phase1Ms) + ")").c_str());
        check(timing.trigramMs >= 0.0,
              ("trigramMs must be non-negative for glob-trigram (got: " + std::to_string(timing.trigramMs) + ")").c_str());

        std::cout << "    timing: trigram=" << timing.trigramMs
                  << "ms phase1=" << timing.phase1Ms
                  << "ms path=" << timing.searchPath << "\n";
    }

    // -- Test 15: Pasted full paths with spaces and editor locations --
    std::cout << "\n  --- Test 15: Pasted full path normalization ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"opendesign_prompt_spain_2026.md",
                           "/Users/joshguo/Documents/New project/xiaohongshu",
                           1, 100, 1000});
        records.push_back({"opendesign_prompt_spain_2026.md",
                           "/Users/joshguo/Documents/Archive/xiaohongshu",
                           1, 200, 2000});
        engine.loadRecords(std::move(records));

        const std::string fullPath =
            "/Users/joshguo/Documents/New project/xiaohongshu/"
            "opendesign_prompt_spain_2026.md";

        auto res = engine.query(fullPath);
        check(res.size() == 1,
              "Pasted absolute path with spaces matches the exact path");

        res = engine.query(fullPath + ":1");
        check(res.size() == 1,
              "Pasted Codex path with line suffix matches the file");

        res = engine.query(fullPath + ":42:7");
        check(res.size() == 1,
              "Pasted editor path with line and column matches the file");
    }
}
