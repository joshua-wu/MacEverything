<p align="center">
  <img src="MacEverything/Assets.xcassets/AppIcon.appiconset/icon_256.png" alt="MacEverything" width="128" />
</p>

<h1 align="center">MacEverything</h1>

<p align="center">
  <b>macOS 极速文件搜索工具</b> — 在数百万文件中毫秒级定位任意文件。<br/>
  灵感源自 Windows 上的 <a href="https://www.voidtools.com/">Everything</a>，Mac 上无出其右。
</p>

<p align="center">
  <b>中文</b> | <a href="README_EN.md">English</a>
</p>

<p align="center">
  <a href="https://github.com/crimson-gzx/MacEverything/releases"><img src="https://img.shields.io/badge/download-DMG%20(Green%20Edition)-orange?logo=apple" alt="Download DMG" /></a>
  <img src="https://img.shields.io/badge/SSD%20Writes--99.97%25-brightgreen" alt="SSD Friendly" />
  <a href="#安装"><img src="https://img.shields.io/badge/macOS-13%2B-blue?logo=apple" alt="macOS 13+" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="MIT License" /></a>
  <a href="#测试体系"><img src="https://img.shields.io/badge/tests-79%20modules-brightgreen" alt="79 test modules" /></a>
  <a href="#ai-工具集成-mcp"><img src="https://img.shields.io/badge/MCP-compatible-blueviolet" alt="MCP Compatible" /></a>
</p>

> 🌿 **本仓库为【绿色护盘版】（MacEverything-Green）**
> 彻底解决原版因持久化合并阈值过低与系统临时目录高频变动引起的 **SSD 严重磨损 Bug**（原版每 30 秒全量重写 275MB，日写盘量高达 ~700 GB）。
> 本分支通过 FSEvents 系统临时路径智能过滤与合并策略重构，将实际写盘量**降低 99.97%（日均 < 150MB）**，在保持 < 5ms 极速检索体验的同时彻底消除固态硬盘损耗焦虑与发热。

---

<p align="center">
  <img src="assets/screen-shot.jpg" alt="MacEverything Screenshot" width="720" />
</p>

## 🌿 为什么选择绿色护盘版？

### 1. 痛点：原版静默“烧盘”隐患
原版在监听全盘文件系统变动（FSEvents）时，未过滤系统高频更新的临时目录（如 `~/Library/Caches`、`~/Library/Biome` 等），且持久化合并阈值仅硬编码为 `100` 条变动。这导致后台平均每 30 秒就会将整个 275MB 的索引底库全量重写到磁盘一次：
* **原版每小时写盘**：约 **30 GB**
* **原版每天写盘**：高达 **700 GB+**
* **潜在危害**：Mac 标配 512GB SSD 寿命（TBW）通常为 150 TB。以原版写盘速率，仅需半年到一年就会耗尽 SSD 寿命，且全板焊死无法更换。

### 2. 实盘运行对比（macOS 内核 `rusage` 实测）

| 核心指标 | 原版（Unpatched） | 绿色护盘版（本分支） | 优化效果 |
| :--- | :---: | :---: | :---: |
| **底库全量重写频率** | 约 **30 秒/次** | **仅累计 50,000 条有效变动时** | 消除无效全量刷盘 |
| **36 分钟内核物理写入** | **~17.6 GB** | **4.38 MB** | 📉 **暴降 99.97%** |
| **24 小时预估总写入** | **~700 GB** | **< 150 MB** | 📉 **暴降 99.98%** |
| **512GB SSD 预计耗损耗尽** | ~1 年 | **> 1000 年** | 🛡️ **彻底解除硬件焦虑** |
| **搜索响应延迟** | < 5ms | < 5ms | ⚡ 毫秒级性能完全一致 |
| **单元测试验证** | 79 模块全通 | 79 模块（574 个测试）全通 | ✅ 零回归与零破坏 |

### 3. 一行命令：检测你机器上的 MacEverything 写入量
若你正在使用原版，可在系统终端运行以下命令，直接查看它运行至今的真实物理写盘量：
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
    print(f'MacEverything 累计物理写盘: {ru.diskio_byteswritten / (1024*1024):.2f} MB')
except Exception as e:
    print('未检测到运行中的 MacEverything 进程')
"
```
*如果数值已达几十甚至上百 GB，说明你的 SSD 正在遭受持续磨损，建议立即升级为本绿色护盘版。*

---

## 功能亮点

### 极速搜索

索引整块磁盘 **500 万+ 文件只需 14 秒**，之后每次搜索 **不到 5ms** 返回结果。比 Spotlight 快两个数量级。

| 对比项 | MacEverything | Spotlight | `find` |
|--------|:---:|:---:|:---:|
| 索引 500 万文件 | ~14 秒 | 数分钟以上 | 无索引 |
| 搜索延迟 | **< 5ms** | 200ms–2s | 5–30s |
| 实时文件监控 | FSEvents | FSEvents | 无 |
| 内容搜索 | Trigram 索引 | 侧重元数据 | `grep` |
| AI 工具集成 | 内置 MCP | 不支持 | 不支持 |

### 随叫随到

按 **`Option+Space`** 随时唤出搜索窗口（快捷键可自定义），搜索栏自动获得焦点 — 唤起即输入，搜完即走。支持开机自启（最小化后台运行），不打扰你的工作流。

### 智能输入体验

- **Ghost 文本自动补全** — 输入时自动显示半透明建议文字，来自搜索历史（按频率排序）或系统关键词（如输入 `ex` 提示 `ext:`）。按 **Tab** 一键接受
- **搜索栏语法高亮** — 实时彩色标注：过滤器名紫色、参数蓝色、引号字符串橙色、运算符红色
- **搜索选项徽章** — 搜索栏旁的彩色徽章，一键切换 Regex / Case Sensitive / Whole Word / Match Filename

### Everything 风格查询语法

完整的 AST 解析器，支持 15+ 过滤器、布尔运算、glob 通配符、正则表达式。内置语法帮助窗口（**Cmd+?**）。

| 查询 | 说明 |
|------|------|
| `readme` | 文件名包含 "readme" |
| `*.swift` | 所有 Swift 源文件 |
| `ext:py size:>1mb` | 大于 1MB 的 Python 文件 |
| `dm:today` | 今天修改过的文件 |
| `config path:/usr` | `/usr` 下包含 "config" 的文件 |
| `"exact phrase"` | 精确短语匹配 |
| `foo OR bar` | 布尔 OR 运算 |
| `case:Makefile` | 区分大小写搜索 |
| `regex:^test_.*\.py$` | 正则表达式搜索 |
| `type:folder node_modules` | 仅搜索目录 |
| `~/Documents/*.pdf` | Tilde 展开 + glob |
| `infile:TODO ext:cpp` | C++ 文件中搜索 "TODO" |

<details>
<summary><b>全部过滤器列表</b></summary>

| 过滤器 | 说明 | 示例 |
|--------|------|------|
| `ext:` | 文件扩展名 | `ext:swift,h` |
| `size:` | 文件大小 | `size:>1mb`, `size:100kb-5mb` |
| `type:` | 文件/目录 | `type:folder` |
| `path:` | 路径包含 | `path:Downloads` |
| `nopath:` | 路径排除 | `nopath:node_modules` |
| `parent:` | 直接父目录 | `parent:src` |
| `depth:` | 目录深度 | `depth:<3` |
| `dm:` | 修改日期 | `dm:today`, `dm:>2024-01-01` |
| `dc:` | 创建日期 | `dc:thisweek` |
| `da:` | 访问日期 | `da:last7days` |
| `len:` | 文件名长度 | `len:>50` |
| `case:` | 区分大小写 | `case:README` |
| `regex:` | 正则表达式 | `regex:^test_` |
| `ww:` | 全词匹配 | `ww:test` |
| `wfn:` | 全文件名匹配 | `wfn:Makefile` |
| `content:` / `infile:` | 内容搜索 | `infile:TODO` |
| `audio:` `video:` `pic:` `doc:` `zip:` | 文件类型宏 | `audio:` = 所有音频文件 |

</details>

### 全文内容搜索

输入 `infile:关键词` 搜索文件内容，结果附带关键词高亮上下文片段。基于 Trigram 索引加速，仅重新索引变更文件。可在「内容设置」中配置索引的文件类型和最大文件大小。

### 实时同步，永不过时

- **文件监控** — 基于 FSEvents 实时监听文件系统变更，新建、重命名、删除的文件立即出现在搜索结果中
- **两阶段即时启动** — 启动时先加载磁盘缓存（立即可搜），后台通过 FSEvents 增量追赶变更，搜索零等待
- **焦点感知省电** — 窗口不在前台时暂停刷新，回到前台时批量追赶，几乎零后台 CPU 占用

### 交互细节

- **智能高亮** — 搜索结果中匹配部分高亮标记，基于 AST 感知：正确处理 glob 通配符、正则、大小写、NOT 排除等复杂场景
- **拖放** — 直接从搜索结果拖放文件到 Finder、VS Code、Xcode 等任意应用
- **右键菜单** — 打开 / 在 Finder 中显示 / 复制路径
- **Cmd+Click** — 快速在 Finder 中定位文件
- **最近文件** — 搜索栏为空时自动展示最近修改的文件

### AI 工具集成 (MCP)

内置 [Model Context Protocol](https://modelcontextprotocol.io/) 服务器，让 AI 编程工具即时搜索你的文件系统。在菜单栏一键开启，支持 **Claude Code**、**Cursor**、**Claude Desktop**。

```
Claude Code / Cursor / Claude Desktop
       │
       ▼  (stdio JSON-RPC 2.0)
  MacEverythingMCP
       │
       ▼  (HTTP localhost:19860)
  MacEverything.app
```

| 工具 | 说明 |
|------|------|
| `search_files` | 文件名搜索（Trigram 加速） |
| `search_content` | 全文内容搜索 |
| `recent_files` | 最近修改的文件 |
| `index_status` | 索引统计与健康状态 |

### HTTP API

本地 REST API 监听 `localhost:19860`，方便脚本调用和自动化：

```bash
curl "http://localhost:19860/api/search?q=readme&limit=10"       # 搜索文件
curl "http://localhost:19860/api/search/content?q=TODO"           # 内容搜索
curl "http://localhost:19860/api/recent?limit=20"                 # 最近文件
curl "http://localhost:19860/api/status"                          # 索引状态
```

### 安装

#### 下载 DMG（开箱即用，推荐）

1. 从 [Releases 页面](https://github.com/crimson-gzx/MacEverything/releases) 下载最新的 `MacEverything-Green.dmg`
2. 打开 DMG，将 `MacEverything.app` 拖入「应用程序」文件夹
3. 首次启动如提示“无法打开，因为无法验证开发者”，请前往 **系统设置 -> 隐私与安全性**，找到底部提示并点击 **“仍要打开”**
4. 启动后按提示授予 **完全磁盘访问权限（Full Disk Access）**
5. 等待初始扫描完成（约 14 秒），按 `Option+Space` 随时极速检索

#### 从源码构建

**环境要求：** macOS 13+，Xcode 15+，Homebrew (`brew install re2 abseil`)

```bash
git clone https://github.com/crimson-gzx/MacEverything.git && cd MacEverything

xcodebuild -project MacEverything.xcodeproj -scheme MacEverything \
  -configuration Release build SYMROOT=build

hdiutil create -volname MacEverything \
  -srcfolder build/Release/MacEverything.app \
  -ov -format UDZO MacEverything.dmg
```

#### CLI 守护进程

无头模式，适用于服务器或自动化环境：

```bash
make daemon
./maceverything-daemon --port 19860 --root /
```

---

<h2 align="center">开发者篇：技术深度</h2>

<p align="center">
  以下内容面向对实现细节感兴趣的开发者。
</p>

### 架构总览

```
┌─────────────────────────────────────┐
│       SwiftUI 应用层                │  界面 · ViewModel · MVVM
├─────────────────────────────────────┤
│    Objective-C++ 桥接层             │  零开销互操作
├─────────────────────────────────────┤
│       C++20 核心引擎                │  扫盘 · 索引 · 搜索 · 持久化
└─────────────────────────────────────┘
```

同一套 C++20 核心引擎驱动三种部署模式：

| 模式 | 说明 |
|------|------|
| **GUI 应用** | SwiftUI 菜单栏应用，`Option+Space` 全局快捷键 |
| **CLI 守护进程** | 无头 `maceverything-daemon` — 相同引擎，无 UI |
| **MCP 服务器** | `MacEverythingMCP` — stdio JSON-RPC 代理，供 AI 工具调用 |

### 核心引擎

| 组件 | 关键设计 |
|------|---------|
| **DirectoryScanner** | 多线程工作窃取 + `getattrlistbulk` 单次系统调用批量获取文件属性，4–32 线程自适应 |
| **SearchEngine** | Trigram 倒排索引（name + path 双索引）+ 竞争选择最优候选集 + SoA 列式过滤 |
| **ContentIndex** | Trigram 全文倒排索引，FNV-1a 哈希增量更新，仅重新索引变更文件 |
| **SIMDSearch** | ARM NEON 128-bit first-last byte 向量化匹配 + 2x 循环展开，单线程 11.5 GB/s |
| **IndexPersistence** | WAL + CRC32 + 分页脏页刷写 + 原子 rename，COW 无阻塞压缩（锁持有 < 100ms） |
| **FileSystemWatcher** | FSEvents + eventId 增量回放 + 日志截断检测自动子树重扫 |
| **PathTable** | 路径字符串 intern 化 — 目录路径仅存 `uint32` 索引，百万文件节省 ~550MB |
| **QueryParser** | 完整 AST 管线：Tokenizer → FilterParser → Parser → QueryAST，30+ 过滤器关键词 |

### 基准测试

测试环境：macOS Darwin 24.3.0，**540 万索引文件**，48 种查询类型：

#### 搜索延迟

| 查询类型 | 平均延迟 | 示例 |
|----------|:---------:|------|
| 长关键词 (7+ 字符) | **0.1–1ms** | `screenshot` 0.1ms, `dockerfile` 0.1ms |
| 中等关键词 (4–6 字符) | **1–5ms** | `readme` 1.2ms, `config` 4.7ms |
| Glob 模式 | **0.7–18ms** | `*.cpp` 0.7ms, `*.swift` 1.5ms |
| 路径查询 | **3–32ms** | `package.json` 2.9ms |
| 全部 48 种查询 (均值) | **10.5ms** | SoA 优化后最新结果 |

#### Trigram vs 线性扫描

| 查询 | Trigram | 线性扫描 | 加速比 |
|------|:------:|:------:|:------:|
| `node_modules` | 0.5ms | 154ms | **308x** |
| `application` | 2.1ms | 175ms | **83x** |
| `readme` | 1.2ms | 49ms | **41x** |

#### SIMD 字符串搜索 (Apple M3 Pro)

| 方法 | 吞吐量 | 对比 `std::string::find` |
|------|:------:|:------------------------:|
| `std::string::find` | 1.2 GB/s | 基准线 |
| **NEON 128-bit（单线程）** | **11.5 GB/s** | **9.5x** |
| **NEON 128-bit（12 线程）** | **74.3 GB/s** | **60.7x** |

### 关键技术

| 技术 | 效果 |
|------|------|
| `getattrlistbulk` | 单次系统调用批量获取文件属性 — 避免逐文件 `stat` |
| Trigram 倒排索引 | 亚线性搜索：比线性扫描快 33x–308x |
| SoA 列式布局 | 缓存友好的内存访问模式，纯过滤查询 SIMD 批量判断 16 条记录 |
| `__builtin_prefetch` | 预取距离 8，隐藏候选验证阶段的随机内存访问延迟 |
| ARM NEON SIMD | 128-bit 向量化字符串匹配，2x 循环展开，逼近内存带宽上限 |
| GCD 并行扫描 | Trigram 无法加速时启用多核线性扫描 |
| StringPool 连续内存 | 文件名紧凑排列在单一 `char` 缓冲区，SIMD 友好 |
| PathTable intern 化 | 目录路径仅存 `uint32` 索引 — 百万文件节省 ~550MB |
| Generation 计数器 | 每 1024 次迭代检查，快速输入时零开销取消过时查询 |
| APFS Firmlink 去重 | inode + devid 检测，正确处理 macOS Data/System 卷合并环路 |
| Regex Trigram 预过滤 | 从正则中提取字面量生成 trigram 候选，~7s → <100ms |
| 自适应 Trigram 旁路 | 候选集过大时自动回退并行扫描，避免无效索引查找 |
| COW 无阻塞压缩 | 写时复制，压缩期间独占锁持有 < 100ms（原 30–60s） |
| 分页增量持久化 | 仅写入脏页，典型 flush I/O 从 ~112MB 降至 KB 级 |

### 测试体系

79 个测试模块覆盖完整技术栈，支持 AddressSanitizer 和 ThreadSanitizer：

```bash
make test          # 快速单元测试 + 桥接层 lint
make test-slow     # 集成测试（全盘扫描、FSEvents、端到端）
make test-all      # 全部测试
make test-asan     # AddressSanitizer
make test-tsan     # ThreadSanitizer
```

覆盖范围：
- **核心引擎**：扫描、查询、变更、压缩、排序、路径搜索
- **持久化**：WAL CRC 完整性、批量回放、竞态条件、分页持久化 v5
- **内容索引**：Trigram、压缩、修改时间跟踪、WAL 跟踪
- **搜索/查询**：分词器、解析器、过滤器、日期过滤、结构化查询、正则 Trigram、高亮提示
- **性能**：SIMD 搜索、千万条记录合成基准、Trigram 竞争测试
- **集成**：线程安全、端到端、HTTP 引擎热替换、MCP 协议
- **内存安全**：ASan + TSan 构建

### 项目结构

```
MacEverything/
├── Core/                  # C++20 核心引擎
│   ├── SearchEngine       # Trigram 索引 + 并行查询（5 个 .cpp 文件）
│   ├── DirectoryScanner   # 多线程批量扫描器
│   ├── ContentIndex       # 全文倒排索引
│   ├── IndexPersistence   # WAL + 分页持久化
│   ├── FileSystemWatcher  # FSEvents 实时监控
│   ├── HttpServer         # 内嵌 REST API 服务器
│   ├── SIMDSearch         # ARM NEON 向量化搜索
│   ├── QueryAST/Parser    # 完整查询语言管线
│   ├── PathTable          # 字符串 intern 表
│   └── ServiceEngine      # 生命周期编排
├── Bridge/                # Objective-C++ 桥接层
│   └── MacSearchBridge    # C++ ↔ Swift 零开销互操作
├── App/                   # SwiftUI 应用层
│   ├── ContentView        # 主搜索界面
│   ├── SearchViewModel    # MVVM + 分级防抖
│   ├── HotkeyManager      # 全局快捷键注册
│   └── MCPConfigManager   # MCP 一键配置
├── CLI/                   # 命令行工具
│   ├── daemon_main        # 无头守护进程
│   └── mcp_main           # MCP 服务器（stdio JSON-RPC）
└── tests/                 # 79 个测试模块
```

## 参与贡献

欢迎贡献代码！请遵循以下流程：

1. Fork 本仓库
2. 创建功能分支 (`feat/...`) 或修复分支 (`fix/...`)
3. 为新功能编写测试
4. 确保 `make test-all` 通过
5. 提交 Pull Request

## 许可证

本项目基于 MIT 许可证开源 — 详见 [LICENSE](LICENSE) 文件。

---

<p align="center">
  <b>如果 MacEverything 让你找文件更快了，请给一颗 Star 支持！</b>
</p>
