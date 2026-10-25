# MacEverything 中文增强版说明

这是基于 [joshua-wu/MacEverything](https://github.com/joshua-wu/MacEverything) 的中文体验增强分支。

原项目是一个 macOS 极速文件搜索工具，灵感来自 Windows Everything，使用 C++ 核心和 SwiftUI App，在大量文件场景下追求毫秒级搜索体验。

## 二改方向

这个分支主要做中文用户体验增强：

- 主界面状态文案中文化
- 扫描 / 同步 / 索引 / 匹配数量中文化
- 内容搜索提示中文化
- 权限提示中文化
- 快捷键设置中文化
- 内容设置中文化
- 后续可继续补完整中文菜单、安装引导、首次运行说明和中文发行包

## 当前已改

- `MacEverything/App/ContentView.swift`
- `MacEverything/App/PermissionView.swift`
- `MacEverything/App/ShortcutSettingsView.swift`
- `MacEverything/App/ContentSettingsView.swift`

## 保留原项目信息

本项目保留原 MIT License 和原作者/贡献者版权信息。

如果继续发布自己的二改版，建议：

1. 明确写明基于上游项目修改。
2. 保留 MIT License。
3. 不要删掉原作者信息。
4. 改一个自己的发行版名称，例如 `MacEverything 中文增强版` 或 `MacEverything-CN`。
5. 如果长期维护，建议定期同步上游更新。

## 后续建议

下一步可以继续做：

1. 菜单栏完整中文化。
2. 搜索语法帮助窗口中文化。
3. 首次运行引导中文化。
4. Release 页面中文化。
5. 打包 DMG，提供中文安装说明。
6. 如果要公开发布，最好加上“基于 joshua-wu/MacEverything 修改”的说明。
