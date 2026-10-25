#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include "Column.h"

/// Open-addressing hash index that maps string keys to uint32 ids without
/// storing the keys. Each slot holds (hash32 << 32 | id) in 8 bytes; key
/// comparisons call back into caller-owned storage (StringPools), so the
/// index costs ~8-12 bytes per entry instead of a heap string per entry.
///
/// A key is described by KeyParts: a + ('/' if sep) + b. Callers pass a
/// keyOf(id) -> KeyParts functor to every operation that compares keys.
/// Invariant: every stored id must still resolve to its original key, so
/// entries must be erased before the backing strings are tombstoned.
class FlatKeyIndex {
public:
    struct KeyParts {
        std::string_view a;
        std::string_view b;
        bool sep = false;
    };

    static constexpr size_t npos = SIZE_MAX;

    static uint64_t hashBytes(const char* p, size_t n) {
        uint64_t h = 0x9E3779B97F4A7C15ull ^ (n * 0xff51afd7ed558ccdull);
        while (n >= 8) {
            uint64_t w;
            std::memcpy(&w, p, 8);
            h = (h ^ w) * 0xbf58476d1ce4e5b9ull;
            h ^= h >> 31;
            p += 8;
            n -= 8;
        }
        uint64_t w = 0;
        std::memcpy(&w, p, n);
        h = (h ^ w) * 0x94d049bb133111ebull;
        h ^= h >> 29;
        h *= 0xbf58476d1ce4e5b9ull;
        h ^= h >> 32;
        return h;
    }

    static uint64_t hashKey(std::string_view key) { return hashBytes(key.data(), key.size()); }

    static uint64_t hashParts(const KeyParts& k) {
        if (!k.sep && k.b.empty()) return hashKey(k.a);
        thread_local std::string buf;
        buf.assign(k.a);
        if (k.sep) buf += '/';
        buf.append(k.b);
        return hashKey(buf);
    }

    static bool equals(std::string_view key, const KeyParts& k) {
        size_t sepLen = k.sep ? 1 : 0;
        if (key.size() != k.a.size() + sepLen + k.b.size()) return false;
        if (std::memcmp(key.data(), k.a.data(), k.a.size()) != 0) return false;
        if (k.sep && key[k.a.size()] != '/') return false;
        return std::memcmp(key.data() + k.a.size() + sepLen, k.b.data(), k.b.size()) == 0;
    }

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    size_t memoryBytes() const { return slots_.capacity() * sizeof(uint64_t); }

    void clear() {
        slots_.clear();
        slots_.shrink_to_fit();
        size_ = 0;
        mask_ = 0;
    }

    void reserve(size_t n) {
        size_t cap = 16;
        while (cap * 7 < n * 10) cap <<= 1;
        if (cap > slots_.size()) rehash(cap);
    }

    /// Returns the slot holding `key`, or npos.
    template <class KeyOf>
    size_t find(std::string_view key, KeyOf&& keyOf) const {
        if (size_ == 0) return npos;
        uint32_t h32 = static_cast<uint32_t>(hashKey(key) >> 32);
        for (size_t pos = h32 & mask_;; pos = (pos + 1) & mask_) {
            uint64_t s = slots_[pos];
            if (s == kEmpty) return npos;
            if (static_cast<uint32_t>(s >> 32) == h32 && equals(key, keyOf(static_cast<uint32_t>(s))))
                return pos;
        }
    }

    template <class KeyOf>
    uint32_t get(std::string_view key, KeyOf&& keyOf, uint32_t fallback = UINT32_MAX) const {
        size_t pos = find(key, keyOf);
        return pos == npos ? fallback : valueAt(pos);
    }

    uint32_t valueAt(size_t slot) const { return static_cast<uint32_t>(slots_[slot]); }

    /// Insert or overwrite the id stored under `key`.
    template <class KeyOf>
    void assign(std::string_view key, uint32_t id, KeyOf&& keyOf) {
        assignHashed(hashKey(key), key, id, keyOf);
    }

    /// Insert or overwrite using the key of `id` itself (id's strings must be live).
    template <class KeyOf>
    void assignSelf(uint32_t id, KeyOf&& keyOf) {
        KeyParts k = keyOf(id);
        if (!k.sep && k.b.empty()) {
            assignHashed(hashKey(k.a), k.a, id, keyOf);
            return;
        }
        thread_local std::string buf;
        buf.assign(k.a);
        if (k.sep) buf += '/';
        buf.append(k.b);
        assignHashed(hashKey(buf), buf, id, keyOf);
    }

    /// Like assignSelf, with hashParts(keyOf(id)) precomputed (e.g. in parallel).
    template <class KeyOf>
    void assignSelfHashed(uint64_t h, uint32_t id, KeyOf&& keyOf) {
        if ((size_ + 1) * 10 > slots_.size() * 7) {
            rehash(slots_.empty() ? 16 : slots_.size() * 2);
        }
        uint32_t h32 = static_cast<uint32_t>(h >> 32);
        uint64_t entry = (static_cast<uint64_t>(h32) << 32) | id;
        for (size_t pos = h32 & mask_;; pos = (pos + 1) & mask_) {
            uint64_t s = slots_[pos];
            if (s == kEmpty) {
                slots_[pos] = entry;
                size_++;
                return;
            }
            if (static_cast<uint32_t>(s >> 32) == h32 && sameKey(keyOf(id), keyOf(static_cast<uint32_t>(s)))) {
                slots_[pos] = entry;
                return;
            }
        }
    }

    /// Erase the entry in `slot` (backward-shift deletion, no tombstones).
    void eraseSlot(size_t slot) {
        size_t i = slot;
        size_t j = slot;
        for (;;) {
            j = (j + 1) & mask_;
            uint64_t s = slots_[j];
            if (s == kEmpty) break;
            size_t home = static_cast<uint32_t>(s >> 32) & mask_;
            bool between = (i <= j) ? (i < home && home <= j) : (i < home || home <= j);
            if (between) continue;
            slots_[i] = s;
            i = j;
        }
        slots_[i] = kEmpty;
        size_--;
    }

    template <class KeyOf>
    bool erase(std::string_view key, KeyOf&& keyOf) {
        size_t pos = find(key, keyOf);
        if (pos == npos) return false;
        eraseSlot(pos);
        return true;
    }

    /// Erase the entry whose id is `id`, locating it via id's own key.
    template <class KeyOf>
    bool eraseId(uint32_t id, KeyOf&& keyOf) {
        if (size_ == 0) return false;
        uint32_t h32 = static_cast<uint32_t>(hashParts(keyOf(id)) >> 32);
        for (size_t pos = h32 & mask_;; pos = (pos + 1) & mask_) {
            uint64_t s = slots_[pos];
            if (s == kEmpty) return false;
            if (static_cast<uint32_t>(s) == id) {
                eraseSlot(pos);
                return true;
            }
        }
    }

    /// Raw slot table, for persisting the index next to the strings it refers to.
    const uint64_t* slotData() const { return slots_.data(); }
    size_t slotCount() const { return slots_.size(); }

    /// Install a slot table written by slotData() (e.g. a mapped file section).
    /// The ids in it must resolve to the same keys as when it was written.
    bool adoptSlots(Column<uint64_t>&& slots) {
        size_t n = slots.size();
        if (n != 0 && (n & (n - 1)) != 0) return false;
        size_t live = 0;
        for (uint64_t s : slots) live += s != kEmpty;
        if (n != 0 && live * 10 > n * 7) return false;
        slots_ = std::move(slots);
        size_ = live;
        mask_ = n ? n - 1 : 0;
        return true;
    }

    template <class F>
    void forEachValue(F&& f) const {
        for (uint64_t s : slots_) {
            if (s != kEmpty) f(static_cast<uint32_t>(s));
        }
    }

private:
    static constexpr uint64_t kEmpty = UINT64_MAX;

    Column<uint64_t> slots_;
    size_t size_ = 0;
    size_t mask_ = 0;

    static bool sameKey(const KeyParts& x, const KeyParts& y) {
        if (!x.sep && x.b.empty()) return equals(x.a, y);
        thread_local std::string buf;
        buf.assign(x.a);
        if (x.sep) buf += '/';
        buf.append(x.b);
        return equals(buf, y);
    }

    template <class KeyOf>
    void assignHashed(uint64_t h, std::string_view key, uint32_t id, KeyOf&& keyOf) {
        if ((size_ + 1) * 10 > slots_.size() * 7) {
            rehash(slots_.empty() ? 16 : slots_.size() * 2);
        }
        uint32_t h32 = static_cast<uint32_t>(h >> 32);
        uint64_t entry = (static_cast<uint64_t>(h32) << 32) | id;
        for (size_t pos = h32 & mask_;; pos = (pos + 1) & mask_) {
            uint64_t s = slots_[pos];
            if (s == kEmpty) {
                slots_[pos] = entry;
                size_++;
                return;
            }
            if (static_cast<uint32_t>(s >> 32) == h32 && equals(key, keyOf(static_cast<uint32_t>(s)))) {
                slots_[pos] = entry;
                return;
            }
        }
    }

    void rehash(size_t newCap) {
        Column<uint64_t> old = std::move(slots_);
        slots_.assign(newCap, kEmpty);
        mask_ = newCap - 1;
        for (uint64_t s : old) {
            if (s == kEmpty) continue;
            size_t pos = static_cast<uint32_t>(s >> 32) & mask_;
            while (slots_[pos] != kEmpty) pos = (pos + 1) & mask_;
            slots_[pos] = s;
        }
    }
};
