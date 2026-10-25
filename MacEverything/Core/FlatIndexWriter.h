#pragma once
#include "SearchEngine.h"
#include "IndexWAL.h"  // for IndexWAL::crc32()
#include <string>
#include <cstdint>
#include <vector>

/// Flat SoA persistence for SearchEngine (index.v6).
///
/// Single-file format:
///   Header (64 bytes): magic + version + counts + lastEventId + CRCs
///   Section Table: sectionCount * 24 bytes
///   Sections: SoA column data, each with its own CRC32
///
/// Version 2 stores every column, string pool, lookup table and directory
/// list as a raw array starting on a kMapAlign boundary, so load() maps them
/// privately instead of reading them: the index costs page cache, not process
/// memory, until a page is modified. Version 1 (packed sections) still loads,
/// by copying.
class FlatIndexWriter {
public:
    explicit FlatIndexWriter(const std::string& path);

    /// Load records from the file into engine.
    /// Phase 1 only — secondary indices are deferred to Phase 2.
    /// Returns false if file doesn't exist or is corrupt.
    bool load(SearchEngine& engine, IndexMetadata* outMeta);

    /// Full rewrite of the file from the engine's storage, after which the
    /// engine runs on mappings of the new file. Uses tmp + fsync + rename.
    bool fullRewrite(SearchEngine& engine, const IndexMetadata& meta);

    /// Whether the file exists.
    bool exists() const;

    /// Format version of the last successful load() (0 if none).
    uint32_t loadedVersion() const { return loadedVersion_; }

    const std::string& path() const { return path_; }

    // --- Format constants ---
    static constexpr char kMagic[4] = {'M', 'E', 'V', '6'};
    static constexpr uint32_t kVersion = 2;
    static constexpr uint32_t kVersionPacked = 1;
    static constexpr uint32_t kHeaderSize = 64;

    // Section IDs shared by both versions
    static constexpr uint32_t kSectionPathIndices    = 5;
    static constexpr uint32_t kSectionTypes          = 6;
    static constexpr uint32_t kSectionSizes          = 7;
    static constexpr uint32_t kSectionModTimes       = 8;
    static constexpr uint32_t kSectionInodes         = 9;
    static constexpr uint32_t kSectionDevIds         = 10;
    static constexpr uint32_t kSectionMetadataKV     = 11;

    // Version 1: packed StringPool sections (bufSize + buffer + count + entries)
    static constexpr uint32_t kSectionNamesOrig     = 1;
    static constexpr uint32_t kSectionNamesLower     = 2;
    static constexpr uint32_t kSectionPathPool       = 3;
    static constexpr uint32_t kSectionLowerPathPool  = 4;

    // Version 2: StringPool buffers and entries as separate raw arrays
    static constexpr uint32_t kSectionNamesOrigBuf       = 21;
    static constexpr uint32_t kSectionNamesOrigEntries   = 22;
    static constexpr uint32_t kSectionNamesLowerBuf      = 23;
    static constexpr uint32_t kSectionNamesLowerEntries  = 24;
    static constexpr uint32_t kSectionPathBuf            = 25;
    static constexpr uint32_t kSectionPathEntries        = 26;
    static constexpr uint32_t kSectionLowerPathBuf       = 27;
    static constexpr uint32_t kSectionLowerPathEntries   = 28;
    // Version 2: lookup tables (FlatKeyIndex slots) and directory lists (CSR)
    static constexpr uint32_t kSectionPathLookupSlots      = 31;
    static constexpr uint32_t kSectionLowerPathLookupSlots = 32;
    static constexpr uint32_t kSectionPathIndexSlots       = 33;
    static constexpr uint32_t kSectionDirOffsets           = 41;
    static constexpr uint32_t kSectionDirIds               = 42;

private:
    std::string path_;
    uint32_t loadedVersion_ = 0;

    // Section table entry (on-disk layout)
    struct SectionEntry {
        uint32_t sectionId;
        uint32_t reserved;     // padding for alignment
        uint64_t offset;       // byte offset from file start
        uint32_t byteLength;   // section data size in bytes
        uint32_t crc32;        // CRC32 of section data
    };
    static_assert(sizeof(SectionEntry) == 24, "SectionEntry must be 24 bytes");

    // Header (on-disk layout)
    struct Header {
        char magic[4];
        uint32_t version;
        uint32_t recordCount;
        uint32_t liveCount;
        int64_t  timestamp;
        uint64_t lastEventId;
        uint32_t sectionCount;
        uint32_t headerCRC;    // CRC32 of the first 36 bytes
        uint32_t tableCRC;     // v2: CRC32 of the section table
        char     reserved[16];
    };
    static_assert(sizeof(Header) == 64, "Header must be 64 bytes");

    bool writeFile(const SearchEngine::StorageView& v, const IndexMetadata& meta);
    bool mapFile(SearchEngine::Storage& st, IndexMetadata* outMeta, bool verify) const;
    bool loadPacked(SearchEngine& engine, IndexMetadata* outMeta);

    static std::vector<uint8_t> serializeMetadata(const IndexMetadata& meta);
    static bool readMetadataSection(const uint8_t* data, size_t len, IndexMetadata& meta);
};
