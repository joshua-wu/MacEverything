import Foundation
import Combine

struct FileItem: Identifiable {
    let id: String      // path-based stable ID
    let index: UInt32   // engine index for record lookup
    let name: String
    let path: String
    let type: UInt8
    let size: UInt64
    let modTime: time_t
}

struct ContentFileItem: Identifiable {
    let id: String      // stable ID: filePath + ":" + matchOffset
    let fileName: String
    let filePath: String
    let snippet: String
    let matchOffset: UInt32
    let fileType: UInt8
}

@MainActor
class SearchViewModel: ObservableObject {
    @Published var searchText: String = ""
    @Published var displayItems: [FileItem] = []
    @Published var totalMatches: Int = 0
    @Published var isScanning: Bool = false
    @Published var scanComplete: Bool = false
    @Published var totalRecords: UInt32 = 0
    @Published var queryTimeMs: Double = 0
    @Published var isMonitoring: Bool = false
    @Published var scannedCount: UInt64 = 0
    var isLoadingMore: Bool = false
    @Published var showingRecent: Bool = false
    @Published var isContentSearch: Bool = false
    @Published var contentResults: [ContentFileItem] = []
    @Published var isContentIndexing: Bool = false
    @Published var contentIndexProgress: (indexed: UInt32, total: UInt32)?
    @Published var contentIndexedCount: UInt32 = 0
    @Published var isSyncing: Bool = false
    @Published var ghostSuggestion: String? = nil

    /// Structured highlight hints extracted from the C++ query AST.
    /// Replaces the old keyword-based approach with field-aware, mode-aware hints.
    var highlightHints: [HighlightHint] {
        let query = searchOptions.buildQuery(searchText)
        guard !query.isEmpty else { return [] }
        return bridge.parseHighlightHints(query).map { HighlightHint(from: $0) }
    }

    private let bridge = MacSearchBridge.shared()
    private let historyStore = SearchHistoryStore()
    private let searchOptions = SearchOptions.shared
    private var searchTask: Task<Void, Never>?
    private var recentTask: Task<Void, Never>?
    private var settledTask: Task<Void, Never>?
    private var optionsSink: AnyCancellable?
    private var cachedResults: [MEFileResult] = []
    private var loadedCount: Int = 0
    private var searchGeneration: UInt64 = 0
    private static let guiSessionId: UInt64 = 1

    private static let pageSize: Int = 100
    private static let maxResults: UInt32 = 10000
    private static let indexChangeThrottleNs: UInt64 = 5_000_000_000 // 5 seconds

    private var indexChangeTask: Task<Void, Never>?
    let refreshThrottle = IndexRefreshThrottle()

    static var cacheDir: String {
        let base = NSSearchPathForDirectoriesInDomains(
            .cachesDirectory, .userDomainMask, true
        ).first ?? NSTemporaryDirectory()
        let appCache = (base as NSString).appendingPathComponent("com.maceverything.app")
        try? FileManager.default.createDirectory(
            atPath: appCache, withIntermediateDirectories: true
        )
        return appCache
    }

    static var cachePath: String {
        return (cacheDir as NSString).appendingPathComponent("index.bin")
    }

    static var walPath: String {
        return (cacheDir as NSString).appendingPathComponent("index.wal")
    }

    static var pagesPath: String {
        return (cacheDir as NSString).appendingPathComponent("index.pages")
    }

    static var ptablePath: String {
        return (cacheDir as NSString).appendingPathComponent("index.ptable")
    }

    init() {
        optionsSink = searchOptions.objectWillChange.sink { [weak self] _ in
            Task { @MainActor [weak self] in
                self?.onSearchOptionsChanged()
            }
        }
        startIncremental()
    }

    private func onSearchOptionsChanged() {
        guard scanComplete, !searchText.isEmpty, !isContentSearch else { return }
        searchTask?.cancel()
        searchGeneration &+= 1
        bridge.cancelSession(Self.guiSessionId)
        performSearch(searchText)
    }

    func startIncremental() {
        isScanning = true
        scannedCount = 0

        bridge.onScanProgress = { [weak self] fileCount, dirCount in
            Task { @MainActor in
                self?.scannedCount = fileCount + dirCount
            }
        }

        bridge.onContentIndexProgress = { [weak self] indexed, total in
            Task { @MainActor in
                guard let self = self else { return }
                self.isContentIndexing = true
                self.contentIndexProgress = (indexed, total)
            }
        }

        bridge.onContentIndexComplete = { [weak self] totalIndexed in
            Task { @MainActor in
                guard let self = self else { return }
                self.isContentIndexing = false
                self.contentIndexProgress = nil
                self.contentIndexedCount = totalIndexed
                // Auto-refresh content search results after indexing completes
                if self.isContentSearch && !self.contentKeyword.isEmpty {
                    self.performContentSearch(self.contentKeyword)
                }
            }
        }

        // Set up FSEvents change callback before starting
        bridge.onIndexChanged = { [weak self] in
            Task { @MainActor in
                self?.onIndexChanged()
            }
        }

        bridge.startIncremental(from: "/",
                                cachePath: Self.cachePath,
                                walPath: Self.walPath) { [weak self] count, didFullScan in
            Task { @MainActor in
                guard let self = self else { return }
                self.totalRecords = count
                self.isScanning = false
                self.scanComplete = true
                self.isMonitoring = self.bridge.isMonitoring
                self.isSyncing = self.bridge.isSyncing

                if !self.searchText.isEmpty {
                    self.performSearch(self.searchText)
                } else {
                    self.loadRecentFiles()
                }
            }
        }
    }

    func rebuildIndex() {
        guard !isScanning else { return }
        searchTask?.cancel()
        recentTask?.cancel()
        indexChangeTask?.cancel()
        searchGeneration &+= 1
        scanComplete = false
        displayItems = []
        totalMatches = 0
        queryTimeMs = 0
        contentResults = []
        isContentSearch = false
        contentKeyword = ""

        // Delete cached index files so startIncremental does a full scan
        try? FileManager.default.removeItem(atPath: Self.cachePath)
        try? FileManager.default.removeItem(atPath: Self.walPath)
        try? FileManager.default.removeItem(atPath: Self.pagesPath)
        try? FileManager.default.removeItem(atPath: Self.ptablePath)

        startIncremental()
    }

    func onSearchTextChanged() {
        searchTask?.cancel()
        recentTask?.cancel()
        searchGeneration &+= 1
        isLoadingMore = false
        let text = searchText

        if text.isEmpty {
            totalMatches = 0
            queryTimeMs = 0
            cachedResults = []
            loadedCount = 0
            isContentSearch = false
            contentResults = []
            contentKeyword = "" // H-9: reset cached keyword
            ghostSuggestion = nil
            settledTask?.cancel()
            // Cancel any in-flight queries for this GUI session
            bridge.cancelSession(Self.guiSessionId)
            if scanComplete {
                // Slight delay so the stale query's dispatch_apply threads
                // detect the generation change and exit before we compete for the thread pool
                recentTask = Task { @MainActor [weak self] in
                    try? await Task.sleep(nanoseconds: 20_000_000) // 20ms
                    guard !Task.isCancelled, let self else { return }
                    self.loadRecentFiles()
                }
            } else {
                displayItems = []
                showingRecent = false
            }
            return
        }
        showingRecent = false

        let lowerText = text.lowercased()
        if lowerText.hasPrefix("infile:") {
            isContentSearch = true
            displayItems = []
            cachedResults = []
            loadedCount = 0

            let keyword = String(text.dropFirst(7))
            contentKeyword = keyword // H-9: cache computed keyword
            guard !keyword.isEmpty else {
                contentResults = []
                totalMatches = 0
                queryTimeMs = 0
                return
            }

            searchTask = Task { @MainActor in
                // 300ms debounce for content search (heavier)
                try? await Task.sleep(nanoseconds: 300_000_000)
                guard !Task.isCancelled else { return }
                performContentSearch(keyword)
            }
        } else {
            isContentSearch = false
            contentResults = []
            contentKeyword = "" // H-9: reset cached keyword

            searchTask = Task { @MainActor in
                // 80ms debounce
                try? await Task.sleep(nanoseconds: 80_000_000)
                guard !Task.isCancelled else { return }
                performSearch(text)
            }
        }

        updateGhostSuggestion()
        scheduleHistoryRecord()
    }

    private func performSearch(_ keyword: String) {
        let bridge = self.bridge
        let maxResults = Self.maxResults
        let pageSize = Self.pageSize
        let gen = searchGeneration
        let query = searchOptions.buildQuery(keyword)
        Task.detached { [weak self] in
            let start = CFAbsoluteTimeGetCurrent()
            // P-4: Use batch method — single engine lock, no NSNumber boxing
            let results = bridge.queryResults(query, maxResults: maxResults, sessionId: Self.guiSessionId)
            let elapsed = (CFAbsoluteTimeGetCurrent() - start) * 1000
            let totalCount = results.count

            let firstPageCount = min(totalCount, pageSize)
            var items: [FileItem] = []
            items.reserveCapacity(firstPageCount)
            for i in 0..<firstPageCount {
                let r = results[i]
                items.append(FileItem(
                    id: "\(r.path)/\(r.name)", index: 0,
                    name: r.name, path: r.path,
                    type: r.type, size: r.size, modTime: r.modTime
                ))
            }

            let finalItems = items
            await MainActor.run { [weak self] in
                guard let self, self.searchGeneration == gen else { return }
                self.cachedResults = results
                self.loadedCount = firstPageCount
                self.displayItems = finalItems
                self.totalMatches = totalCount
                self.queryTimeMs = elapsed
            }
        }
    }

    private func performContentSearch(_ keyword: String) {
        let bridge = self.bridge
        let gen = searchGeneration
        Task.detached { [weak self] in
            let start = CFAbsoluteTimeGetCurrent()
            let results = bridge.queryContent(keyword, maxResults: 200)
            let elapsed = (CFAbsoluteTimeGetCurrent() - start) * 1000

            var items: [ContentFileItem] = []
            items.reserveCapacity(results.count)
            for r in results {
                items.append(ContentFileItem(
                    id: "\(r.filePath):\(r.matchOffset)",
                    fileName: r.fileName,
                    filePath: r.filePath,
                    snippet: r.snippet,
                    matchOffset: r.matchOffset,
                    fileType: r.fileType
                ))
            }

            let finalItems = items
            await MainActor.run { [weak self] in
                guard let self, self.searchGeneration == gen else { return }
                self.contentResults = finalItems
                self.totalMatches = finalItems.count
                self.queryTimeMs = elapsed
            }
        }
    }

    func loadMore() {
        guard !isLoadingMore else { return }
        guard loadedCount < cachedResults.count else { return }

        isLoadingMore = true
        let results = cachedResults
        let currentLoaded = loadedCount
        let pageSize = Self.pageSize
        let gen = searchGeneration

        // P-4: Results are already fetched — just slice the next page, no bridge calls needed
        Task.detached { [weak self] in
            let nextEnd = min(currentLoaded + pageSize, results.count)
            var newItems: [FileItem] = []
            newItems.reserveCapacity(nextEnd - currentLoaded)
            for i in currentLoaded..<nextEnd {
                let r = results[i]
                newItems.append(FileItem(
                    id: "\(r.path)/\(r.name)", index: 0,
                    name: r.name, path: r.path,
                    type: r.type, size: r.size, modTime: r.modTime
                ))
            }

            let finalNewItems = newItems
            await MainActor.run { [weak self] in
                guard let self, self.searchGeneration == gen else {
                    self?.isLoadingMore = false
                    return
                }
                self.displayItems.append(contentsOf: finalNewItems)
                self.loadedCount = nextEnd
                self.isLoadingMore = false
            }
        }
    }

    private func loadRecentFiles() {
        recentTask?.cancel()
        let bridge = self.bridge
        let gen = searchGeneration
        recentTask = Task.detached { [weak self] in
            // P-4: Use batch method — single engine lock, no NSNumber boxing
            let results = bridge.recentResults(100)
            var items: [FileItem] = []
            items.reserveCapacity(results.count)
            for r in results {
                guard !Task.isCancelled else { return }
                items.append(FileItem(
                    id: "\(r.path)/\(r.name)", index: 0,
                    name: r.name, path: r.path,
                    type: r.type, size: r.size, modTime: r.modTime
                ))
            }
            let finalItems = items
            await MainActor.run { [weak self] in
                guard let self, self.searchGeneration == gen else { return }
                self.displayItems = finalItems
                self.showingRecent = true
            }
        }
    }

    private func onIndexChanged() {
        if refreshThrottle.indexChanged() {
            performIndexRefresh()
            scheduleCooldown()
        }
    }

    func onWindowFocusChanged(_ focused: Bool) {
        if !focused {
            // Record search text to history when window loses focus
            let text = searchText
            if text.count >= 2 && !text.lowercased().hasPrefix("infile:") {
                historyStore.recordQuery(text)
            }
        }
        if refreshThrottle.focusChanged(focused) {
            performIndexRefresh()
            scheduleCooldown()
        }
    }

    private func scheduleCooldown() {
        indexChangeTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: Self.indexChangeThrottleNs)
            guard let self else { return }
            self.indexChangeTask = nil
            if self.refreshThrottle.cooldownExpired() {
                self.performIndexRefresh()
                self.scheduleCooldown()
            }
        }
    }

    private func performIndexRefresh() {
        totalRecords = bridge.liveRecordCount()
        isMonitoring = bridge.isMonitoring
        isSyncing = bridge.isSyncing
        contentIndexedCount = bridge.contentIndexedFileCount()

        // Skip expensive search/query when app is not focused.
        // Results will refresh on focus regain via onWindowFocusChanged.
        guard refreshThrottle.isFocused else { return }

        if !searchText.isEmpty && !isContentSearch {
            performSearch(searchText)
        } else if isContentSearch && !contentKeyword.isEmpty {
            performContentSearch(contentKeyword)
        } else if showingRecent {
            loadRecentFiles()
        }
    }

    var hasMoreResults: Bool {
        loadedCount < cachedResults.count
    }

    // H-9: Cached to avoid recomputing lowercased() + hasPrefix on every access
    private(set) var contentKeyword: String = ""

    // MARK: - Ghost text autocomplete

    private static let systemKeywords: [String] = [
        // Basic filters (most common first)
        "ext:", "size:", "file:", "folder:", "path:", "nopath:", "parent:", "depth:", "len:",
        // Date filters
        "dm:", "dc:", "da:", "datemodified:", "datecreated:", "dateaccessed:",
        // Modifiers
        "case:", "nocase:", "regex:", "ww:", "wfn:", "wholeword:", "wholefilename:",
        // Macros
        "audio:", "video:", "pic:", "doc:", "exe:", "zip:",
        // Other
        "content:", "type:",
    ]

    private static func systemKeywordMatch(for text: String) -> String? {
        let lower = text.lowercased()
        // Only match single partial words: no spaces, no colon already present
        guard !lower.isEmpty, !lower.contains(" "), !lower.contains(":") else { return nil }

        let matches = systemKeywords.filter { $0.hasPrefix(lower) }
        // Prefer shorter keywords, then alphabetical
        return matches.min { a, b in
            if a.count != b.count { return a.count < b.count }
            return a < b
        }
    }

    private func updateGhostSuggestion() {
        let text = searchText
        guard !text.isEmpty, !text.lowercased().hasPrefix("infile:") else {
            ghostSuggestion = nil
            return
        }
        // Priority 1: search history match
        if let match = historyStore.bestMatch(for: text),
           match.lowercased() != text.lowercased() {
            ghostSuggestion = text + match.dropFirst(text.count)
            return
        }
        // Priority 2: system keyword match
        if let keyword = Self.systemKeywordMatch(for: text) {
            ghostSuggestion = text + keyword.dropFirst(text.count)
            return
        }
        ghostSuggestion = nil
    }

    private func scheduleHistoryRecord() {
        settledTask?.cancel()
        let text = searchText
        guard text.count >= 2, !text.lowercased().hasPrefix("infile:") else { return }
        settledTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 2_000_000_000) // 2 seconds
            guard !Task.isCancelled, let self, self.searchText == text else { return }
            self.historyStore.recordQuery(text)
        }
    }

    func acceptGhostSuggestion() {
        guard let suggestion = ghostSuggestion else { return }
        searchText = suggestion
        ghostSuggestion = nil
        historyStore.recordQuery(suggestion)
    }
}
