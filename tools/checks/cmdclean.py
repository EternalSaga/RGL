"""编译命令清理（compile_commands.json 条目 → libclang 可用 flag 列表）。

本机实测坑全在本模块（勿回退，原始记录见 main.py 文件头汇总）：
1. libclang C API 不展开 @response file——模块参数全藏在 *.modmap 里，须内联展开
   （clang CLI 自动展开）；
2. 输入文件与驱动路径不能混进 argv（文件走 source_filename 参数；混入 → parse
   返回 NULL）；
3. Gentoo 布局 resource dir 在 /usr/lib/clang/22，与 libclang 默认推导不一致——
   须显式 -resource-dir（否则 stddef.h not found、AST 残缺）；
4b. clean 后首次构建：@modmap 与 -fmodule-file 指向的 BMI 都是构建产物、此时不
   存在——bmi_ready 预检整体跳过未就绪文件（纯文本规则不受影响照常跑），不当失
   败报；编译产物就绪后自然恢复全量检查。
4c. BMI 竞态（2026-09-23 确诊）：检查目标与编译并行时读到 ninja 半写 .pcm/
   .modmap → parse NULL 甚至 libclang crash。对策 = CMake 侧 source_checks 挂
   add_dependencies(rgl) 串行化 + 本模块产物稳定性双采样（bmi_stable）兜底手动
   并行场景（just checks 撞上别的构建进程）。
"""

import os
import shlex
import time


def at_missing_is_fatal(entry):
    """@response（.modmap）缺失**是否算"构建产物未就绪"**——`expand_at` 与 `dep_paths`
    共用这一处判据，别在调用点各写一遍（2026-10-21 实测：`dep_paths` 漏了这条例外，
    于是 `expand_at` 的例外**从来没生效过**）。

    背景（坑 4b + 批次 A 的例外）：@response 是构建产物，clean 后不存在 ⟹ 判未就绪、
    跳过 libclang 解析。但 imgqa 不用 modules，它的 .modmap 实测是 **0 字节空文件**——
    "不存在"与"存在但为空"对 libclang 完全等价；而 imgqa 是 EXCLUDE_FROM_ALL
    （`just build` 不编它），若照坑 4b 一律判未就绪，工具侧样式护栏就在**最常见的调用
    形态**（每次构建都跑 checks、imgqa 从没编过）下静默空转。故此类单元（打了
    `entry["at_optional"]`）缺 @response 时照常解析。
    带 modules 的单元**绝不许**打这个标记——它们的 modmap 里就是 -fmodule-file 本体。

    🔴 判据漏到 `dep_paths` 之外的代价（实测）：不存在的 @response 被当依赖去 stat ⟹
    `bmi_stable` 恒 False ⟹ 每个 TU 白烧 1+2+4s 退避，并把"从没编过"误报成"疑似并发
    构建中"（改前 `just checks` 1m58s / 64 条误报；改后 41.8s / 0 条且真解析）。
    @param entry compile_commands 条目
    @return True = 缺失即未就绪（该单元跳过）；False = 缺失可容忍（照常解析）
    """
    return not entry.get("at_optional")


def expand_at(entry):
    """展开命令里的 @response file（modmap）→ raw token 列表。

    @response file 是构建产物；clean 后尚未编译时不存在 → 返回 None（未就绪），
    调用方据此跳过而非崩溃（坑 4b）。例外判据 = `at_missing_is_fatal`（含 imgqa 的
    `at_optional` 豁免，理由与实测教训见该函数）。"""
    raw = []
    for a in shlex.split(entry["command"], posix=os.name != "nt"):
        if a.startswith("@"):
            rf = a[1:]
            if not os.path.isabs(rf):
                rf = os.path.join(entry["directory"], rf)
            if not os.path.exists(rf):
                if not at_missing_is_fatal(entry):
                    continue
                return None
            with open(rf, encoding="utf-8") as f:
                raw.extend(shlex.split(f.read().strip()))
        else:
            raw.append(a)
    return raw


def abs_module_files(raw, directory):
    """-fmodule-file 的 BMI 相对路径按 CWD 解析（构建时 CWD=build 目录）——转绝对。"""
    out = []
    for a in raw:
        if a.startswith("-fmodule-file="):
            v = a.removeprefix("-fmodule-file=")
            if "=" in v:
                name, path = v.split("=", 1)
            else:
                name, path = None, v
            if not os.path.isabs(path):
                path = os.path.abspath(os.path.join(directory, path))
            a = f"-fmodule-file={name}={path}" if name else f"-fmodule-file={path}"
        out.append(a)
    return out


def bmi_ready(entry):
    """libclang 检查是否可用：@modmap 存在 + 全部 -fmodule-file BMI 存在。

    clean 后首次构建产物尚未生成 → False，主进程跳过 libclang 解析（纯文本规则不受影响）。
    `entry["at_optional"]`（工具侧 imgqa）只豁免 @modmap 存在性，**不豁免** BMI 存在性
    ——判据见 expand_at 注释（imgqa 没有 -fmodule-file，所以它没有可豁免的 BMI）。"""
    raw = expand_at(entry)
    if raw is None:
        return False
    for a in abs_module_files(raw, entry["directory"]):
        if a.startswith("-fmodule-file="):
            path = a.removeprefix("-fmodule-file=").split("=", 1)[-1]
            if not os.path.exists(path):
                return False
    return True


def dep_paths(entry):
    """本编译单元读取的全部构建产物路径（@response 文件 + 其内/命令内的
    -fmodule-file BMI），绝对化去重。供稳定性双采样用（坑 4c）。

    🔴 缺失的 @response 是否算依赖，走 `at_missing_is_fatal` —— 与 `expand_at` **同一处**
    判据（imgqa 那类 `at_optional` 单元缺 .modmap 是常态，不是"半写"；把它当依赖去 stat
    会让 `bmi_stable` 恒 False，退避重试全烧空并把护栏变成误报，实测教训见该函数）。"""
    out = []
    raw = []
    for a in shlex.split(entry["command"], posix=os.name != "nt"):
        if a.startswith("@"):
            rf = a[1:] if os.path.isabs(a[1:]) else os.path.join(entry["directory"], a[1:])
            if os.path.exists(rf):
                out.append(rf)
                with open(rf, encoding="utf-8") as f:
                    raw.extend(shlex.split(f.read().strip()))
            else:
                if at_missing_is_fatal(entry):
                    out.append(rf)  # 真缺产物：留在依赖里，让 bmi_stable 判未就绪
                raw.append(a)  # 未就绪：保留原 token（下方 -fmodule-file 扫不到即可）
        else:
            raw.append(a)
    for a in abs_module_files(raw, entry["directory"]):
        if a.startswith("-fmodule-file="):
            out.append(a.removeprefix("-fmodule-file=").split("=", 1)[-1])
    return list(dict.fromkeys(out))


def _stat_all(paths):
    sig = []
    for p in paths:
        try:
            st = os.stat(p)
            sig.append((p, st.st_size, st.st_mtime_ns))
        except OSError:
            sig.append((p, None, None))
    return sig


def bmi_stable(entry, settle=0.08):
    """产物稳定性双采样（坑 4c）：全部存在且两次 stat 全等 → 稳定。

    🔴 **返回 `(稳定, 缺失清单)` 而不是单个 bool**：「依赖缺失（没构建）」与「依赖存在但
    两次 stat 变了（并发构建半写）」是两件事，合成一个 bool 后调用点只能说"不稳定"，
    于是 2026-10-21 那次把"imgqa 从没编过"报成了"疑似并发构建中"——**误报的代价是让人
    去查一个不存在的竞态**（本项目为此连跑了多轮 checks）。
    @param settle 两次采样间隔秒数
    @return (是否稳定, 缺失的依赖路径清单)
    """
    paths = dep_paths(entry)
    sig = _stat_all(paths)
    missing = [p for p, size, _ns in sig if size is None]
    if missing:
        return False, missing
    time.sleep(settle)
    return sig == _stat_all(paths), []


def clean_cmd(entry, full, scratch_dir, resdir):
    """compile_commands 命令 → libclang 可用的纯 flag 列表。"""
    raw = expand_at(entry)
    if raw is None:
        raise RuntimeError("构建产物未就绪：@response file（modmap）缺失，需先 just build")
    raw = abs_module_files(raw, entry["directory"])
    cmd = []
    skip = False
    for a in raw:
        if skip:
            skip = False
            continue
        if a == "-c" or a.startswith("-fmodule-output="):
            continue
        if a == "-o":
            skip = True
            continue
        if a == full:
            continue
        base = os.path.basename(a)
        if base in ("clang++", "clang", "clang++.exe", "clang.exe"):
            continue
        cmd.append(a)
    cmd += ["-fmodule-output=" + os.path.join(scratch_dir,
                                              os.path.basename(full) + ".pcm"),
            "-fsyntax-only", "-resource-dir", resdir]
    return cmd
