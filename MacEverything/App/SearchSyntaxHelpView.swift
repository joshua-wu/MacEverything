import SwiftUI

struct SearchSyntaxHelpView: View {
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                basicSearchSection
                booleanSection
                wildcardSection
                regexSection
                extensionFilterSection
                sizeFilterSection
                typeFilterSection
                pathFilterSection
                dateFilterSection
                lengthFilterSection
                modifierSection
                typeMacroSection
                structuredPathSection
                contentSearchSection
                tildeSection
            }
            .padding(24)
        }
    }

    // MARK: - Sections

    private var basicSearchSection: some View {
        SyntaxSection(title: "基础搜索") {
            SyntaxRow("hello", "子字符串匹配（不区分大小写）")
            SyntaxRow("hello world", "AND — 两个关键词都需要匹配")
            SyntaxRow("\"exact phrase\"", "引号内的完整短语匹配")
        }
    }

    private var booleanSection: some View {
        SyntaxSection(title: "布尔运算符") {
            SyntaxRow("A | B", "OR — 任意一个匹配即可")
            SyntaxRow("!A", "NOT — 排除匹配项")
            SyntaxRow("<A | B>", "使用尖括号进行分组")
            SyntaxNote("示例：<hello | world> .txt")
        }
    }

    private var wildcardSection: some View {
        SyntaxSection(title: "通配符 / Glob") {
            SyntaxRow("*", "零个或多个字符")
            SyntaxRow("?", "恰好一个字符")
            SyntaxRow("*.txt", "以 .txt 结尾的文件")
            SyntaxRow("test*", "以 test 开头的文件")
            SyntaxRow("*keyword*", "包含 keyword 的文件")
        }
    }

    private var regexSection: some View {
        SyntaxSection(title: "正则表达式") {
            SyntaxRow("regex:pattern", "ECMAScript 正则表达式（不区分大小写）")
            SyntaxNote("示例：regex:^test  regex:\\.cpp$")
        }
    }

    private var extensionFilterSection: some View {
        SyntaxSection(title: "扩展名筛选") {
            SyntaxRow("ext:cpp", "只匹配 .cpp 扩展名文件")
            SyntaxRow("ext:cpp;h;hpp", "多个扩展名，用分号分隔")
        }
    }

    private var sizeFilterSection: some View {
        SyntaxSection(title: "大小筛选") {
            SyntaxRow("size:>1mb", "大于 1 MB")
            SyntaxRow("size:<500kb", "小于 500 KB")
            SyntaxRow("size:>=1gb", "至少 1 GB")
            SyntaxRow("size:100kb..1mb", "介于 100 KB 和 1 MB 之间")
            SyntaxNote("单位：b, kb/k, mb/m, gb/g, tb/t")
        }
    }

    private var typeFilterSection: some View {
        SyntaxSection(title: "类型筛选") {
            SyntaxRow("file:", "只匹配文件")
            SyntaxRow("folder:", "只匹配文件夹")
            SyntaxRow("type:file", "等同于 file:")
            SyntaxRow("type:folder", "等同于 folder:")
        }
    }

    private var pathFilterSection: some View {
        SyntaxSection(title: "路径筛选") {
            SyntaxRow("path:keyword", "路径包含 keyword")
            SyntaxRow("nopath:keyword", "路径不包含 keyword")
            SyntaxRow("parent:dirname", "直接父级目录匹配")
            SyntaxRow("depth:<3", "目录深度小于 3")
            SyntaxRow("depth:>5", "目录深度大于 5")
        }
    }

    private var dateFilterSection: some View {
        SyntaxSection(title: "日期筛选") {
            SyntaxNote("前缀：dm:（修改时间） dc:（创建时间） da:（访问时间）")
            SyntaxRow("dm:today", "今天修改")
            SyntaxRow("dm:yesterday", "昨天修改")
            SyntaxRow("dm:thisweek", "本周修改")
            SyntaxRow("dm:lastmonth", "上个月修改")
            SyntaxRow("dm:last7days", "最近 7 天内修改")
            SyntaxRow("dm:last3months", "最近 3 个月内修改")
            SyntaxRow("dm:2024-01-15", "指定日期修改")
            SyntaxRow("dm:>2024-01", "2024 年 1 月之后修改")
            SyntaxRow("dm:2024-01..2024-06", "2024 年 1 月到 6 月之间修改")
            SyntaxNote("也支持：datemodified:  datecreated:  dateaccessed:")
        }
    }

    private var lengthFilterSection: some View {
        SyntaxSection(title: "文件名长度筛选") {
            SyntaxRow("len:>10", "文件名长度大于 10 个字符")
            SyntaxRow("len:<5", "文件名长度小于 5 个字符")
            SyntaxRow("len:>=8", "文件名长度至少 8 个字符")
        }
    }

    private var modifierSection: some View {
        SyntaxSection(title: "匹配修饰符") {
            SyntaxRow("case:term", "强制区分大小写匹配")
            SyntaxRow("nocase:term", "强制不区分大小写匹配")
            SyntaxRow("ww:hello", "整词匹配")
            SyntaxRow("wfn:readme", "完整文件名匹配（不含扩展名）")
            SyntaxNote("别名：wholeword:  wholefilename:")
        }
    }

    private var typeMacroSection: some View {
        SyntaxSection(title: "文件类型快捷筛选") {
            SyntaxRow("audio:", "mp3, wav, flac, aac, ogg, m4a, wma, alac")
            SyntaxRow("video:", "mp4, avi, mkv, mov, wmv, flv, webm, m4v")
            SyntaxRow("pic:", "jpg, png, gif, bmp, tiff, webp, svg, heic ...")
            SyntaxRow("doc:", "pdf, doc, docx, xls, xlsx, ppt, pptx, txt, md ...")
            SyntaxRow("exe:", "app, dmg, pkg, sh, command")
            SyntaxRow("zip:", "zip, rar, 7z, tar, gz, bz2, xz, tgz, zst, lz4")
        }
    }

    private var structuredPathSection: some View {
        SyntaxSection(title: "结构化路径查询") {
            SyntaxRow("/abc/def", "文件名匹配 def，路径包含 abc")
            SyntaxRow("/abc/def/", "abc 下名为 def 的目录")
            SyntaxRow("/abc/def/*", "列出 abc 下 def 目录的子项")
            SyntaxRow("/abc/*/def", "非相邻路径段：abc 和 def 中间可有其它目录")
            SyntaxRow("/Users/me/My Folder/file.md", "可直接粘贴包含空格的完整路径")
            SyntaxRow("/path/file.swift:42:7", "自动忽略编辑器的行号和列号后缀")
            SyntaxNote("路径段会从右往左匹配；粘贴完整路径时不需要手动加引号")
        }
    }

    private var contentSearchSection: some View {
        SyntaxSection(title: "内容搜索") {
            SyntaxRow("content:keyword", "在文件内容中搜索 keyword")
        }
    }

    private var tildeSection: some View {
        SyntaxSection(title: "波浪号展开") {
            SyntaxRow("~/Downloads", "将 ~ 展开为你的用户主目录")
            SyntaxRow("~/*.txt", "在用户主目录中使用 Glob 匹配")
            SyntaxNote("只有当 ~ 位于查询开头时才会展开")
        }
    }
}

// MARK: - Helper Components

private struct SyntaxSection<Content: View>: View {
    let title: String
    @ViewBuilder let content: () -> Content

    init(title: String, @ViewBuilder content: @escaping () -> Content) {
        self.title = title
        self.content = content
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title)
                .font(.headline)
                .padding(.bottom, 2)
            content()
        }
    }
}

private struct SyntaxRow: View {
    let syntax: String
    let description: String

    init(_ syntax: String, _ description: String) {
        self.syntax = syntax
        self.description = description
    }

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Text(syntax)
                .font(.system(.body, design: .monospaced))
                .foregroundColor(.accentColor)
                .frame(width: 180, alignment: .leading)
            Text(description)
                .foregroundColor(.secondary)
            Spacer()
        }
    }
}

private struct SyntaxNote: View {
    let text: String

    init(_ text: String) {
        self.text = text
    }

    var body: some View {
        Text(text)
            .font(.callout)
            .foregroundColor(.secondary)
            .italic()
            .padding(.leading, 4)
    }
}
