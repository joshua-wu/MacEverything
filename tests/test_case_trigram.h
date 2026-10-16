// Part 72 — case: modifier trigram pre-filtering tests
// Verifies that case-sensitive queries use trigram pre-filtering
// instead of falling back to full linear scan (P24 fix).

#pragma once

#include <vector>
#include <string>

static void runCaseTrigramTests() {
    std::cout << "\n╔══════════════════════════════════════════╗\n";
    std::cout << "║  Part 72: Case-Sensitive Trigram (P24)   ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n\n";

    int localPassed = 0, localFailed = 0;
    auto check = [&](bool cond, const std::string& msg) {
        if (cond) { localPassed++; passed++; }
        else { localFailed++; failed++; std::cout << "  FAIL: " << msg << "\n"; }
    };

    // ── Test 1: AST level — case: sets textLower ──
    {
        auto node = QueryNode::makeFilter("case", "README");
        QueryFilterParser::parse(*node);
        check(node->type == QueryNodeType::TERM, "72.1 case:README → TERM");
        check(node->text == "README", "72.1 case:README text preserved");
        check(node->textLower == "readme", "72.1 case:README textLower = 'readme'");
        check(node->caseSensitive == true, "72.1 case:README caseSensitive = true");
        check(node->mode == MatchMode::SUBSTRING, "72.1 case:README mode = SUBSTRING");
    }

    // ── Test 2: AST level — case: with mixed case ──
    {
        auto node = QueryNode::makeFilter("case", "Makefile");
        QueryFilterParser::parse(*node);
        check(node->textLower == "makefile", "72.2 case:Makefile textLower = 'makefile'");
        check(node->text == "Makefile", "72.2 case:Makefile text preserved as-is");
    }

    // ── Test 3: Engine level — case: query uses trigram path ──
    {
        SearchEngine engine;
        engine.setTrigramIndexEnabled(true);
        // Insert 500 records to make trigram pre-filtering meaningful
        for (int i = 0; i < 500; i++) {
            FileRecord r;
            r.name = "generic_data_" + std::to_string(i) + ".dat";
            r.path = "/data";
            r.size = 100;
            r.modTime = 1000000;
            r.type = 1;
            engine.updateByPath(r.path + "/" + r.name, std::move(r));
        }
        // Insert target with exact case
        FileRecord target;
        target.name = "README.md";
        target.path = "/project";
        target.size = 200;
        target.modTime = 2000000;
        target.type = 1;
        engine.updateByPath(target.path + "/" + target.name, std::move(target));

        // Also insert a lowercase "readme" that should NOT match case:README
        FileRecord decoy;
        decoy.name = "readme.txt";
        decoy.path = "/docs";
        decoy.size = 150;
        decoy.modTime = 2000000;
        decoy.type = 1;
        engine.updateByPath(decoy.path + "/" + decoy.name, std::move(decoy));

        QueryTimingInfo timing;
        auto results = engine.queryAdvanced("case:README", 100, true, timing);

        // Must find README.md but NOT readme.txt
        check(results.size() == 1, "72.3 case:README finds exactly 1 result");
        if (!results.empty()) {
            auto rec = engine.getRecord(results[0]);
            check(rec.name == "README.md", "72.3 case:README matches README.md");
        }

        // With 502 records and trigram, should use trigram path
        // (candidates should be much less than total)
        check(timing.searchPath.find("trigram") != std::string::npos,
              "72.3 case:README uses trigram path (was: " + timing.searchPath + ")");
    }

    // ── Test 4: case: correctness — only exact case matches ──
    {
        SearchEngine engine;
        engine.setTrigramIndexEnabled(true);
        for (int i = 0; i < 200; i++) {
            FileRecord r;
            r.name = "padding_" + std::to_string(i) + ".log";
            r.path = "/logs";
            r.size = 50;
            r.modTime = 1000000;
            r.type = 1;
            engine.updateByPath(r.path + "/" + r.name, std::move(r));
        }
        // Three variants of "makefile" with different cases
        // Each must have a unique path to avoid updateByPath overwriting
        auto addFile = [&](const std::string& name, const std::string& dir) {
            FileRecord r;
            r.name = name;
            r.path = dir;
            r.size = 100;
            r.modTime = 2000000;
            r.type = 1;
            engine.updateByPath(r.path + "/" + r.name, std::move(r));
        };
        addFile("Makefile", "/build/projA");
        addFile("makefile", "/build/projB");
        addFile("MAKEFILE", "/build/projC");

        QueryTimingInfo timing;
        auto results = engine.queryAdvanced("case:Makefile", 100, true, timing);
        check(results.size() == 1, "72.4 case:Makefile finds exactly 1 match");
        if (!results.empty()) {
            auto rec = engine.getRecord(results[0]);
            check(rec.name == "Makefile", "72.4 matches 'Makefile' only");
        }
    }

    // ── Test 5: case: short keyword (< 3 chars) falls back to linear ──
    {
        SearchEngine engine;
        engine.setTrigramIndexEnabled(true);
        for (int i = 0; i < 50; i++) {
            FileRecord r;
            r.name = "file_" + std::to_string(i) + ".AB";
            r.path = "/tmp";
            r.size = 10;
            r.modTime = 1000;
            r.type = 1;
            engine.updateByPath(r.path + "/" + r.name, std::move(r));
        }
        QueryTimingInfo timing;
        auto results = engine.queryAdvanced("case:AB", 100, true, timing);
        // Short key can't use trigram (needs >= 3 chars), should still produce correct results
        check(results.size() == 50, "72.5 case:AB (short) finds all 50 matches");
    }

    // ── Test 6: nocase: still works correctly (regression check) ──
    {
        auto node = QueryNode::makeFilter("nocase", "README");
        QueryFilterParser::parse(*node);
        check(node->textLower == "readme", "72.6 nocase:README textLower = 'readme' (unchanged)");
        check(node->caseSensitive == false, "72.6 nocase:README caseSensitive = false");
    }

    std::cout << "  Part 72 summary: " << localPassed << " passed, " << localFailed << " failed\n\n";
}
