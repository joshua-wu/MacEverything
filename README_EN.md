<p align="center">
  <img src="MacEverything/Assets.xcassets/AppIcon.appiconset/icon_256.png" alt="MacEverything" width="128" />
</p>

<h1 align="center">MacEverything</h1>

<p align="center">
  <b>Instant file search for macOS</b> — find any file among millions in milliseconds.<br/>
  Inspired by <a href="https://www.voidtools.com/">Everything</a> on Windows. Nothing else comes close on Mac.
</p>

<p align="center">
  <a href="README.md">中文</a> | <b>English</b>
</p>

<p align="center">
  <a href="https://github.com/crimson-gzx/MacEverything/releases"><img src="https://img.shields.io/badge/download-DMG%20(Green%20Edition)-orange?logo=apple" alt="Download DMG" /></a>
  <img src="https://img.shields.io/badge/SSD%20Writes--99.97%25-brightgreen" alt="SSD Friendly" />
  <a href="#installation"><img src="https://img.shields.io/badge/macOS-13%2B-blue?logo=apple" alt="macOS 13+" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="MIT License" /></a>
  <a href="#testing"><img src="https://img.shields.io/badge/tests-79%20modules-brightgreen" alt="79 test modules" /></a>
  <a href="#ai-tool-integration-mcp"><img src="https://img.shields.io/badge/MCP-compatible-blueviolet" alt="MCP Compatible" /></a>
</p>

> 🌿 **This fork is the "Green Edition" (MacEverything-Green)**
> Fixes a critical persistence compaction bug in upstream that continuously wrote ~30 GB/hour (~700 GB/day) to your SSD due to unthrottled temporary cache file churn (`~/Library/Caches`, `Biome`) and an overly aggressive compaction threshold (`kCompactThreshold = 100`).
> This edition cuts disk writes by **99.97% (< 150 MB/day)** while preserving instant < 5ms search speeds, zero regressions, and passing all 574 test cases.

---

<p align="center">
  <img src="assets/screen-shot.jpg" alt="MacEverything Screenshot" width="720" />
</p>

## 🌿 Why Choose the Green Edition?

### 1. The Issue: Upstream's Silent SSD Wear
In the original implementation, the FSEvents watcher listened to whole-disk filesystem events without excluding high-frequency system temporary cache paths. Coupled with a low compaction threshold (`100` dirty operations), the entire 275MB index database was being rewritten to disk every ~30 seconds:
* **Upstream writes**: ~**30 GB / hour**
* **Upstream daily writes**: ~**700 GB / day**
* **Hardware impact**: Mac SSDs are soldered to the logic board. A 512GB SSD with typical 150 TBW endurance would exhaust its lifespan within 6–12 months.

### 2. Kernel-Level Benchmark (`proc_pid_rusage`)

| Metric | Upstream (Unpatched) | Green Edition (This Fork) | Improvement |
| :--- | :---: | :---: | :---: |
| **Full Rewrite Frequency** | Every ~30 seconds | **Only after 50,000 valid mutations** | Eliminated redundant full flushes |
| **36-Minute Disk Writes** | **~17.6 GB** | **4.38 MB** | 📉 **-99.97%** |
| **Estimated 24h Writes** | **~700 GB** | **< 150 MB** | 📉 **-99.98%** |
| **512GB SSD Lifespan Depletion** | ~1 Year | **> 1,000 Years** | 🛡️ **Zero SSD anxiety** |
| **Search Query Latency** | < 5ms | < 5ms | ⚡ Fully identical performance |
| **Unit Test Coverage** | 79 modules | 79 modules (574 tests passed) | ✅ 100% verified |

### 3. One-Liner: Check Your Current MacEverything Disk Writes
Run this in Terminal to check how much data your current MacEverything process has written to your SSD:
```bash
python3 -c "
import ctypes, subprocess
class rusage_info_v4(ctypes.Structure):
    _fields_ = [('dummy', ctypes.c_uint8 * 144), ('diskio_bytesread', ctypes.c_uint64), ('diskio_byteswritten', ctypes.c_uint64)]
libproc = ctypes.CDLL('/usr/lib/libproc.dylib')
ru = rusage_info_v4()
try:
    pid = int(subprocess.check_output(['pgrep', '-f', 'MacEverything']).split()[0])
    libproc.proc_pid_rusage(pid, 4, ctypes.byref(ru))
    print(f'MacEverything Total Written: {ru.diskio_byteswritten / (1024*1024):.2f} MB')
except Exception as e:
    print('MacEverything process not found')
"
```
*If you see gigabytes of writes within a few hours of uptime, switch to the Green Edition immediately.*

---

## Feature Highlights

### Blazing Fast Search

Index an entire disk — **5 million+ files in just 14 seconds** — then get results in **under 5ms** for every search. Two orders of magnitude faster than Spotlight.

| Comparison | MacEverything | Spotlight | `find` |
|------------|:---:|:---:|:---:|
| Index 5M files | ~14s | Minutes+ | No index |
| Search latency | **< 5ms** | 200ms–2s | 5–30s |
| Real-time monitoring | FSEvents | FSEvents | None |
| Content search | Trigram index | Metadata-heavy | `grep` |
| AI tool integration | Built-in MCP | No | No |

### Always at Your Fingertips

Press **`Option+Space`** to summon the search window anytime (hotkey is customizable). The search bar auto-focuses — start typing immediately, find what you need, and move on. Supports launch-at-login with minimized background mode, staying out of your workflow.

### Smart Input Experience

- **Ghost text autocomplete** — semi-transparent suggestions appear as you type, drawn from search history (sorted by frequency) or system keywords (e.g., typing `ex` suggests `ext:`). Press **Tab** to accept
- **Search bar syntax highlighting** — real-time color coding: filter names in purple, arguments in blue, quoted strings in orange, operators in red
- **Search option badges** — colorful badges next to the search bar for one-click toggling of Regex / Case Sensitive / Whole Word / Match Filename

### Everything-Style Query Syntax

Full AST parser supporting 15+ filters, boolean operators, glob wildcards, and regular expressions. Built-in syntax help window (**Cmd+?**).

| Query | Description |
|-------|-------------|
| `readme` | Files containing "readme" in the name |
| `*.swift` | All Swift source files |
| `ext:py size:>1mb` | Python files larger than 1MB |
| `dm:today` | Files modified today |
| `config path:/usr` | Files containing "config" under `/usr` |
| `"exact phrase"` | Exact phrase matching |
| `foo OR bar` | Boolean OR operation |
| `case:Makefile` | Case-sensitive search |
| `regex:^test_.*\.py$` | Regular expression search |
| `type:folder node_modules` | Search directories only |
| `~/Documents/*.pdf` | Tilde expansion + glob |
| `infile:TODO ext:cpp` | Search "TODO" inside C++ files |

<details>
<summary><b>Full filter list</b></summary>

| Filter | Description | Example |
|--------|-------------|---------|
| `ext:` | File extension | `ext:swift,h` |
| `size:` | File size | `size:>1mb`, `size:100kb-5mb` |
| `type:` | File/folder | `type:folder` |
| `path:` | Path contains | `path:Downloads` |
| `nopath:` | Path excludes | `nopath:node_modules` |
| `parent:` | Direct parent directory | `parent:src` |
| `depth:` | Directory depth | `depth:<3` |
| `dm:` | Date modified | `dm:today`, `dm:>2024-01-01` |
| `dc:` | Date created | `dc:thisweek` |
| `da:` | Date accessed | `da:last7days` |
| `len:` | Filename length | `len:>50` |
| `case:` | Case-sensitive | `case:README` |
| `regex:` | Regular expression | `regex:^test_` |
| `ww:` | Whole word match | `ww:test` |
| `wfn:` | Whole filename match | `wfn:Makefile` |
| `content:` / `infile:` | Content search | `infile:TODO` |
| `audio:` `video:` `pic:` `doc:` `zip:` | File type macros | `audio:` = all audio files |

</details>

### Full-Text Content Search

Type `infile:keyword` to search file contents — results include highlighted context snippets around the keyword. Powered by a Trigram index for speed, only re-indexing changed files. Configure indexed file types and max file size in Content Settings.

### Real-Time Sync, Always Up to Date

- **File monitoring** — FSEvents-based real-time file system listener; new, renamed, or deleted files appear in search results immediately
- **Two-phase instant startup** — loads disk cache on launch (searchable immediately), then catches up with changes via FSEvents in the background. Zero wait time
- **Focus-aware power saving** — pauses refresh when the window is in the background, batch-catches up on refocus. Near-zero background CPU usage

### Interaction Details

- **Smart highlighting** — matches highlighted in search results, AST-aware: correctly handles glob wildcards, regex, case sensitivity, NOT exclusions, and other complex patterns
- **Drag & drop** — drag files directly from search results to Finder, VS Code, Xcode, or any application
- **Context menu** — Open / Reveal in Finder / Copy Path
- **Cmd+Click** — quickly locate files in Finder
- **Recent files** — automatically shows recently modified files when the search bar is empty

### AI Tool Integration (MCP)

Built-in [Model Context Protocol](https://modelcontextprotocol.io/) server — lets AI coding tools instantly search your file system. Enable with one click from the menu bar. Supports **Claude Code**, **Cursor**, and **Claude Desktop**.

```
Claude Code / Cursor / Claude Desktop
       │
       ▼  (stdio JSON-RPC 2.0)
  MacEverythingMCP
       │
       ▼  (HTTP localhost:19860)
  MacEverything.app
```

| Tool | Description |
|------|-------------|
| `search_files` | Filename search (Trigram-accelerated) |
| `search_content` | Full-text content search |
| `recent_files` | Recently modified files |
| `index_status` | Index statistics & health |

### HTTP API

Local REST API on `localhost:19860` for scripting and automation:

```bash
curl "http://localhost:19860/api/search?q=readme&limit=10"       # Search files
curl "http://localhost:19860/api/search/content?q=TODO"           # Content search
curl "http://localhost:19860/api/recent?limit=20"                 # Recent files
curl "http://localhost:19860/api/status"                          # Index status
```

### Installation

#### Download DMG (Ready to use, Recommended)

1. Download the latest `MacEverything-Green.dmg` from the [Releases Page](https://github.com/crimson-gzx/MacEverything/releases)
2. Open the DMG and drag `MacEverything.app` into your Applications folder
3. On first launch, if macOS alerts "cannot be opened because Apple cannot check it for malicious software", go to **System Settings -> Privacy & Security**, scroll down and click **"Open Anyway"**
4. Grant **Full Disk Access** when prompted to enable global indexing
5. Press `Option+Space` anytime to start searching

#### Build from Source

**Requirements:** macOS 13+, Xcode 15+, Homebrew (`brew install re2 abseil`)

```bash
git clone https://github.com/crimson-gzx/MacEverything.git && cd MacEverything

xcodebuild -project MacEverything.xcodeproj -scheme MacEverything \
  -configuration Release build SYMROOT=build

hdiutil create -volname MacEverything \
  -srcfolder build/Release/MacEverything.app \
  -ov -format UDZO MacEverything.dmg
```

#### CLI Daemon

Headless mode for servers or automation environments:

```bash
make daemon
./maceverything-daemon --port 19860 --root /
```

---

<h2 align="center">For Developers: Technical Deep Dive</h2>

<p align="center">
  The following content is for developers interested in implementation details.
</p>

### Architecture Overview

```
┌─────────────────────────────────────┐
│       SwiftUI App Layer             │  UI · ViewModel · MVVM
├─────────────────────────────────────┤
│    Objective-C++ Bridge Layer       │  Zero-overhead interop
├─────────────────────────────────────┤
│       C++20 Core Engine             │  Scan · Index · Search · Persist
└─────────────────────────────────────┘
```

The same C++20 core engine powers three deployment modes:

| Mode | Description |
|------|-------------|
| **GUI App** | SwiftUI menu-bar app, `Option+Space` global hotkey |
| **CLI Daemon** | Headless `maceverything-daemon` — same engine, no UI |
| **MCP Server** | `MacEverythingMCP` — stdio JSON-RPC proxy for AI tools |

### Core Engine

| Component | Key Design |
|-----------|-----------|
| **DirectoryScanner** | Multi-threaded work-stealing + `getattrlistbulk` single-syscall bulk attribute reads, 4–32 threads adaptive |
| **SearchEngine** | Trigram inverted index (name + path dual indexes) + competitive optimal candidate selection + SoA columnar filtering |
| **ContentIndex** | Trigram full-text inverted index, FNV-1a hash incremental updates, only re-indexes changed files |
| **SIMDSearch** | ARM NEON 128-bit first-last byte vectorized matching + 2x loop unrolling, 11.5 GB/s single-thread |
| **IndexPersistence** | WAL + CRC32 + paged dirty-page flushing + atomic rename, COW non-blocking compaction (lock held < 100ms) |
| **FileSystemWatcher** | FSEvents + eventId incremental replay + log truncation detection with automatic subtree rescan |
| **PathTable** | Path string interning — directory paths stored as `uint32` index, saving ~550MB at million-file scale |
| **QueryParser** | Full AST pipeline: Tokenizer → FilterParser → Parser → QueryAST, 30+ filter keywords |

### Benchmarks

Test environment: macOS Darwin 24.3.0, **5.4 million indexed files**, 48 query types:

#### Search Latency

| Query Type | Avg Latency | Example |
|------------|:-----------:|---------|
| Long keywords (7+ chars) | **0.1–1ms** | `screenshot` 0.1ms, `dockerfile` 0.1ms |
| Medium keywords (4–6 chars) | **1–5ms** | `readme` 1.2ms, `config` 4.7ms |
| Glob patterns | **0.7–18ms** | `*.cpp` 0.7ms, `*.swift` 1.5ms |
| Path queries | **3–32ms** | `package.json` 2.9ms |
| All 48 queries (average) | **10.5ms** | Latest results after SoA optimization |

#### Trigram vs Linear Scan

| Query | Trigram | Linear Scan | Speedup |
|-------|:------:|:------:|:------:|
| `node_modules` | 0.5ms | 154ms | **308x** |
| `application` | 2.1ms | 175ms | **83x** |
| `readme` | 1.2ms | 49ms | **41x** |

#### SIMD String Search (Apple M3 Pro)

| Method | Throughput | vs `std::string::find` |
|--------|:---------:|:----------------------:|
| `std::string::find` | 1.2 GB/s | baseline |
| **NEON 128-bit (single-thread)** | **11.5 GB/s** | **9.5x** |
| **NEON 128-bit (12 threads)** | **74.3 GB/s** | **60.7x** |

### Key Technologies

| Technology | Impact |
|------------|--------|
| `getattrlistbulk` | Single syscall for bulk file attributes — avoids per-file `stat` |
| Trigram inverted index | Sub-linear search: 33x–308x faster than linear scan |
| SoA columnar layout | Cache-friendly memory access, SIMD batch-checks 16 records for pure filter queries |
| `__builtin_prefetch` | Prefetch distance 8, hides random memory access latency during candidate verification |
| ARM NEON SIMD | 128-bit vectorized string matching, 2x loop unrolling, approaches memory bandwidth ceiling |
| GCD parallel scan | Multi-core linear scan when trigram can't accelerate |
| StringPool contiguous memory | Filenames packed in a single `char` buffer, SIMD-friendly |
| PathTable interning | Directory paths stored as `uint32` index — saves ~550MB at million-file scale |
| Generation counter | Checked every 1024 iterations, zero-overhead cancellation of stale queries during fast typing |
| APFS Firmlink dedup | inode + devid detection, correctly handles macOS Data/System volume merge loops |
| Regex Trigram pre-filter | Extracts literals from regex to generate trigram candidates, ~7s → <100ms |
| Adaptive Trigram bypass | Falls back to parallel scan when candidate set is too large, avoiding wasteful index lookups |
| COW non-blocking compaction | Copy-on-write, exclusive lock held < 100ms during compaction (was 30–60s) |
| Paged incremental persistence | Only writes dirty pages, typical flush I/O drops from ~112MB to KB-level |

### Testing

79 test modules covering the full stack, with AddressSanitizer and ThreadSanitizer support:

```bash
make test          # Fast unit tests + bridge lint
make test-slow     # Integration tests (full disk scan, FSEvents, E2E)
make test-all      # All tests
make test-asan     # AddressSanitizer
make test-tsan     # ThreadSanitizer
```

Coverage:
- **Core engine**: scan, query, mutation, compaction, ranking, path search
- **Persistence**: WAL CRC integrity, batch replay, race conditions, paged persistence v5
- **Content index**: trigram, compaction, mod-time tracking, WAL tracking
- **Search/Query**: tokenizer, parser, filters, date filters, structured queries, regex trigram, highlight hints
- **Performance**: SIMD search, 10M-record synthetic benchmarks, trigram competition
- **Integration**: thread safety, E2E, HTTP engine hot-swap, MCP protocol
- **Memory safety**: ASan + TSan builds

### Project Structure

```
MacEverything/
├── Core/                  # C++20 core engine
│   ├── SearchEngine       # Trigram index + parallel query (5 .cpp files)
│   ├── DirectoryScanner   # Multi-threaded bulk scanner
│   ├── ContentIndex       # Full-text inverted index
│   ├── IndexPersistence   # WAL + paged persistence
│   ├── FileSystemWatcher  # FSEvents real-time monitor
│   ├── HttpServer         # Embedded REST API server
│   ├── SIMDSearch         # ARM NEON vectorized search
│   ├── QueryAST/Parser    # Full query language pipeline
│   ├── PathTable          # String interning table
│   └── ServiceEngine      # Lifecycle orchestration
├── Bridge/                # Objective-C++ bridge
│   └── MacSearchBridge    # C++ ↔ Swift zero-overhead interop
├── App/                   # SwiftUI application
│   ├── ContentView        # Main search UI
│   ├── SearchViewModel    # MVVM + tiered debouncing
│   ├── HotkeyManager      # Global hotkey registration
│   └── MCPConfigManager   # One-click MCP setup
├── CLI/                   # Command-line tools
│   ├── daemon_main        # Headless daemon
│   └── mcp_main           # MCP server (stdio JSON-RPC)
└── tests/                 # 79 test modules
```

## Contributing

Contributions are welcome! Please follow this workflow:

1. Fork the repository
2. Create a feature branch (`feat/...`) or bugfix branch (`fix/...`)
3. Write tests for new functionality
4. Ensure `make test-all` passes
5. Submit a Pull Request

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.

---

<p align="center">
  <b>If MacEverything helps you find files faster, consider giving it a star!</b>
</p>
