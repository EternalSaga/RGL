# Phase 10b 调试交互增补批：imgui 中文字体 + HTTP POST RPC

> **状态**：历史（归档 2026-10-18；2026-09-28 全落地，§6 验收清单实测全过；覆盖两件事：① imgui 面板中文显示为问号的修复、
> ② 轻量 loopback HTTP POST RPC（= [archive/phase10_plan.md](phase10_plan.md) 决策表
> **D12 翻案落地**，翻案条件"≥2 次因无编程驱动通道而绕路"已由用户 2026-09-28 主动提出达成）。
> 本批是 Phase 11（阴影）开工前的插队小批；结论回写 [next_roadmap.md](next_roadmap.md) 与
> git:调试历史。

## 1. 目标

| # | 需求（用户原话口径） | 验收 |
|---|---|---|
| B1 | imgui 不能有效显示中文，都是问号——"以后总要有中文的" | 面板截图中中文不再是问号 |
| B2 | 简易 HTTP POST RPC，"给你移动视角和调节 shader 参数目检用的"，直接用 httplib（同步头文件库） | curl 改机位/调参 → `/shot` 出图，闭环目检 |
| B3 | 字体优先 **MiSans Light**（用户指定，"我个人恶趣味"）；Windows 届时再说 | 本机命中 MiSans-Light.ttf |

## 2. 调研结论（2026-09-28 实测，作为设计依据）

1. **问号根因**：`imgui_layer_impl.cppm` 从未加载自定义字体 → 默认内建字体 ProggysizeClean
   只含 ASCII；面板中文（`param_semantics.json` 标签、页名"渲染/灯光/材质/选中"）源编码 UTF-8
   正确，纯粹是**字形缺失**。
2. **vcpkg 没有字体文件包**：`vcpkg search font/noto` 只有 freetype/fontconfig/harfbuzz 等**库**，
   装不到 Noto/思源 → 字体只能走"系统字体运行时探测 + 配置覆盖"。
3. **CFF 坑**：`/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc` 解析表目录 = `CFF ` 轮廓
   （非 `glyf`），**imgui 默认 stb_truetype 不支持 CFF → 加载即失败/空白**。必须开
   `imgui[freetype]`。
4. **freetype 零代码启用**：vcpkg portfile 把 `IMGUI_ENABLE_FREETYPE` 打进 `imconfig.h`，
   imgui CMake `target_compile_definitions(PUBLIC IMGUI_ENABLE_FREETYPE)` → 库内默认 font loader
   自动切 FreeType（`imgui_draw.cpp:4303`），`imgui-config.cmake` 已 `find_dependency(freetype)`
   → CMake 侧零改动，只加 feature。
5. **imgui 1.92 字体系统已改**：字形按需动态烘焙，`GetGlyphRanges*()` 全部 obsolete，
   "specifying glyph ranges is now unnecessary" → **不需要**合并中文/希腊/箭头 ranges，
   `AddFontFromFileTTF(path, size)` 一个调用即可，字体里有的字全部按需出来。
6. **MiSans 实测**（`/usr/share/fonts/misans/MiSans-Light.ttf`，7.8MB）：轮廓 = `glyf`（TrueType，
   stb 也能啃）；cmap 覆盖 `中曝渲材选度镜强度（）α→·` 实测**零缺字**。只在本机系统目录读，
   **不进仓库**（许可与仓库体积都无关）。
7. **RPC 落点现成**：`ConfigReloadHook` 帧边界（`configReloadTick` = renderSystem 帧首、
   **system 链之前** → 套用即本帧生效）；D12 当时预案原话即"轻量 loopback HTTP 命令队列，
   复用 ConfigReloadHook 模式"。组合追加模式照抄 `wireShaderHotReload`。
8. **截图载体现成**：`Renderer::requestFrameCapture()`（drawFrame 前）→ `hasFrameCapture()`
   → `llm_debug::writeCapturePng()` 写 PNG；**不复用 llm_debug 的置 AppExit 语义**（RPC 截图
   不退出进程，可连续多张）。
9. **依赖**：`cpp-httplib 0.53.0` 在 vcpkg（header-only，同步版，正是用户点名的形态）；
   `nlohmann_json` 经 rllogger PUBLIC 链接已可用（显式 `find_package` 一次）。

## 3. 决策表

| ID | 决策 | 结论 | 理由/备选 |
|---|---|---|---|
| D1 | 字体来源 | **系统字体探测链 + `ui_font` 配置覆盖** | vcpkg 无字体包；备选"仓库内置 Noto 子集"被否（仓库体积 + 子集外字仍问号） |
| D2 | 首选字体 | **MiSans-Light.ttf**（用户指定），失败降 MiSans-Regular → Noto CJK → wqy → Windows msyh/simhei | 逐个 `AddFontFromFileTTF` 试到非空；全 miss = 默认字体 + warn，不崩 |
| D3 | 轮廓支持 | **`imgui[freetype]` 常开** | CFF 兜底（Noto 路径/其他 Linux）；本机 MiSans 是 TrueType 其实不需要，但一个 feature 换整条链无条件可跑 |
| D4 | glyph ranges | **不指定**（imgui 1.92 按需烘焙） | 指定反而会把 `GetGlyphRanges*` 的 obsolete 警告引进来 |
| D5 | 字号 | `ui_font_size = 18.0`（默认 13 太挤） | Light 字重在暗色主题可能偏细 → 截图验收点，太细调到 19/20 或首选降 Regular（配置改动，零代码） |
| D6 | RPC 库 | `cpp-httplib`，**`default-features: false`**（关 brotli） | 同步版；loopback 明文 HTTP，不引压缩/SSL 依赖 |
| D7 | 绑定与端口 | **硬编码 127.0.0.1**，端口配置 `rpc_port = 8788` | 调试面不暴露 host 配置项，防误暴露 |
| D8 | 门控 | `diagnostics.lua` 的 `rpc = false`（默认关，与 `imgui` 门独立，headless 可用） | 默认零行为变化红线 |
| D9 | 线程模型 | httplib 自带线程池收请求 → 队列{payload+promise} → **帧边界**渲染线程 applier 结算 | registry 只能渲染线程碰；同 ConfigReloadHook/PickingHook 函数注入模式 |
| D10 | 端点面 | `GET /state`、`POST /camera`、`POST /param`、`POST /shot`（+`GET /` 路由提示） | 用户选定"再加 GET /state 参数发现" |
| D11 | 参数寻址 | 按 **label 字符串**匹配（页内唯一），`material` 字段先设 `DebugPanelState.materialSel` 再重建页 | 复用 `buildDebugParamPages` 的 get/set 通道，零契约改动；label 由 `/state` 发现 |
| D12 | 超时与停机 | `/camera|/param|/state` 2s、`/shot` 10s；server 停止/AppExit 时未决请求全 503 | 防 HTTP 线程永久悬挂 |

## 4. 批次

### 批 11b.1 — imgui 中文字体

| 文件 | 改动 |
|---|---|
| `vcpkg.json` | imgui features 追加 `"freetype"` |
| `configs/runtime.lua` | 新增 `ui_font = ""`（空=探测链）、`ui_font_size = 18.0` |
| `rgl/runtime_config.cppm` / `_impl.cppm` | `RuntimeConfig` 加 `uiFont`/`uiFontSize` + sol2 解析（全可选回落默认） |
| `rgl/imgui_layer.cppm` / `_impl.cppm` | `init()` 增字体参数；`CreateContext` 后、SDL3 后端 init 前跑探测链 `io.Fonts->AddFontFromFileTTF()`；命中打 info 一行（路径+字号），全 miss 打 warn |

探测链（命中即停）：

```
ui_font 显式值
→ /usr/share/fonts/misans/MiSans-Light.ttf
→ /usr/share/fonts/misans/MiSans-Regular.ttf
→ /usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc          （本机 Debian 路径）
→ /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc     （Fedora/Arch）
→ /usr/share/fonts/truetype/wqy/wqy-zenhei.ttc
→ C:/Windows/Fonts/msyh.ttc → C:/Windows/Fonts/simhei.ttf    （Windows）
```

细节：TTC 用 `ImFontConfig::FontNo = 0` 取首面（Noto ttc 首面 = JP 子字体，**字符集与 SC
完全相同只差字形变体**，无缺字风险）；探测链是 `:impl` 内私有数据，不进接口。

### 批 11b.2 — HTTP POST RPC

| 文件 | 改动 |
|---|---|
| `vcpkg.json` | `+ "cpp-httplib"`（`"default-features": false`） |
| `rgl/CMakeLists.txt` | `find_package(httplib CONFIG REQUIRED)` + `find_package(nlohmann_json CONFIG REQUIRED)`，link `httplib::httplib`；新 `.cppm` 登记 FILE_SET |
| `rlstd/rlstd.cppm` | **补 `<thread>/<mutex>/<future>/<condition_variable>` 四个头 + 导出 `thread/mutex/lock_guard/unique_lock/promise/future/future_status/future_error/condition_variable/cv_status`**（全项目第一个线程，rlstd 现无这些名；触发全量重编，一次性） |
| `rgl/debug_rpc.cppm`（新） | **传输层接口**：`RpcRequest`/`RpcReply`/`RpcResponder`（可跨帧持有、resolve 幂等）/`DebugRpcServer{start,stop,setApplier,poll}` + 私有 `awaitReply`；**不 import ecs**（函数注入，同 ConfigReloadHook/PickingHook） |
| `rgl/debug_rpc_impl.cppm`（新） | `<httplib.h>` + `<nlohmann/json.hpp>` 收在 GMF（第三方头合规）；pimpl 体（httplib 服务/队列/在途登记）+ 超时/停机/503 结算 |
| `rgl/debug_rpc_exec.cppm`（新） | **执行层接口**：只一条 `wireDebugRpc(registry, assetTable, renderer, llmActive)` 声明 |
| `rgl/debug_rpc_exec_impl.cppm`（新） | 协议解析 + 套用（相机 / 参数页 / 截图）+ 装配（门控、applier 闭包、ConfigReloadHook 组合追加）；`<entt><glm><fmt><nlohmann/json>` GMF |
| `rgl/diagnostics.cppm` / `_impl.cppm` | `bool rpc = false`、`uint32_t rpcPort = 8788` + sol2 解析（端口越界回落 8788） |
| `configs/diagnostics.lua` | `rpc = false`、`rpc_port = 8788` 两行 + 注释 |
| `rgl/main_impl.cppm` | `import debug_rpc` + `import debug_rpc_exec`；`wireDebugRpc(...)` 一行装配（声明序在 registry/assets/backEnd 之后 = 析构序其先，**stop 走 RAII** 不占 `main()` 复杂度预算）；**端口占用 = bind 失败降级 nullptr（start 内已日志），不崩**，同 imgui init 纪律 |

**落地偏差（2026-09-28 实施时定）**：`wireDebugRpc` 原计划落 `main.cppm`/`main_scene.cppm`
（与其余 `wire*` 同处）。实测 `main_scene.cppm` 已 446/500 代码行，协议执行约 150 代码行会顶破
文件行数红线（`DEFAULT_FILE_THRESHOLD = 500`）→ 协议执行 + 装配整体迁到新模块 `debug_rpc_exec`
（接口 = 声明、实现 = 叶子单元的标准拆分），**`main.cppm`/`main_scene.cppm` 零改动**，
main 启动流只多一行调用；传输/执行分界（D9/D10）不变。

#### 线程/时序模型

```
HTTP 线程（httplib::Server 连接线程）
  请求 → [mutex] 队列 {RpcRequest, shared_ptr<promise>} → future.wait_for(超时)
渲染线程（唯一碰 registry/Vulkan 的线程）
  renderSystem 帧首 configReloadTick → ConfigReloadHook.poll 组合链
    └→ DebugRpcServer::poll()
         ① drain 队列 → applier(闭包：registry/assetTable/renderer) → resolve
         ② 结算上帧挂起的 /shot（hasFrameCapture → writeCapturePng → resolve）
  → system 链（camera/projection/renderlist）→ requestFrameCapture → drawFrame
```

关键时序事实：`configReloadTick` 在 **system 链之前** → `/camera`、`/param` 套用后**本帧即
生效**；`/shot` 在帧首请求 → 本帧录回读 → 下帧边界结算（响应含 PNG 绝对路径）。

#### 协议（全 JSON、UTF-8、127.0.0.1:8788）

| 端点 | 请求 | 响应 |
|---|---|---|
| `GET /` | — | `{"ok":true,"routes":[...],"frame":N}` |
| `GET /state` | — | `{"ok":true,"frame":N,"camera":{mode,position,target,yawDegrees,pitchDegrees,up,fovDegrees},"pages":[{name,entries:[{kind,label,min,max,speed,value}]}]}`；kind ∈ `slider\|int\|color\|drag3\|text` |
| `POST /camera` | `{position?,target?,yawDegrees?,pitchDegrees?,up?,fovDegrees?,mode?}` 任意子集 | `{"ok":true,"camera":{...套用后姿态}}`；缺字段不改；角度字段一律**度**（同 `camera.lua` 的 `fovDegrees` 语义） |
| `POST /param` | `{label,value,page?,material?}`；value = 数字或 `[r,g,b]` | `{"ok":true,"label":...,"value":...}`；未知 label → 404，类型不符 → 400 |
| `POST /shot` | `{file?,gbuf?}`（缺省 `screenshots/rpc_<n>.png`；gbuf ≥0 = G-buffer 诊断通道） | `{"ok":true,"file":绝对路径}`（宽高/frame 观察值走 `/state`，不重复）；超时 → 504 |

错误通例：body JSON 解析失败 → 400；停机/AppExit 未决 → 503；未知路由 → 404。
**`material` 语义**：先写 `DebugPanelState.materialSel` 再重建页 → 该次写入作用于目标材质
（材质页/选中页共用 `appendMaterialControls`）。

## 5. 红线与风险

- **默认零变化**：`rpc=false`、`imgui=false`（默认）→ 画面必须 BIT-IDENTICAL；
  两门各自独立，互不牵连。
- **依赖方向**：`debug_rpc`（传输）不 import ecs、不进 `ZERO_ENTT_MODULES` 名单；协议执行 +
  装配整体在 `debug_rpc_exec`（`main.cppm`/`main_scene.cppm` 零改动）。**`debug_rpc` 不得被
  ecs/相机/param 模块反向 import**。
- **声明序**：server 必须声明在 registry / assetTable / backEnd **之后**（析构序其先）→
  try 块退出时 `~DebugRpcServer = stop()` 先于 registry 销毁，闭包里的裸服务指针绝不越权，
  在途请求全 503（不需显式调用，也就不占 `main()` 的认知复杂度预算）。
- **样式红线**（skill `rgl-new-module`）：新文件 >500 行拆、函数体 >100 行进 `_impl`、
  GMF 只许 `ranges/filesystem/chrono` 例外（httplib/nlohmann 是第三方头 ✓）、Doxygen 注释、
  成员自然命名。
- **rlstd 扩面**：`<thread>` 等四头 + 导出名，先补 `rlstd.cppm` 再在新模块用；BMI 改动 =
  全量重编（一次性，预期内）。
- **配置副本坑**：验收期临时改 `build/rgl/configs/`（下次 `just build` 会按源复原），
  不改源 configs 做验证。
- **Light 字重发虚**：D5 的截图验收点，处置 = 调字号或首选降 Regular（纯配置）。

## 6. 验收清单（2026-09-28 实测，全过）

1. [x] `just build`（vcpkg 装 `cpp-httplib` + imgui 重建带 freetype）+ `just checks --strict`
   → `[style-check] 0 警告 / 98 .cppm`、`[doc-check] 0 警告 / 944 声明`
2. [x] **默认路径零变化**：stash 基线 vs 改动后同会话全量构建，headless 出图 `cmp` 相同
   （md5 `6098eb62…`，两次 run exit=0、`[error]` 0）
3. [x] 中文字体 → 读图确认中文无问号、MiSans Light 字重可读（D5 判定）。
   **实施偏差**：不用 `llm_debug=true` 出图——`vkrenderer_impl` 里 `capturer.record` 在
   `frameEndHook` **之前**，llm 截图天生不含 UI（见 `git:调试历史` 2026-09-28 条）。
   改为窗口模式（`headless=false` + `diagnostics.imgui=true`）+ `spectacle -b -n -o` 外部截图：
   日志 `面板字体 /usr/share/fonts/misans/MiSans-Light.ttf @ 18px`，
   图内 `profiler = 0（configs/diagnostics.lua 设 profiler=N 出读数）`、页签
   `渲染/灯光/材质/选中`、`曝光 exposure`、`env 强度`、`env mip 范围` 全部字形完整。
4. [x] RPC：`rpc=true` 起进程 → `GET /state`（label 发现，中文 label 正常）→ `POST /camera`
   → `POST /param` → `POST /shot`（PNG 781542B，目检 = 相机移到位后的场景）；
   headless 下重复一遍（无窗可用）。负例全绿：unknown route 404 / bad json 400 /
   unknown label 404 / 类型不符 400 / `GET /camera` 404 / `/state?x=1` 200。
5. [x] 退出卫生：杀进程前进程健在、`rgl exit=143`（正常信号退出）、日志
   `debug_rpc: 已停机（在途请求全部 503）`、`[error]` 0；llm 模式自动退出 `exit=0` 同样打印。
6. [ ] Windows 构建（有环境时）：`cpp-httplib`/`freetype` 特征正常编译（用户自述"测得少"，
   不作为本批验收门）

**验收期额外抓到并修掉的两个真 bug**（详见 `git:调试历史` 同日条目）：
`set_error_handler` 无条件覆盖自产 4xx body；`rpcDispatch` 把 responder move 进下层导致
catch 分支空句柄 → 异常请求 504。两者均已修 + 复测。

## 7. 文档回写

- [x] git:调试历史：imgui 中文字体根因条目（默认字体无 CJK + stb 不吃 CFF +
  imgui 1.92 字形按需烘焙）+ RPC 两个 bug（error handler 覆盖 / responder move）
  + "并行双实例污染测量"无关，勿混
- [x] [技术概念.md](../技术概念.md)：§RPC 调试通道（协议表 + 帧边界时序 + 线程模型 + 配置键）
- [x] [next_roadmap.md](next_roadmap.md)：Phase 10 收口处补"2026-09-28 增补批（中文 + RPC，
  D12 翻案）"；`archive/phase10_plan.md` 不动（已归档）
- [x] AGENTS.md：运行要点加 RPC 一行（门/端口/端点速查）
