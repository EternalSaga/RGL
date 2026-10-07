// imgqa CLI 注册表 —— 子命令 / flag / 默认值 / 帮助文本的**单一真相源**（批次 B）。
//
// ## 为什么必须有这张表（根因，不是代码洁癖）
//
// 改之前：默认值写在 main.cpp 的 dispatch 里（`P.get(args, "--burst", "96")`），帮助
// 文本写在另一段字符串字面量里，两边靠人肉同步。实测已经漂到**误导判据口径**的程度：
//
//   - `temporal-homography --env/--ao`：帮助写 "1.0 / 0.5"，代码默认是 **−1 / −1**
//     （负数 = 不覆写 = 门关）——读帮助的人会以为 IBL 开着跑出来的数；
//   - `temporal-tail --burst` 写 64 实 96、`--max-lag` 写 24 实 16；
//   - `temporal-residual --static-boost` 写 0.02 实 0。
//
// 本工具自己的红线是「**拒绝在默认值上下判据**」（engine.cpp 的键名校验、
// Engine::paramValue 找不到就抛）。帮助文本写错默认值 = 这条红线在 CLI 层自我违反。
// 于是：默认值只准出现在下面这张表里，`-h` 的 synopsis、`args` 的回显、dispatch 拿到的
// 值，全部从同一张表来。
//
// ## 三条机制
//
// 1. **Args 绑定 spec**：默认值只能来自 spec。三参数 `get(args, key, def)` 已删——
//    留着它，字面量默认值就还有地方活。读未声明的 flag 直接抛。
// 2. **未知参数报错（rc=2）**：拼错 `--strenght` 过去会被静默忽略，然后在默认值上
//    跑完一次判据（正是工具禁止的失效形态），现在直接挡下并点名合法参数集。
// 3. **`imgqa args <子命令>`**：只回显解析后的 (flag, 值)，不跑任何东西。它既是"我到底
//    会用什么默认值跑"，也是"默认值没变"这条判据的机械入口——grep 源码不是判据。
//
// ## 表里那两段散文
//
// summary/notes 是**判据契约原文**（判决线、阳性锚点、口径警告都在里面），逐字保留；
// 只有 synopsis 行由表生成。改散文就改这张表，别再抄第三份。flag 的 help 同理：只准从
// 既有权威散文（-h 正文 / imgqa.h 头注释 / dispatch 代码注释）里摘，**不许新写事实**。
#include "cli.h"

#include <cstdio>
#include <string>
#include <vector>

namespace RGL::imgqa {
namespace {

/// -h 里 summary 的起始列（与原 -h 对齐：短 synopsis 补空格到这一列，长的空两格）。
constexpr size_t kSummaryCol = 45;

/// 终端显示宽度。synopsis 里带中文（`[--camera 名]` 那几条），按字节补空格会让 summary
/// 少跑十几个空格——`-h` 是给人读的，对齐差一列就会被看成「这行没对齐」。
/// 粗略够用：UTF-8 首字节 ≥ 0xE0（三字节起，覆盖本项目出现的全部 CJK 与箭头）算 2 列。
size_t displayWidth(const std::string& s)
{
    size_t w = 0;
    for (const unsigned char ch : s) {
        if ((ch & 0xC0) == 0x80)
            continue;  // 续字节
        w += ch >= 0xE0 ? 2 : 1;
    }
    return w;
}

const char* const kHeader =
    "imgqa —— 区域归因的图像判据工具箱（判据的区域来自几何，不来自图像）\n\n"
    "用法: imgqa <子命令> [选项]\n";

bool isFlagToken(const std::string& a)
{
    return a.size() > 2 && a[0] == '-' && a[1] == '-';
}

const FlagSpec* findFlag(const SubcommandSpec& spec, const std::string& flag)
{
    for (const auto& f : spec.flags)
        if (flag == f.name)
            return &f;
    return nullptr;
}

std::string join(const std::vector<std::string>& v, const char* sep)
{
    std::string s;
    for (size_t i = 0; i < v.size(); ++i)
        s += (i == 0 ? "" : sep) + v[i];
    return s;
}

/// 本命令的合法参数一览。错误消息必须点名它——只说"未知参数"等于没校验。
std::string legalArgsFor(const SubcommandSpec& spec)
{
    std::string s;
    for (const auto& f : spec.flags) {
        s += (s.empty() ? "" : " ");
        s += f.name;
        if (f.kind != FlagKind::Bool)
            s += " <值>";
    }
    if (spec.allowPositional)
        s += (s.empty() ? "" : " ") + std::string("位置参数");
    return s;
}

/// 默认值为空串的 flag 在 synopsis 里显示成 `''`——留白会被读成"没这个参数"。
std::string shownDef(const FlagSpec& f)
{
    if (f.kind == FlagKind::Bool)
        return "";
    return (f.def != nullptr && f.def[0] != '\0') ? f.def : "''";
}

/// synopsis：`cmd [positional] [--a 默认] [--b]`；同名 flag（值 + 存在性）只列一次。
/// seen 用空格做分隔符 ⟹ 查 "--clip" 不会误命中已列出的 "--clip-soft"。
std::string synopsisFor(const SubcommandSpec& spec)
{
    std::string s = spec.name;
    if (spec.positionalUsage != nullptr && spec.positionalUsage[0] != '\0')
        s += std::string(" ") + spec.positionalUsage;
    std::string seen = " ";
    for (const auto& f : spec.flags) {
        const std::string slot = std::string(" ") + f.name + " ";
        if (seen.find(slot) != std::string::npos)
            continue;
        seen += slot;
        s += " [";
        s += f.name;
        if (f.kind != FlagKind::Bool)
            s += " " + shownDef(f);
        s += "]";
    }
    return s;
}

/// 一行回显：值 flag 出 `--x = '值'`；Bool flag 未点亮只出 `--x(bool)`，点亮才写
/// `= true`（未点亮写 false 会让"没数"和"数是 false"混起来）。
std::string echoLine(const std::string& flag, bool isBool, const Args& args)
{
    if (!isBool)
        return "   " + flag + " = '" + args.get(flag) + "'\n";
    return "   " + flag + "(bool)" + (args.has(flag) ? " = true" : "") + "\n";
}

}  // namespace

const SubcommandSpec* findSubcommand(const std::string& name)
{
    for (const auto& s : subcommands())
        if (name == s.name)
            return &s;
    return nullptr;
}

Args::Args(const std::vector<std::string>& argv, const SubcommandSpec& spec) : spec_(&spec)
{
    for (size_t i = 0; i < argv.size(); ++i) {
        const std::string& a = argv[i];
        const FlagSpec* f = findFlag(spec, a);
        if (f == nullptr) {
            (isFlagToken(a) ? unknown_ : positional_).push_back(a);
            continue;
        }
        supplied_.insert(a);
        if (f->kind == FlagKind::Bool)
            continue;
        if (i + 1 < argv.size())
            values_.emplace(a, argv[++i]);  // 首次出现优先（与旧 P.get 逐字一致）
        else
            missing_.push_back(a);
    }
}

const FlagSpec& Args::need(const std::string& flag) const
{
    const FlagSpec* f = findFlag(*spec_, flag);
    if (f == nullptr)
        throw UsageError(std::string("注册表里 ") + spec_->name + " 没有 flag " + flag
            + "（读未声明的 flag = 默认值没有真相源）");
    return *f;
}

std::string Args::get(const std::string& flag) const
{
    const FlagSpec& f = need(flag);
    const auto it = values_.find(flag);
    if (it != values_.end())
        return it->second;
    return f.def != nullptr ? f.def : "";  // Bool 没有值：调用方该用 has()
}

double Args::number(const std::string& flag) const
{
    return std::stod(get(flag));
}

int Args::integer(const std::string& flag) const
{
    return std::stoi(get(flag));
}

bool Args::has(const std::string& flag) const
{
    (void)need(flag);  // 未声明就抛：has() 的判定也必须有真相源
    return supplied_.contains(flag);
}

std::string Args::legalArgs() const
{
    return legalArgsFor(*spec_);
}

void Args::rejectUnknown() const
{
    const std::string hint = std::string("；") + spec_->name + " 的合法参数：" + legalArgs();
    if (!unknown_.empty())
        throw UsageError("未知参数 " + join(unknown_, " ") + hint
            + "（拼错的 flag 若被静默忽略，就会在默认值上跑完一次判据——本工具拒绝这种失效）");
    if (!missing_.empty())
        throw UsageError("flag 缺值：" + join(missing_, " ") + hint);
    if (!positional_.empty() && !spec_->allowPositional)
        throw UsageError("多余的位置参数 " + join(positional_, " ")
            + "；" + spec_->name + " 不收位置参数" + hint);
}

std::vector<std::string> Args::positional() const
{
    return positional_;
}

std::string usageText()
{
    std::string out = kHeader;
    for (const auto& s : subcommands()) {
        const std::string syn = synopsisFor(s);
        const size_t used = 2 + displayWidth(syn);
        out += "  " + syn + (used >= kSummaryCol ? "  " : std::string(kSummaryCol - used, ' '));
        out += s.summary;
        out += "\n";
        for (const char* n : s.notes)
            out += std::string(" ") + n + "\n";
    }
    return out;
}

std::string detailFor(const SubcommandSpec& spec)
{
    std::string out = "  " + synopsisFor(spec) + "\n    " + spec.summary + "\n";
    for (const auto& f : spec.flags)
        if (f.help != nullptr && f.help[0] != '\0')
            out += std::string("    ") + f.name + "  " + f.help + "\n";
    for (const char* n : spec.notes)
        out += std::string("  ") + n + "\n";
    return out;
}

std::string echoFor(const SubcommandSpec& spec, const Args& args)
{
    std::string out = std::string(spec.name) + "\n";
    for (const auto& f : spec.flags)
        out += echoLine(f.name, f.kind == FlagKind::Bool, args);
    for (const auto& f : spec.flags)  // 值 + 存在性都被读的，再补一行 (bool)
        if (f.presenceQueried)
            out += echoLine(f.name, true, args);
    for (const auto& p : args.positional())
        out += "   <positional> = '" + p + "'\n";
    return out;
}

void printUsage()
{
    std::fputs(usageText().c_str(), stdout);
}

int cmdArgs(const std::vector<std::string>& argv)
{
    if (argv.empty())
        throw UsageError("用法: imgqa args <子命令> [flags...]（-h 看子命令列表）");
    const SubcommandSpec* target = findSubcommand(argv[0]);
    if (target == nullptr)
        throw UsageError("未知子命令 " + argv[0] + "（-h 看用法）");
    const std::vector<std::string> rest(argv.begin() + 1, argv.end());
    const Args a(rest, *target);
    a.rejectUnknown();
    std::fputs(echoFor(*target, a).c_str(), stdout);
    return 0;
}

}  // namespace RGL::imgqa
