#include "IndexWAL.h"
#include "Logger.h"
#include <cstring>
#include <unistd.h>
#include <array>

// ── CRC32 (ISO 3309 / zlib polynomial 0xEDB88320) ──
// Uses ARM CRC32 hardware instructions on Apple Silicon (~15 GB/s),
// falls back to slicing-by-4 software table on other architectures (~2 GB/s).

#if defined(__aarch64__) && defined(__APPLE__)
#include <arm_acle.h>

uint32_t IndexWAL::crc32Update(uint32_t prev, const void* data, size_t len) {
    auto* p = static_cast<const uint8_t*>(data);
    uint32_t crc = ~prev;

    // Process 8 bytes at a time using hardware CRC32 (ISO polynomial)
    while (len >= 8) {
        uint64_t val;
        memcpy(&val, p, 8);
        crc = __crc32d(crc, val);
        p += 8;
        len -= 8;
    }
    // Process remaining 4 bytes
    if (len >= 4) {
        uint32_t val;
        memcpy(&val, p, 4);
        crc = __crc32w(crc, val);
        p += 4;
        len -= 4;
    }
    // Process remaining bytes one at a time
    while (len-- > 0) {
        crc = __crc32b(crc, *p++);
    }

    return crc ^ 0xFFFFFFFF;
}

#else
// Software fallback: slicing-by-4 CRC32

static const uint32_t (*getCRC32Tables())[256] {
    static uint32_t tables[4][256];
    static bool initialized = false;
    if (!initialized) {
        // Build base table
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
            tables[0][i] = c;
        }
        // Build extended tables for slicing-by-4
        for (uint32_t i = 0; i < 256; i++) {
            tables[1][i] = (tables[0][i] >> 8) ^ tables[0][tables[0][i] & 0xFF];
            tables[2][i] = (tables[1][i] >> 8) ^ tables[0][tables[1][i] & 0xFF];
            tables[3][i] = (tables[2][i] >> 8) ^ tables[0][tables[2][i] & 0xFF];
        }
        initialized = true;
    }
    return tables;
}

uint32_t IndexWAL::crc32Update(uint32_t prev, const void* data, size_t len) {
    auto tables = getCRC32Tables();
    auto* p = static_cast<const uint8_t*>(data);
    uint32_t crc = ~prev;

    // Process 4 bytes at a time
    while (len >= 4) {
        uint32_t val;
        memcpy(&val, p, 4);
        crc ^= val;
        crc = tables[3][crc & 0xFF]
            ^ tables[2][(crc >> 8) & 0xFF]
            ^ tables[1][(crc >> 16) & 0xFF]
            ^ tables[0][(crc >> 24) & 0xFF];
        p += 4;
        len -= 4;
    }
    // Process remaining bytes
    while (len-- > 0) {
        crc = tables[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFF;
}

#endif

IndexWAL::~IndexWAL() {
    close();
}

bool IndexWAL::open(const std::string& walPath) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) return false;

    path_ = walPath;
    file_ = fopen(walPath.c_str(), "ab");
    if (!file_) return false;

    // H-3: Write magic+version header if this is a new (empty) file
    long pos = ftell(file_);
    if (pos == 0) {
        uint32_t magic = kMagic;
        uint32_t version = kVersion;
        if (fwrite(&magic, sizeof(uint32_t), 1, file_) != 1 ||
            fwrite(&version, sizeof(uint32_t), 1, file_) != 1) {
            fclose(file_);
            file_ = nullptr;
            return false;
        }
        fflush(file_);
        currentSize_ = 2 * sizeof(uint32_t); // header: magic + version
    } else {
        currentSize_ = static_cast<size_t>(pos);
    }

    entryCount_ = 0;
    return true;
}

bool IndexWAL::append(WALOp op, const std::string& fullPath, const FileRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_) return false;

    // H-6: Check WAL file size limit before writing
    if (currentSize_ >= kMaxWALSize) {
        return false;
    }

    // Build entry into a buffer for CRC32 computation
    std::string buf;
    uint8_t opByte = static_cast<uint8_t>(op);
    buf.append(reinterpret_cast<const char*>(&opByte), 1);

    uint32_t pathLen = static_cast<uint32_t>(fullPath.size());
    buf.append(reinterpret_cast<const char*>(&pathLen), sizeof(uint32_t));
    if (pathLen > 0) buf.append(fullPath.data(), pathLen);

    if (op == WALOp::Add || op == WALOp::Update) {
        // Serialize record fields into buffer
        uint32_t nameLen = static_cast<uint32_t>(record.name.size());
        buf.append(reinterpret_cast<const char*>(&nameLen), sizeof(uint32_t));
        if (nameLen > 0) buf.append(record.name.data(), nameLen);

        uint32_t rpathLen = static_cast<uint32_t>(record.path.size());
        buf.append(reinterpret_cast<const char*>(&rpathLen), sizeof(uint32_t));
        if (rpathLen > 0) buf.append(record.path.data(), rpathLen);

        buf.append(reinterpret_cast<const char*>(&record.type), sizeof(uint8_t));
        buf.append(reinterpret_cast<const char*>(&record.size), sizeof(uint64_t));
        int64_t mod = static_cast<int64_t>(record.modTime);
        buf.append(reinterpret_cast<const char*>(&mod), sizeof(int64_t));
        buf.append(reinterpret_cast<const char*>(&record.inode), sizeof(uint64_t));
        buf.append(reinterpret_cast<const char*>(&record.devId), sizeof(int32_t));
    }

    // Write entry + CRC32
    if (fwrite(buf.data(), 1, buf.size(), file_) != buf.size()) return false;
    uint32_t checksum = crc32(buf.data(), buf.size());
    if (fwrite(&checksum, sizeof(uint32_t), 1, file_) != 1) return false;

    currentSize_ += buf.size() + sizeof(uint32_t);

    fflush(file_);
    entryCount_++;
    unflushedCount_++;
    dirty_.store(true, std::memory_order_relaxed);

    // Batch fsync: only fsync every syncInterval_ entries
    if (syncInterval_ > 0 && unflushedCount_ >= syncInterval_) {
        fsync(fileno(file_));
        unflushedCount_ = 0;
    }

    return true;
}

std::vector<WALEntry> IndexWAL::readAll(const std::string& walPath) {
    std::vector<WALEntry> entries;

    FILE* f = fopen(walPath.c_str(), "rb");
    if (!f) return entries;

    // H-3: Verify magic+version header
    uint32_t magic = 0, version = 0;
    if (fread(&magic, sizeof(uint32_t), 1, f) != 1 ||
        fread(&version, sizeof(uint32_t), 1, f) != 1 ||
        magic != kMagic || version != kVersion) {
        // Legacy WAL without header — try reading from the beginning
        fseek(f, 0, SEEK_SET);
    }

    while (true) {
        // Record the start position to re-read the raw bytes for CRC verification
        long startPos = ftell(f);
        if (startPos < 0) break;

        WALEntry entry;

        uint8_t opByte;
        if (fread(&opByte, 1, 1, f) != 1) break;
        if (opByte < 1 || opByte > 3) break; // invalid op
        entry.op = static_cast<WALOp>(opByte);

        uint32_t pathLen;
        if (fread(&pathLen, sizeof(uint32_t), 1, f) != 1) break;
        if (pathLen > 65536) break; // sanity limit
        entry.fullPath.resize(pathLen);
        if (pathLen > 0 && fread(entry.fullPath.data(), 1, pathLen, f) != pathLen) break;

        if (entry.op == WALOp::Add || entry.op == WALOp::Update) {
            if (!readRecord(f, entry.record)) break;
        }

        // Read and verify CRC32
        long endPos = ftell(f);
        if (endPos < 0) break;
        size_t entryLen = static_cast<size_t>(endPos - startPos);

        uint32_t storedCRC;
        if (fread(&storedCRC, sizeof(uint32_t), 1, f) != 1) break;

        // Re-read entry bytes for CRC computation
        std::vector<uint8_t> rawBuf(entryLen);
        long afterCRC = ftell(f);
        fseek(f, startPos, SEEK_SET);
        if (fread(rawBuf.data(), 1, entryLen, f) != entryLen) break;
        fseek(f, afterCRC, SEEK_SET);

        uint32_t computedCRC = crc32(rawBuf.data(), rawBuf.size());
        if (computedCRC != storedCRC) {
            // H-4: Log CRC mismatch location for diagnostics
            LOG_ERROR("IndexWAL", "CRC mismatch at offset " << startPos
                      << ", recovered " << entries.size() << " entries");
            break;
        }

        entries.push_back(std::move(entry));
    }

    fclose(f);
    return entries;
}

void IndexWAL::sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ && unflushedCount_ > 0) {
        fsync(fileno(file_));
        unflushedCount_ = 0;
    }
}

void IndexWAL::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        if (unflushedCount_ > 0) {
            fsync(fileno(file_));
            unflushedCount_ = 0;
        }
        fclose(file_);
        file_ = nullptr;
    }
}

void IndexWAL::closeAndDelete() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        fclose(file_);
        file_ = nullptr;
    }
    if (!path_.empty()) {
        remove(path_.c_str());
    }
}

void IndexWAL::updatePath(const std::string& newPath) {
    std::lock_guard<std::mutex> lock(mutex_);
    path_ = newPath;
}

bool IndexWAL::writeRecord(FILE* f, const FileRecord& r) {
    uint32_t nameLen = static_cast<uint32_t>(r.name.size());
    if (fwrite(&nameLen, sizeof(uint32_t), 1, f) != 1) return false;
    if (nameLen > 0 && fwrite(r.name.data(), 1, nameLen, f) != nameLen) return false;

    uint32_t pathLen = static_cast<uint32_t>(r.path.size());
    if (fwrite(&pathLen, sizeof(uint32_t), 1, f) != 1) return false;
    if (pathLen > 0 && fwrite(r.path.data(), 1, pathLen, f) != pathLen) return false;

    if (fwrite(&r.type, sizeof(uint8_t), 1, f) != 1) return false;
    if (fwrite(&r.size, sizeof(uint64_t), 1, f) != 1) return false;
    int64_t mod = static_cast<int64_t>(r.modTime);
    if (fwrite(&mod, sizeof(int64_t), 1, f) != 1) return false;
    if (fwrite(&r.inode, sizeof(uint64_t), 1, f) != 1) return false;
    if (fwrite(&r.devId, sizeof(int32_t), 1, f) != 1) return false;

    return true;
}

bool IndexWAL::readRecord(FILE* f, FileRecord& r) {
    uint32_t nameLen;
    if (fread(&nameLen, sizeof(uint32_t), 1, f) != 1) return false;
    if (nameLen > 65536) return false;
    r.name.resize(nameLen);
    if (nameLen > 0 && fread(r.name.data(), 1, nameLen, f) != nameLen) return false;

    uint32_t pathLen;
    if (fread(&pathLen, sizeof(uint32_t), 1, f) != 1) return false;
    if (pathLen > 65536) return false;
    r.path.resize(pathLen);
    if (pathLen > 0 && fread(r.path.data(), 1, pathLen, f) != pathLen) return false;

    if (fread(&r.type, sizeof(uint8_t), 1, f) != 1) return false;
    if (fread(&r.size, sizeof(uint64_t), 1, f) != 1) return false;
    int64_t mod;
    if (fread(&mod, sizeof(int64_t), 1, f) != 1) return false;
    r.modTime = static_cast<time_t>(mod);
    if (fread(&r.inode, sizeof(uint64_t), 1, f) != 1) return false;
    if (fread(&r.devId, sizeof(int32_t), 1, f) != 1) return false;

    return true;
}
