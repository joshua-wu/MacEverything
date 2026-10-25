#pragma once
// Part 7e: C5 — DirectoryScanner scan() re-entrancy

static void runScannerReentryTest() {
    std::cout << "═══ Part 7e: DirectoryScanner Re-entrancy ═══\n\n";

    // Create temp directory structure
    std::string tmpDir = "/tmp/maceverything_c5_test_" + std::to_string(getpid());
    fs::create_directories(tmpDir + "/sub1");
    fs::create_directories(tmpDir + "/sub2");

    // Create test files
    for (int i = 0; i < 3; i++) {
        std::ofstream(tmpDir + "/file" + std::to_string(i) + ".txt") << "test";
    }
    std::ofstream(tmpDir + "/sub1/deep.txt") << "deep file";
    std::ofstream(tmpDir + "/sub2/other.txt") << "other file";

    DirectoryScanner scanner;

    // First scan
    scanner.scan(tmpDir);
    auto results1 = scanner.takeResults();
    uint64_t fileCount1 = scanner.getStats().fileCount.load();
    uint64_t dirCount1 = scanner.getStats().dirCount.load();

    check(results1.size() > 0, "C5: First scan found results");
    check(fileCount1 == 5, "C5: First scan found exactly 5 files");

    // Second scan on same scanner instance — must NOT fail or return empty
    scanner.scan(tmpDir);
    auto results2 = scanner.takeResults();
    uint64_t fileCount2 = scanner.getStats().fileCount.load();
    uint64_t dirCount2 = scanner.getStats().dirCount.load();

    check(results2.size() > 0, "C5: Second scan found results (not stuck on done_=true)");
    check(results2.size() == results1.size(), "C5: Second scan found same number of entries");
    check(fileCount2 == fileCount1, "C5: Second scan file count matches first");
    check(dirCount2 == dirCount1, "C5: Second scan dir count matches first");

    // Third scan on a different directory
    std::string tmpDir2 = "/tmp/maceverything_c5_test2_" + std::to_string(getpid());
    fs::create_directories(tmpDir2);
    std::ofstream(tmpDir2 + "/single.txt") << "single";

    scanner.scan(tmpDir2);
    auto results3 = scanner.takeResults();

    // Should NOT contain results from previous scans
    check(results3.size() == 1, "C5: Third scan on different dir returns exactly 1 result");
    check(results3.size() < results1.size(), "C5: Third scan has fewer entries than first (different dir)");

    // ── Unreachable/empty roots must fail fast, not produce empty scans ──
    // Regression: a rescan of an empty or unmounted root used to yield an
    // empty result set which the caller reconciled into a full index wipe
    // (99.99% tombstone ratio in production logs).
    {
        DirectoryScanner s1;
        s1.scan("");
        check(s1.hasFailed(), "C5: scan(\"\") refuses to run (hasFailed)");
        check(s1.takeResults().empty(), "C5: scan(\"\") returns no results");

        DirectoryScanner s2;
        s2.scan("/definitely/not/a/real/path_" + std::to_string(getpid()));
        check(s2.hasFailed(), "C5: scan(unreachable root) refuses to run (hasFailed)");
        check(s2.takeResults().empty(), "C5: scan(unreachable root) returns no results");

        // A reachable root must NOT be marked as failed
        DirectoryScanner s3;
        s3.scan(tmpDir);
        check(!s3.hasFailed(), "C5: scan(reachable root) not marked failed");
        check(s3.takeResults().size() > 0, "C5: scan(reachable root) still returns results");
    }

    fs::remove_all(tmpDir);
    fs::remove_all(tmpDir2);
    std::cout << "\n";
}
