/// MacEverything CLI daemon — headless file indexer with HTTP API.
/// Uses the same ServiceEngine as the GUI app.
///
/// Usage: maceverything-daemon [--port PORT] [--root PATH] [--cache-dir PATH] [--log-dir PATH]

#include "ServiceEngine.h"
#include "PathUtils.h"
#include "Logger.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dispatch/dispatch.h>
#include <string>

// ═══════════════════════════════════════════════════════
//  Globals for signal handling
// ═══════════════════════════════════════════════════════

static ServiceEngine* g_engine = nullptr;
static dispatch_source_t g_sigintSource = nullptr;
static dispatch_source_t g_sigtermSource = nullptr;

// ═══════════════════════════════════════════════════════
//  Argument parsing
// ═══════════════════════════════════════════════════════

struct DaemonOptions {
    uint16_t port = 19860;
    std::string root = "/";
    std::string cacheDir;
    std::string logDir;
};

static void printUsage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "Options:\n"
        "  --port PORT       HTTP server port (default: 19860)\n"
        "  --root PATH       Scan root directory (default: /)\n"
        "  --cache-dir PATH  Cache directory (default: ~/Library/Caches/com.maceverything.app)\n"
        "  --log-dir PATH    Log directory (default: ~/Library/Logs/MacEverything)\n"
        "  --help            Show this help\n",
        prog);
}

static DaemonOptions parseArgs(int argc, char* argv[]) {
    DaemonOptions opts;
    opts.cacheDir = PathUtils::getDefaultCachePath();
    opts.logDir = PathUtils::getDefaultLogPath();

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            opts.port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
            opts.root = argv[++i];
        } else if (strcmp(argv[i], "--cache-dir") == 0 && i + 1 < argc) {
            opts.cacheDir = argv[++i];
        } else if (strcmp(argv[i], "--log-dir") == 0 && i + 1 < argc) {
            opts.logDir = argv[++i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            printUsage(argv[0]);
            exit(1);
        }
    }
    return opts;
}

// ═══════════════════════════════════════════════════════
//  Signal handling via GCD dispatch sources
// ═══════════════════════════════════════════════════════

static void installSignalHandlers() {
    // Block SIGINT/SIGTERM from default handling — GCD sources take over
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);

    dispatch_queue_t mainQ = dispatch_get_main_queue();

    g_sigintSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, mainQ);
    g_sigtermSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, mainQ);

    auto shutdownHandler = ^{
        LOG_INFO("Daemon", "Received termination signal — shutting down");
        if (g_engine) {
            g_engine->shutdown();
            LOG_INFO("Logger", "=== Log session ended ===");
            me::Logger::instance().shutdown();
        }
        exit(0);
    };

    dispatch_source_set_event_handler(g_sigintSource, shutdownHandler);
    dispatch_source_set_event_handler(g_sigtermSource, shutdownHandler);

    dispatch_resume(g_sigintSource);
    dispatch_resume(g_sigtermSource);
}

// ═══════════════════════════════════════════════════════
//  Main
// ═══════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    // Disable buffering so output appears immediately (important for background mode)
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);

    DaemonOptions opts = parseArgs(argc, argv);

    // Initialize logger
    me::Logger::instance().init(opts.logDir, me::LogLevel::Info);
    LOG_INFO("Daemon", "=== MacEverything daemon starting ===");
    LOG_INFO("Daemon", "root=" << opts.root << " port=" << opts.port
             << " cache=" << opts.cacheDir);

    // Create ServiceEngine
    ServiceConfig config;
    config.scanRoot = opts.root;
    config.scanNetworkVolumes = true;
    config.cachePath = opts.cacheDir;
    config.logPath = opts.logDir;

    ServiceEngine engine(config);
    g_engine = &engine;

    // Install signal handlers
    installSignalHandlers();

    // Set up admin callbacks for HttpServer
    HttpServer::AdminCallbacks callbacks;
    callbacks.onRebuildIndex = [&engine] {
        // Trigger full rescan (not directly available — use startFullScan)
        LOG_INFO("Daemon", "Rebuild index requested via HTTP");
        engine.startFullScan([](uint32_t count, bool) {
            LOG_INFO("Daemon", "Rebuild index completed: " << count << " records");
        });
    };
    callbacks.onRebuildContentIndex = [&engine] {
        LOG_INFO("Daemon", "Rebuild content index requested via HTTP");
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            engine.rebuildContentIndex();
        });
    };
    callbacks.onSetContentConfig = [&engine](const std::vector<std::string>& exts, uint64_t maxSize) {
        auto ci = engine.safeContentIndex();
        if (!ci) return;
        ci->setExtensions(exts);
        ci->setMaxFileSize(maxSize);
    };
    callbacks.onGetContentExtensions = [&engine]() -> std::vector<std::string> {
        auto ci = engine.safeContentIndex();
        if (!ci) return {};
        return ci->getExtensions();
    };
    callbacks.onGetContentMaxFileSize = [&engine]() -> uint64_t {
        auto ci = engine.safeContentIndex();
        if (!ci) return 0;
        return ci->getMaxFileSize();
    };

    engine.adminCallbacks = std::move(callbacks);

    // Set up progress logging
    engine.onScanProgress = [](uint64_t scanned, uint64_t dirs) {
        if (scanned % 100000 == 0 && scanned > 0) {
            LOG_INFO("Daemon", "Scan progress: " << scanned << " files, " << dirs << " dirs");
        }
    };
    engine.onContentIndexProgress = [](uint64_t indexed, uint64_t total) {
        if (indexed % 1000 == 0 && indexed > 0) {
            LOG_INFO("Daemon", "Content indexing: " << indexed << "/" << total);
        }
    };
    engine.onContentIndexComplete = [](uint32_t totalIndexed) {
        LOG_INFO("Daemon", "Content indexing complete: " << totalIndexed << " files");
    };

    // Start incremental load (will fall back to full scan if no cache)
    engine.startIncremental([&opts](uint32_t totalRecords, bool didFullScan) {
        LOG_INFO("Daemon", "Ready: " << totalRecords << " records"
                 << (didFullScan ? " (full scan)" : " (cached)"));
        fprintf(stderr, "MacEverything daemon ready: %u records indexed\n", totalRecords);
    });

    // Start HTTP server
    engine.startHttpServer(opts.port);
    fprintf(stderr, "HTTP server listening on port %u\n", opts.port);

    // Enter GCD main event loop (required for FSEvents)
    dispatch_main();

    // dispatch_main() never returns, but just in case:
    return 0;
}
