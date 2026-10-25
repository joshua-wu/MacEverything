import SwiftUI

struct ContentView: View {
    @StateObject private var viewModel = SearchViewModel()
    @ObservedObject private var searchOptions = SearchOptions.shared
    @State private var scrollViewID = 0
    @FocusState private var isSearchFieldFocused: Bool

    var body: some View {
        VStack(spacing: 0) {
            // Permission banner
            PermissionView()

            // Search bar (Alfred-style)
            HStack(spacing: 12) {
                Image(systemName: "magnifyingglass")
                    .font(.system(size: 26, weight: .medium))
                    .foregroundColor(.blue)
                HighlightedSearchField(
                    text: $viewModel.searchText,
                    placeholder: "Search files... (infile: for content search)",
                    ghostSuggestion: viewModel.ghostSuggestion,
                    isFocused: $isSearchFieldFocused,
                    onTab: {
                        if viewModel.ghostSuggestion != nil {
                            viewModel.acceptGhostSuggestion()
                            return true
                        }
                        return false
                    }
                )
                .frame(height: 36)
                .onChange(of: viewModel.searchText) {
                    viewModel.onSearchTextChanged()
                }
                SearchOptionBadges(options: searchOptions)
                if !viewModel.searchText.isEmpty {
                    Button {
                        viewModel.searchText = ""
                        // H-8: .onChange(of: searchText) will trigger onSearchTextChanged() automatically
                    } label: {
                        Image(systemName: "xmark.circle.fill")
                            .font(.system(size: 18))
                            .foregroundColor(.secondary)
                    }
                    .buttonStyle(.plain)
                    .accessibilityIdentifier("clearButton")
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 16)
            .background(.ultraThinMaterial)
            .cornerRadius(10)
            .overlay(
                RoundedRectangle(cornerRadius: 10)
                    .stroke(Color.blue, lineWidth: 2)
            )
            .padding(.horizontal, 8)
            .padding(.top, 8)

            Divider()

            // Status bar
            HStack {
                if viewModel.isScanning {
                    ProgressView()
                        .controlSize(.small)
                    Text("正在扫描…已扫描 \(viewModel.scannedCount) 项")
                        .foregroundColor(.secondary)
                } else if viewModel.scanComplete {
                    if viewModel.isSyncing {
                        ProgressView()
                            .controlSize(.small)
                        Text("同步中…")
                            .foregroundColor(.orange)
                    } else if viewModel.isMonitoring {
                        Circle()
                            .fill(.green)
                            .frame(width: 6, height: 6)
                        Text("实时监听")
                            .foregroundColor(.green)
                            .fontWeight(.medium)
                    }
                    Text("已索引 \(viewModel.totalRecords) 个文件")
                        .foregroundColor(.secondary)
                        .accessibilityIdentifier("indexedCount")
                    if viewModel.isContentIndexing, let progress = viewModel.contentIndexProgress {
                        Text("·")
                            .foregroundColor(.secondary)
                        ProgressView()
                            .controlSize(.small)
                        Text("内容索引 \(progress.indexed)/\(progress.total)")
                            .foregroundColor(.orange)
                    }
                    if viewModel.totalMatches > 0 {
                        Text("·")
                            .foregroundColor(.secondary)
                        Text("匹配 \(viewModel.totalMatches) 项")
                            .foregroundColor(.secondary)
                            .accessibilityIdentifier("matchCount")
                        Text("·")
                            .foregroundColor(.secondary)
                        Text(String(format: "%.1fms", viewModel.queryTimeMs))
                            .foregroundColor(.secondary)
                    }
                }
                Spacer()
            }
            .font(.callout)
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
            .background(Color(nsColor: .controlBackgroundColor))
            .accessibilityIdentifier("statusBar")

            Divider()

            // Results list
            if viewModel.isScanning {
                VStack(spacing: 12) {
                    Spacer()
                    ProgressView()
                        .controlSize(.large)
                    Text("正在建立文件索引…已扫描 \(viewModel.scannedCount) 项")
                        .font(.callout)
                        .foregroundColor(.secondary)
                    Spacer()
                }
            } else if viewModel.isContentSearch {
                // Content search results
                if viewModel.contentResults.isEmpty && !viewModel.contentKeyword.isEmpty && viewModel.scanComplete {
                    VStack(spacing: 8) {
                        Spacer()
                        if viewModel.isContentIndexing {
                            ProgressView()
                                .controlSize(.large)
                                .padding(.bottom, 4)
                            Text("正在建立内容索引…")
                                .font(.headline)
                                .foregroundColor(.orange)
                            if let progress = viewModel.contentIndexProgress {
                                Text("已索引 \(progress.indexed) / \(progress.total) 个文件")
                                    .font(.subheadline)
                                    .foregroundColor(.secondary)
                            }
                            Text("索引完成后会显示搜索结果")
                                .font(.caption)
                                .foregroundColor(.secondary)
                        } else {
                            Image(systemName: "doc.text.magnifyingglass")
                                .font(.system(size: 36))
                                .foregroundColor(.secondary.opacity(0.5))
                                .padding(.bottom, 4)
                            Text("没有找到内容匹配")
                                .foregroundColor(.secondary)
                            if viewModel.contentIndexedCount == 0 {
                                Text("还没有建立内容索引。请在内容设置里配置扩展名。")
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                            }
                        }
                        Spacer()
                    }
                } else if viewModel.contentKeyword.isEmpty {
                    VStack {
                        Spacer()
                        Text("在 infile: 后输入关键词即可搜索文件内容")
                            .foregroundColor(.secondary)
                        Spacer()
                    }
                } else {
                    ScrollView {
                        LazyVStack(spacing: 0) {
                            ForEach(viewModel.contentResults) { item in
                                ContentResultRow(item: item, keyword: viewModel.contentKeyword)
                                    .padding(.horizontal, 8)
                                    .padding(.vertical, 2)
                            }
                        }
                    }
                    .id(scrollViewID)
                    .accessibilityIdentifier("contentResultsList")

                    if viewModel.totalMatches > 0 {
                        HStack {
                            Spacer()
                            Text("内容匹配 \(viewModel.contentResults.count) 项")
                                .font(.callout)
                                .foregroundColor(.secondary)
                                .padding(8)
                            Spacer()
                        }
                        .background(Color(nsColor: .controlBackgroundColor))
                    }
                }
            } else if viewModel.displayItems.isEmpty && !viewModel.searchText.isEmpty && viewModel.scanComplete {
                VStack {
                    Spacer()
                    Text("没有找到结果")
                        .foregroundColor(.secondary)
                        .accessibilityIdentifier("noResultsLabel")
                    Spacer()
                }
            } else {
                ScrollView {
                    LazyVStack(spacing: 0) {
                        if viewModel.showingRecent && !viewModel.displayItems.isEmpty {
                            HStack {
                                HStack(spacing: 4) {
                                    Image(systemName: "clock")
                                    Text("最近文件")
                                        .font(.callout)
                                        .fontWeight(.medium)
                                }
                                .foregroundColor(.white)
                                .padding(.horizontal, 8)
                                .padding(.vertical, 3)
                                .background(
                                    RoundedRectangle(cornerRadius: 5)
                                        .fill(Color.orange)
                                )
                                Spacer()
                            }
                            .padding(.horizontal, 12)
                            .padding(.vertical, 4)
                        }
                        ForEach(viewModel.displayItems) { item in
                            ResultRow(item: item, hints: viewModel.highlightHints)
                                .padding(.horizontal, 8)
                                .padding(.vertical, 2)
                                .id(item.id)
                        }
                        if viewModel.hasMoreResults {
                            HStack {
                                Spacer()
                                ProgressView()
                                    .controlSize(.small)
                                Text("正在加载更多结果…")
                                    .font(.callout)
                                    .foregroundColor(.secondary)
                                Spacer()
                            }
                            .padding(.vertical, 8)
                            .onAppear {
                                viewModel.loadMore()
                            }
                        }
                    }
                }
                .id(scrollViewID)
                .accessibilityIdentifier("fileResultsList")
                .animation(.easeInOut(duration: 0.25), value: viewModel.showingRecent)

                if viewModel.totalMatches > 0 {
                    HStack {
                        Spacer()
                        Text("显示 \(viewModel.displayItems.count) / \(viewModel.totalMatches) 个结果")
                            .font(.callout)
                            .foregroundColor(.secondary)
                            .padding(8)
                        Spacer()
                    }
                    .background(Color(nsColor: .controlBackgroundColor))
                }
            }
        }
        .frame(minWidth: 600, minHeight: 400)
        .onReceive(NotificationCenter.default.publisher(for: .rebuildIndex)) { _ in
            viewModel.rebuildIndex()
        }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            viewModel.onWindowFocusChanged(true)
            isSearchFieldFocused = true
        }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didResignActiveNotification)) { _ in
            viewModel.onWindowFocusChanged(false)
        }
        .onReceive(NotificationCenter.default.publisher(for: NSWindow.didDeminiaturizeNotification)) { _ in
            scrollViewID += 1
        }
    }
}
