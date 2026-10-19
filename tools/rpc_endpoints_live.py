#!/usr/bin/env python3
"""RPC 端点实机回包对拍（docs/rpc_单源_plan.md 批 3；判据文档 → docs/风格检查规则.md §10.9）。

干什么：起一次引擎（headless + `diagnostics.rpc`，配置经临时目录注入，**不碰仓库 configs
与 build 副本**，同 tools/smoke.py 的 RGL_CONFIG_DIR 口径），把九个端点（含 `/pass` 两个分支）
逐个真实请求，对账三件事：

1. `GET /` 的 routes 数组**逐条含序** == `rgl/rpc_schema.lua` 的记录顺序（批 1 的既有行为）；
2. 每端点成功回包的**顶层键集合** == schema `response.fields`（无条件字段必须都在；出现
   声明外的键 = FAIL；条件性字段缺席合法）；
3. `/state` 的 camera/render/graph/pages[0] 与 `/camera` 的 camera **嵌套键集合** == 根级
   `shapes` 声明；外加两条反向用例（不带 `yaw_at_frame` 无 `yawPinnedFrame`；`on` 形态不回
   `toggleFrames`）钉"条件性"确实是条件性。

它钉的是**键集与序**，不钉值——值的行为锚另有其人（`tests/test_burst_protocol.cppm` 的文案、
`just regress burst` 与两个截图锚）。为什么值得存在：`response` 段没有运行期消费者，
rules_rpc 面 5/6 只能对"声明自洽 + 文档产物一致"，"schema ↔ 处理函数实际回包"这条缝只有
真发一次请求量得到（面 1 的盲区口径 → §10.7）。改回包字段没同步 schema ⟹ 本脚本红。

跑法：先 `just build`，然后 `python3 tools/rpc_endpoints_live.py`（要真设备；无设备时引擎
起不来 = 本脚本报错退出，不静默跳过）。退出码 0 = 全部对拍一致；引擎按 `/quit` 干净收尾。
"""

import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "checks"))
from rules_rpc import parse_lua_table  # 同一份 Lua 表解析器，不另造第二套读法

PORT = 8788
BUILD_RGL = ROOT / "build" / "rgl"


def patch_lua(path: Path, key: str):
    """把一个布尔键翻成 true（只服务 rpc / headless 两个已知写法 `key = false,`）。"""
    text = path.read_text(encoding="utf-8")
    old, new = f"{key} = false,", f"{key} = true,"
    assert old in text, f"{path.name} 里没找到 {old!r}（配置形态变了？同步改本脚本）"
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def main() -> int:
    root = parse_lua_table((ROOT / "rgl" / "rpc_schema.lua").read_text(encoding="utf-8"))
    endpoints = root["endpoints"]
    shapes = {s["name"]: {f["name"] for f in s["fields"]} for s in root["shapes"]}

    def declared(eid: str, only_required: bool):
        e = next(x for x in endpoints if x["id"] == eid)
        return {f["name"] for f in e["response"]["fields"]
                if not (only_required and f.get("optional"))}

    if not (BUILD_RGL / "rgl").exists():
        print("FAIL: build/rgl/rgl 不存在（先 just build）")
        return 2

    with tempfile.TemporaryDirectory(prefix="rgl_rpc_live_") as tmp:
        cfg = Path(tmp)
        shutil.copytree(ROOT / "configs", cfg, dirs_exist_ok=True)
        patch_lua(cfg / "diagnostics.lua", "rpc")
        patch_lua(cfg / "runtime.lua", "headless")
        env = dict(os.environ, RGL_CONFIG_DIR=str(cfg))
        proc = subprocess.Popen(["./rgl"], cwd=BUILD_RGL, env=env,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        failures: list[str] = []
        shots = [BUILD_RGL / "screenshots" / "rpcverify_shot.png",
                 BUILD_RGL / "screenshots" / "rpcverify"]
        try:
            deadline = time.time() + 60
            while time.time() < deadline:
                try:
                    with socket.create_connection(("127.0.0.1", PORT), timeout=0.5):
                        break
                except OSError:
                    if proc.poll() is not None:
                        print(f"FAIL: 引擎提前退出 rc={proc.returncode}（设备缺失？VVL 报错？）")
                        return 1
                    time.sleep(0.2)
            else:
                print("FAIL: 60s 内 8788 端口没起来")
                return 1
            time.sleep(1.5)  # 让帧边界跑几轮

            def rpc(method, path, body=None):
                req = urllib.request.Request(
                    f"http://127.0.0.1:{PORT}{path}", method=method,
                    data=None if body is None else json.dumps(body).encode(),
                    headers={"Content-Type": "application/json"})
                with urllib.request.urlopen(req, timeout=130) as resp:
                    return json.loads(resp.read())

            def check(name, got, want):
                if got == want:
                    brief = (sorted(want) if isinstance(want, (set, list)) and not isinstance(want, str)
                             else want)
                    print(f"  OK   {name}: {brief}")
                else:
                    failures.append(name)
                    print(f"  FAIL {name}\n       实际 = {got}\n       期望 = {want}")

            def check_keys(label, body, eid):
                got = set(body.keys())
                extra = got - declared(eid, False)
                missing = declared(eid, True) - got
                if extra or missing:
                    failures.append(label)
                    print(f"  FAIL {label}\n       实际 = {sorted(got)}"
                          f"\n       声明全量 = {sorted(declared(eid, False))}"
                          f"\n       多出 = {sorted(extra)} 缺(无条件) = {sorted(missing)}")
                else:
                    print(f"  OK   {label}: {sorted(got)}（条件性字段缺席合法）")

            print("== 1. GET / 路由顺序（批 1 既有行为）==")
            r = rpc("GET", "/")
            check("routes 逐条含序", r["routes"],
                  [f"{e['method']} {e['path']}" for e in endpoints])
            check_keys("GET / 顶层键", r, "routes")

            print("== 2. GET /state ==")
            st = rpc("GET", "/state")
            check_keys("/state 顶层键", st, "state")
            check("/state.camera 键", set(st["camera"].keys()), shapes["camera"])
            check("/state.render 键", set(st["render"].keys()), shapes["stateRender"])
            check("/state.graph 键", set(st["graph"].keys()), shapes["stateGraph"])
            check("/state.pages[0] 键", set(st["pages"][0].keys()), shapes["statePage"])

            print("== 3. POST /camera ==")
            cam = rpc("POST", "/camera", {"fovDegrees": 60.0})
            check_keys("/camera 顶层键", cam, "camera")
            check("/camera.camera 键", set(cam["camera"].keys()), shapes["camera"])
            check("反向：不带 yaw_at_frame 时无 yawPinnedFrame", "yawPinnedFrame" in cam, False)

            print("== 4. POST /param ==")
            label = value = None
            for page in st["pages"]:
                for ent in page["entries"]:
                    if ent.get("kind") == "slider":
                        label, value = ent["label"], ent["value"]
                        break
                if label:
                    break
            check_keys("/param 顶层键", rpc("POST", "/param", {"label": label, "value": value}),
                       "param")

            print("== 5. POST /shot ==")
            sh = rpc("POST", "/shot", {"file": "screenshots/rpcverify_shot.png"})
            check_keys("/shot 顶层键", sh, "shot")

            print("== 6. POST /shotburst ==")
            (BUILD_RGL / "screenshots" / "rpcverify").mkdir(parents=True, exist_ok=True)
            bu = rpc("POST", "/shotburst", {"n": 3, "dir": "screenshots/rpcverify"})
            check_keys("/shotburst 顶层键", bu, "shotburst")

            print("== 7. POST /resize ==")
            rz = rpc("POST", "/resize", {"width": 900, "height": 640})
            check_keys("/resize 顶层键", rz, "resize")

            print("== 8. POST /pass（两个分支）==")
            dyn = st["graph"]["dynamicPasses"]
            # 默认档可能没有在册可开关的 pass；回包形状不依赖名字有效性，兜底一个已知名
            name = dyn[0] if dyn else "highlight"
            p_on = rpc("POST", "/pass", {"name": name, "on": True})
            check_keys("/pass on 形态顶层键", p_on, "pass")
            check("/pass on 形态必有键",
                  {"ok", "name", "on", "passes", "recompiles", "frame"} - set(p_on.keys()), set())
            p_tg = rpc("POST", "/pass", {"name": name, "toggle_frames": 2})
            check_keys("/pass toggle 形态顶层键", p_tg, "pass")
            check("/pass toggle 形态必有键",
                  {"ok", "name", "toggleFrames", "startFrame", "frame"} - set(p_tg.keys()), set())
            check("反向：on 形态不回 toggleFrames", "toggleFrames" in p_on, False)
            check("反向：toggle 形态不回 on/passes/recompiles",
                  {"on", "passes", "recompiles"} & set(p_tg.keys()), set())

            print("== 9. POST /quit ==")
            qt = rpc("POST", "/quit", {})
            check_keys("/quit 顶层键", qt, "quit")
        finally:
            try:
                proc.wait(timeout=20)
                print(f"引擎按 /quit 干净退出（rc={proc.returncode}）")
            except subprocess.TimeoutExpired:
                proc.kill()
                print("⚠ /quit 后 20s 没退，已 kill")
                failures.append("quit 未干净退出")
            for s in shots:  # 清掉本次实验的产物（不留在 screenshots/ 里冒充基线）
                if s.is_file():
                    s.unlink()
                elif s.is_dir():
                    shutil.rmtree(s, ignore_errors=True)

    print("\n== 总结 ==")
    if failures:
        print(f"🔴 {len(failures)} 条不符：{failures}")
        return 1
    print("全部对拍一致（顶层键集合 + 嵌套 shapes + 路由顺序 + 条件性字段反向）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
