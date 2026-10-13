#!/usr/bin/env python3
"""`just install-hooks` —— 把**仓里的**钩子装进 `.git/hooks/`（债-10）。

## 为什么需要一条"安装"命令

`.git/hooks/` 不进版本库。所以钩子本体住 `tools/hooks/`（可评审、可 diff、跟着分支走），
本脚本负责把它接到 `.git/hooks/`。这条分工正是本条债的病根所在：**任何"每台机器要做一次"
的事，只要没有一条命令去做它，就等于没做**（`just test` 的 435 例躺了那么久就是这么来的）。

## 三条不显然的处理

1. **POSIX 用软链、Windows 用复制**：软链让改 `tools/hooks/pre-push` 立即生效（不用重装）；
   Windows 上建软链要特权或开发者模式，失败即回落成复制（代价：改完钩子要重跑本命令）。
2. **🔴 已存在的钩子绝不静默覆盖**：本仓 `.git/hooks/pre-push` 原本是 **Git LFS** 的钩子
   （`git lfs ls-files` 里确有 `assets/` 下的对象）。覆盖它 = push 时不再上传 LFS 对象，
   症状出在远端、离真因八跳。默认**拒绝**并说明；`FORCE=1` 才把它另存成 `pre-push.local`，
   由钩子本体在测试通过后**接力**（见 `tools/hooks/pre-push` 的「接力」段）。
3. **执行位自己补**：git 只执行带 X 位的钩子。源文件的 X 位在不同机器/不同 checkout 方式下
   不保证还在，安装时一并 `chmod 0o755` 并**事后校验**（校验不过 = 装了一条不会跑的钩子，
   那比不装更糟）。
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

# 要装的钩子 = tools/hooks/ 下的同名文件。加新钩子只改这一行（别在别处抄清单）。
HOOKS = ("pre-push",)


def _die(msg: str) -> None:
    print(f"install-hooks: ✗ {msg}", file=sys.stderr)
    sys.exit(1)


def _git(*args: str) -> str | None:
    try:
        r = subprocess.run(["git", *args], capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
    except OSError:
        return None
    return r.stdout.strip() if r.returncode == 0 else None


def _same(a: Path, b: Path) -> bool:
    """内容相同（用于判断"已经装过了"，比对字节不比对 mtime——复制安装不留 mtime 关系）。"""
    try:
        return a.read_bytes() == b.read_bytes()
    except OSError:
        return False


def _install_one(root: Path, hooks_dir: Path, name: str, force: bool) -> str:
    src = root / "tools" / "hooks" / name
    if not src.is_file():
        _die(f"找不到钩子本体 {src}")
    os.chmod(src, 0o755)                       # git 只认带 X 位的钩子
    dst = hooks_dir / name

    # ── 已存在：分清「我们装的」与「别人的（如 Git LFS）」，后者绝不静默覆盖 ──
    if dst.is_symlink():
        if dst.resolve() == src.resolve():
            return f"{name}: 已装（软链）"
        _die(f"{dst} 是指向 {dst.resolve()} 的软链，不是本仓库的钩子。"
             "不动它。要接管：先手工把它移成 pre-push.local（钩子会接力），再跑本命令。")
    if dst.exists():
        if _same(dst, src):
            os.chmod(dst, 0o755)
            return f"{name}: 已装（副本，内容一致）"
        if not force:
            head = "\n".join(f"          | {ln}" for ln in
                             dst.read_text(encoding="utf-8", errors="replace").splitlines()[:2])
            _die(f"{dst} 已存在且**不是**本仓库的钩子，拒绝覆盖：\n{head}\n"
                 f"         本命令不覆盖别人的钩子（本仓这一位原本是 Git LFS 的，覆盖 = push 不再"
                 f"上传 LFS 对象）。\n"
                 f"         要接管：just install-hooks FORCE=1 —— 它把原钩子另存成 "
                 f"{(hooks_dir / (name + '.local')).name}，测试通过后由钩子本体原样接力。")
        chain = hooks_dir / (name + ".local")
        if chain.exists():
            _die(f"{chain} 已经存在（说明已经有一次接管），拒绝再备份一份。"
                 "先确认它是什么，必要时手工删掉再跑。")
        dst.rename(chain)
        os.chmod(chain, 0o755)
        print(f"install-hooks:   原钩子已另存 → {chain.name}（测试通过后接力执行）")

    # ── 安装：POSIX 软链优先（改仓里那份立即生效），建不了就复制 ──
    if dst.exists() or dst.is_symlink():
        dst.unlink()
    mode = ""
    try:
        os.symlink(src, dst)
        mode = "软链 → tools/hooks/" + name
    except (OSError, NotImplementedError):
        shutil.copy2(src, dst)
        os.chmod(dst, 0o755)
        mode = "复制（改完钩子要重跑本命令）"

    # ── 事后校验：装了一条不会跑的钩子比不装更糟 ──
    if not dst.is_file():
        _die(f"{dst} 安装后读不到（软链断了？），未装。")
    if not os.access(dst, os.X_OK):
        _die(f"{dst} 装好了但没有执行位，git 不会跑它。未算装好。")
    return f"{name}: 已装（{mode}）"


def main() -> int:
    force = any(a.upper() == "FORCE=1" or a in ("--force", "-f") for a in sys.argv[1:])
    top = _git("rev-parse", "--show-toplevel")
    if not top:
        _die("`git rev-parse --show-toplevel` 失败：这里不是 git 仓库。")
    root = Path(top)
    hooks = _git("rev-parse", "--git-path", "hooks")   # 认 core.hooksPath，不硬编码 .git/hooks
    if not hooks:
        _die("`git rev-parse --git-path hooks` 失败：拿不到钩子目录。")
    hooks_dir = Path(hooks)
    if not hooks_dir.is_absolute():
        hooks_dir = root / hooks_dir
    hooks_dir.mkdir(parents=True, exist_ok=True)

    print(f"install-hooks: 目标目录 {hooks_dir}"
          + ("（来自 core.hooksPath）" if _git("config", "--get", "core.hooksPath") else ""))
    for name in HOOKS:
        print(f"install-hooks:   {_install_one(root, hooks_dir, name, force)}")
    print("install-hooks: ✓ 完成。注意 .git/hooks/ **不进版本库**——换机器 / 重新 clone /"
          " 新建 worktree 后要再跑一次 `just install-hooks`。")
    print("install-hooks:   试跑（不 push）：just pre-push")
    print("install-hooks:   逃生门：git push --no-verify")
    return 0


if __name__ == "__main__":
    sys.exit(main())
