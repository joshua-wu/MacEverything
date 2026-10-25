# MacEverything-Green 全网宣发文案矩阵

本文件汇总了针对三大核心渠道的宣发文案初稿，可直接复制或微调后发布。

---

## 渠道一：V2EX（节点：`/go/macos` 或 `/go/programmer`）

**标题：**
```text
[排查实录] 用 MacEverything 的同学注意查下写入量：日均偷写 700GB SSD 底层定位与修复（附绿色版）
```

**正文内容：**

```markdown
大家好，最近排查 Mac 耗电和后台进程时，偶然发现了一个隐蔽但极其严重的“静默烧盘”问题。

我平时是 Windows 上 Everything 的重度依赖者，迁移到 Mac 后一直使用开源的 MacEverything，秒开检索非常爽。但今天看系统 I/O 统计时惊呆了：**MacEverything 在后台常驻 6 天，累计往固态硬盘里写了整整 1.53 TB 数据！**

### 1. 现象复现与自查命令
在后台没有任何主动搜索、仅仅让它常驻的情况下，只要你的电脑在正常运行，它就在以 **每小时 ~30 GB** 的速度狂写硬盘。

如果你也在用原版，建议打开系统终端运行这行 Python 脚本自查（直接调用内核 `proc_pid_rusage` 读取物理写入）：

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
    print(f'MacEverything 累计物理写入: {ru.diskio_byteswritten / (1024*1024):.2f} MB')
except Exception as e:
    print('未检测到 MacEverything 进程')
"
```
如果看到写了几十 GB 甚至几百 GB，不要怀疑，你的 SSD 正在被它高频折磨。

---

### 2. 源码深挖：Bug 是怎么产生的？
拉了源码仔细看持久化实现（`IndexPersistence.cpp` 和 `ServiceEngine+FSEvents.cpp`），发现问题出在持久化合并机制与系统临时文件的联动死循环上：

1. **临时目录被全盘监听**：FSEvents 监听全盘，包含了 macOS 系统后台极度活跃的临时目录（比如 `~/Library/Caches`、`~/Library/Biome`、`~/Library/Logs` 等）。浏览器一刷、微信一收消息，这些目录就会产生几十上百条临时文件变更。
2. **合并阈值极其激进**：源码里的 `kCompactThreshold` 硬编码成了 `100`。
   在后台，每隔 30 秒做一次定时轮询，只要增量 WAL 里累计了超过 100 条变动，它就会调用 `flatWriter_->fullRewrite()` 把**整个 275MB 的索引底库重新全量覆写一遍**。

也就是说：**30 秒产生 100 个缓存变更 $\to$ 全量重写 275MB 底库 $\to$ 每小时写 30GB $\to$ 每天写 700GB+！**

对于 512GB 焊死在主板上的 Mac（TBW 寿命一般就 150 TB 左右），按原版这个写盘速度，不到一年 SSD 寿命就会被消耗殆尽。

---

### 3. 修复方案与实测效果
我们在 Fork 分支上做了两处关键重构：
1. **智能排除高频临时缓存路径**：在 FSEvents 监听中针对系统临时目录（Caches, Biome, Logs, /private/var/db）进行事件过滤，真正只关注用户有意义的增删改。
2. **合并阈值提高至 50,000 条**：彻底消除 30 秒刷盘循环，只在变动真正达到规模时再做全量持久化。

**实测 36 分钟内核物理写盘数据：**
* 优化前：9 分钟刷盘 16 次，写入 **4.41 GB**
* 优化后：36 分钟全量重写 0 次，内核报告物理写入仅 **4.38 MB**（日均从 700GB 骤降到 < 150MB，降幅 99.97%）
* 79 个测试模块（574 个测试）全部 100% 通过，毫秒级搜索性能毫无影响。

---

### 4. 绿色版开源仓库与 DMG 下载
为了方便不会自己配 Xcode 编译的同学，我们在 GitHub 上配置了 Actions 自动构建工作流，提供开箱即用的免编译 DMG：

* **GitHub 仓库**：https://github.com/crimson-gzx/MacEverything
* **绿色版 DMG 下载**：https://github.com/crimson-gzx/MacEverything/releases
* 也已给上游提交了详细的 Issue 和 PR 说明，希望作者后续能合并回主干。

有在用这款工具的朋友强烈建议更新或替换，别让焊死的固态硬盘默默替 bug 买单。
```

---

## 渠道二：知乎 / 掘金（技术专栏与思考）

**标题：**
```text
从每小时狂写 30GB 到仅写 4MB：开源 macOS 文件搜索工具持久化踩坑与性能调优实录
```

**核心大纲与内容重点：**
1. **背景引入**：
   - 为什么 Mac 需要 Everything？Spotlight 的元数据局限与 Everything 倒排索引的高效。
   - 为什么本地搜索工具的持久化设计极其考验权衡？
2. **架构剖析**：
   - `MacEverything` 的存储模型：内存倒排 + Base Flat 索引（`index.v6` 275MB）+ 增量日志（WAL）。
   - 为什么作者设计了 WAL 却依然掉入了“30秒全量重写”的陷阱？
3. **FSEvents 的陷阱**：
   - macOS 各种隐藏目录的变动频率远超开发者想象（Biome 机器学习埋点、CloudKit 同步缓存、WebKit 缓存）。
   - 监听 `/` 时如果不做层级判定与白名单/黑名单剪枝，单日事件量可达数百万级。
4. **调优策略**：
   - 阈值从 100 调整到 50,000 的考量依据。
   - 为什么不直接调到几小时？冷启动 WAL Replay 性能与 Tombstone 内存占用的边界推演。
5. **文末指引**：
   - 开源分支代码与 Release 体验地址。

---

## 渠道三：小红书（爆款图文）

**选题切入：** 《查查你的 Mac！这个常用开源神器正在后台悄悄烧你的固态硬盘》

**封面图文设计建议（3:4 比例）：**
* **大标题（中文为主）**：Mac 固态寿命告急？！
* **副标题**：这个装机必备神器，居然每天偷写 700GB 硬盘！
* **视觉核心**：一张对比鲜明的写盘柱状图（修复前 700GB vs 修复后 0.15GB，标红警示）。

**正文文案（去 AI 味，清醒实用派）：**

```text
很多从 Windows 转到 Mac 的同学，都会装一个叫 MacEverything 的神器，在状态栏秒搜几百万个文件，确实比 Spotlight 快太多。

但今天查电源和磁盘监控时，发现了惊悚的一幕：
这个软件在后台常驻 6 天，居然悄悄写了 1.5TB 的固态硬盘！平均每天偷写 700GB！

要知道，现在不管是 M1、M2 还是 M3 的 MacBook，固态硬盘全都是焊死在主板上的！
512GB 的固态寿命一般就 150TB 左右，每天被它偷写 700GB，不到一年硬盘健康度就会直接暴跌！

⚠️ 怎么查你的 Mac 有没有中招？
打开「终端」，输入这条命令回车：
（见置顶评论代码，自动读取该软件累计真实写入量）

🔍 为什么会这样？
它每隔 30 秒就会检测一下电脑变动。但系统后台平时刷网页、收消息都在产生临时文件，一超过 100 个，它就把整个 275MB 的底库重新覆写一遍，变成了无休止的“烧盘循环”！

🛠️ 怎么解决？
我们已经在 GitHub 上推出了修复后的【绿色护盘版】：
1. 过滤掉无用的系统临时缓存；
2. 刷盘阈值大幅优化，写盘量直接暴降 99.97%（一天写不到 150MB）；
3. 搜索速度依然是 5 毫秒内秒开。

免编译、打包好的安装包直接放在主页/GitHub 了：
开源地址：github.com/crimson-gzx/MacEverything（点击 Releases 就能直接下 DMG 安装）

赶紧自查一下你的 Mac 固态写入量吧，别让焊死的硬盘为 Bug 买单！
#Mac技巧 #Mac使用指南 #程序员日常 #固态硬盘 #独立开发 #Mac必备软件
```
