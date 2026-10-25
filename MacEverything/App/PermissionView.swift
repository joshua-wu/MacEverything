import SwiftUI

struct PermissionView: View {
    @State private var hasFullDiskAccess: Bool = true

    var body: some View {
        Group {}.onAppear { checkAccess() }
            .task {
                while !hasFullDiskAccess {
                    try? await Task.sleep(for: .seconds(3))
                    checkAccess()
                }
            }

        if !hasFullDiskAccess {
            HStack {
                Image(systemName: "exclamationmark.triangle.fill")
                    .foregroundColor(.yellow)
                Text("需要开启完全磁盘访问权限，才能扫描所有文件。")
                    .font(.caption)
                Spacer()
                Button("打开设置") {
                    openPrivacySettings()
                }
                .font(.caption)
                .buttonStyle(.bordered)
            }
            .padding(8)
            .background(Color.yellow.opacity(0.15))
        }
    }

    private func checkAccess() {
        // Test read access to a TCC-protected directory
        let testPath = NSHomeDirectory() + "/Library/Safari"
        hasFullDiskAccess = FileManager.default.isReadableFile(atPath: testPath)
    }

    private func openPrivacySettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles") {
            NSWorkspace.shared.open(url)
        }
    }
}
