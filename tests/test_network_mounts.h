#pragma once
// ═══════════════════════════════════════════════════════
//  Part 77: NetworkMounts Tests
// ═══════════════════════════════════════════════════════

#include "NetworkMounts.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static void runNetworkMountsTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 77: NetworkMounts Tests\n";
    std::cout << "========================================\n\n";

    // ── Test 1: filterNetworkMounts ──
    std::cout << "  --- filterNetworkMounts ---\n";
    {
        std::vector<struct statfs> entries;

        // SMB mount under /Volumes → included
        struct statfs e{};
        strncpy(e.f_mntonname, "/Volumes/gvr", MAXPATHLEN);
        e.f_flags = MNT_DONTBROWSE;  // network: no MNT_LOCAL
        entries.push_back(e);

        // WebDAV mount → included
        struct statfs w{};
        strncpy(w.f_mntonname, "/Volumes/192.168.3.6", MAXPATHLEN);
        w.f_flags = 0;
        entries.push_back(w);

        // Local volume → excluded (covered by the root scan)
        struct statfs l{};
        strncpy(l.f_mntonname, "/Volumes/Macintosh HD", MAXPATHLEN);
        l.f_flags = MNT_LOCAL | MNT_JOURNALED;
        entries.push_back(l);

        // Autofs trigger map → excluded
        struct statfs a{};
        strncpy(a.f_mntonname, "/Volumes/auto_nas", MAXPATHLEN);
        a.f_flags = MNT_AUTOMOUNTED;
        entries.push_back(a);

        // Not under /Volumes → excluded
        struct statfs h{};
        strncpy(h.f_mntonname, "/System/Volumes/Data/home", MAXPATHLEN);
        h.f_flags = MNT_AUTOMOUNTED;
        entries.push_back(h);

        // Nested mount point → excluded (reached via parent traversal)
        struct statfs n{};
        strncpy(n.f_mntonname, "/Volumes/gvr/sub", MAXPATHLEN);
        n.f_flags = 0;
        entries.push_back(n);

        auto out = filterNetworkMounts(entries);
        check(out.size() == 2, "filterNetworkMounts: exactly 2 network mounts kept");
        bool hasGvr = false, hasWebdav = false;
        for (const auto& m : out) {
            if (m == "/Volumes/gvr") hasGvr = true;
            if (m == "/Volumes/192.168.3.6") hasWebdav = true;
        }
        check(hasGvr, "filterNetworkMounts: /Volumes/gvr included");
        check(hasWebdav, "filterNetworkMounts: /Volumes/192.168.3.6 included");
    }

    // ── Test 2: DirectoryScanner multi-root seeding ──
    std::cout << "\n  --- DirectoryScanner multi-root ---\n";
    {
        namespace fs = std::filesystem;
        std::string rootA = "/tmp/mace_netmount_A_" + std::to_string(getpid());
        std::string rootB = "/tmp/mace_netmount_B_" + std::to_string(getpid());
        fs::create_directories(rootA + "/sub");
        fs::create_directories(rootB);
        for (int i = 0; i < 3; i++) {
            std::ofstream(rootA + "/a" + std::to_string(i) + ".txt") << "a";
        }
        std::ofstream(rootA + "/sub/deep.txt") << "d";
        for (int i = 0; i < 2; i++) {
            std::ofstream(rootB + "/b" + std::to_string(i) + ".txt") << "b";
        }

        {
            // Extra root's files must be collected alongside the primary root
            DirectoryScanner scanner;
            scanner.addExtraRoot(rootB);
            scanner.scan(rootA);
            check(!scanner.hasFailed(), "multi-root: scan not failed");
            check(scanner.getStats().fileCount.load() == 6,
                  "multi-root: files from both roots collected (4 + 2)");
        }

        {
            // An unreachable extra root must not fail the scan of the primary root
            DirectoryScanner scanner;
            scanner.addExtraRoot("/definitely/not/here_" + std::to_string(getpid()));
            scanner.scan(rootA);
            check(!scanner.hasFailed(), "multi-root: unreachable extra root doesn't fail scan");
            check(scanner.getStats().fileCount.load() == 4,
                  "multi-root: primary root still fully scanned (4 files)");
        }

        fs::remove_all(rootA);
        fs::remove_all(rootB);
    }

    std::cout << "\n";
}
