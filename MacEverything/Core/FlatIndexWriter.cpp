#include "FlatIndexWriter.h"
#include "Logger.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <unordered_map>

FlatIndexWriter::FlatIndexWriter(const std::string& path) : path_(path) {}

bool FlatIndexWriter::exists() const {
    struct stat st;
    return stat(path_.c_str(), &st) == 0 && st.st_size > 0;
}

// ---------------------------------------------------------------------------
// Metadata section helpers
// ---------------------------------------------------------------------------

std::vector<uint8_t> FlatIndexWriter::serializeMetadata(const IndexMetadata& meta) {
    std::vector<uint8_t> out;
    auto put = [&](const void* p, size_t n) {
        auto* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    };
    uint32_t count = static_cast<uint32_t>(meta.extra.size());
    put(&count, sizeof(count));
    for (const auto& [key, value] : meta.extra) {
        uint32_t kLen = static_cast<uint32_t>(key.size());
        put(&kLen, sizeof(kLen));
        put(key.data(), kLen);
        uint32_t vLen = static_cast<uint32_t>(value.size());
        put(&vLen, sizeof(vLen));
        put(value.data(), vLen);
    }
    return out;
}

bool FlatIndexWriter::readMetadataSection(const uint8_t* data, size_t len, IndexMetadata& meta) {
    if (len < sizeof(uint32_t)) return false;
    size_t pos = 0;

    uint32_t count;
    memcpy(&count, data + pos, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    for (uint32_t i = 0; i < count; i++) {
        if (pos + sizeof(uint32_t) > len) return false;
        uint32_t kLen;
        memcpy(&kLen, data + pos, sizeof(uint32_t));
        pos += sizeof(uint32_t);
        if (pos + kLen > len) return false;
        std::string key(reinterpret_cast<const char*>(data + pos), kLen);
        pos += kLen;

        if (pos + sizeof(uint32_t) > len) return false;
        uint32_t vLen;
        memcpy(&vLen, data + pos, sizeof(uint32_t));
        pos += sizeof(uint32_t);
        if (pos + vLen > len) return false;
        std::string value(reinterpret_cast<const char*>(data + pos), vLen);
        pos += vLen;

        meta.extra[std::move(key)] = std::move(value);
    }
    return true;
}

// ---------------------------------------------------------------------------
// fullRewrite — write all storage as v2, then run the engine on its mappings
// ---------------------------------------------------------------------------

bool FlatIndexWriter::fullRewrite(SearchEngine& engine, const IndexMetadata& meta) {
    return engine.persistStorage(
        [&](const SearchEngine::StorageView& v) { return writeFile(v, meta); },
        [&](SearchEngine::Storage& st) { return mapFile(st, nullptr, false); });
}

namespace {

/// Sequential writer that tracks the offset and pads to kMapAlign.
struct SectionFile {
    int fd = -1;
    uint64_t pos = 0;
    bool ok = true;

    void write(const void* p, size_t n) {
        auto* b = static_cast<const uint8_t*>(p);
        while (ok && n > 0) {
            ssize_t w = ::write(fd, b, std::min<size_t>(n, 64u << 20));
            if (w <= 0) { ok = false; return; }
            b += w;
            n -= static_cast<size_t>(w);
            pos += static_cast<uint64_t>(w);
        }
    }
    void padTo(uint64_t target) {
        static const char zeros[4096] = {};
        while (ok && pos < target) write(zeros, std::min<uint64_t>(sizeof(zeros), target - pos));
    }
    void align() { padTo(me::alignToMapPage(pos)); }
};

} // namespace

bool FlatIndexWriter::writeFile(const SearchEngine::StorageView& v, const IndexMetadata& meta) {
    std::string tmpPath = path_ + ".tmp";
    int fd = open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        LOG_ERROR("FlatIndexWriter", "Cannot open " << tmpPath << " for writing");
        return false;
    }
    // Bypass page cache to avoid evicting hot search data
    fcntl(fd, F_NOCACHE, 1);

    // Directory lists: written from the engine when complete, else built now.
    DirPostings builtDirs;
    const DirPostings* dirs = &v.dirRecords;
    uint32_t pathCount = v.pathPool.entryCount();
    if (!v.dirRecordsComplete || v.dirRecords.size() < pathCount) {
        builtDirs = DirPostings::build(v.types, v.pathIndices, pathCount);
        dirs = &builtDirs;
    }
    std::vector<uint32_t> dirOffsets;
    dirOffsets.reserve(static_cast<size_t>(pathCount) + 1);
    dirOffsets.push_back(0);
    for (uint32_t p = 0; p < pathCount; p++) {
        dirOffsets.push_back(dirOffsets.back() + static_cast<uint32_t>((*dirs)[p].size()));
    }

    struct Blob { uint32_t id; const void* data; size_t bytes; };
    std::vector<Blob> blobs = {
        {kSectionNamesOrigBuf, v.origNamePool.rawBuffer(), v.origNamePool.rawSize()},
        {kSectionNamesOrigEntries, v.origNamePool.entries(), v.origNamePool.entryCount() * sizeof(StringPool::Entry)},
        {kSectionNamesLowerBuf, v.namePool.rawBuffer(), v.namePool.rawSize()},
        {kSectionNamesLowerEntries, v.namePool.entries(), v.namePool.entryCount() * sizeof(StringPool::Entry)},
        {kSectionPathBuf, v.pathPool.rawBuffer(), v.pathPool.rawSize()},
        {kSectionPathEntries, v.pathPool.entries(), v.pathPool.entryCount() * sizeof(StringPool::Entry)},
        {kSectionLowerPathBuf, v.lowerPathPool.rawBuffer(), v.lowerPathPool.rawSize()},
        {kSectionLowerPathEntries, v.lowerPathPool.entries(), v.lowerPathPool.entryCount() * sizeof(StringPool::Entry)},
        {kSectionPathIndices, v.pathIndices.data(), v.pathIndices.size() * sizeof(uint32_t)},
        {kSectionTypes, v.types.data(), v.types.size() * sizeof(uint8_t)},
        {kSectionSizes, v.sizes.data(), v.sizes.size() * sizeof(uint64_t)},
        {kSectionModTimes, v.modTimes.data(), v.modTimes.size() * sizeof(int64_t)},
        {kSectionInodes, v.inodes.data(), v.inodes.size() * sizeof(uint64_t)},
        {kSectionDevIds, v.devIds.data(), v.devIds.size() * sizeof(int32_t)},
        {kSectionPathLookupSlots, v.pathLookup.slotData(), v.pathLookup.slotCount() * sizeof(uint64_t)},
        {kSectionLowerPathLookupSlots, v.lowerPathLookup.slotData(), v.lowerPathLookup.slotCount() * sizeof(uint64_t)},
        {kSectionPathIndexSlots, v.pathIndex.slotData(), v.pathIndex.slotCount() * sizeof(uint64_t)},
        {kSectionDirOffsets, dirOffsets.data(), dirOffsets.size() * sizeof(uint32_t)},
    };
    std::vector<uint8_t> metaBlob = serializeMetadata(meta);

    const uint32_t sectionCount = static_cast<uint32_t>(blobs.size()) + 2; // + dir ids + metadata
    std::vector<SectionEntry> table(sectionCount);

    SectionFile out{fd};
    out.padTo(kHeaderSize + sectionCount * sizeof(SectionEntry));

    // Metadata goes first, unaligned: it is read, not mapped.
    uint32_t si = 0;
    table[si++] = {kSectionMetadataKV, 0, out.pos, static_cast<uint32_t>(metaBlob.size()),
                   IndexWAL::crc32(metaBlob.data(), metaBlob.size())};
    out.write(metaBlob.data(), metaBlob.size());

    for (const auto& b : blobs) {
        if (b.bytes > UINT32_MAX) { out.ok = false; break; }
        out.align();
        table[si++] = {b.id, 0, out.pos, static_cast<uint32_t>(b.bytes),
                       b.bytes ? IndexWAL::crc32(b.data, b.bytes) : IndexWAL::crc32(nullptr, 0)};
        out.write(b.data, b.bytes);
    }

    // Directory record ids, streamed list by list.
    out.align();
    {
        SectionEntry& e = table[si++];
        e = {kSectionDirIds, 0, out.pos, 0, 0};
        uint64_t start = out.pos;
        uint32_t crc = 0;
        for (uint32_t p = 0; p < pathCount && out.ok; p++) {
            DirPostings::List list = (*dirs)[p];
            if (list.empty()) continue;
            crc = IndexWAL::crc32Update(crc, list.data(), list.size_bytes());
            out.write(list.data(), list.size_bytes());
        }
        if (out.pos - start > UINT32_MAX) out.ok = false;
        e.byteLength = static_cast<uint32_t>(out.pos - start);
        e.crc32 = crc;
    }
    out.align();

    Header header{};
    memcpy(header.magic, kMagic, 4);
    header.version = kVersion;
    header.recordCount = v.origNamePool.entryCount();
    header.liveCount = v.liveCount;
    header.timestamp = meta.timestamp > 0 ? meta.timestamp : static_cast<int64_t>(time(nullptr));
    header.lastEventId = meta.lastEventId;
    header.sectionCount = sectionCount;
    header.tableCRC = IndexWAL::crc32(table.data(), table.size() * sizeof(SectionEntry));
    header.headerCRC = IndexWAL::crc32(&header, 36);

    bool ok = out.ok &&
        pwrite(fd, &header, sizeof(header), 0) == static_cast<ssize_t>(sizeof(header)) &&
        pwrite(fd, table.data(), table.size() * sizeof(SectionEntry), kHeaderSize) ==
            static_cast<ssize_t>(table.size() * sizeof(SectionEntry));
    ok = ok && fsync(fd) == 0;
    close(fd);

    if (!ok || rename(tmpPath.c_str(), path_.c_str()) != 0) {
        LOG_ERROR("FlatIndexWriter", "Failed to write " << tmpPath);
        remove(tmpPath.c_str());
        return false;
    }

    LOG_INFO("FlatIndexWriter", "v6 written: " << header.recordCount << " records ("
             << header.liveCount << " live), " << sectionCount << " sections, "
             << (out.pos >> 20) << " MB");
    return true;
}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------

bool FlatIndexWriter::load(SearchEngine& engine, IndexMetadata* outMeta) {
    loadedVersion_ = 0;
    FILE* f = fopen(path_.c_str(), "rb");
    if (!f) return false;
    Header header{};
    bool readOk = fread(&header, sizeof(Header), 1, f) == 1;
    fclose(f);
    if (!readOk || memcmp(header.magic, kMagic, 4) != 0) return false;

    if (header.version == kVersionPacked) {
        if (!loadPacked(engine, outMeta)) return false;
        loadedVersion_ = kVersionPacked;
        return true;
    }
    if (header.version != kVersion) {
        LOG_ERROR("FlatIndexWriter", "Unknown v6 version: " << header.version);
        return false;
    }

    SearchEngine::Storage st;
    IndexMetadata meta;
    if (!mapFile(st, &meta, true)) return false;
    uint32_t n = st.origNamePool.entryCount();
    engine.loadStorage(std::move(st));
    if (outMeta) *outMeta = std::move(meta);
    loadedVersion_ = kVersion;
    LOG_INFO("FlatIndexWriter", "v6 mapped: " << n << " records, lastEventId=" << header.lastEventId);
    return true;
}

bool FlatIndexWriter::mapFile(SearchEngine::Storage& st, IndexMetadata* outMeta, bool verify) const {
    int fd = open(path_.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct FdCloser { int fd; ~FdCloser() { close(fd); } } closer{fd};

    struct stat fst;
    if (fstat(fd, &fst) != 0) return false;
    uint64_t fileSize = static_cast<uint64_t>(fst.st_size);

    Header header{};
    if (pread(fd, &header, sizeof(header), 0) != static_cast<ssize_t>(sizeof(header))) return false;
    if (memcmp(header.magic, kMagic, 4) != 0 || header.version != kVersion) return false;
    if (header.headerCRC != IndexWAL::crc32(&header, 36)) {
        LOG_ERROR("FlatIndexWriter", "Header CRC mismatch");
        return false;
    }
    if (header.sectionCount == 0 || header.sectionCount > 256) return false;
    std::vector<SectionEntry> table(header.sectionCount);
    size_t tableBytes = table.size() * sizeof(SectionEntry);
    if (pread(fd, table.data(), tableBytes, kHeaderSize) != static_cast<ssize_t>(tableBytes)) return false;
    if (header.tableCRC != IndexWAL::crc32(table.data(), tableBytes)) {
        LOG_ERROR("FlatIndexWriter", "Section table CRC mismatch");
        return false;
    }
    std::unordered_map<uint32_t, SectionEntry> sections;
    for (const auto& e : table) sections[e.sectionId] = e;

    const uint32_t n = header.recordCount;

    // Locate a mappable section holding exactly `count` elements of T.
    auto mapSection = [&]<typename T>(uint32_t id, size_t count, Column<T>& out, const char* name) -> bool {
        auto it = sections.find(id);
        if (it == sections.end()) {
            LOG_ERROR("FlatIndexWriter", "Missing section: " << name);
            return false;
        }
        const SectionEntry& e = it->second;
        if (e.byteLength != count * sizeof(T) || e.offset % me::kMapAlign != 0 ||
            e.offset + me::alignToMapPage(e.byteLength) > fileSize) {
            LOG_ERROR("FlatIndexWriter", "Section " << name << " has bad bounds");
            return false;
        }
        if (!adoptMappedColumn(fd, e.offset, count, out)) {
            LOG_ERROR("FlatIndexWriter", "Failed to map section " << name);
            return false;
        }
        if (verify && IndexWAL::crc32(out.data(), e.byteLength) != e.crc32) {
            LOG_ERROR("FlatIndexWriter", "CRC mismatch for section " << name);
            return false;
        }
        return true;
    };
    auto sectionCount = [&](uint32_t id, size_t elemSize, size_t& count) -> bool {
        auto it = sections.find(id);
        if (it == sections.end() || it->second.byteLength % elemSize != 0) return false;
        count = it->second.byteLength / elemSize;
        return true;
    };
    auto mapPool = [&](uint32_t bufId, uint32_t entriesId, size_t entryCount, StringPool& pool,
                       const char* name) -> bool {
        size_t bufBytes;
        if (!sectionCount(bufId, 1, bufBytes)) return false;
        Column<char> buf;
        Column<StringPool::Entry> entries;
        if (!mapSection(bufId, bufBytes, buf, name) ||
            !mapSection(entriesId, entryCount, entries, name)) return false;
        if (verify) {
            for (const auto& en : entries) {
                if (static_cast<size_t>(en.offset) + en.length > bufBytes) {
                    LOG_ERROR("FlatIndexWriter", "String pool " << name << " entry out of range");
                    return false;
                }
            }
        }
        pool.loadRaw(std::move(buf), std::move(entries));
        return true;
    };
    auto mapSlots = [&](uint32_t id, FlatKeyIndex& index, const char* name) -> bool {
        size_t count;
        if (!sectionCount(id, sizeof(uint64_t), count)) return false;
        Column<uint64_t> slots;
        if (!mapSection(id, count, slots, name)) return false;
        if (!index.adoptSlots(std::move(slots))) {
            LOG_ERROR("FlatIndexWriter", "Invalid lookup table " << name);
            return false;
        }
        return true;
    };

    size_t pathCount;
    if (!sectionCount(kSectionPathEntries, sizeof(StringPool::Entry), pathCount)) return false;

    if (!mapPool(kSectionNamesOrigBuf, kSectionNamesOrigEntries, n, st.origNamePool, "NAMES_ORIG") ||
        !mapPool(kSectionNamesLowerBuf, kSectionNamesLowerEntries, n, st.namePool, "NAMES_LOWER") ||
        !mapPool(kSectionPathBuf, kSectionPathEntries, pathCount, st.pathPool, "PATH_POOL") ||
        !mapPool(kSectionLowerPathBuf, kSectionLowerPathEntries, pathCount, st.lowerPathPool, "LOWER_PATH_POOL") ||
        !mapSection(kSectionPathIndices, n, st.pathIndices, "PATH_INDICES") ||
        !mapSection(kSectionTypes, n, st.types, "TYPES") ||
        !mapSection(kSectionSizes, n, st.sizes, "SIZES") ||
        !mapSection(kSectionModTimes, n, st.modTimes, "MOD_TIMES") ||
        !mapSection(kSectionInodes, n, st.inodes, "INODES") ||
        !mapSection(kSectionDevIds, n, st.devIds, "DEV_IDS") ||
        !mapSlots(kSectionPathLookupSlots, st.pathLookup, "PATH_LOOKUP") ||
        !mapSlots(kSectionLowerPathLookupSlots, st.lowerPathLookup, "LOWER_PATH_LOOKUP") ||
        !mapSlots(kSectionPathIndexSlots, st.pathIndex, "PATH_INDEX")) {
        return false;
    }
    if (verify) {
        for (uint32_t pi : st.pathIndices) {
            if (pi >= pathCount) {
                LOG_ERROR("FlatIndexWriter", "Path index out of range");
                return false;
            }
        }
    }

    Column<uint32_t> dirOffsets, dirIds;
    size_t dirIdCount;
    if (!sectionCount(kSectionDirIds, sizeof(uint32_t), dirIdCount) ||
        !mapSection(kSectionDirOffsets, pathCount + 1, dirOffsets, "DIR_OFFSETS") ||
        !mapSection(kSectionDirIds, dirIdCount, dirIds, "DIR_IDS")) {
        return false;
    }
    if (verify) {
        for (size_t p = 0; p < pathCount; p++) {
            if (dirOffsets[p] > dirOffsets[p + 1]) return false;
        }
        for (uint32_t id : dirIds) {
            if (id >= n) return false;
        }
    }
    if (!st.dirRecords.adopt(std::move(dirOffsets), std::move(dirIds))) {
        LOG_ERROR("FlatIndexWriter", "Invalid directory lists");
        return false;
    }

    if (outMeta) {
        IndexMetadata meta;
        meta.formatVersion = 6;
        meta.timestamp = header.timestamp;
        meta.lastEventId = header.lastEventId;
        auto it = sections.find(kSectionMetadataKV);
        if (it != sections.end()) {
            std::vector<uint8_t> raw(it->second.byteLength);
            if (pread(fd, raw.data(), raw.size(), static_cast<off_t>(it->second.offset)) !=
                    static_cast<ssize_t>(raw.size()) ||
                IndexWAL::crc32(raw.data(), raw.size()) != it->second.crc32 ||
                !readMetadataSection(raw.data(), raw.size(), meta)) {
                LOG_ERROR("FlatIndexWriter", "Metadata section corrupt");
                return false;
            }
        }
        *outMeta = std::move(meta);
    }

    st.hasLookups = true;
    st.hasDirRecords = true;
    st.liveCount = header.liveCount;
    return true;
}

// ---------------------------------------------------------------------------
// loadPacked — version 1 files, read by copying
// ---------------------------------------------------------------------------

bool FlatIndexWriter::loadPacked(SearchEngine& engine, IndexMetadata* outMeta) {
    FILE* f = fopen(path_.c_str(), "rb");
    if (!f) return false;
    struct FileCloser { FILE* f; ~FileCloser() { fclose(f); } } closer{f};

    Header header{};
    if (fread(&header, sizeof(Header), 1, f) != 1) return false;
    if (header.headerCRC != IndexWAL::crc32(&header, 36)) {
        LOG_ERROR("FlatIndexWriter", "Header CRC mismatch");
        return false;
    }
    if (header.sectionCount == 0 || header.sectionCount > 256) return false;

    fseek(f, kHeaderSize, SEEK_SET);
    std::vector<SectionEntry> table(header.sectionCount);
    if (fread(table.data(), sizeof(SectionEntry), table.size(), f) != table.size()) return false;
    std::unordered_map<uint32_t, SectionEntry> sections;
    for (const auto& e : table) sections[e.sectionId] = e;

    auto readSection = [&](uint32_t id, std::vector<uint8_t>& out, const char* name) -> bool {
        auto it = sections.find(id);
        if (it == sections.end()) {
            LOG_ERROR("FlatIndexWriter", "Missing section: " << name);
            return false;
        }
        out.resize(it->second.byteLength);
        fseek(f, static_cast<long>(it->second.offset), SEEK_SET);
        if (fread(out.data(), 1, out.size(), f) != out.size() ||
            IndexWAL::crc32(out.data(), out.size()) != it->second.crc32) {
            LOG_ERROR("FlatIndexWriter", "CRC mismatch for section " << name);
            return false;
        }
        return true;
    };
    // bufSize(4) + buffer(bufSize) + entryCount(4) + entries
    auto readPool = [&](uint32_t id, StringPool& pool, const char* name) -> bool {
        std::vector<uint8_t> raw;
        if (!readSection(id, raw, name) || raw.size() < 8) return false;
        uint32_t bufSize;
        memcpy(&bufSize, raw.data(), 4);
        if (4 + static_cast<size_t>(bufSize) + 4 > raw.size()) return false;
        uint32_t entryCount;
        memcpy(&entryCount, raw.data() + 4 + bufSize, 4);
        size_t entryBytes = static_cast<size_t>(entryCount) * sizeof(StringPool::Entry);
        if (8 + static_cast<size_t>(bufSize) + entryBytes > raw.size()) return false;
        Column<char> buffer(bufSize);
        if (bufSize) memcpy(buffer.data(), raw.data() + 4, bufSize);
        Column<StringPool::Entry> entries(entryCount);
        if (entryCount) memcpy(entries.data(), raw.data() + 8 + bufSize, entryBytes);
        pool.loadRaw(std::move(buffer), std::move(entries));
        return true;
    };
    auto readArray = [&]<typename T>(uint32_t id, Column<T>& vec, uint32_t count, const char* name) -> bool {
        auto it = sections.find(id);
        if (it == sections.end() || it->second.byteLength != count * sizeof(T)) {
            LOG_ERROR("FlatIndexWriter", "Section " << name << " missing or size mismatch");
            return false;
        }
        vec.resize(count);
        fseek(f, static_cast<long>(it->second.offset), SEEK_SET);
        if (fread(vec.data(), 1, it->second.byteLength, f) != it->second.byteLength ||
            IndexWAL::crc32(vec.data(), it->second.byteLength) != it->second.crc32) {
            LOG_ERROR("FlatIndexWriter", "CRC mismatch for section " << name);
            return false;
        }
        return true;
    };

    SearchEngine::Storage st;
    if (!readPool(kSectionNamesOrig, st.origNamePool, "NAMES_ORIG") ||
        !readPool(kSectionNamesLower, st.namePool, "NAMES_LOWER") ||
        !readPool(kSectionPathPool, st.pathPool, "PATH_POOL") ||
        !readPool(kSectionLowerPathPool, st.lowerPathPool, "LOWER_PATH_POOL")) {
        return false;
    }
    uint32_t n = st.origNamePool.entryCount();
    if (!readArray(kSectionPathIndices, st.pathIndices, n, "PATH_INDICES") ||
        !readArray(kSectionTypes, st.types, n, "TYPES") ||
        !readArray(kSectionSizes, st.sizes, n, "SIZES") ||
        !readArray(kSectionModTimes, st.modTimes, n, "MOD_TIMES") ||
        !readArray(kSectionInodes, st.inodes, n, "INODES") ||
        !readArray(kSectionDevIds, st.devIds, n, "DEV_IDS")) {
        return false;
    }

    IndexMetadata meta;
    meta.formatVersion = 6;
    meta.timestamp = header.timestamp;
    meta.lastEventId = header.lastEventId;
    if (sections.count(kSectionMetadataKV)) {
        std::vector<uint8_t> raw;
        if (readSection(kSectionMetadataKV, raw, "METADATA")) {
            readMetadataSection(raw.data(), raw.size(), meta);
        }
    }

    engine.loadStorage(std::move(st));
    if (outMeta) *outMeta = std::move(meta);
    LOG_INFO("FlatIndexWriter", "v6 (packed) loaded: " << n << " records, lastEventId="
             << header.lastEventId);
    return true;
}
