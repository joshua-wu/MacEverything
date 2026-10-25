#pragma once
// ═══════════════════════════════════════════════════════
//  NetworkMounts — enumerate mounted network filesystems
//  (SMB / WebDAV / NFS / AFP) under /Volumes so the scanner
//  can include them as extra scan roots.
//  Header-only so tests can include directly.
// ═══════════════════════════════════════════════════════

#include <sys/mount.h>
#include <string>
#include <vector>

/// Filter raw statfs entries down to indexable network mount points:
/// non-local (network) filesystems that are not autofs trigger maps and
/// are mounted directly under /Volumes.
inline std::vector<std::string> filterNetworkMounts(
        const std::vector<struct statfs>& entries) {
    std::vector<std::string> out;
    for (const auto& fs : entries) {
        if (fs.f_flags & MNT_LOCAL) continue;        // local volumes come from the root scan
        if (fs.f_flags & MNT_AUTOMOUNTED) continue;  // autofs trigger maps, not real mounts
        std::string mnt(fs.f_mntonname);
        if (mnt.rfind("/Volumes/", 0) != 0) continue;
        // Anything deeper than /Volumes/<name> is reached by traversing its
        // parent mount; only top-level mount points are extra roots.
        if (mnt.find('/', sizeof("/Volumes/") - 1) != std::string::npos) continue;
        out.push_back(mnt);
    }
    return out;
}

inline std::vector<std::string> networkMountPoints() {
    int n = getfsstat(nullptr, 0, MNT_NOWAIT);
    if (n <= 0) return {};
    std::vector<struct statfs> entries(static_cast<size_t>(n));
    n = getfsstat(entries.data(),
                  static_cast<int>(entries.size() * sizeof(struct statfs)),
                  MNT_NOWAIT);
    if (n <= 0) return {};
    entries.resize(static_cast<size_t>(n));
    return filterNetworkMounts(entries);
}
