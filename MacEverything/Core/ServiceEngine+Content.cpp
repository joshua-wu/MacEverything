#include "ServiceEngine.h"
#include "Logger.h"
#include <filesystem>
#include <unordered_set>
#include <dispatch/dispatch.h>

namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════
//  Content persistence setup
// ═══════════════════════════════════════════════════════

void ServiceEngine::setupContentPersistence() {
    auto contentIndex = safeContentIndex();
    auto engine = safeEngine();
    if (!contentIndex || !engine) return;

    std::string cacheDir = config_.cachePath;
    fs::create_directories(cacheDir);

    std::string basePath = cacheDir + "/content_index.bin";
    std::string walPath  = cacheDir + "/content_index.wal";

    auto newContentPersistence = std::make_shared<ContentIndexPersistence>(contentIndex, basePath, walPath);
    setContentPersistence(newContentPersistence);
    newContentPersistence->load();

    // Prune content entries whose fileIndex no longer points to a regular file
    // in the search engine.  This handles accumulated drift from search engine
    // compactions that remapped indices after the content base file was saved.
    {
        uint32_t total = engine->recordCount();
        std::unordered_set<uint32_t> validFileIndices;
        validFileIndices.reserve(total);
        for (uint32_t i = 0; i < total; i++) {
            FileRecord rec = engine->getRecord(i);
            if (rec.type == 1) {
                validFileIndices.insert(i);
            }
        }
        uint32_t pruned = contentIndex->pruneStaleEntries(validFileIndices);
        if (pruned > 0) {
            LOG_INFO("ServiceEngine", "Pruned " << pruned
                      << " stale content entries after load");
            // Save the pruned state directly to base file.  WAL isn't attached
            // yet, so compact() would skip — write the base file instead.
            contentIndex->saveToFile(basePath);
        }
    }

    newContentPersistence->attachWAL();
    newContentPersistence->startAutoCompaction(300.0);

    // Wire content persistence into IndexPersistence so fullCompact() can
    // flush content index after remapping file indices.
    auto persistence = safePersistence();
    if (persistence) {
        persistence->setContentIndexPersistence(newContentPersistence);
    }
}

// ═══════════════════════════════════════════════════════
//  Content indexing (parallel via dispatch_apply)
// ═══════════════════════════════════════════════════════

void ServiceEngine::startContentIndexing() {
    auto engine = safeEngine();
    auto contentIndex = safeContentIndex();
    if (!engine || !contentIndex) return;

    isContentIndexing_.store(true, std::memory_order_relaxed);
    cancelContentIndexing_.store(false, std::memory_order_relaxed);
    uint64_t myGeneration = contentIndexGeneration_.load(std::memory_order_acquire);

    LOG_INFO("ServiceEngine", "Content indexing started");

    auto contentPersistence = safeContentPersistence();

    dispatch_group_async(backgroundGroup_, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        auto contentStart = std::chrono::steady_clock::now();

        // Build lightweight list of (index, fullPath, modTime) for regular files.
        // Two-phase: if content index already has entries (restart), check only
        // those files (incremental). Otherwise full scan (first run).
        struct FileEntry { uint32_t idx; std::string fullPath; time_t modTime; };
        auto fileEntries = std::make_shared<std::vector<FileEntry>>();
        {
            auto indexedIndices = contentIndex->getIndexedFileIndices();
            bool incremental = !indexedIndices.empty();

            if (incremental) {
                // Incremental mode: only check already-indexed files
                fileEntries->reserve(indexedIndices.size());
                engine->forEachRecordWithPath(indexedIndices, [&](uint32_t idx, const FileRecord& r, const std::string& path) {
                    if (r.type != 1) return;
                    fileEntries->push_back({idx, SearchEngine::makeFullPath(path, r.name), r.modTime});
                });
                LOG_INFO("ServiceEngine", "Content indexing: incremental mode, "
                         << fileEntries->size() << " previously-indexed files to check");
            } else {
                // Full scan: first run, no persisted content index
                // Only files with a configured extension: building a full
                // path for every record would allocate hundreds of MB.
                uint32_t total = engine->recordCount();
                engine->forEachRecordInRange(0, total, [&](uint32_t idx, const FileRecord& r, const std::string& path) {
                    if (r.type != 1 || !contentIndex->hasAllowedExtension(r.name)) return;
                    fileEntries->push_back({idx, SearchEngine::makeFullPath(path, r.name), r.modTime});
                });
                fileEntries->shrink_to_fit();
                LOG_INFO("ServiceEngine", "Content indexing: full scan mode, "
                         << fileEntries->size() << " regular files to index");
            }
        }

        uint32_t total = static_cast<uint32_t>(fileEntries->size());
        auto indexed = std::make_shared<std::atomic<uint32_t>>(0);
        auto skipped = std::make_shared<std::atomic<uint32_t>>(0);
        auto lastReported = std::make_shared<std::atomic<uint32_t>>(0);

        dispatch_queue_t concurrentQ = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
        const auto& entries = *fileEntries;

        dispatch_apply(total, concurrentQ, ^(size_t i) {
            if (this->shuttingDown_.load(std::memory_order_relaxed)) return;
            if (this->cancelContentIndexing_.load(std::memory_order_relaxed)) return;
            if (this->contentIndexGeneration_.load(std::memory_order_acquire) != myGeneration) return;

            const auto& entry = entries[i];
            bool didIndex = contentIndex->indexFile(entry.idx, entry.fullPath, entry.modTime);

            if (didIndex && contentPersistence) {
                ContentFileInfo info;
                if (contentIndex->getFileInfo(entry.idx, info)) {
                    contentPersistence->walAppendAdd(entry.idx, info.contentHash, info.trigrams, info.lastModTime);
                }
            } else if (!didIndex && contentIndex->isFileIndexed(entry.idx)) {
                skipped->fetch_add(1, std::memory_order_relaxed);
            }

            uint32_t current = indexed->fetch_add(1, std::memory_order_relaxed) + 1;

            // Report progress every 500 files
            if (current - lastReported->load(std::memory_order_relaxed) >= 500) {
                lastReported->store(current, std::memory_order_relaxed);
                if (this->onContentIndexProgress) {
                    this->onContentIndexProgress(current, total);
                }
            }
        });

        auto contentElapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - contentStart).count();
        uint32_t totalIndexed = contentIndex->indexedFileCount();
        uint32_t skippedCount = skipped->load(std::memory_order_relaxed);
        LOG_INFO("ServiceEngine", "Content indexing completed: "
                 << totalIndexed << " files (" << skippedCount
                 << " skipped by modTime) in " << contentElapsed << "s");

        this->isContentIndexing_.store(false, std::memory_order_relaxed);
        dispatch_semaphore_signal(this->contentIndexingSemaphore_);

        if (this->onContentIndexComplete) {
            this->onContentIndexComplete(totalIndexed);
        }
    });
}

// ═══════════════════════════════════════════════════════
//  Rebuild content index (cancel in-flight, clear, re-index)
// ═══════════════════════════════════════════════════════

void ServiceEngine::rebuildContentIndex() {
    auto engine = safeEngine();
    auto contentIndex = safeContentIndex();
    if (!engine || !contentIndex) return;

    // Cancel in-flight content indexing and wait
    contentIndexGeneration_.fetch_add(1, std::memory_order_acq_rel);
    if (isContentIndexing_.load(std::memory_order_relaxed)) {
        cancelContentIndexing_.store(true, std::memory_order_relaxed);
        dispatch_semaphore_wait(contentIndexingSemaphore_,
                                dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    }

    // Stop old content persistence
    {
        auto oldCP = safeContentPersistence();
        if (oldCP) {
            oldCP->stopAutoCompactionAndWait();
        }
        setContentPersistence(nullptr);
    }

    // Replace contentIndex under exclusive lock
    {
        auto exts = contentIndex->getExtensions();
        auto maxSize = contentIndex->getMaxFileSize();

        auto newIndex = std::make_shared<ContentIndex>();
        newIndex->setExtensions(exts);
        newIndex->setMaxFileSize(maxSize);

        std::unique_lock lock(contentMutex_);
        contentIndex_ = newIndex;
    }

    // Re-setup persistence and re-index
    setupContentPersistence();
    startContentIndexing();
}

// ═══════════════════════════════════════════════════════
//  Per-file content update (called from FSEvents path)
// ═══════════════════════════════════════════════════════

void ServiceEngine::updateContentForPath(
    const std::string& fullPath, bool removed,
    std::shared_ptr<SearchEngine> engine)
{
    auto contentIndex = safeContentIndex();
    if (!engine || !contentIndex) return;

    auto contentPersistence = safeContentPersistence();

    if (removed) {
        uint32_t fileIndex = engine->indexForPath(fullPath);
        if (fileIndex != UINT32_MAX && contentIndex->isFileIndexed(fileIndex)) {
            contentIndex->removeFile(fileIndex);
            if (contentPersistence) {
                contentPersistence->walAppendRemove(fileIndex);
            }
        }
    } else {
        uint32_t fileIndex = engine->indexForPath(fullPath);
        if (fileIndex != UINT32_MAX) {
            time_t modTime = 0;
            engine->forEachRecordWithPath({fileIndex}, [&](uint32_t, const FileRecord& r, const std::string&) {
                modTime = r.modTime;
            });
            bool didIndex = contentIndex->indexFile(fileIndex, fullPath, modTime);
            if (didIndex && contentPersistence) {
                ContentFileInfo info;
                if (contentIndex->getFileInfo(fileIndex, info)) {
                    contentPersistence->walAppendAdd(fileIndex, info.contentHash, info.trigrams, info.lastModTime);
                }
            }
        }
    }
}
