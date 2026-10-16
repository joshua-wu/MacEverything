#pragma once
// Part 32: Paged persistence tests
// Verifies PagedIndexWriter two-file format (.pages + .ptable):
//   basic read/write, incremental dirty-page flush, CRC32 integrity,
//   dead space reclamation, V3→V4 migration, and adaptive interval.

static void testPagedBasicReadWrite() {
    std::cout << "\n--- Paged: basic read/write (2048 records, 2 pages) ---\n";

    std::string tmpDir = "/tmp/test_paged_basic_" + std::to_string(getpid());
    fs::create_directories(tmpDir);
    std::string pagesPath  = tmpDir + "/index.pages";
    std::string ptablePath = tmpDir + "/index.ptable";

    // Write 2048 records across 2 pages
    auto engine = std::make_shared<SearchEngine>();
    for (uint32_t i = 0; i < 2048; i++) {
        FileRecord rec;
        rec.name = "file" + std::to_string(i) + ".txt";
        rec.path = "/data/dir";
        rec.type = 1;
        rec.size = i * 10;
        rec.modTime = 1000000 + static_cast<time_t>(i);
        rec.inode = i + 1;
        rec.devId = 1;
        engine->addRecord(std::move(rec));
    }

    check(engine->liveRecordCount() == 2048, "P32-1: engine has 2048 records");

    // Flush via PagedIndexWriter
    PagedIndexWriter writer(pagesPath, ptablePath);
    IndexMetadata meta;
    meta.lastEventId = 42;
    check(writer.fullRewrite(*engine, meta), "P32-1: fullRewrite succeeds");
    check(fs::exists(pagesPath), "P32-1: .pages file created");
    check(fs::exists(ptablePath), "P32-1: .ptable file created");

    // Load into a fresh engine
    auto engine2 = std::make_shared<SearchEngine>();
    PagedIndexWriter reader(pagesPath, ptablePath);
    IndexMetadata loadedMeta;
    check(reader.load(*engine2, &loadedMeta), "P32-1: load succeeds");
    check(loadedMeta.lastEventId == 42, "P32-1: lastEventId preserved");
    check(engine2->liveRecordCount() == 2048, "P32-1: loaded 2048 records");

    // Spot-check a few records
    auto results = engine2->query("file100.txt", 10);
    bool found100 = false;
    for (uint32_t idx : results) {
        auto rec = engine2->getRecord(idx);
        if (rec.name == "file100.txt" && rec.size == 1000) { found100 = true; break; }
    }
    check(found100, "P32-1: file100.txt found with correct size=1000");

    results = engine2->query("file2047.txt", 10);
    bool found2047 = false;
    for (uint32_t idx : results) {
        auto rec = engine2->getRecord(idx);
        if (rec.name == "file2047.txt") { found2047 = true; break; }
    }
    check(found2047, "P32-1: file2047.txt (last record) found");

    fs::remove_all(tmpDir);
}

static void testPagedIncrementalFlush() {
    std::cout << "\n--- Paged: incremental dirty-page flush ---\n";

    std::string tmpDir = "/tmp/test_paged_incr_" + std::to_string(getpid());
    fs::create_directories(tmpDir);
    std::string pagesPath  = tmpDir + "/index.pages";
    std::string ptablePath = tmpDir + "/index.ptable";

    // Use PagedIndexWriter directly to avoid WAL dirty-flag gating
    auto engine = std::make_shared<SearchEngine>();
    // 3 pages worth of records (3072)
    for (uint32_t i = 0; i < 3072; i++) {
        FileRecord rec;
        rec.name = "f" + std::to_string(i) + ".txt";
        rec.path = "/data";
        rec.type = 1;
        rec.size = 100;
        rec.modTime = 1000000;
        rec.inode = i + 1;
        rec.devId = 1;
        engine->addRecord(std::move(rec));
    }

    // Full write first
    PagedIndexWriter writer(pagesPath, ptablePath);
    IndexMetadata meta;
    meta.lastEventId = 1;
    check(writer.fullRewrite(*engine, meta), "P32-2: initial fullRewrite");
    check(fs::exists(ptablePath), "P32-2: ptable written after initial write");
    auto sizeAfterFull = fs::file_size(pagesPath);

    // Now modify only 1 record in page 1 (index 1024..2047)
    engine->clearDirtyPages();
    FileRecord updated;
    updated.name = "f1024.txt";
    updated.path = "/data";
    updated.type = 1;
    updated.size = 999;
    updated.modTime = 2000000;
    updated.inode = 1025;
    updated.devId = 1;
    engine->updateByPath("/data/f1024.txt", std::move(updated));

    // Same name and path: updateByPath rewrites the record in place (page 1 only)
    auto dirtyPages = engine->getDirtyPageNumbers();
    check(dirtyPages.size() == 1, "P32-2: 1 dirty page after in-place update");
    // Verify page 1 (old record) is dirty
    bool hasPage1 = false;
    for (auto p : dirtyPages) { if (p == 1) hasPage1 = true; }
    check(hasPage1, "P32-2: page 1 (old record) is dirty");

    // Incremental flush via PagedIndexWriter
    meta.lastEventId = 2;
    check(writer.flushDirtyPages(*engine, meta), "P32-2: flushDirtyPages succeeds");

    auto sizeAfterIncr = fs::file_size(pagesPath);
    check(sizeAfterIncr > sizeAfterFull, "P32-2: .pages grew after incremental flush (append)");
    // Incremental should append much less than full (1 page vs 3 pages)
    check(sizeAfterIncr - sizeAfterFull < sizeAfterFull, "P32-2: incremental append < full write size");

    // Reload and verify the update was persisted
    auto engine2 = std::make_shared<SearchEngine>();
    PagedIndexWriter reader(pagesPath, ptablePath);
    IndexMetadata loadedMeta;
    check(reader.load(*engine2, &loadedMeta), "P32-2: reload succeeds");
    check(loadedMeta.lastEventId == 2, "P32-2: lastEventId updated to 2");
    // updateByPath updated the record in place, so total records stay 3072
    // loadRecords correctly counts only live records (skipping tombstones)
    check(engine2->liveRecordCount() == 3072, "P32-2: 3072 live records (tombstone excluded)");

    auto results = engine2->query("f1024.txt", 10);
    bool foundUpdated = false;
    for (uint32_t idx : results) {
        auto rec = engine2->getRecord(idx);
        if (rec.name == "f1024.txt" && rec.size == 999) { foundUpdated = true; break; }
    }
    check(foundUpdated, "P32-2: updated record (size=999) persisted correctly");

    fs::remove_all(tmpDir);
}

static void testPagedCRC32Integrity() {
    std::cout << "\n--- Paged: CRC32 integrity check ---\n";

    std::string tmpDir = "/tmp/test_paged_crc_" + std::to_string(getpid());
    fs::create_directories(tmpDir);
    std::string pagesPath  = tmpDir + "/index.pages";
    std::string ptablePath = tmpDir + "/index.ptable";

    auto engine = std::make_shared<SearchEngine>();
    for (uint32_t i = 0; i < 100; i++) {
        FileRecord rec;
        rec.name = "crc" + std::to_string(i) + ".txt";
        rec.path = "/test";
        rec.type = 1;
        rec.size = 50;
        rec.modTime = 1000000;
        rec.inode = i + 1;
        rec.devId = 1;
        engine->addRecord(std::move(rec));
    }

    PagedIndexWriter writer(pagesPath, ptablePath);
    IndexMetadata meta;
    meta.lastEventId = 10;
    check(writer.fullRewrite(*engine, meta), "P32-3: write for CRC test");

    // Tamper with the .pages file (corrupt some bytes in the middle)
    {
        FILE* f = fopen(pagesPath.c_str(), "r+b");
        check(f != nullptr, "P32-3: open .pages for tampering");
        if (f) {
            // Seek past magic (4 bytes) + some data
            fseek(f, 20, SEEK_SET);
            uint8_t garbage[] = {0xFF, 0xFF, 0xFF, 0xFF};
            fwrite(garbage, 1, sizeof(garbage), f);
            fclose(f);
        }
    }

    // Load should fail due to CRC mismatch
    auto engine2 = std::make_shared<SearchEngine>();
    PagedIndexWriter reader(pagesPath, ptablePath);
    IndexMetadata loadedMeta;
    // Suppress expected CRC error output
    std::cerr.setstate(std::ios_base::failbit);
    bool loadResult = reader.load(*engine2, &loadedMeta);
    std::cerr.clear();
    check(!loadResult, "P32-3: load fails when .pages data is corrupted (CRC mismatch)");

    fs::remove_all(tmpDir);
}

static void testPagedDeadSpaceReclaim() {
    std::cout << "\n--- Paged: dead space reclamation ---\n";

    std::string tmpDir = "/tmp/test_paged_dead_" + std::to_string(getpid());
    fs::create_directories(tmpDir);
    std::string pagesPath  = tmpDir + "/index.pages";
    std::string ptablePath = tmpDir + "/index.ptable";

    auto engine = std::make_shared<SearchEngine>();
    for (uint32_t i = 0; i < 1024; i++) {
        FileRecord rec;
        rec.name = "dead" + std::to_string(i) + ".txt";
        rec.path = "/test";
        rec.type = 1;
        rec.size = 100;
        rec.modTime = 1000000;
        rec.inode = i + 1;
        rec.devId = 1;
        engine->addRecord(std::move(rec));
    }

    PagedIndexWriter writer(pagesPath, ptablePath);
    IndexMetadata meta;
    meta.lastEventId = 1;
    check(writer.fullRewrite(*engine, meta), "P32-4: initial fullRewrite");
    check(writer.deadSpaceRatio() < 0.01, "P32-4: no dead space after fullRewrite");

    auto sizeAfterFull = fs::file_size(pagesPath);

    // Flush dirty page multiple times to create dead space
    for (int round = 0; round < 5; round++) {
        // Update a record in page 0 to make it dirty
        FileRecord updated;
        updated.name = "dead0.txt";
        updated.path = "/test";
        updated.type = 1;
        updated.size = 200 + round;
        updated.modTime = 2000000;
        updated.inode = 1;
        updated.devId = 1;
        engine->updateByPath("/test/dead0.txt", std::move(updated));

        meta.lastEventId = static_cast<uint64_t>(2 + round);
        check(writer.flushDirtyPages(*engine, meta), ("P32-4: flush round " + std::to_string(round)).c_str());
    }

    auto sizeAfterFlushes = fs::file_size(pagesPath);
    check(sizeAfterFlushes > sizeAfterFull, "P32-4: .pages grew from repeated flushes");
    check(writer.deadSpaceRatio() > 0.1, "P32-4: dead space ratio > 10% after repeated flushes");

    // Full rewrite should reclaim dead space
    meta.lastEventId = 100;
    check(writer.fullRewrite(*engine, meta), "P32-4: fullRewrite to reclaim");
    auto sizeAfterReclaim = fs::file_size(pagesPath);
    // After reclaim, .pages should be close to original size
    // Allow some tolerance for record size changes (updates changed size field)
    check(sizeAfterReclaim < sizeAfterFlushes, "P32-4: .pages shrunk after reclaim");
    check(writer.deadSpaceRatio() < 0.01, "P32-4: dead space ratio ~0 after reclaim");

    fs::remove_all(tmpDir);
}

static void testPagedV3Migration() {
    std::cout << "\n--- Paged: V3 → V4 auto-migration ---\n";

    std::string tmpDir = "/tmp/test_paged_v3_" + std::to_string(getpid());
    fs::create_directories(tmpDir);
    std::string basePath   = tmpDir + "/index.bin";
    std::string walPath    = tmpDir + "/index.wal";
    std::string pagesPath  = tmpDir + "/index.pages";
    std::string ptablePath = tmpDir + "/index.ptable";

    // Create a v3 format file using saveToFile
    auto engine = std::make_shared<SearchEngine>();
    for (uint32_t i = 0; i < 500; i++) {
        FileRecord rec;
        rec.name = "v3file" + std::to_string(i) + ".txt";
        rec.path = "/legacy";
        rec.type = 1;
        rec.size = i * 5;
        rec.modTime = 1000000 + static_cast<time_t>(i);
        rec.inode = i + 1;
        rec.devId = 1;
        engine->addRecord(std::move(rec));
    }
    IndexMetadata v3Meta;
    v3Meta.lastEventId = 77;
    engine->saveToFile(basePath, v3Meta);
    check(fs::exists(basePath), "P32-5: v3 index.bin created");
    check(!fs::exists(ptablePath), "P32-5: no ptable before migration");

    // Load via IndexPersistence — should auto-migrate
    auto engine2 = std::make_shared<SearchEngine>();
    IndexPersistence persistence(engine2, basePath, walPath, pagesPath, ptablePath, basePath + ".v6");
    uint64_t eventId = persistence.load();

    check(eventId == 77, "P32-5: eventId preserved from v3");
    check(engine2->liveRecordCount() == 500, "P32-5: all 500 records loaded");
    std::string v6Path = basePath + ".v6";
    check(fs::exists(v6Path), "P32-5: v6 file created by migration");
    check(fs::exists(basePath), "P32-5: v3 file retained as backup");

    // Reload from v6 format
    auto engine3 = std::make_shared<SearchEngine>();
    FlatIndexWriter reader(v6Path);
    IndexMetadata loadedMeta;
    check(reader.load(*engine3, &loadedMeta), "P32-5: reload from v6 format");
    check(engine3->liveRecordCount() == 500, "P32-5: all 500 records in v6 format");
    check(loadedMeta.lastEventId == 77, "P32-5: eventId in v6 format matches v3");

    // Spot-check a record
    engine3->completePhase2();
    auto results = engine3->query("v3file42.txt", 10);
    bool found42 = false;
    for (uint32_t idx : results) {
        auto rec = engine3->getRecord(idx);
        if (rec.name == "v3file42.txt" && rec.size == 210) { found42 = true; break; }
    }
    check(found42, "P32-5: v3file42.txt migrated correctly (size=210)");

    fs::remove_all(tmpDir);
}

static void testPagedAdaptiveInterval() {
    std::cout << "\n--- Paged: adaptive interval computation ---\n";

    std::string tmpDir = "/tmp/test_paged_adapt_" + std::to_string(getpid());
    fs::create_directories(tmpDir);
    std::string basePath   = tmpDir + "/index.bin";
    std::string walPath    = tmpDir + "/index.wal";
    std::string pagesPath  = tmpDir + "/index.pages";
    std::string ptablePath = tmpDir + "/index.ptable";

    // Create engine with enough records for 10 pages (10240 records)
    auto engine = std::make_shared<SearchEngine>();
    for (uint32_t i = 0; i < 10240; i++) {
        FileRecord rec;
        rec.name = "a" + std::to_string(i) + ".txt";
        rec.path = "/data";
        rec.type = 1;
        rec.size = 100;
        rec.modTime = 1000000;
        rec.inode = i + 1;
        rec.devId = 1;
        engine->addRecord(std::move(rec));
    }

    IndexPersistence persistence(engine, basePath, walPath, pagesPath, ptablePath, basePath + ".v6");
    persistence.attachWAL();

    // Flush to clear dirty pages
    persistence.flush(1, /*force=*/true);

    // With no dirty pages and no WAL pressure, should get max interval
    engine->clearDirtyPages();
    // No easy way to test computeAdaptiveInterval directly (private),
    // but we can verify the constants are sane
    check(IndexPersistence::kMinIntervalSec == 30.0, "P32-6: kMinIntervalSec = 30");
    check(IndexPersistence::kBaseIntervalSec == 300.0, "P32-6: kBaseIntervalSec = 300");
    check(IndexPersistence::kMaxIntervalSec == 600.0, "P32-6: kMaxIntervalSec = 600");
    check(IndexPersistence::kWALSizeFlushThreshold == 2 * 1024 * 1024, "P32-6: WAL threshold = 2MB");

    // Verify that dirty page tracking works correctly across pages
    // updateByPath with an unchanged name/path updates in place:
    // a0.txt dirties page 0, a5120.txt dirties page 5 (2 pages)
    FileRecord upd1;
    upd1.name = "a0.txt"; upd1.path = "/data"; upd1.type = 1;
    upd1.size = 999; upd1.modTime = 2000000; upd1.inode = 1; upd1.devId = 1;
    engine->updateByPath("/data/a0.txt", std::move(upd1));

    FileRecord upd2;
    upd2.name = "a5120.txt"; upd2.path = "/data"; upd2.type = 1;
    upd2.size = 888; upd2.modTime = 2000000; upd2.inode = 5121; upd2.devId = 1;
    engine->updateByPath("/data/a5120.txt", std::move(upd2));

    auto dirtyPages = engine->getDirtyPageNumbers();
    check(dirtyPages.size() == 2, "P32-6: 2 dirty pages (page 0 and 5, updated in place)");

    // With 2/10 pages dirty (20%), should be in the mid range (> 0.1)
    double totalPages = (engine->liveRecordCount() + SearchEngine::kRecordsPerPage - 1) / SearchEngine::kRecordsPerPage;
    double dirtyRatio = static_cast<double>(dirtyPages.size()) / totalPages;
    check(dirtyRatio > 0.1 && dirtyRatio <= 0.3,
          "P32-6: dirty ratio ~20% in mid-range (should yield 150s interval)");

    fs::remove_all(tmpDir);
}

static void testPagedCrashRecovery() {
    std::cout << "\n--- Paged: crash recovery (partial write) ---\n";

    std::string tmpDir = "/tmp/test_paged_crash_" + std::to_string(getpid());
    fs::create_directories(tmpDir);
    std::string pagesPath  = tmpDir + "/index.pages";
    std::string ptablePath = tmpDir + "/index.ptable";

    auto engine = std::make_shared<SearchEngine>();
    for (uint32_t i = 0; i < 2048; i++) {
        FileRecord rec;
        rec.name = "crash" + std::to_string(i) + ".txt";
        rec.path = "/data";
        rec.type = 1;
        rec.size = 100;
        rec.modTime = 1000000;
        rec.inode = i + 1;
        rec.devId = 1;
        engine->addRecord(std::move(rec));
    }

    // Write a valid v4 format
    PagedIndexWriter writer(pagesPath, ptablePath);
    IndexMetadata meta;
    meta.lastEventId = 50;
    check(writer.fullRewrite(*engine, meta), "P32-7: initial write for crash test");

    // Verify it loads fine
    auto engine2 = std::make_shared<SearchEngine>();
    PagedIndexWriter reader1(pagesPath, ptablePath);
    IndexMetadata meta2;
    check(reader1.load(*engine2, &meta2), "P32-7: pre-crash load succeeds");
    check(engine2->liveRecordCount() == 2048, "P32-7: 2048 records before crash");

    // Simulate crash: truncate .pages file (ptable still references full data)
    {
        auto fullSize = fs::file_size(pagesPath);
        // Truncate to half
        truncate(pagesPath.c_str(), static_cast<off_t>(fullSize / 2));
    }

    // Load should fail: ptable references data beyond truncated .pages
    auto engine3 = std::make_shared<SearchEngine>();
    PagedIndexWriter reader2(pagesPath, ptablePath);
    IndexMetadata meta3;
    std::cerr.setstate(std::ios_base::failbit);
    bool loadResult = reader2.load(*engine3, &meta3);
    std::cerr.clear();
    check(!loadResult, "P32-7: load fails after .pages truncation (simulated crash)");

    fs::remove_all(tmpDir);
}

static void testPagedEmptyEngine() {
    std::cout << "\n--- Paged: empty engine write/load ---\n";

    std::string tmpDir = "/tmp/test_paged_empty_" + std::to_string(getpid());
    fs::create_directories(tmpDir);
    std::string pagesPath  = tmpDir + "/index.pages";
    std::string ptablePath = tmpDir + "/index.ptable";

    auto engine = std::make_shared<SearchEngine>();
    // No records added

    PagedIndexWriter writer(pagesPath, ptablePath);
    IndexMetadata meta;
    meta.lastEventId = 0;
    check(writer.fullRewrite(*engine, meta), "P32-8: fullRewrite with 0 records");

    auto engine2 = std::make_shared<SearchEngine>();
    PagedIndexWriter reader(pagesPath, ptablePath);
    IndexMetadata loadedMeta;
    check(reader.load(*engine2, &loadedMeta), "P32-8: load empty index");
    check(engine2->liveRecordCount() == 0, "P32-8: 0 records after loading empty index");

    fs::remove_all(tmpDir);
}

static void runPagedPersistenceTests() {
    std::cout << "═══ Part 32: Paged Persistence ═══\n";

    testPagedBasicReadWrite();
    testPagedIncrementalFlush();
    testPagedCRC32Integrity();
    testPagedDeadSpaceReclaim();
    testPagedV3Migration();
    testPagedAdaptiveInterval();
    testPagedCrashRecovery();
    testPagedEmptyEngine();

    std::cout << "\n";
}
