// MacEverything — Full Test Suite + Performance Report
// Compile: clang++ -std=c++20 -O2 -framework CoreServices MacEverything/Core/*.cpp test_all.cpp -o test_all
// Run:     ./test_all [root_path]            (all tests, default root: /)
//          ./test_all --fast                 (fast unit tests only: 3, 3b-3e, 5, 7-14, 15)
//          ./test_all --slow [root_path]     (slow integration tests: 1, 4, 6)
//          ./test_all --part 3 --part 3b     (specific parts)

#include "MacEverything/Core/DirectoryScanner.h"
#include "MacEverything/Core/SearchEngine.h"
#include "MacEverything/Core/ContentIndex.h"
#include "MacEverything/Core/IndexPersistence.h"
#include "MacEverything/Core/ContentIndexPersistence.h"
#include "MacEverything/Core/FileSystemWatcher.h"
#include "MacEverything/Core/Logger.h"
#include "MacEverything/Core/InstanceLock.h"
#include "MacEverything/Core/HttpServer.h"
#include "MacEverything/Core/ServiceEngine.h"
#include "MacEverything/Core/QueryTokenizer.h"
#include "MacEverything/Core/QueryParser.h"
#include "MacEverything/Core/QueryFilterParser.h"
#include <chrono>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <mach/mach.h>
#include <sys/stat.h>
#include <cassert>
#include <set>
#include <dispatch/dispatch.h>

namespace fs = std::filesystem;

// ─────────── Test modules ───────────
#include "tests/test_helpers.h"
#include "tests/test_scan_query.h"
#include "tests/test_mutation.h"
#include "tests/test_path_search.h"
#include "tests/test_metadata.h"
#include "tests/test_compaction.h"
#include "tests/test_ranking.h"
#include "tests/test_fsevents.h"
#include "tests/test_thread_safety.h"
#include "tests/test_e2e.h"
#include "tests/test_wal_crc.h"
#include "tests/test_compact_content.h"
#include "tests/test_destructor_safety.h"
#include "tests/test_wal_race.h"
#include "tests/test_save_file.h"
#include "tests/test_scanner_reentry.h"
#include "tests/test_content_index.h"
#include "tests/test_trigram_index.h"
#include "tests/test_content_query_bench.h"
#include "tests/test_wal_batch_fsync.h"
#include "tests/test_recent_indices.h"
#include "tests/test_parallel_snippets.h"
#include "tests/test_scanner_cancel.h"
#include "tests/test_network_mounts.h"
#include "tests/test_wal_replay_timeout.h"
#include "tests/test_recent_cache.h"
#include "tests/test_query_cancel.h"
#include "tests/test_critical_high.h"
#include "tests/test_rapid_typing.h"
#include "tests/test_path_table.h"
#include "tests/test_memory_optimizations.h"
#include "tests/test_batch_rescan.h"
#include "tests/test_wal_race_indexpersistence.h"
#include "tests/test_p2_fixes.h"
#include "tests/test_logger.h"
#include "tests/test_instance_lock.h"
#include "tests/test_wal_batch_replay.h"
#include "tests/test_wal_rename_chain.h"
#include "tests/test_rescan_debounce.h"
#include "tests/test_dirty_compaction.h"
#include "tests/test_compact_threshold.h"
#include "tests/test_paged_persistence.h"
#include "tests/test_content_wal_tracking.h"
#include "tests/test_content_compact_threshold.h"
#include "tests/test_content_compaction_guard.h"
#include "tests/test_fswatcher_eventid.h"
#include "tests/test_content_modtime.h"
#include "tests/test_http_engine_swap.h"
#include "tests/test_service_engine.h"
#include "tests/test_daemon_startup.h"
#include "tests/test_wal_inplace_replay.h"
#include "tests/test_event_driven_compaction.h"
#include "tests/test_query_perf.h"
#include "tests/test_record_dedup.h"
#include "tests/test_query_perf_10m.h"
#include "tests/test_path_trigram.h"
#include "tests/test_slash_query.h"
#include "tests/test_mcp_protocol.h"
#include "tests/test_string_pool.h"
#include "tests/test_simd_search.h"
#include "tests/test_paged_persistence_v5.h"
#include "tests/test_query_tokenizer.h"
#include "tests/test_query_parser.h"
#include "tests/test_query_filters.h"
#include "tests/test_query_date_filters.h"
#include "tests/test_structured_query.h"
#include "tests/test_query_modifiers.h"
#include "tests/test_regex_trigram.h"
#include "tests/test_fsevents_batch.h"
#include "tests/test_query_needs_analysis.h"
#include "tests/test_simd_batch_filter.h"
#include "tests/test_ast_structured_transform.h"
#include "tests/test_tilde_expansion.h"
#include "tests/test_whitespace_trim.h"
#include "tests/test_query_simplification.h"
#include "tests/test_preprocess_unified.h"
#include "tests/test_trigram_competition.h"
#include "tests/test_highlight_hints.h"
#include "tests/test_flat_persistence_v6.h"
#include "tests/test_case_trigram.h"
#include "tests/test_compiled_glob_evalterm.h"
#include "tests/test_extension_index.h"
#include "tests/test_re2_integration.h"
#include "tests/test_fsevents_search_latency.h"

// ═══════════════════════════════════════════════════════
//  Main
// ═══════════════════════════════════════════════════════

static void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options] [root_path]\n";
    std::cout << "  --fast             Run fast unit tests only (3, 3b-3e, 5, 7-7f, 8-22)\n";
    std::cout << "  --slow             Run slow integration tests only (1, 4, 6)\n";
    std::cout << "  --bench            Run performance benchmarks only (44, 46)\n";
    std::cout << "  --part <id>        Run specific part (can be repeated)\n";
    std::cout << "  --help             Show this help\n";
    std::cout << "  root_path          Root path for disk scan (default: /)\n";
    std::cout << "\nPart IDs: 1 (scan+query), 3 (mutation), 3b (path search),\n";
    std::cout << "  3c (metadata), 3d (compaction), 3e (ranking), 4 (FSEvents),\n";
    std::cout << "  5 (thread safety), 6 (end-to-end), 7 (compact+content),\n";
    std::cout << "  7b (destructor safety), 7c (WAL race), 7d (saveToFile),\n";
    std::cout << "  7e (scanner re-entry), 7f (content index), 8 (trigram index),\n";
    std::cout << "  9 (content index query benchmark), 10 (WAL batch fsync),\n";
    std::cout << "  11 (recentIndices), 12 (parallel snippets),\n";
    std::cout << "  13 (scanner cancel), 14 (WAL replay timeout),\n";
    std::cout << "  15 (WAL CRC), 16 (recent cache), 17 (query cancel), 18 (critical/high fixes),\n";
    std::cout << "  19 (rapid typing), 20 (path table), 21 (memory optimizations),\n";
    std::cout << "  22 (batch rescan), 23 (IndexPersistence WAL race),\n";
    std::cout << "  24 (P2 fixes), 25 (logger), 26 (instance lock),\n";
    std::cout << "  27 (WAL batch replay), 28 (WAL rename chain),\n";
    std::cout << "  29 (rescan debounce), 30 (dirty compaction),\n";
    std::cout << "  31 (compact threshold), 32 (paged persistence),\n";
    std::cout << "  33 (compaction timer), 34 (content WAL tracking),\n";
    std::cout << "  35 (content compact threshold),\n";
    std::cout << "  36 (content compaction guard),\n";
    std::cout << "  37 (FSWatcher eventId),\n";
    std::cout << "  38 (content modTime),\n";
    std::cout << "  39 (http engine swap),\n";
    std::cout << "  40 (service engine),\n";
    std::cout << "  41 (daemon startup),\n";
    std::cout << "  42 (WAL in-place replay),\n";
    std::cout << "  43 (event-driven compaction),\n";
    std::cout << "  44 (query performance),\n";
    std::cout << "  45 (record deduplication),\n";
    std::cout << "  46 (10M query performance),\n";
    std::cout << "  49 (MCP protocol),\n";
    std::cout << "  50 (string pool), 51 (SIMD search), 52 (lowerPathPool),\n";
    std::cout << "  53 (paged persistence v5),\n";
    std::cout << "  54 (query tokenizer), 55 (query parser),\n";
    std::cout << "  56 (query filters),\n";
    std::cout << "  57 (query date filters),\n";
    std::cout << "  58 (structured query),\n";
    std::cout << "  59 (query modifiers & macros),\n";
    std::cout << "  60 (regex trigram pre-filtering),\n";
    std::cout << "  61 (FSEvents batch mutation),\n";
    std::cout << "  62 (query needs analysis),\n";
    std::cout << "  63 (SIMD batch filter),\n";
    std::cout << "  64 (AST structured transform),\n";
    std::cout << "  65 (tilde expansion),\n";
    std::cout << "  66 (whitespace trim),\n";
    std::cout << "  67 (query simplification),\n";
    std::cout << "  68 (unified preprocessing),\n";
    std::cout << "  69 (trigram competition),\n";
    std::cout << "  70 (highlight hints),\n";
    std::cout << "  71 (flat persistence v6),\n";
    std::cout << "  72 (case trigram),\n";
    std::cout << "  73 (compiled glob evalTerm),\n";
    std::cout << "  74 (extension index),\n";
    std::cout << "  75 (RE2 integration),\n";
    std::cout << "  76 (FSEvents search latency)\n";
}

int main(int argc, char* argv[]) {
    std::string rootPath = "/";
    std::set<std::string> selectedParts;
    bool explicitSelection = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--fast") {
            explicitSelection = true;
            selectedParts.insert({"3", "3b", "3c", "3d", "3e", "5", "7", "7b", "7c", "7d", "7e", "7f", "8", "9", "10", "11", "12", "13", "14", "15", "16", "17", "18", "19", "20", "21", "22", "23", "24", "25", "26", "27", "28", "29", "30", "31", "32", "34", "35", "36", "37", "38", "39", "40", "41", "42", "43", "44", "45", "47", "48", "49", "50", "51", "52", "53", "54", "55", "56", "57", "58", "59", "60", "61", "62", "63", "64", "65", "66", "67", "68", "69", "70", "71", "72", "73", "74", "75", "76", "77"});
        } else if (arg == "--bench") {
            explicitSelection = true;
            selectedParts.insert({"44", "46"});
        } else if (arg == "--slow") {
            explicitSelection = true;
            selectedParts.insert({"1", "4", "6"});
        } else if (arg == "--part") {
            explicitSelection = true;
            if (i + 1 < argc) {
                selectedParts.insert(argv[++i]);
            } else {
                std::cerr << "Error: --part requires an argument\n";
                return 1;
            }
        } else if (arg[0] != '-') {
            rootPath = arg;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    // If no explicit selection, run all parts
    if (!explicitSelection) {
        selectedParts = {"1", "3", "3b", "3c", "3d", "3e", "4", "5", "6", "7", "7b", "7c", "7d", "7e", "7f", "8", "9", "10", "11", "12", "13", "14", "15", "16", "17", "18", "19", "20", "21", "22", "23", "24", "25", "26", "27", "28", "29", "30", "31", "32", "34", "35", "36", "37", "38", "39", "40", "41", "42", "43", "44", "45", "47", "48", "49", "50", "51", "52", "53", "54", "55", "56", "57", "58", "59", "60", "61", "62", "63", "64", "65", "66", "67", "68", "69", "70", "71", "73", "74", "76"};
    }

    // Validate root path if scan test is selected
    if (selectedParts.count("1")) {
        int testfd = open(rootPath.c_str(), O_RDONLY | O_DIRECTORY);
        if (testfd < 0) {
            std::cerr << "Error: cannot open '" << rootPath << "': " << strerror(errno) << "\n";
            return 1;
        }
        close(testfd);
    }

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════╗\n";
    std::cout << "║  MacEverything Full Test & Benchmark     ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n\n";

    if (explicitSelection) {
        std::cout << "  Selected parts:";
        for (const auto& p : selectedParts) std::cout << " " << p;
        std::cout << "\n\n";
    }

    if (selectedParts.count("1"))  runScanAndQueryBenchmark(rootPath);
    if (selectedParts.count("3"))  runMutationTests();
    if (selectedParts.count("3b")) runPathSearchTests();
    if (selectedParts.count("3c")) runIndexMetadataTests();
    if (selectedParts.count("3d")) runCompactionTests();
    if (selectedParts.count("3e")) runSearchRankingTests();
    if (selectedParts.count("4"))  runFSEventsTest();
    if (selectedParts.count("5"))  runThreadSafetyTest();
    if (selectedParts.count("6"))  runEndToEndTest();
    if (selectedParts.count("7"))  runCompactContentIndexTest();
    if (selectedParts.count("7b")) runDestructorSafetyTest();
    if (selectedParts.count("7c")) runWalRaceTest();
    if (selectedParts.count("7d")) runSaveToFileTest();
    if (selectedParts.count("7e")) runScannerReentryTest();
    if (selectedParts.count("7f")) runContentIndexTests();
    if (selectedParts.count("8"))  runTrigramIndexTests();
    if (selectedParts.count("9"))  runContentIndexQueryBenchmark();
    if (selectedParts.count("10")) runWalBatchFsyncBenchmark();
    if (selectedParts.count("11")) runRecentIndicesTests();
    if (selectedParts.count("12")) runParallelSnippetTests();
    if (selectedParts.count("13")) runScannerCancelTest();
    if (selectedParts.count("14")) runWalReplayTimeoutTest();
    if (selectedParts.count("15")) runWalCrcTests();
    if (selectedParts.count("16")) runRecentCacheTests();
    if (selectedParts.count("17")) runQueryCancelTests();
    if (selectedParts.count("18")) runCriticalHighTests();
    if (selectedParts.count("19")) runRapidTypingTest();
    if (selectedParts.count("20")) runPathTableTests();
    if (selectedParts.count("21")) runMemoryOptimizationTests();
    if (selectedParts.count("22")) runBatchRescanTests();
    if (selectedParts.count("23")) runWalRaceIndexPersistenceTest();
    if (selectedParts.count("24")) runP2FixTests();
    if (selectedParts.count("25")) runLoggerTests();
    if (selectedParts.count("26")) runInstanceLockTests();
    if (selectedParts.count("27")) runWalBatchReplayTests();
    if (selectedParts.count("28")) runWalRenameChainTest();
    if (selectedParts.count("77")) runNetworkMountsTests();
    if (selectedParts.count("29")) runRescanDebounceTests();
    if (selectedParts.count("30")) runDirtyCompactionTests();
    if (selectedParts.count("31")) runCompactThresholdTests();
    if (selectedParts.count("32")) runPagedPersistenceTests();
    if (selectedParts.count("34")) runContentWalTrackingTests();
    if (selectedParts.count("35")) runContentCompactThresholdTests();
    if (selectedParts.count("36")) runContentCompactionGuardTests();
    if (selectedParts.count("37")) runFSWatcherEventIdTests();
    if (selectedParts.count("38")) runContentModTimeTests();
    if (selectedParts.count("39")) runPart39();
    if (selectedParts.count("40")) runPart40();
    if (selectedParts.count("41")) runPart41();
    if (selectedParts.count("42")) runWalInplaceReplayTests();
    if (selectedParts.count("43")) runEventDrivenCompactionTests();
    if (selectedParts.count("44")) runQueryPerfBenchmarks();
    if (selectedParts.count("45")) runRecordDedupTests();
    if (selectedParts.count("46")) runLargeScalePerfBenchmarks();
    if (selectedParts.count("47")) runPathTrigramTests();
    if (selectedParts.count("48")) runSlashQueryTests();
    if (selectedParts.count("49")) runMcpProtocolTests();
    if (selectedParts.count("50")) runStringPoolTests();
    if (selectedParts.count("51")) runSIMDSearchTests();
    if (selectedParts.count("52")) runLowerPathPoolTests();
    if (selectedParts.count("53")) runPagedPersistenceV5Tests();
    if (selectedParts.count("54")) runQueryTokenizerTests();
    if (selectedParts.count("55")) runQueryParserTests();
    if (selectedParts.count("56")) runQueryFilterTests();
    if (selectedParts.count("57")) runQueryDateFilterTests();
    if (selectedParts.count("58")) runStructuredQueryTests();
    if (selectedParts.count("59")) runQueryModifierTests();
    if (selectedParts.count("60")) runRegexTrigramTests();
    if (selectedParts.count("61")) runFSEventsBatchTests();
    if (selectedParts.count("62")) runQueryNeedsAnalysisTests();
    if (selectedParts.count("63")) runSIMDBatchFilterTests();
    if (selectedParts.count("64")) runASTStructuredTransformTests();
    if (selectedParts.count("65")) runTildeExpansionTests();
    if (selectedParts.count("66")) runWhitespaceTrimTests();
    if (selectedParts.count("67")) runQuerySimplificationTests();
    if (selectedParts.count("68")) runPreprocessUnifiedTests();
    if (selectedParts.count("69")) runTrigramCompetitionTests();
    if (selectedParts.count("70")) runHighlightHintTests();
    if (selectedParts.count("71")) runFlatPersistenceV6Tests();
    if (selectedParts.count("72")) runCaseTrigramTests();
    if (selectedParts.count("73")) runCompiledGlobEvaltermTests();
    if (selectedParts.count("74")) runExtensionIndexTests();
    if (selectedParts.count("75")) runRE2IntegrationTests();
    if (selectedParts.count("76")) runFSEventsSearchLatencyTest();

    // ── Final Summary ──
    std::cout << "╔══════════════════════════════════════════╗\n";
    std::cout << "║  Final Summary                           ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n\n";
    std::cout << "  Tests passed: " << passed << "\n";
    std::cout << "  Tests failed: " << failed << "\n";
    std::cout << "  Result:       " << (failed == 0 ? "ALL PASSED ✓" : "SOME FAILED ✗") << "\n\n";

    return failed > 0 ? 1 : 0;
}
