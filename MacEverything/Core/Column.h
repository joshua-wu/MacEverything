#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

/// Index columns hold millions of entries. Column<T> is a std::vector whose
/// large blocks are separate anonymous mappings (returned to the OS on free)
/// and which can adopt a private file mapping as its storage: pages read from
/// the index file stay clean and file-backed, so they do not count toward the
/// process footprint until written to (copy-on-write).
namespace me {

/// Section alignment in mappable index files: the arm64 page size, which is
/// also a multiple of the 4 KiB x86 page.
constexpr size_t kMapAlign = 16384;

inline size_t alignToMapPage(size_t n) { return (n + kMapAlign - 1) & ~(kMapAlign - 1); }

/// Blocks of at least this size get their own mapping; smaller ones use malloc.
constexpr size_t kOwnMappingMinBytes = 256 * 1024;

/// One-shot handoff of a prepared region to the next allocation of exactly
/// `bytes` on this thread (see adoptMappedColumn).
struct RegionHandoff {
    void* addr = nullptr;
    size_t bytes = 0;
};
inline thread_local RegionHandoff tlsRegionHandoff;

inline void* allocateBlock(size_t bytes) {
    if (bytes >= kOwnMappingMinBytes) {
        void* p = mmap(nullptr, alignToMapPage(bytes), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) throw std::bad_alloc();
        return p;
    }
    void* p = std::malloc(bytes ? bytes : 1);
    if (!p) throw std::bad_alloc();
    return p;
}

inline void freeBlock(void* p, size_t bytes) noexcept {
    if (!p) return;
    if (bytes >= kOwnMappingMinBytes) {
        munmap(p, alignToMapPage(bytes));
    } else {
        std::free(p);
    }
}

/// Reserve a region of `capBytes` (rounded up to kMapAlign) whose first
/// `fileBytes` are a private mapping of `fd` at `offset`; the rest is
/// zero-filled anonymous memory for appends. `offset` must be kMapAlign
/// aligned and the file must extend to alignToMapPage(fileBytes) past it.
inline void* mapFileRegion(int fd, uint64_t offset, size_t fileBytes, size_t capBytes) {
    size_t cap = alignToMapPage(capBytes);
    void* base = mmap(nullptr, cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) return nullptr;
    if (fileBytes > 0) {
        void* p = mmap(base, alignToMapPage(fileBytes), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_FIXED, fd, static_cast<off_t>(offset));
        if (p == MAP_FAILED) {
            munmap(base, cap);
            return nullptr;
        }
    }
    return base;
}

} // namespace me

template <class T>
struct MapAllocator {
    using value_type = T;
    using is_always_equal = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;

    MapAllocator() noexcept = default;
    template <class U>
    MapAllocator(const MapAllocator<U>&) noexcept {}

    T* allocate(size_t n) {
        size_t bytes = n * sizeof(T);
        auto& h = me::tlsRegionHandoff;
        if (h.addr && h.bytes == bytes) {
            void* p = h.addr;
            h = {};
            return static_cast<T*>(p);
        }
        return static_cast<T*>(me::allocateBlock(bytes));
    }

    void deallocate(T* p, size_t n) noexcept { me::freeBlock(p, n * sizeof(T)); }

    /// Default-initialize instead of value-initialize, so resizing over an
    /// adopted mapping keeps the file contents and does not dirty its pages.
    template <class U>
    void construct(U* p) noexcept(std::is_nothrow_default_constructible_v<U>) {
        ::new (static_cast<void*>(p)) U;
    }
    template <class U, class... Args>
    void construct(U* p, Args&&... args) {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }

    friend bool operator==(const MapAllocator&, const MapAllocator&) noexcept { return true; }
    friend bool operator!=(const MapAllocator&, const MapAllocator&) noexcept { return false; }
};

template <class T>
using Column = std::vector<T, MapAllocator<T>>;

/// Reserve room for `extra` more elements, growing by ~1/16 instead of the
/// default doubling. Index columns hold millions of entries and are loaded at
/// exact size, so doubling on the first append after load would waste
/// hundreds of MB; amortized cost stays O(1) with a geometric factor.
template <class V>
inline void reserveForAppend(V& v, size_t extra = 1) {
    size_t need = v.size() + extra;
    if (need <= v.capacity()) return;
    v.reserve(std::max(need, v.size() + v.size() / 16 + 64));
}

/// Make `out` hold `count` elements stored in `fd` at `offset`, with room for
/// about a quarter more appends. Large columns adopt a private mapping of the
/// file (zero copy, clean pages); small ones are read into memory.
/// `offset` must be kMapAlign aligned and the file padded to kMapAlign.
template <class T>
inline bool adoptMappedColumn(int fd, uint64_t offset, size_t count, Column<T>& out) {
    static_assert(std::is_trivially_copyable_v<T>, "mapped columns hold plain data");
    size_t cap = count + count / 4 + 1024;
    size_t capBytes = me::alignToMapPage(cap * sizeof(T));
    cap = capBytes / sizeof(T);
    Column<T> col;
    if (cap * sizeof(T) < me::kOwnMappingMinBytes) {
        col.resize(count);
        size_t bytes = count * sizeof(T);
        if (bytes > 0 && pread(fd, col.data(), bytes, static_cast<off_t>(offset)) != static_cast<ssize_t>(bytes)) {
            return false;
        }
        out = std::move(col);
        return true;
    }
    void* region = me::mapFileRegion(fd, offset, count * sizeof(T), cap * sizeof(T));
    if (!region) return false;
    me::tlsRegionHandoff = {region, cap * sizeof(T)};
    col.reserve(cap);
    if (me::tlsRegionHandoff.addr || col.data() != region) {
        // The vector did not take the region (unexpected allocation pattern).
        if (me::tlsRegionHandoff.addr) munmap(region, me::alignToMapPage(cap * sizeof(T)));
        me::tlsRegionHandoff = {};
        return false;
    }
    col.resize(count);
    out = std::move(col);
    return true;
}
