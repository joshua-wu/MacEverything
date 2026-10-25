import SwiftUI

struct ContentSettingsView: View {
    @State private var extensions: [String] = []
    @State private var newExtension: String = ""
    @State private var maxFileSizeMB: Double = 1.0
    @State private var indexedCount: UInt32 = 0

    @State private var initialExtensions: [String] = []
    @State private var initialMaxFileSizeMB: Double = 1.0

    private let bridge = MacSearchBridge.shared()

    var body: some View {
        Form {
            Section("内容索引") {
                VStack(alignment: .leading, spacing: 8) {
                    Text("已索引文件：\(indexedCount)")
                        .font(.headline)

                    Divider()

                    Text("最大文件大小")
                        .font(.subheadline)
                    HStack {
                        Slider(value: $maxFileSizeMB, in: 0.1...10.0, step: 0.1)
                        Text(String(format: "%.1f MB", maxFileSizeMB))
                            .frame(width: 60)
                    }

                    Divider()

                    Text("文件扩展名")
                        .font(.subheadline)

                    ScrollView {
                        FlowLayout(spacing: 4) {
                            ForEach(extensions, id: \.self) { ext in
                                HStack(spacing: 2) {
                                    Text(".\(ext)")
                                        .font(.caption)
                                    Button {
                                        removeExtension(ext)
                                    } label: {
                                        Image(systemName: "xmark.circle.fill")
                                            .font(.caption2)
                                    }
                                    .buttonStyle(.plain)
                                }
                                .padding(.horizontal, 6)
                                .padding(.vertical, 2)
                                .background(Capsule().fill(Color.secondary.opacity(0.2)))
                            }
                        }
                    }
                    .frame(maxHeight: 120)

                    HStack {
                        TextField("添加扩展名…", text: $newExtension)
                            .textFieldStyle(.roundedBorder)
                            .onSubmit { addExtension() }
                        Button("添加") { addExtension() }
                            .disabled(newExtension.isEmpty)
                    }
                }
            }

            Section {
                Button("应用") {
                    applySettings()
                }
            }
        }
        .padding()
        .frame(width: 400, height: 420)
        .onAppear { loadSettings() }
    }

    private func loadSettings() {
        indexedCount = bridge.contentIndexedFileCount()
        extensions = (bridge.contentGetExtensions() as? [String]) ?? []
        maxFileSizeMB = Double(bridge.contentGetMaxFileSize()) / (1024.0 * 1024.0)

        initialExtensions = extensions
        initialMaxFileSizeMB = maxFileSizeMB
    }

    private func addExtension() {
        let ext = newExtension.trimmingCharacters(in: .whitespaces).lowercased()
            .replacingOccurrences(of: ".", with: "")
        guard !ext.isEmpty, !extensions.contains(ext) else { return }
        extensions.append(ext)
        newExtension = ""
    }

    private func removeExtension(_ ext: String) {
        extensions.removeAll { $0 == ext }
    }

    private func applySettings() {
        let dirty = extensions != initialExtensions || maxFileSizeMB != initialMaxFileSizeMB
        guard dirty else { return }
        bridge.setContentMaxFileSize(UInt64(maxFileSizeMB * 1024 * 1024))
        bridge.setContentExtensions(extensions)
        DispatchQueue.global().async {
            bridge.rebuildContentIndex()
        }
        initialExtensions = extensions
        initialMaxFileSizeMB = maxFileSizeMB
    }
}

struct FlowLayout: Layout {
    var spacing: CGFloat = 4

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let result = layout(in: proposal.width ?? 0, subviews: subviews)
        return result.size
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        let result = layout(in: bounds.width, subviews: subviews)
        for (index, point) in result.positions.enumerated() {
            subviews[index].place(at: CGPoint(x: bounds.minX + point.x, y: bounds.minY + point.y),
                                  proposal: .unspecified)
        }
    }

    private func layout(in width: CGFloat, subviews: Subviews) -> (size: CGSize, positions: [CGPoint]) {
        var positions: [CGPoint] = []
        var x: CGFloat = 0
        var y: CGFloat = 0
        var rowHeight: CGFloat = 0
        var maxWidth: CGFloat = 0

        for subview in subviews {
            let size = subview.sizeThatFits(.unspecified)
            if x + size.width > width && x > 0 {
                x = 0
                y += rowHeight + spacing
                rowHeight = 0
            }
            positions.append(CGPoint(x: x, y: y))
            rowHeight = max(rowHeight, size.height)
            x += size.width + spacing
            maxWidth = max(maxWidth, x)
        }

        return (CGSize(width: maxWidth, height: y + rowHeight), positions)
    }
}
