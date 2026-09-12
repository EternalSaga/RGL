"""编译命令清理（compile_commands.json 条目 → libclang 可用 flag 列表）。

本机实测坑全在本模块（勿回退，原始记录见 stylecheck.py 文件头汇总）：
1. libclang C API 不展开 @response file——模块参数全藏在 *.modmap 里，须内联展开
   （clang CLI 自动展开，doccheck 的 -ast-dump 路径不受影响）；
2. 输入文件与驱动路径不能混进 argv（文件走 source_filename 参数；混入 → parse
   返回 NULL）；
3. Gentoo 布局 resource dir 在 /usr/lib/clang/22，与 libclang 默认推导不一致——
   须显式 -resource-dir（否则 stddef.h not found、AST 残缺）；
4b. clean 后首次构建：@modmap 与 -fmodule-file 指向的 BMI 都是构建产物、此时不
   存在——bmi_ready 预检整体跳过未就绪文件（纯文本规则不受影响照常跑），不当失
   败报；编译产物就绪后自然恢复全量检查。
"""

import os
import shlex


def expand_at(entry):
    """展开命令里的 @response file（modmap）→ raw token 列表。

    @response file 是构建产物；clean 后尚未编译时不存在 → 返回 None（未就绪），
    调用方据此跳过而非崩溃（坑 4b）。"""
    raw = []
    for a in shlex.split(entry["command"], posix=os.name != "nt"):
        if a.startswith("@"):
            rf = a[1:]
            if not os.path.isabs(rf):
                rf = os.path.join(entry["directory"], rf)
            if not os.path.exists(rf):
                return None
            raw.extend(shlex.split(open(rf, encoding="utf-8").read().strip()))
        else:
            raw.append(a)
    return raw


def abs_module_files(raw, directory):
    """-fmodule-file 的 BMI 相对路径按 CWD 解析（构建时 CWD=build 目录）——转绝对。"""
    out = []
    for a in raw:
        if a.startswith("-fmodule-file="):
            v = a[len("-fmodule-file="):]
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

    clean 后首次构建产物尚未生成 → False，主进程跳过 libclang 解析（纯文本规则不受影响）。"""
    raw = expand_at(entry)
    if raw is None:
        return False
    for a in abs_module_files(raw, entry["directory"]):
        if a.startswith("-fmodule-file="):
            path = a[len("-fmodule-file="):].split("=", 1)[-1]
            if not os.path.exists(path):
                return False
    return True


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
