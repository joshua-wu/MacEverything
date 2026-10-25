#include "DirectoryScanner.h"
#include "Logger.h"
#include <sys/attr.h>
#include <sys/vnode.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <memory>
#include <sys/stat.h>

static constexpr size_t ATTR_BUF_SIZE = 1 * 1024 * 1024; // 1 MB per-thread buffer

void DirectoryScanner::scan(const std::string& rootPath) {
    // Reset state so scanner can be reused across multiple scans
    done_.store(false, std::memory_order_relaxed);
    cancelled_.store(false, std::memory_order_relaxed);
    failed_.store(false, std::memory_order_relaxed);
    activeTasks_.store(0, std::memory_order_relaxed);

    // An empty or unreachable root would produce an empty result set, which
    // callers reconcile against by tombstoning everything under the prefix —
    // for an empty root that means the ENTIRE index. Refuse to run instead.
    if (rootPath.empty()) {
        LOG_ERROR("Scanner", "Refusing to scan: root path is empty");
        failed_.store(true, std::memory_order_relaxed);
        done_.store(true, std::memory_order_relaxed);
        return;
    }
    struct stat probeStat;
    if (stat(rootPath.c_str(), &probeStat) != 0) {
        LOG_ERROR("Scanner", "Refusing to scan: root unreachable: " << rootPath
            << " (" << strerror(errno) << ")");
        failed_.store(true, std::memory_order_relaxed);
        done_.store(true, std::memory_order_relaxed);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(dedupMutex_);
        visitedDirs_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        while (!workQueue_.empty()) workQueue_.pop();
    }
    threadResults_.clear();
    stats_.fileCount.store(0, std::memory_order_relaxed);
    stats_.dirCount.store(0, std::memory_order_relaxed);
    stats_.symlinkCount.store(0, std::memory_order_relaxed);
    stats_.otherCount.store(0, std::memory_order_relaxed);
    stats_.errorCount.store(0, std::memory_order_relaxed);

    // Determine root device ID to skip cross-mount directories (autofs, devfs, etc.)
    struct stat rootStat;
    if (stat(rootPath.c_str(), &rootStat) == 0) {
        rootDevId_ = rootStat.st_dev;
    } else {
        rootDevId_ = 0; // disable cross-mount filtering if stat fails
    }

    unsigned numThreads = std::thread::hardware_concurrency();
    if (numThreads < 4) numThreads = 4;
    if (numThreads > 32) numThreads = 32;

    LOG_INFO("Scanner", "Scanning from: " << rootPath
        << " (using " << numThreads << " threads, rootDev=" << rootDevId_ << ")");

    threadResults_.resize(numThreads);
    for (auto& v : threadResults_) {
        v.reserve(50000);
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        workQueue_.push(rootPath);
    }

    std::vector<std::thread> threads;
    threads.reserve(numThreads);
    for (unsigned i = 0; i < numThreads; i++) {
        threads.emplace_back(&DirectoryScanner::workerThread, this, static_cast<int>(i));
    }

    for (auto& t : threads) {
        t.join();
    }
}

std::vector<FileRecord> DirectoryScanner::takeResults() {
    std::vector<FileRecord> merged;
    size_t total = 0;
    for (auto& v : threadResults_) total += v.size();
    merged.reserve(total);
    for (auto& v : threadResults_) {
        merged.insert(merged.end(),
                      std::make_move_iterator(v.begin()),
                      std::make_move_iterator(v.end()));
        v.clear();
        v.shrink_to_fit();
    }
    threadResults_.clear();
    return merged;
}

void DirectoryScanner::workerThread(int threadIndex) {
    auto buffer = std::make_unique<char[]>(ATTR_BUF_SIZE);

    for (;;) {
        std::string dirPath;

        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCV_.wait(lock, [this] {
                return !workQueue_.empty()
                    || cancelled_.load(std::memory_order_relaxed)
                    || (activeTasks_.load(std::memory_order_acquire) == 0 && workQueue_.empty());
            });

            if (cancelled_.load(std::memory_order_relaxed)) {
                done_ = true;
                queueCV_.notify_all();
                return;
            }

            if (workQueue_.empty() && activeTasks_.load(std::memory_order_acquire) == 0) {
                done_ = true;
                queueCV_.notify_all();
                return;
            }

            if (done_) return;

            dirPath = std::move(workQueue_.front());
            workQueue_.pop();
            // INVARIANT: activeTasks_ is incremented inside queueMutex_ (before
            // popping work), and decremented outside the lock (after scanDirectory
            // returns and any new work has been pushed under queueMutex_). This
            // ensures the termination condition (activeTasks_==0 && workQueue_.empty())
            // cannot fire while discoverable work remains.
            activeTasks_.fetch_add(1, std::memory_order_acq_rel);
        }

        scanDirectory(dirPath, buffer.get(), threadIndex);

        activeTasks_.fetch_sub(1, std::memory_order_acq_rel);
        queueCV_.notify_all();
    }
}

void DirectoryScanner::scanDirectory(const std::string& dirPath, char* buffer, int threadIndex) {
    if (cancelled_.load(std::memory_order_relaxed)) return;

    int dirfd = open(dirPath.c_str(), O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) {
        if (errno == EACCES || errno == EPERM) {
            stats_.errorCount.fetch_add(1, std::memory_order_relaxed);
        } else if (errno != ENOENT && errno != ENOTDIR) {
            stats_.errorCount.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    struct attrlist attrList;
    memset(&attrList, 0, sizeof(attrList));
    attrList.bitmapcount = ATTR_BIT_MAP_COUNT;
    attrList.commonattr = ATTR_CMN_RETURNED_ATTRS
                        | ATTR_CMN_NAME
                        | ATTR_CMN_ERROR
                        | ATTR_CMN_DEVID
                        | ATTR_CMN_OBJTYPE
                        | ATTR_CMN_MODTIME
                        | ATTR_CMN_FILEID;
    attrList.fileattr = ATTR_FILE_DATALENGTH;

    // Collect subdirectories locally, then batch-push to work queue
    // to reduce queueMutex_ contention (one lock per batch, not per directory).
    std::vector<std::string> pendingDirs;

    for (;;) {
        if (cancelled_.load(std::memory_order_relaxed)) break;

        int retcount = getattrlistbulk(dirfd, &attrList, buffer, ATTR_BUF_SIZE, FSOPT_NOFOLLOW);

        if (retcount == -1) {
            if (errno == EACCES || errno == EPERM) {
                stats_.errorCount.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }
        if (retcount == 0) {
            break;
        }

        char* entry = buffer;
        for (int i = 0; i < retcount; i++) {
            char* field = entry;

            // 1. Entry length
            uint32_t entryLength;
            memcpy(&entryLength, field, sizeof(uint32_t));
            char* nextEntry = entry + entryLength;
            field += sizeof(uint32_t);

            // 2. Returned attributes
            attribute_set_t returned;
            memcpy(&returned, field, sizeof(attribute_set_t));
            field += sizeof(attribute_set_t);

            // 3. Error (special position after returned_attrs)
            uint32_t error = 0;
            if (returned.commonattr & ATTR_CMN_ERROR) {
                memcpy(&error, field, sizeof(uint32_t));
                field += sizeof(uint32_t);
            }

            // 4. Name reference (ATTR_CMN_NAME = 0x01)
            char* nameRefPtr = nullptr;
            attrreference_t nameRef = {};
            if (returned.commonattr & ATTR_CMN_NAME) {
                nameRefPtr = field;
                memcpy(&nameRef, field, sizeof(attrreference_t));
                field += sizeof(attrreference_t);
            }

            // 5. Device ID (ATTR_CMN_DEVID = 0x02)
            dev_t devid = 0;
            if (returned.commonattr & ATTR_CMN_DEVID) {
                memcpy(&devid, field, sizeof(dev_t));
                field += sizeof(dev_t);
            }

            // 6. Object type (ATTR_CMN_OBJTYPE = 0x08)
            fsobj_type_t objtype = VNON;
            if (returned.commonattr & ATTR_CMN_OBJTYPE) {
                memcpy(&objtype, field, sizeof(fsobj_type_t));
                field += sizeof(fsobj_type_t);
            }

            // 7. Modification time (ATTR_CMN_MODTIME = 0x400)
            struct timespec modtime = {};
            if (returned.commonattr & ATTR_CMN_MODTIME) {
                memcpy(&modtime, field, sizeof(struct timespec));
                field += sizeof(struct timespec);
            }

            // 8. File ID (ATTR_CMN_FILEID = 0x02000000)
            uint64_t fileid = 0;
            if (returned.commonattr & ATTR_CMN_FILEID) {
                memcpy(&fileid, field, sizeof(uint64_t));
                field += sizeof(uint64_t);
            }

            // 9. Data length (ATTR_FILE_DATALENGTH = 0x200, file attr, only for VREG)
            off_t datalength = 0;
            if (returned.fileattr & ATTR_FILE_DATALENGTH) {
                memcpy(&datalength, field, sizeof(off_t));
                field += sizeof(off_t);
            }

            // Skip entries with errors
            if (error != 0) {
                entry = nextEntry;
                continue;
            }

            // Extract name string
            if (!nameRefPtr) {
                entry = nextEntry;
                continue;
            }
            const char* name = nameRefPtr + nameRef.attr_dataoffset;
            if (name[0] == '\0') {
                entry = nextEntry;
                continue;
            }

            // Process entry
            if (objtype == VDIR) {
                // Skip cross-mount directories (autofs, devfs, NFS, etc.)
                // These can block indefinitely on open() or produce irrelevant results.
                if (rootDevId_ != 0 && devid != rootDevId_) {
                    entry = nextEntry;
                    continue;
                }

                if (tryVisitDirectory(devid, fileid)) {
                    std::string childPath = dirPath;
                    if (childPath.back() != '/') childPath += '/';
                    childPath += name;

                    // Detect .app bundles — record as type 5, skip recursion
                    size_t nameLen = strlen(name);
                    bool isAppBundle = (nameLen > 4 &&
                        name[nameLen-4] == '.' &&
                        tolower(name[nameLen-3]) == 'a' &&
                        tolower(name[nameLen-2]) == 'p' &&
                        tolower(name[nameLen-1]) == 'p');

                    if (!isAppBundle) {
                        pendingDirs.push_back(std::move(childPath));
                    }

                    stats_.dirCount.fetch_add(1, std::memory_order_relaxed);
                    threadResults_[threadIndex].push_back({name, dirPath,
                        static_cast<uint8_t>(isAppBundle ? 5 : 2),
                        0, modtime.tv_sec, fileid, static_cast<int32_t>(devid)});
                }
            } else if (objtype == VREG) {
                stats_.fileCount.fetch_add(1, std::memory_order_relaxed);
                threadResults_[threadIndex].push_back({name, dirPath, 1, static_cast<uint64_t>(datalength), modtime.tv_sec, fileid, static_cast<int32_t>(devid)});
            } else if (objtype == VLNK) {
                stats_.symlinkCount.fetch_add(1, std::memory_order_relaxed);
                threadResults_[threadIndex].push_back({name, dirPath, 3, 0, modtime.tv_sec, fileid, static_cast<int32_t>(devid)});
            } else {
                stats_.otherCount.fetch_add(1, std::memory_order_relaxed);
                threadResults_[threadIndex].push_back({name, dirPath, 4, 0, modtime.tv_sec, fileid, static_cast<int32_t>(devid)});
            }

            entry = nextEntry;
        }
    }

    close(dirfd);

    // Batch-push all discovered subdirectories in a single lock acquisition
    if (!pendingDirs.empty()) {
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            for (auto& dir : pendingDirs) {
                workQueue_.push(std::move(dir));
            }
        }
        queueCV_.notify_all();
    }
}

bool DirectoryScanner::tryVisitDirectory(dev_t dev, uint64_t ino) {
    InodeKey key{dev, ino};
    std::lock_guard<std::mutex> lock(dedupMutex_);
    return visitedDirs_.insert(key).second;
}
