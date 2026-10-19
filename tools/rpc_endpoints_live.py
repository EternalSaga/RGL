#!/usr/bin/env python3
"""RPC 端点实机回包对拍（docs/rpc_单源_plan.md 批 3；判据文档 → docs/风格检查规则.md §10.9）。

干什么：起引擎（headless + `diagnostics.rpc`，配置经临时目录注入，**不碰仓库 configs
与 build 副本**，同 tools/smoke.py 的 RGL_CONFIG_DIR 口径），把九个端点（含 `/pass` 两个分支）
逐个真实请求，对账三件事：

1. `GET /` 的 routes 数组**逐条含序** == `rgl/rpc_schema.lua` 的记录顺序（批 1 的既有行为）；
2. 每端点成功回包的**顶层键集合** == schema `response.fields`（无条件字段必须都在；出现
   声明外的键 = FAIL；条件性字段缺席合法）；
3. `/state` 的 camera/render/graph/pages[0]/capabilities/fsr 与 `/camera` 的 camera **嵌套键
   集合** == 根级 `shapes` 声明；外加反向用例（不带 `yaw_at_frame` 无 `yawPinnedFrame`；`on`
   形态不回 `toggleFrames`；出厂档 fsr 段 usable=false 时尺寸四键**不得出现**）钉
   "条件性"确实是条件性。

它钉的是**键集与序**，不钉值——值的行为锚另有其人（`tests/test_burst_protocol.cppm` 的文案、
`just regress burst` 与两个截图锚）。为什么值得存在：`response` 段没有运行期消费者，
rules_rpc 面 5/6 只能对"声明自洽 + 文档产物一致"，"schema ↔ 处理函数实际回包"这条缝只有
真发一次请求量得到（面 1 的盲区口径 → §10.7）。改回包字段没同步 schema ⟹ 本脚本红。

能力段对拍（docs/能力观测_plan.md 批 1，§6「实机对拍」条）——「能力段报的附件名集合 ==
同一帧编译图的附件名集合」在进程外没有整份第二读数（编译图只活在进程里；这正是批 1 要
补的洞），所以拆成三条**独立发射器**的交叉对账，逐条都是别的代码路径打的字：

* **数量**：启动日志 `RenderGraph: N attachments / M passes compiled` 由编译调用点打印
  （= 编译图自己的读数）⟹ 能力段条数必须恰等于它；
* **名单 + 尺寸**：启动日志逐 RT 行（`scene_hdr (…): WxH slot K` 等，render_graph_rtslot
  装配代码打印）⟹ 每个出现在日志里的 RT 名必须能在能力段找到，且 width/height 逐条相等、
  rtBuilt = true；出厂门关的（ssr/taa/velocity/ao_applied）两侧都必须**不**出现（反向半边）；
* **引用解析**：`/shot attachment=<名>` 走的是 frame_capture 对**编译图**的按名解析 +
  对 **RT 缓存**的 find()——能力段报的每个颜色图属 RT 必须拍得出图（200 = 图里有这个名字
  且缓存里真建了，这就是「rtBuilt 与缓存实际状态一致」的可外推半边），拼错的名字必须 400。

「名单逐条 == 编译图」的**完整**判据（含 swapchain/depth/buffer 档与顺序）在无设备层由
`tests/test_rpc_capabilities.cppm` 钉死（脏窗口用例 = 计划 §6 判据 ①）；本脚本是它的
实机扩面，两侧失效形态不同，不互相替代。

跑法：先 `just build`，然后 `python3 tools/rpc_endpoints_live.py`（要真设备；无设备时引擎
起不来 = 本脚本报错退出，不静默跳过）。退出码 0 = 全部对拍一致；引擎按 `/quit` 干净收尾。
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
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "checks"))
from rules_rpc import parse_lua_table  # 同一份 Lua 表解析器，不另造第二套读法

PORT = 8788
BUILD_RGL = ROOT / "build" / "rgl"

# 启动日志逐 RT 行的抽取（render_graph_rtslot.cppm 的 logger->info 文案）。
# 「WxH slot」形态一族：scene_hdr / ao / velocity / ao_applied / ssr / ssr_trace；
# shadow_map 行同形态。ssr_trace 行多一段 "×0.5 渲染网格"，非贪婪到 `): ` 前。
RT_LINE_RE = re.compile(
    r"\[info\] \[render_graph_rtslot\.cppm:\d+\] "
    r"(?P<name>\w+) \([^)]*\): (?P<w>\d+)x(?P<h>\d+) (?:out slot|slot)")
# 编译计数行（main_impl 在 compileRenderGraph 成功后打印 = 编译图自己的读数）。
COMPILED_RE = re.compile(r"RenderGraph: (\d+) attachments / (\d+) passes compiled")
# RT 行名 ≠ 附件名的两条（taa 行一行报 out/hist 两个槽 → taa_out/taa_hist；ssr 行 = apply
# pass 的 RT，附件名是 ssr_applied）：出厂档这两行都不存在，遇到即跳过，别拿行名去能力段
# 查一个本来就不叫那名的附件。要按门开档扩对拍时，在这里补行名 → 附件名的映射再放开。
RT_LINE_SKIP_NAMES = {"taa", "ssr"}
# 出厂默认档（ssr/aa 关、picking 关）不该存在的附件名（反向半边；smoke.py 靠同类判据抓门）。
GATED_ABSENT = ("ssr_trace", "ssr_applied", "taa_out", "taa_hist", "velocity",
                "ao_applied", "object_id")


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
    # 条件性字段单独摘出（rtBuilt 的「该有必有、不该有必无」判据吃这两个集合）。
    cap_att_all = shapes["capabilityAttachment"]
    cap_att_optional = {f["name"] for f in
                        next(s for s in root["shapes"] if s["name"] == "capabilityAttachment")
                        ["fields"] if f.get("optional")}
    cap_att_required = cap_att_all - cap_att_optional
    # fsr 段（能力观测批 2）同样分「必在 / conditional」两半（尺寸四键 = usable 才在）。
    fsr_shape = next(s for s in root["shapes"] if s["name"] == "stateFsr")
    fsr_all = {f["name"] for f in fsr_shape["fields"]}
    fsr_optional = {f["name"] for f in fsr_shape["fields"] if f.get("optional")}
    fsr_required = fsr_all - fsr_optional

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
        log_path = Path(tmp) / "engine.log"
        log_file = log_path.open("w", encoding="utf-8")
        proc = subprocess.Popen(["./rgl"], cwd=BUILD_RGL, env=env,
                                stdout=log_file, stderr=subprocess.STDOUT)
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

            def rpc_status(method, path, body=None):
                """发一次请求只取 HTTP 状态码（400/404 类失败回包的形状不在本脚本的核对范围）。"""
                req = urllib.request.Request(
                    f"http://127.0.0.1:{PORT}{path}", method=method,
                    data=None if body is None else json.dumps(body).encode(),
                    headers={"Content-Type": "application/json"})
                try:
                    with urllib.request.urlopen(req, timeout=130) as resp:
                        return resp.status
                except urllib.error.HTTPError as exc:
                    return exc.code

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

            print("== 2b. GET /state 能力段（能力观测批 1）==")
            caps = st["capabilities"]
            check("/state.capabilities 键", set(caps.keys()), shapes["stateCapabilities"])
            check("能力段 frame == 顶层 frame（同一次快照）", caps["frame"], st["frame"])
            atts, passes = caps["attachments"], caps["passes"]
            check("附件表非空（空表会让下面每条遍历恒真 = 假绿）", len(atts) > 0, True)
            check("pass 表非空", len(passes) > 0, True)
            # 逐条键集：无条件字段必在、声明外键不得出现；rtBuilt 的「该有必有、不该有必无」
            # 由 kind 档决定（图属 RT 两档才有；swapchain/depth/buffer 没有）。
            # 不符者必须**点名差在哪个键**：只列条目名字的话，读者看到九条全违例也只知道
            # "键集不对"，还得自己去数（同脚本的 /resize 那条早就在报 实际/声明 两个键集了）。
            def key_diff(entries, required, allowed):
                out = []
                for e in entries:
                    ks = set(e.keys())
                    if required <= ks <= allowed and (required is not allowed or ks == allowed):
                        continue
                    out.append(f"{e.get('name', '?')}"
                               f"(缺={sorted(required - ks)}, 多={sorted(ks - allowed)})")
                return out

            check("附件条目键集逐条 == capabilityAttachment",
                  key_diff(atts, cap_att_required, cap_att_all), [])
            want_rt = {"offscreen", "depthOffscreen"}
            rt_built_kinds = [(a["name"], a["kind"], "rtBuilt" in a)
                              for a in atts if ("rtBuilt" in a) != (a["kind"] in want_rt)]
            check("rtBuilt 出现与否逐条 == kind 档", rt_built_kinds, [])
            check("附件名互不重复", len({a["name"] for a in atts}), len(atts))
            check("pass 条目键集逐条 == capabilityPass",
                  key_diff(passes, shapes["capabilityPass"], shapes["capabilityPass"]), [])
            check("pass order 逐条 == 下标（执行序 = 编译产物序）",
                  [p["order"] for p in passes], list(range(len(passes))))
            cap_pass_names = [p["name"] for p in passes]
            check("pass 名互不重复", len(set(cap_pass_names)), len(passes))
            # 稳态：图属 RT 每帧 ensure ⟹ rtBuilt 全 true（false 半边 = 单测，进程外造不出）。
            # ⚠ 只问图属 RT 两档——swapchain/depth/buffer 本来就没有 rtBuilt 键（上面那条
            # 「出现与否 == kind 档」管它们），把它们算进来会把「没有该字段」读成「没建」。
            check("稳态 rtBuilt 全 true（与 RT 缓存实际状态一致）",
                  [a["name"] for a in atts
                   if a["kind"] in want_rt and a.get("rtBuilt") is not True], [])
            # 反向半边：出厂关的门不得在能力段留附件 / pass。
            check("出厂档无门关附件（ssr/taa/velocity/ao_applied/object_id）",
                  sorted(n for n in GATED_ABSENT
                         if n in {a["name"] for a in atts}
                         or n in cap_pass_names), [])
            # 与 graph 段的一致性（**不是**来源核对：两者在稳态相等是事实，来源判据 = 单测
            # 脏窗口用例；这条抓的是「能力段整个读错了图 / 读了空图」这类粗失效）。
            check("能力段 pass 名集合 == graph.presentPasses（稳态一致性）",
                  set(cap_pass_names), set(st["graph"]["presentPasses"]))

            print("== 2c. GET /state FSR 段（能力观测批 2）==")
            # 本脚本跑**出厂档**（aa 关）⟹ 这里钉的是反向半边：usable/gateOpen/时域/抖动
            # 全 false、conditional 的尺寸四键**不得出现**（出现 = 门关不拢 / schema 与
            # 处理函数一起自洽地漂了）。正向半边（aa=fsr 档 usable=true 且尺寸四键必在）
            # 的实机消费者 = `just smoke --aa fsr`（判据本体在 tools/smoke.py 的 fsr 段）。
            fsrSec = st["fsr"]
            check("/state.fsr 键（出厂档 usable=false ⟹ 尺寸四键缺席合法）",
                  set(fsrSec.keys()), fsr_required)
            check("出厂档 fsr 门关（gateOpen/usable/temporalEffective/jitterWired 全 false）",
                  [fsrSec["gateOpen"], fsrSec["usable"],
                   fsrSec["temporalEffective"], fsrSec["jitterWired"]],
                  [False, False, False, False])
            check("出厂档 jitterPhase == 0", fsrSec["jitterPhase"], 0)
            check("temporalConfig 是布尔（申报侧回显配置键，值随 configs/runtime.lua 不钉死）",
                  isinstance(fsrSec["temporalConfig"], bool), True)
            # 跨字段不变量（与档位无关）：生效侧三枚都蕴含 usable——「FSR 没跑却在按时域/
            # 抖动」的回包是拼装错位，出厂档恰好全 false 时它们恒真，所以这两条要写成
            # **蕴含式**而不是常量比对（门开档由 smoke 判据吃，这里兜形状错）。
            check("蕴含式：temporalEffective ⟹ usable",
                  (not fsrSec["temporalEffective"]) or fsrSec["usable"], True)
            check("蕴含式：jitterWired ⟹ usable",
                  (not fsrSec["jitterWired"]) or fsrSec["usable"], True)

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

            print("== 5b. POST /shot 逐附件回读（能力段名单 × RT 缓存交叉对账）==")
            # /shot 的 attachment 解析吃的是 frame_capture 持有的**编译图** + RT 缓存 find()
            # ⟹ 200 = 「图里有这个名字」且「缓存里真建了这张」，与能力段的 rtBuilt=true 是
            # 两条独立代码路径读同一份事实。反向：图里没有的名字必须被拒（400，引用解析失败；
            # 批 3 计划改 404——本脚本按**当前**行为对拍，改语义时同步改这里）。
            (BUILD_RGL / "screenshots" / "rpcverify").mkdir(parents=True, exist_ok=True)
            for a in atts:
                if a["kind"] != "offscreen":
                    continue  # swapchain/depth/buffer 不占 RT 缓存；depthOffscreen 深度面回读不在本条判据覆盖范围
                got = rpc_status("POST", "/shot",
                                 {"attachment": a["name"],
                                  "file": f"screenshots/rpcverify/cap_{a['name']}.png"})
                check(f"/shot attachment={a['name']}（rtBuilt=true 的图属 RT 必须拍得出）",
                      got, 200)
            # 点名的附件不在编译图里 ⟹ **404**（批 3：原来是 400）。分类口径 = 请求写得完全合法、
            # 只是它点名的东西不存在，与未知路由同族；400 留给「参数不合协议」（如 gbuf 越界）。
            # 🔴 这条同时是「端点没被打死」的判据：404 必须**当帧**回来，若退化成 504 超时
            #    （= 回读请求悬挂），说明批 3 之前那个「一次拼错打死 /shot」的缺陷回来了。
            check("反向：/shot attachment=zzz_not_in_graph → 404（不是 400，也不是 504 悬挂）",
                  rpc_status("POST", "/shot",
                             {"attachment": "zzz_not_in_graph",
                              "file": "screenshots/rpcverify/cap_zzz.png"}), 404)

            print("== 6. POST /shotburst ==")
            (BUILD_RGL / "screenshots" / "rpcverify").mkdir(parents=True, exist_ok=True)
            bu = rpc("POST", "/shotburst", {"n": 3, "dir": "screenshots/rpcverify"})
            check_keys("/shotburst 顶层键", bu, "shotburst")
            # 批 3 删掉了 /shotburst 的静态白名单 oneOf = {swapchain, taa_out}（能力伪装成协议）。
            # 删了之后必须与 /shot **同一口径**：点名的东西不存在 → 404，而不是 400 也不是挂死。
            check("反向：/shotburst attachment=zzz_not_in_graph → 404（与 /shot 同口径）",
                  rpc_status("POST", "/shotburst",
                             {"n": 1, "dir": "screenshots/rpcverify",
                              "attachment": "zzz_not_in_graph"}), 404)
            check("反向后 /shotburst 仍可用（端点没被一次拼错打死）",
                  rpc_status("POST", "/shotburst", {"n": 1, "dir": "screenshots/rpcverify"}), 200)

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
            log_file.close()
            for s in shots:  # 清掉本次实验的产物（不留在 screenshots/ 里冒充基线）
                if s.is_file():
                    s.unlink()
                elif s.is_dir():
                    shutil.rmtree(s, ignore_errors=True)

        print("== 10. 能力段 × 启动日志（编译图自己的读数）==")
        engine_log = log_path.read_text(encoding="utf-8", errors="replace")
        m = COMPILED_RE.search(engine_log)
        if not m:
            failures.append("编译计数日志")
            print("  FAIL: 启动日志里没有 'RenderGraph: N attachments / M passes compiled'"
                  "（文案变了？能力段数量对拍失去对账伙伴，本条必须红）")
        else:
            n_att, n_pass = int(m.group(1)), int(m.group(2))
            check("能力段附件数 == 编译图 attachments 数", len(caps["attachments"]), n_att)
            check("能力段 pass 数 == 编译图 passes 数", len(caps["passes"]), n_pass)
        # 逐 RT 行：日志里出现的每个 (名, W, H) 必须能在能力段找到同尺寸条目且 rtBuilt=true。
        rt_lines = [lm for lm in RT_LINE_RE.finditer(engine_log)
                    if lm.group("name") not in RT_LINE_SKIP_NAMES]
        check("启动日志有逐 RT 行（空会让下面每条恒真）", len(rt_lines) > 0, True)
        by_name = {a["name"]: a for a in caps["attachments"]}
        for lm in rt_lines:
            name, w, h = lm.group("name"), int(lm.group("w")), int(lm.group("h"))
            entry = by_name.get(name)
            if entry is None:
                failures.append(f"RT 行 {name}")
                print(f"  FAIL RT 行 {name} 在能力段找不到同名附件（日志建了、图里报没有？）")
                continue
            check(f"RT 行 {name} 尺寸 == 能力段 {w}x{h}",
                  (entry["width"], entry["height"]), (w, h))
            check(f"RT 行 {name} ⟹ 能力段 rtBuilt true", entry.get("rtBuilt"), True)
        # 反向半边：出厂关门的 RT 行不得出现在日志里（出现了却没被上面找到 = 上面已红）。
        logged_names = {lm.group("name") for lm in rt_lines}
        check("出厂档日志无门关 RT 行",
              sorted(n for n in ("ssr", "ssr_trace", "taa", "velocity", "ao_applied")
                     if n in logged_names), [])

    print("\n== 总结 ==")
    if failures:
        print(f"🔴 {len(failures)} 条不符：{failures}")
        return 1
    print("全部对拍一致（顶层键集合 + 嵌套 shapes + 路由顺序 + 条件性字段反向 + 能力段三条交叉对账）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
