#pragma once
#include "Column.h"
#include <algorithm>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

/// Sorted live record indices per directory (pathPool_ index).
///
/// The bulk is a CSR table (offsets + ids) that can be mapped straight from
/// the index file; directories whose lists change afterwards get a private
/// heap copy in `changed_`. The CSR costs 4 bytes per record plus 4 per
/// directory, against ~40 bytes per directory for a vector of vectors.
class DirPostings {
public:
    using List = std::span<const uint32_t>;

    /// Number of directory slots (lists past the CSR are empty unless changed).
    size_t size() const { return size_; }

    List operator[](uint32_t pi) const {
        if (!changed_.empty()) {
            auto it = changed_.find(pi);
            if (it != changed_.end()) return {it->second.data(), it->second.size()};
        }
        if (static_cast<size_t>(pi) + 1 < offsets_.size()) {
            uint32_t begin = offsets_[pi];
            return {ids_.data() + begin, offsets_[pi + 1] - begin};
        }
        return {};
    }

    void resize(size_t n) {
        if (n > size_) size_ = n;
    }

    void insertSorted(uint32_t pi, uint32_t idx) {
        resize(static_cast<size_t>(pi) + 1);
        auto& list = mutableList(pi);
        auto pos = std::lower_bound(list.begin(), list.end(), idx);
        if (pos != list.end() && *pos == idx) return;
        list.insert(pos, idx);
    }

    void eraseSorted(uint32_t pi, uint32_t idx) {
        if (pi >= size_) return;
        List cur = (*this)[pi];
        auto it = std::lower_bound(cur.begin(), cur.end(), idx);
        if (it == cur.end() || *it != idx) return;
        auto& list = mutableList(pi);
        list.erase(list.begin() + (it - cur.begin()));
    }

    /// Build from the record columns: every live record under its directory.
    template <class Types, class PathIndices>
    static DirPostings build(const Types& types, const PathIndices& pathIndices, uint32_t pathCount) {
        DirPostings d;
        d.offsets_.assign(static_cast<size_t>(pathCount) + 1, 0);
        size_t n = types.size();
        for (size_t i = 0; i < n; i++) {
            if (types[i] != 0 && pathIndices[i] < pathCount) d.offsets_[pathIndices[i] + 1]++;
        }
        for (size_t p = 0; p < pathCount; p++) d.offsets_[p + 1] += d.offsets_[p];
        d.ids_.resize(d.offsets_[pathCount]);
        Column<uint32_t> fill(d.offsets_.begin(), d.offsets_.end() - 1);
        for (size_t i = 0; i < n; i++) {
            if (types[i] != 0 && pathIndices[i] < pathCount) {
                d.ids_[fill[pathIndices[i]]++] = static_cast<uint32_t>(i);
            }
        }
        d.size_ = pathCount;
        return d;
    }

    /// Visit the current list of every directory in order (for persistence).
    template <class F>
    void forEachList(F&& f) const {
        for (size_t p = 0; p < size_; p++) f((*this)[static_cast<uint32_t>(p)]);
    }

    /// Install a CSR table written from forEachList (offsets has size()+1 entries).
    bool adopt(Column<uint32_t>&& offsets, Column<uint32_t>&& ids) {
        if (offsets.empty() || offsets.front() != 0 || offsets.back() != ids.size()) return false;
        offsets_ = std::move(offsets);
        ids_ = std::move(ids);
        changed_.clear();
        size_ = offsets_.size() - 1;
        return true;
    }

    size_t changedCount() const { return changed_.size(); }

private:
    Column<uint32_t> offsets_;
    Column<uint32_t> ids_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> changed_;
    size_t size_ = 0;

    std::vector<uint32_t>& mutableList(uint32_t pi) {
        auto [it, inserted] = changed_.try_emplace(pi);
        if (inserted && static_cast<size_t>(pi) + 1 < offsets_.size()) {
            const uint32_t* b = ids_.data() + offsets_[pi];
            it->second.assign(b, static_cast<const uint32_t*>(ids_.data() + offsets_[pi + 1]));
        }
        return it->second;
    }
};
