#pragma once
#include "test_helpers.h"
#include "../MacEverything/Core/SearchEngine.h"
#include "../MacEverything/Core/QueryNeedsAnalysis.h"
#include <iostream>

inline void runExtensionIndexTests() {
    std::cout << "\n-- Part 74: Extension Index Tests --\n\n";

    // 74a: Build extension index from records
    {
        SearchEngine engine;
        engine.addRecord({"hello.txt", "/test", 1, 100, 1000});
        engine.addRecord({"world.py", "/test", 1, 200, 2000});
        engine.addRecord({"readme.txt", "/test", 1, 50, 3000});
        engine.addRecord({"noext", "/test", 1, 10, 4000});
        engine.addRecord({"folder", "/test", 2, 0, 5000});

        // Query for ext:txt should find 2 results
        QueryTimingInfo timing;
        auto results = engine.queryAdvanced("ext:txt", 100, true, timing);
        CHECK(results.size() == 2);
        std::cout << "    74a: ext:txt found " << results.size() << " results\n";
    }

    // 74b: Extension index maintained after addRecord
    {
        SearchEngine engine;
        engine.addRecord({"a.cpp", "/test", 1, 100, 1000});
        engine.addRecord({"b.cpp", "/test", 1, 200, 2000});

        QueryTimingInfo timing;
        auto results = engine.queryAdvanced("ext:cpp", 100, true, timing);
        CHECK(results.size() == 2);

        // Add another .cpp file
        engine.addRecord({"c.cpp", "/test", 1, 300, 3000});
        results = engine.queryAdvanced("ext:cpp", 100, true, timing);
        CHECK(results.size() == 3);
        std::cout << "    74b: addRecord ext:cpp count=" << results.size() << "\n";
    }

    // 74c: Extension index maintained after removeByPath
    {
        SearchEngine engine;
        engine.addRecord({"a.js", "/test", 1, 100, 1000});
        engine.addRecord({"b.js", "/test", 1, 200, 2000});

        QueryTimingInfo timing;
        auto results = engine.queryAdvanced("ext:js", 100, true, timing);
        CHECK(results.size() == 2);

        engine.removeByPath("/test/a.js");
        results = engine.queryAdvanced("ext:js", 100, true, timing);
        CHECK(results.size() == 1);
        std::cout << "    74c: removeByPath ext:js count=" << results.size() << "\n";
    }

    // 74d: ext: filter uses extension index path (not linear scan)
    {
        SearchEngine engine;
        engine.setExtensionIndexEnabled(true); // opt-in: off by default to save memory
        // Add enough records to make timing meaningful
        for (int i = 0; i < 1000; i++) {
            std::string name = "file" + std::to_string(i) + ".txt";
            engine.addRecord({name, "/test", 1, 100, 1000});
        }
        for (int i = 0; i < 100; i++) {
            std::string name = "file" + std::to_string(i) + ".py";
            engine.addRecord({name, "/test", 1, 200, 2000});
        }

        QueryTimingInfo timing;
        auto results = engine.queryAdvanced("ext:py", 100, true, timing);
        CHECK(results.size() == 100);
        // Verify the search path used extension index
        CHECK(timing.searchPath == "advanced-ext-index");
        std::cout << "    74d: searchPath=" << timing.searchPath
                  << " candidates=" << timing.candidates << "\n";
    }

    // 74e: ext: with multiple extensions (ext:cpp;h)
    {
        SearchEngine engine;
        engine.addRecord({"main.cpp", "/test", 1, 100, 1000});
        engine.addRecord({"main.h", "/test", 1, 50, 2000});
        engine.addRecord({"readme.md", "/test", 1, 200, 3000});
        engine.addRecord({"test.py", "/test", 1, 150, 4000});

        QueryTimingInfo timing;
        auto results = engine.queryAdvanced("ext:cpp;h", 100, true, timing);
        CHECK(results.size() == 2);
        std::cout << "    74e: ext:cpp;h found " << results.size() << " results\n";
    }

    // 74f: ext: combined with size: filter uses ext index as candidates
    {
        SearchEngine engine;
        for (int i = 0; i < 500; i++) {
            std::string name = "small" + std::to_string(i) + ".txt";
            engine.addRecord({name, "/test", 1, 50, 1000});
        }
        for (int i = 0; i < 500; i++) {
            std::string name = "big" + std::to_string(i) + ".txt";
            engine.addRecord({name, "/test", 1, 20000, 2000});
        }
        for (int i = 0; i < 500; i++) {
            std::string name = "other" + std::to_string(i) + ".py";
            engine.addRecord({name, "/test", 1, 20000, 3000});
        }

        QueryTimingInfo timing;
        auto results = engine.queryAdvanced("ext:txt size:>10000", 1000, true, timing);
        // Should find only big*.txt (500 files with size > 10000 and .txt extension)
        CHECK(results.size() == 500);
        std::cout << "    74f: ext:txt size:>10000 found " << results.size()
                  << " searchPath=" << timing.searchPath << "\n";
    }

    // 74g: ext: with TERM keyword
    {
        SearchEngine engine;
        engine.addRecord({"hello.txt", "/test", 1, 100, 1000});
        engine.addRecord({"hello.py", "/test", 1, 200, 2000});
        engine.addRecord({"world.txt", "/test", 1, 50, 3000});

        QueryTimingInfo timing;
        auto results = engine.queryAdvanced("hello ext:txt", 100, true, timing);
        CHECK(results.size() == 1);
        std::cout << "    74g: hello ext:txt found " << results.size() << " results\n";
    }

    // 74h: QueryNeeds - ext: sets hasExtFilter + needsName
    {
        auto node = QueryNode::makeFilter("ext", "txt");
        QueryNeeds needs = analyzeQueryNeeds(*node);
        CHECK(needs.hasExtFilter == true);
        CHECK(needs.needsName == true);
        CHECK(needs.isPureFilter() == false);
        std::cout << "    74h: ext: hasExtFilter=" << needs.hasExtFilter << " needsName=" << needs.needsName << "\n";
    }

    // 74i: QueryNeeds - ext: + size: has hasExtFilter, not pure filter
    {
        std::vector<std::unique_ptr<QueryNode>> kids;
        kids.push_back(QueryNode::makeFilter("ext", "py"));
        auto sizeNode = QueryNode::makeFilter("size", ">1000");
        sizeNode->op = CompareOp::GT;
        sizeNode->numVal1 = 1000;
        kids.push_back(std::move(sizeNode));
        auto andNode = QueryNode::makeAnd(std::move(kids));

        QueryNeeds needs = analyzeQueryNeeds(*andNode);
        CHECK(needs.hasExtFilter == true);
        CHECK(needs.needsSize == true);
        CHECK(needs.isPureFilter() == false);
        std::cout << "    74i: ext:+size: hasExtFilter=" << needs.hasExtFilter << "\n";
    }

    // 74j: Empty extension index lookup returns no candidates
    {
        SearchEngine engine;
        engine.addRecord({"noext", "/test", 1, 100, 1000});

        QueryTimingInfo timing;
        auto results = engine.queryAdvanced("ext:txt", 100, true, timing);
        CHECK(results.size() == 0);
        std::cout << "    74j: ext:txt on no .txt files found " << results.size() << "\n";
    }
}
