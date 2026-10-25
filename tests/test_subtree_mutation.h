#pragma once
// ═══════════════════════════════════════════════════════
//  Part 3f: FlatKeyIndex, in-place updates, subtree removal
// ═══════════════════════════════════════════════════════

static void runSubtreeMutationTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 3f: In-place Update & Subtree Removal\n";
    std::cout << "========================================\n\n";

    // --- FlatKeyIndex basics ---
    {
        std::vector<std::string> keys;
        for (int i = 0; i < 20000; i++) keys.push_back("/dir" + std::to_string(i % 97) + "/file" + std::to_string(i));
        auto keyOf = [&](uint32_t i) { return FlatKeyIndex::KeyParts{keys[i], {}, false}; };
        FlatKeyIndex idx;
        for (uint32_t i = 0; i < keys.size(); i++) idx.assignSelf(i, keyOf);
        check(idx.size() == keys.size(), "FlatKeyIndex: size after inserts");
        bool allFound = true;
        for (uint32_t i = 0; i < keys.size(); i++) allFound &= idx.get(keys[i], keyOf) == i;
        check(allFound, "FlatKeyIndex: every key found");
        for (uint32_t i = 0; i < keys.size(); i += 2) idx.eraseId(i, keyOf);
        bool erasedOk = true;
        for (uint32_t i = 0; i < keys.size(); i++) {
            uint32_t got = idx.get(keys[i], keyOf);
            erasedOk &= (i % 2 == 0) ? got == UINT32_MAX : got == i;
        }
        check(erasedOk, "FlatKeyIndex: erased keys gone, others intact (backward shift)");
        check(idx.size() == keys.size() / 2, "FlatKeyIndex: size after erase");
        keys.push_back(keys[1]);
        idx.assignSelf(static_cast<uint32_t>(keys.size() - 1), keyOf);
        check(idx.get(keys[1], keyOf) == keys.size() - 1, "FlatKeyIndex: assign overwrites duplicate key");
        check(idx.size() == keys.size() / 2, "FlatKeyIndex: overwrite keeps size");

        auto partsOf = [](uint32_t) { return FlatKeyIndex::KeyParts{"/a/b", "c.txt", true}; };
        check(FlatKeyIndex::hashParts(partsOf(0)) == FlatKeyIndex::hashKey("/a/b/c.txt"),
              "FlatKeyIndex: piecewise hash equals contiguous hash");
        check(FlatKeyIndex::equals("/a/b/c.txt", partsOf(0)), "FlatKeyIndex: piecewise equals");
        check(!FlatKeyIndex::equals("/a/bxc.txt", partsOf(0)), "FlatKeyIndex: separator is compared");
    }

    // --- In-place metadata update ---
    {
        SearchEngine engine;
        std::vector<FileRecord> initial;
        initial.push_back({"notes.txt", "/work", 1, 100, 1000});
        initial.push_back({"work", "/", 2, 0, 1000});
        engine.loadRecords(std::move(initial));

        FileRecord upd{"notes.txt", "/work", 1, 555, 9000};
        engine.updateByPath("/work/notes.txt", std::move(upd));
        check(engine.recordCount() == 2, "In-place update: no tombstone appended");
        uint32_t idx = engine.indexForPath("/work/notes.txt");
        check(idx != UINT32_MAX && engine.getRecord(idx).size == 555, "In-place update: size updated");
        check(engine.getRecord(idx).modTime == 9000, "In-place update: modTime updated");
        check(engine.query("notes").size() == 1, "In-place update: still searchable");

        FileRecord renamed{"NOTES.txt", "/work", 1, 555, 9100};
        engine.updateByPath("/work/NOTES.txt", std::move(renamed));
        uint32_t idx2 = engine.indexForPath("/work/notes.txt");
        check(idx2 != idx && engine.getRecord(idx2).name == "NOTES.txt", "Case change: record replaced with new name");
        check(engine.liveRecordCount() == 2, "Case change: live count unchanged");
        check(engine.query("notes").size() == 1, "Case change: exactly one match");
    }

    // --- Directory removal drops the whole subtree ---
    auto makeTree = [] {
        auto engine = std::make_shared<SearchEngine>();
        std::vector<FileRecord> recs;
        recs.push_back({"proj", "/", 2, 0, 1});
        recs.push_back({"build", "/proj", 2, 0, 1});
        recs.push_back({"obj", "/proj/build", 2, 0, 1});
        recs.push_back({"zqa.o", "/proj/build/obj", 1, 10, 1});
        recs.push_back({"zqb.txt", "/proj/build", 1, 10, 1});
        recs.push_back({"build-x", "/proj", 2, 0, 1});
        recs.push_back({"zqkeep.txt", "/proj/build-x", 1, 10, 1});
        recs.push_back({"buildfile", "/proj", 1, 10, 1});
        engine->loadRecords(std::move(recs));
        return engine;
    };
    {
        auto engine = makeTree();
        std::vector<SearchEngine::MutationOp> ops;
        ops.push_back({SearchEngine::MutationOp::REMOVE, "/proj/build", {}});
        engine->batchMutate(std::move(ops));
        check(engine->indexForPath("/proj/build") == UINT32_MAX, "Dir REMOVE: directory record gone");
        check(engine->indexForPath("/proj/build/obj/zqa.o") == UINT32_MAX, "Dir REMOVE: nested file gone");
        check(engine->indexForPath("/proj/build/zqb.txt") == UINT32_MAX, "Dir REMOVE: direct child gone");
        check(engine->indexForPath("/proj/build-x/zqkeep.txt") != UINT32_MAX, "Dir REMOVE: sibling 'build-x' kept");
        check(engine->indexForPath("/proj/buildfile") != UINT32_MAX, "Dir REMOVE: sibling 'buildfile' kept");
        check(engine->liveRecordCount() == 4, "Dir REMOVE: 4 live records remain");
        check(engine->query("zqa").empty() && engine->query("zqb").empty(), "Dir REMOVE: removed names not searchable");

        // Compaction after subtree removal keeps lookups and search consistent
        engine->compactRecords();
        check(engine->recordCount() == 4, "Dir REMOVE + compact: tombstones reclaimed");
        check(engine->indexForPath("/proj/build-x/zqkeep.txt") != UINT32_MAX, "Dir REMOVE + compact: lookup works");
        check(engine->query("zqkeep").size() == 1, "Dir REMOVE + compact: search works");
    }
    {
        auto engine = makeTree();
        uint32_t removed = engine->removeByPathPrefix("/PROJ/Build");
        check(removed == 4, "removeByPathPrefix: removes dir + 3 descendants (case-insensitive)");
        check(engine->indexForPath("/proj/build-x/zqkeep.txt") != UINT32_MAX, "removeByPathPrefix: sibling kept");
    }
    {
        auto engine = makeTree();
        std::vector<FileRecord> fresh;
        fresh.push_back({"zqnew.txt", "/proj/build", 1, 1, 2});
        engine->batchRescanPrefix("/proj/build", std::move(fresh));
        check(engine->indexForPath("/proj/build/zqb.txt") == UINT32_MAX, "batchRescanPrefix: stale child removed");
        check(engine->indexForPath("/proj/build/zqnew.txt") != UINT32_MAX, "batchRescanPrefix: fresh child added");
        check(engine->indexForPath("/proj/build-x/zqkeep.txt") != UINT32_MAX, "batchRescanPrefix: sibling kept");
        check(engine->indexForPath("/proj/build") != UINT32_MAX, "batchRescanPrefix: rescanned dir's own record kept");
    }
}
