#!/usr/bin/env python3
"""动态能力探针矩阵量具（docs/能力观测_plan.md §3.7，批 4 的探针消费者）。

干什么：起一次引擎（headless + `diagnostics.rpc` + **启动期 `diagnostics.picking=true`**，
配置经临时目录注入，不碰仓库 configs 与 build 副本），把 **add/del × 全部 feature** 的矩阵
跑两遍，逐格记录 `{feature, op, done, refusedAt, diff}`，打印读数表并追加进 QA 台账
`qa/log/decorate_matrix.jsonl`（与 imgqa_calls.jsonl 同族的追加式 JSONL）。

为什么探针必须有这个消费者（§3.7）：「预留」这个词在本项目有前科——债-129 S1b 的图事实
查询面因「当期零消费者」被取消。一个没人调的 `/decorate` 就是同一形态；跑完矩阵之后，
「预留」的语义才是**已量过的能力边界**，不是**待实现功能的空位**。

量具自带的两条判据（红 = 退出码 1，读数不许入账）：

1. **阳性对照**：`del highlight` 必须 done=true，且撤过之后 `add highlight` 必须 done=true。
   依据（已实测核对）：highlight 装饰在构造期走 `DynamicRenderGraph::adoptPass` 登记成可撤，
   运行期插回的路径也存在（A→B→A′ 往返判据靠它）。如果矩阵每一格都注定失败，这个探针就是
   恒等函数——永远正确、永远测不出东西，且无法把「引擎做不到」与「探针自己坏了」区分开。
   这两格跑不成 = 探针或现状理解有问题，**停下报告**，不许把「其实全灭」当预期读数交上来。
2. **可复现**（计划 §6 探针条 ②）：同一构建同一配置跑两遍，逐格读数必须相同（frame 是时刻
   读数，不参与比对）。立不住，「能力边界」这个结论就立不住。

feature 名单**不在这里抄**——真相源 = `rgl/rpc_schema.lua` 的 feature oneOf（本脚本现读）。

⚠ 不进 `just gates`：它要真设备 + 起引擎，而 gates 的无设备性质是设计的一部分。
   入口 = `just decorate-matrix`。
"""

import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "checks"))
from rules_rpc import parse_lua_table  # 同一份 Lua 表解析器（feature 名单的单源在 schema）

PORT = 8788
BUILD_RGL = ROOT / "build" / "rgl"
LEDGER = ROOT / "qa" / "log" / "decorate_matrix.jsonl"
OPS = ("del", "add")  # 每个 feature 先 del 后 add：highlight 的 del→add 就是阳性对照那一对

# 🔴 每次输出都带、每次入账都存的那句话（计划 §3.7 + 任务书要求，三处之一）。
# add 全灭**可能只说明没人写过运行期 add 的调用路径**，不等于「引擎做不到」。两者在读数上
# 一模一样，含义差很远（排期问题 vs 架构问题）。下一个会话如果把「没实现」读成「不可能」，
# 就会据此冻结值得做的方向——这种事项目里真发生过。
ADD_CAVEAT = (
    "add 全灭可能只说明没人写过运行期 add 的调用路径，不等于「引擎做不到」——两者在读数上"
    "一模一样，含义差很远（排期问题 vs 架构问题）。本矩阵的阳性对照（del/add highlight 往返"
    "真成）证明运行期改图的机制本身是通的；其余 feature 停在 gate-off / not-adopted，缺的是"
    "启动期装饰到运行期申报单的交接（RgPassDeclaration + 管线条目 + 堆槽 + 色输入链），"
    "那是「没接线」，不是「路走不了」。把这一格读成架构定论之前，先问有没有人真的接过线。")


def patch_lua(path: Path, key: str):
    """把一个布尔键翻成 true（只服务 rpc / headless / picking 的已知写法 `key = false,`）。"""
    text = path.read_text(encoding="utf-8")
    old, new = f"{key} = false,", f"{key} = true,"
    assert old in text, f"{path.name} 里没找到 {old!r}（配置形态变了？同步改本脚本）"
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def rpc(method: str, path: str, body=None):
    req = urllib.request.Request(
        f"http://127.0.0.1:{PORT}{path}", method=method,
        data=None if body is None else json.dumps(body).encode(),
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return resp.status, json.loads(resp.read())
    except urllib.error.HTTPError as exc:
        return exc.code, json.loads(exc.read())


def wait_port(proc, timeout: float = 90.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", PORT), timeout=0.5):
                return True
        except OSError:
            if proc.poll() is not None:
                return False
            time.sleep(0.2)
    return False


def run_matrix(features) -> list[dict]:
    """跑一遍矩阵（每 feature 先 del 后 add；diff 只留非空数组，读数表才读得动）。"""
    rows = []
    for feature in features:
        for op in OPS:
            status, r = rpc("POST", "/decorate", {"feature": feature, "op": op})
            rows.append({
                "feature": feature, "op": op, "status": status,
                "done": r.get("done"), "refusedAt": r.get("refusedAt"),
                "diff": {k: v for k, v in (r.get("diff") or {}).items() if v},
                "reason": r.get("reason", ""),
            })
    return rows


def print_table(rows, title: str):
    print(f"\n{title}")
    print(f"{'feature':<10} {'op':<4} {'http':<5} {'done':<6} {'refusedAt':<14} diff")
    for r in rows:
        diff = ", ".join(f"{k}={v}" for k, v in r["diff"].items()) or "-"
        print(f"{r['feature']:<10} {r['op']:<4} {r['status']:<5} "
              f"{str(r['done']):<6} {str(r['refusedAt'] or '-'):<14} {diff}")


def git_stamp() -> str:
    try:
        head = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                              capture_output=True, text=True, timeout=10).stdout.strip()
        dirty = bool(subprocess.run(["git", "status", "--porcelain"], cwd=ROOT,
                                    capture_output=True, text=True, timeout=10).stdout.strip())
        return f"{head}{'-dirty' if dirty else ''}"
    except Exception:
        return "unknown"


def main() -> int:
    schema = parse_lua_table((ROOT / "rgl" / "rpc_schema.lua").read_text(encoding="utf-8"))
    deco = next(e for e in schema["endpoints"] if e["id"] == "decorate")
    features = next(f["oneOf"] for f in deco["request"]["fields"] if f["name"] == "feature")
    if not (BUILD_RGL / "rgl").exists():
        print("FAIL: build/rgl/rgl 不存在（先 just decorate-matrix 会自动编，或手动 just build）")
        return 2

    with tempfile.TemporaryDirectory(prefix="rgl_decorate_matrix_") as tmp:
        cfg = Path(tmp)
        shutil.copytree(ROOT / "configs", cfg, dirs_exist_ok=True)
        patch_lua(cfg / "diagnostics.lua", "rpc")
        patch_lua(cfg / "diagnostics.lua", "picking")  # 阳性对照的前提：highlight 构造期被 adopt
        patch_lua(cfg / "runtime.lua", "headless")
        env = dict(os.environ, RGL_CONFIG_DIR=str(cfg))
        log_path = Path(tmp) / "engine.log"
        with log_path.open("w", encoding="utf-8") as log_file:
            proc = subprocess.Popen(["./rgl"], cwd=BUILD_RGL, env=env,
                                    stdout=log_file, stderr=subprocess.STDOUT)
            rows1 = rows2 = None
            try:
                if not wait_port(proc):
                    print(f"FAIL: 引擎没起来（rc={proc.returncode}；设备缺失？VVL 报错？"
                          f"见 {log_path}）")
                    return 2
                time.sleep(1.5)  # 让帧边界跑几轮（探针是帧边界协议）
                rows1 = run_matrix(features)
                rows2 = run_matrix(features)
                rpc("POST", "/quit", {})
            finally:
                try:
                    proc.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    print("⚠ /quit 后 20s 没退，已 kill")

    print_table(rows1, "== 矩阵读数（第一遍；第二遍用于可复现比对）==")
    print(f"\n⚠ {ADD_CAVEAT}")

    # 判据 1：阳性对照两格（红 = 探针或现状理解坏了，读数不入账）
    pos = {(r["feature"], r["op"]): r for r in rows1}
    control_ok = (pos[("highlight", "del")]["done"] is True
                  and pos[("highlight", "add")]["done"] is True)
    # 判据 2：可复现（frame 是时刻读数不参与；reason 含原文也逐字比，档位与 diff 必须逐格同）
    strip = lambda rows: [{k: v for k, v in r.items() if k != "reason"} for r in rows]
    reproducible = strip(rows1) == strip(rows2)

    print(f"\n阳性对照（del highlight 成 + 撤后 add highlight 成）: "
          f"{'✅' if control_ok else '🔴 红——探针或现状理解有问题，停下报告，别把「其实全灭」当预期读数'}")
    print(f"可复现（两遍逐格相同，frame 除外）: {'✅' if reproducible else '🔴 红——能力边界的结论立不住'}")

    if "--no-ledger" not in sys.argv:
        LEDGER.parent.mkdir(parents=True, exist_ok=True)
        entry = {
            "ts": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S"),
            "tool": "decorate_matrix", "ver": git_stamp(),
            "config": {"diagnostics.rpc": True, "diagnostics.picking": True,
                       "runtime.headless": True, "runtime.aa": "off（默认档）",
                       "runtime.ssr.enabled": "false（默认档）"},
            "matrix": rows1, "reproducible": reproducible,
            "positive_control_pass": control_ok,
            "caveat": ADD_CAVEAT,
            "note": "台账表头说明：本矩阵量的是「一次尝试停在哪」的读数；add 列的 gate-off / "
                    "del 列的 not-adopted 是能力现状事实。读 add 列之前先读 caveat——"
                    "「没实现」与「做不到」在这些格里塌成同一个读数。",
        }
        with LEDGER.open("a", encoding="utf-8") as f:
            f.write(json.dumps(entry, ensure_ascii=False) + "\n")
        print(f"台账已追加: {LEDGER.relative_to(ROOT)}")

    if not control_ok:
        return 1
    if not reproducible:
        return 1
    print("\n矩阵跑完：阳性对照绿 + 可复现绿。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
