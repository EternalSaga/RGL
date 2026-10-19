# RPC 端点能力单源化计划（schema + 代码生成）

> **状态：已拍板，开工**（2026-10-19 用户逐条拍板，结论见 §6；本计划的**核心目的**由用户补写一句
> 钉死：「收敛全部外部调试接口到统一」——凡"引擎能做什么"这件事有第二份手写副本，都在本计划要处理的范围内）。
> 由来：给引擎做一个外部 RPC 控制工具（CLI + 后续网页 GUI）开工前的拦截——用户原话
> "必须解决 RPC 端点能力的单源问题……必须有个代码生成模板，散装 RPC 得到此为止"。
> 工具本体排在批 4；在它开工前，先把"端点能力"从多处手写副本收成一份 schema。

## 0. 一句话结论

路由与 HTTP 方法、每端点超时、请求/响应字段形状，现在分散在**至少六处**手写副本里，
其中只有两处（注册与分发）被现有检查规则对账，且那条规则自己写明三处看不到的地方。
本计划把这些事实收进一份 JSON schema，用**既有反射管线**（模板渲染 + 生成物提交进版本库 +
构建期输入哈希守卫）生成 C++ 路由表与人工可读参考，让服务端注册、分发、超时、路由回包
与客户端全部消费生成物：此后新增端点只改 schema（+ 实现处理函数本体），忘同步的部分由
编译器与检查规则当场喊出来。

## 1. 病根：同一事实的副本清点（2026-10-19 实测）

| 事实 | 副本 | 现有对账 |
|---|---|---|
| 路由 + 方法 | ① 注册：`DebugRpcServer::start`（`rgl/debug_rpc_impl.cppm` 里逐行手写 server.Get/Post）；② 分发：`rpcDispatch` / `rpcDispatchPost` 的路径比较（`rgl/debug_rpc_exec_impl.cppm`）；③ 路由回包：`rpcRoutesJson` 里的字面量数组（同文件）；④ 接口注释里的端点列表（`rgl/debug_rpc_exec.cppm` 顶部）；⑤ 文档表：`docs/技术概念.md` §RPC 调试通道；⑥ 客户端常量：`tools/imgqa/engine.cpp`、`tools/cpu_baseline.py`、`tools/regress.py` | 只有 ①↔②（`docs/风格检查规则.md` §10，债-125）。③④⑤⑥ 没有任何东西守着；在 ③ 里加/删一条，今天的构建与全部检查都不会红（§10 的两条正则的判据形态扫不到它） |
| 每端点超时 | ① 按路径挑拣的三档常量（`kTimeoutShot` / `kTimeoutBurst` / 默认，住在 `rgl/debug_rpc_impl.cppm` 的等待函数里）；② `docs/技术概念.md` 同段的"超时表"；③ 客户端各自抄（设计工具时原打算再抄一份客户端超时） | 无 |
| 请求/响应字段 | ① 各处理函数手写解析（`rpcHandleCamera` / `rpcHandleParam` / `parseBurstRequest` 等）；② `docs/技术概念.md` 表的请求/响应列；③ 客户端解析与构造；④ 内存中的形状守卫只有一条"应答必须是对象"（`rgl/debug_rpc_payload.cppm` 的 `rpcOk`） | 无。"引擎到底收哪些字段"只有读代码才知道——正是"用啥先查找啥"的来源 |
| 错误码与文案 | 处理函数里的字面量 | 局部有：仅 `/shotburst` 形状被 `tests/test_burst_protocol.cppm` 逐条钉住 |
| `/param` 的 label 词表 | **已是单源**：`rgl/param_semantics.json` → 生成 `rgl/generated/param_semantics_generated.cppm` | 生成管线（项目已有的先例，本计划就是照它放大） |

`docs/archive/phase10b_plan.md` 里的同款表格是历史记录，不回改、不参与维护。
§10 规则自己写明的三处看不到的地方（原文）：方法不判、只认两种字面量写法、扫描面只有那两个文件。

## 2. 为什么不再给 §10 打补丁

- §10 的办法是"两份名单对账"，它解决的恰好是当时撞见的形态（B2-5 的 `/pass` 404）。但事实的副本不是两份而是六处；再写一条"③↔① 对账"就是第三个补丁，副本数一个不减，每加一处都要重写正则、重写"看不到什么"的自述。
- 项目哲学第 1 条原话："单一来源不承重，**访问路径必须消费那个来源**才承重。" 对账规则让副本继续存在；生成物让副本不存在。
- 现成机制正好干这事：着色器反射管线（`cmake/RglReflection.cmake`）已经在用"JSON/Lua 源 → 模板 → 生成物提交进版本库 → 构建开头比对输入哈希（`cmake/ReflectionManifest.cmake`）"，忘了重生成直接拦住构建。同一形态的第二个先例 = `rgl/features_config.json` 与 `rgl/param_semantics.json` 这两条 JSON→模板渲染管线。本计划**复用它们，不新造工具、不新造守卫**。

## 3. 目标形态

### 3.1 单一来源 = rgl/rpc_schema.lua（拟建，手写，一行一处事实）

**格式 = Lua**（用户拍板：「就是为了全局风格统一」——项目里 `*_schema.lua` / `configs/*.lua`
全是 Lua，这份不该是唯一的例外）。每个端点一条记录：身份（id / method / path）、超时、
给文档与客户端用的一句说明；批 2 起再收请求/响应字段与形状约束。形态示意
（**只是形态示意；端点的唯一名单以该文件本体为准**）：

```lua
-- rgl/rpc_schema.lua —— 引擎外部调试接口的能力清单（唯一真相源）
-- 消费方与生成物见 docs/rpc_单源_plan.md §3.2/§3.3；改本文件后必须 `just reflect`。
return {
    module_name = "rpc_schema_generated",
    version = 1,
    endpoints = {
        {
            id = "example", method = "POST", path = "/example", timeout_ms = 2000,
            summary = "一句说明：给文档参考与客户端帮助用",
            request = { fields = {
                { name = "count", kind = "int", min = 1, max = 1024, required = true,
                  error = "'count' must be an integer in [1, 1024]" },
            } },
            response = { fields = {
                { name = "ok", kind = "bool" }, { name = "frame", kind = "uint32" },
            } },
        },
    },
}
```

两点边界：① 不重复登记 `rgl/param_semantics.json` 已管的 label 词表；② `/state` 的
`pages` 只登记到粗粒度结构（name/entries），细目归参数页那条生成管线。

### 3.2 生成管线 = 既有反射管线加一条（Lua 带来的两处扩展，必须先做）

选 Lua 的代价是**现成管线只吃 JSON**，两处要补（都是扩既有件，不是新造机制）：

- 🔴 **扩展 1｜`reflection_tool render` 加 `--lua` 输入**：现在 `render` 只认 `--json`
  （`tools/reflection_tool.cpp:734` 段），而项目里读 Lua 的能力**已经有了**——
  `tools/schema_loader.cppm:166` 的 `loadSchema` 用 sol2 开沙箱读 `*_schema.lua`。
  本扩展 = 加一个「Lua 表 → `nlohmann::json`」的通用转换（放 `schema_loader`，与
  `loadSchema` 同一沙箱纪律：**`dofile`/`loadfile`/`require`/`package` 一律置 nil**，
  那条不是防攻击，是让"schema 文件里写函数/读环境"这种形态**当场报错而不是静默生效**），
  `render` 再吃它。先例 = 图形侧 6 份 `*_schema.lua` 走的同一条 sol2 读取路径。
- 🔴 **扩展 2｜额外产一份 JSON 生成物给客户端**：`rgl/generated/rpc_schema.json`（**已提交产物**，
  与 `.cppm` 同一次 `just reflect` 生成）。理由 = 批 4 的客户端是 Python，而 Python 没有
  标准库 Lua 解析器；让客户端去 shell 调 `lua` 解释器 = 给工具加一个运行期外部依赖。
  ⚠ **它不是第二份真相源**，是生成物——与 `.cppm` 同源同批，`rules_rpc` 面 1 逐条对账
  （schema ↔ `.cppm` ↔ `.json` 三方，防"只重生成了一份"）。
- 生成器 = `tools/reflection_tool.cpp` 的 render 子命令（`--lua` 源 + `--template` + `--output`）；
- 模板 = tools/templates/rpc_schema_cppm.inja（拟建；表头沿用 "Auto-generated …… DO NOT EDIT"，
  模块名从 schema 的 module_name 字段来）；
- 产物 = rgl/generated/rpc_schema_generated.cppm + rgl/generated/rpc_schema.json（拟建；
  已提交产物，随改动一起提交），`.cppm` 加进 `rgl/CMakeLists.txt` 的 base 模块清单
  （纯查表、零设备，与 debug_rpc 几个模块同表）；
- 守卫 = `cmake/RglReflection.cmake` 里登记 REFLECTION_PAIRS ⟹ 构建开头的 reflection_check
  比对输入哈希；改了 schema 忘跑 `just reflect` = 构建直接拦住并指路（与着色器契约同一条守卫，
  零新增机制）。⚠ 该守卫**只哈希输入**，产物被手改它看不见 ⟹ 产物侧由 `rules_rpc` 面 1 兜
  （同 `rules_reflection.py` 补的那块盲区，一条道理）。

### 3.3 消费点（每个都必须有；没有消费点的字段不进 schema）

| 消费点 | 现状 | 改成 |
|---|---|---|
| 传输层注册 | `DebugRpcServer::start` 逐行手写 server.Get/Post | 遍历生成表逐条注册（手写字面量消失） |
| 传输层超时 | 等待函数里按路径挑三档 | 查生成表的 timeoutMs（超时表不再存在第二份） |
| 执行层分发 | `rpcDispatch` / `rpcDispatchPost` 一串路径比较 | 按路径查生成表 → **穷尽 switch（不写 default）**；新增路由忘写分支 = 编译不过（项目已开 `-Werror`；枚举 switch 缺 case 会被拦下，写 default 也会被现有开关拦下） |
| 路由回包 | `rpcRoutesJson` 手写字面量 | 从生成表拼装（同一次生成的两份产物互相对账，见 §5 单测） |
| 客户端（批 4 工具） | 原设计里手抄路径/超时 | 直接读同一份 schema：子命令、帮助、客户端超时全来自它 |
| 文档（批 3） | `docs/技术概念.md` 表格手写 | 生成一份人工可读的协议参考（生成物，拟放 rgl/generated/ 下）作字段级参考；技术概念只留语义说明 + 指针 |
| 检查规则 | §10 只对账注册↔分发 | 新规则 tools/checks/rules_rpc.py（拟建），见 §5 |

### 3.4 生成物形态（批 1 示意）

```cpp
// Auto-generated by reflection_tool from rpc_schema.json — DO NOT EDIT
export module rpc_schema_generated;
// 路由身份 + 超时；枚举供分发 switch 的穷尽检查用
export enum class RpcRouteId { /* … */ };
export struct RpcRouteDef {
    RpcRouteId id;
    std::string_view method;
    std::string_view path;
    std::string_view summary;
    uint32_t timeoutMs;
};
export inline constexpr std::array<RpcRouteDef, N> kRpcRoutes{ /* … */ };
```

批 2 起追加字段名常量（处理函数用它替换字符串字面量：改 schema 里的字段名 = 编译错）
与形状校验（先试刀一个端点，见批 2）。

## 4. 批次

> 每批独立可验收；**批 1 是机制本身**，即使后面几批不做，路由与超时也已经单源。
> 顺序 = 便宜的先上、贵的先试刀。

### 批 1 —— 路由与超时单源（机制落地）

拟新增：`rgl/rpc_schema.lua`（**Lua**，拍板 1）、`tools/templates/rpc_schema_cppm.inja`、
`rgl/generated/rpc_schema_generated.cppm` + `rgl/generated/rpc_schema.json`（同一次生成的两份产物）、
`tools/checks/rules_rpc.py`、`tests/test_rpc_contract.cppm`。
机制扩展（选 Lua 的代价，§3.2 两处）：`tools/schema_loader.cppm` 加 `loadLuaToJson`（与
`loadSchema` 同一套沙箱纪律）、`tools/reflection_tool.cpp` 的 `render` 加 `--lua` 与 `--output-json`。

改动：`cmake/RglReflection.cmake`（一条 render + 登记守卫）；`rgl/CMakeLists.txt`（base 清单）；
`rgl/debug_rpc_impl.cppm`（注册循环 + 超时查表）；`rgl/debug_rpc_exec_impl.cppm`（分发 switch +
回包拼装）；`rgl/debug_rpc_exec.cppm`（接口注释里的端点列表删掉、指 schema）；
`tools/checks/rules_text.py`、`tools/checks/config.py`、`tools/checks/main.py`（§10 三处删、换新读数）；
`docs/风格检查规则.md` §10 改写为"已由单源取代 + 指针"；`docs/还债总账.md` 债-125 行更新指针；
`docs/技术概念.md` §RPC 顶部指针与"超时表"段。

验收（注入实验逐条实跑，不只写在这里）：

| 注入 | 期望 |
|---|---|
| 改 schema 一条路径，不跑 `just reflect` | 构建被 reflection_check 拦住，报错指路 `just reflect` |
| 跑 `just reflect` 后不写处理分支 | 编译不过，编译器点名缺的枚举 case |
| 任一 RPC 文件里手写 server.Post("/x") 之类注册，或路径比较 | rules_rpc 报错，点名文件与行 |
| 手改生成物里一条路径 | rules_rpc 报错（schema 与生成物不一致） |
| schema 读不到 / 解析 0 条 / 生成物读不到 | rules_rpc 报错"本次未做任何对账"（不许静默变绿） |

行为等价：同一份配置下，重构前后 `GET /` 的应答逐字节相同（前后各捉一份对比）；端点冒烟
（/state、/camera、/param、/shot、/resize、/pass、/shotburst、/quit）；`just regress burst` 全绿；
`just test` 与 `just gates` 全绿。

#### 批 1 落地记录（2026-10-19）

上面两张表的逐条实测读数写在 `docs/风格检查规则.md` §10.8（注入实验，批 1 那七条在同节的表里）
与 §10.1–§10.7（判据与盲区），本处只记**计划没写到而当场必须决策的四处**，以及行为等价读数：

- **超时怎么取**：计划写"查生成表的 timeoutMs"，但没定"按什么查"。落地 = 注册期把路由身份绑进
  handler 闭包，`awaitReply(request, id)` 直接 `rpcRouteById(id).timeoutMs`。理由：按路径反查需要
  给"路径不在表里"编一个兜底档，而那个状态根本不该存在（httplib 只会把注册过的路由送进来）——
  多一个兜底数 = 多一处会漂的事实。
- **方法也进表**（`RpcRouteMethod` 枚举 + `methodText` 上线拼写）：注册侧因此能吃穷尽 switch，
  schema 写第三档方法时生成物直接编不过；分发侧顺带关掉原 §10 自述的"方法不判"盲区。
- **`summary` 在批 1 就有消费者吗**：计划 §3.3 说"没有消费点的字段不进 schema"，而 §3.4 的生成物
  示意里带着 summary。裁决 = 留，消费点 = `.json` 那份产物（批 4 客户端读它）+ `rules_rpc` 要求
  非空（空 = 这条事实只是占位）。单测同步钉 `!summary.empty()`。
- **路由回包搬到 `debug_rpc_payload`**：`rpcRoutesJson` 原本住在 app 层大文件的匿名 namespace 里，
  单测拿不到 ⟹ §5 要的"回包 ↔ 生成表自洽"写不出来。搬进 rlbase 的 `debug_rpc_payload`（它本来就是
  "应答形状"模块）后测试能直接调，宿主文件还短了一截。
- **两条自己踩到并修掉的判据缺陷**（都补了注入实验，读数在 §10.8）：① 消费点的 import 判据原本
  认"文件里出现过这串字"，把 import 注释掉仍然满足 ⟹ 改成认行首 `import` 语句；② `reflection_tool`
  的 debug 日志写的是 `reflection["parameters"]`，对非 const 的 `json` 那是**插入**语义，会往
  `--lua` 的 JSON 产物里塞一个 `"parameters": null` ⟹ 加 `contains` 守卫。

行为等价读数（本机，重构前/后各捉一份）：`GET /` 除 `frame` 计数外逐字节相同（`cmp` 过）；
九端点 + 六条边界路径（404 与 400 两类形态，含 `GET /camera` 这种"路径在表里但方法不对"）
除 `frame`/`startFrame` 外逐字节相同；`just regress burst` 32 帧 → 1 种哈希、最小同相位周期 1 帧；
出厂默认锚 `9dcc8099…` 与门关态锚 `0957692f…` 前后逐位相同且等于登记值；
`just test` 582 用例全过、`just gates` 四步全绿。

### 批 2 —— 字段形状入 schema（**全部 9 端点一次迁完**，用户拍板改选备选）

🔴 **前置（不满足不许开工）**：批 1 的 `just regress burst` 与两个截图锚**逐位全绿**已入账。
用户判断"一次性全迁理论上风险不大"靠的就是这道前置——**先有逐位等价的可信基线，才敢大改**。

schema 扩展：请求/响应字段（名称、类型、必填、范围、互斥）、错误文案（文案是行为契约的一部分，
`tests/test_burst_protocol.cppm` 已经在逐条钉住）。

生成：① 字段名常量（全部端点，处理函数用它替换字面量）；② 形状校验**全部端点**改为消费
生成校验，`/shotburst` 那六条文案从 schema 渲染、**测试逐字不变**（这条是行为锚，批 1/2 全程
不许改一个字）。

🔴 **未知字段改 400（拍板 5）是行为变更，不是重构**：现在静默忽略的调用方会开始收到 400。
切之前逐个核一遍已知调用方（`tools/imgqa/engine.cpp`、`tools/cpu_baseline.py`、`tools/regress.py`、
`tools/smoke.py`），并先跑一次全量冒烟留作回滚依据。

**与"一次性全迁"配套的止损线**：若某端点的形状**表达不出来**（现有约束语言覆盖不了它的条件性
字段），**该端点单独退回手写**并在 schema 里标 `manualShape: true` + 一句为什么——
不许为了"全迁"这个数字去扩约束语言（风险 2 的原话：字段约束语言越做越大）。
残余面如实登记：这类端点的字段名仍有常量消费，但"schema 说的必填/范围"与其手写检查之间无机器对账。

#### 批 2 落地记录（2026-10-19）

迁移结果：**8 个端点的形状进表并由生成校验执行，1 个端点（`/pass`）按止损线退回手写**。
`/` 与 `/state` 是 GET、不解析 body，本来就没有请求形状（分发层只给 POST 过闸）。
逐条实测读数写在 `docs/风格检查规则.md` §10.8（批 2 那六条注入）与 §10.1–§10.7（判据与盲区），
本处只记**计划没写到而当场必须决策的六处**、已披露的行为变更清单、以及行为等价读数。

- **形状闸门的调用点放在分发层，不放各处理函数**（计划没定这一处）。理由：放处理函数就是
  九次"记得调一下"，忘调是静默的；放 `rpcClaimRoute`（查表 + 方法核对那一步）则**新端点不可能
  忘接**。代价是两条错误路径的**先后**变了：`/shot`、`/shotburst` 原来是"状态冲突 409 先、
  形状 400 后"，现在是形状先——只在"请求既畸形又有状态冲突"这种双重非法输入上可观测。
  `/shotburst` 的 `parseBurstRequest` 仍自己调一次校验：`tests/test_burst_protocol.cppm` 不经
  HTTP 直接喂 body，那次调用是六条文案锚能继续成立的唯一途径。
- **`kind` 可以省略 = 不查类型**（计划没定）。为什么：处理函数今天不查类型的字段（靠
  `get<T>()` 抛 nlohmann 异常），若强行给它加一条类型检查就是**凭空多一条会漂的断言**，
  违反 §7「只收当前处理函数真正在查的形状」。
- **一条字段记录最多带四条文案**（`error` + `emptyError`/`oneOfError`/`exclusiveError` 三个覆盖槽）。
  为什么不是"一条字段一条文案"：`/shotburst` 的 `attachment` 一个字段就有四条**不同**文案，
  而"不同字段必须给不同串"是 `tests/test_burst_protocol.cppm` 钉着的契约（九条反向输入收敛到
  五条、一条不多一条不少）——只给一个槽，迁移当场就会把五条压成三条。
- **检查顺序是契约的一部分**，写进 schema 头部：端点内按 `fields` 书写顺序，单字段内固定
  `exclusive → required → kind → nonEmpty → oneOf → min/max`。这条不是风格：`/shotburst` 的
  `attachment` 与 `gbuf` 同时给时必须报"互斥"而不是"attachment 不是字符串"。
- **白名单取值也生成常量**（`rpc_value::<端点>::<字段>::<取值>`）。理由：`/camera` 的 `mode`
  处理函数**要按取值分支**，让它写 `== "trackball"` 就是在代码里抄第二份白名单——改 schema
  里的取值不会有任何东西红。吃常量之后改取值 = 分支编译不过。
- **`response` 段本批不做**（计划 §3.1 的形态示意里带着它）。理由：§3.3「没有消费点的字段
  不进 schema」——响应字段在批 2 找不到任何消费者（批 3 的文档生成器才是它的第一个消费者）。
  登记为未完成部分，不预先写进 schema 当装饰。
- **Python 侧的 schema 读取换成递归下降的 Lua 表解析器**（批 2 的前置坑，主 agent 点出）。
  原解析器只认深度 1 的扁平 `key = value`，遇到 `request = { fields = {…} }` 会把整段读不出来。
  它的失败方向确实是**误报而非漏报**（读不出 → 报"本次未做任何对账"），但字段级对账因此
  **根本不存在**，而"零警告"会被读成"三方一致"。换完之后 schema ↔ `.json` 是整份记录的深度
  相等，且解析器读不懂就抛（不认的转义 / 裸标识符 / 括号不配对 / 位置键与命名键混用）。
  **做到哪一层**：三方全字段深度对账 + 生成物内部两条自洽（方法枚举配 methodText、字段名常量
  的标识符配它的值）。**没退到**"只比两份产物 + 输入哈希"那一层。
  **残余面**：① 对 `.cppm` 是正则解析生成物文本，模板改形态 → 解析式认不出 → 报"解析出 0 条"
  （误报方向，不会假绿）；② schema 与两份产物**同时**被手改成同一个错值，没人守——
  `reflection_check` 只哈希输入。②是"生成物已提交"这一形态的固有代价，与 §7 同口径。

**已披露的行为变更**（除拍板 5 那条之外，都是"错误路径的文案变清楚"，状态码一律不变）：

| 路径 | 切前 | 切后 |
|---|---|---|
| 五个 POST 端点收到未声明的键 | 200（静默忽略）/ 404（`/param` 报的是 label 找不到） | **400** `unknown field '<键>' for <path>（字段清单 = rgl/rpc_schema.lua）` ← 拍板 5 |
| `POST /param` 缺 `label` / 缺 `value` | 400 `[json.exception.out_of_range.403] key 'label' not found` | 400 `'label' must be a string` / `'value' is required` |
| `POST /resize` 缺 `height` | 400 `[json.exception.out_of_range.403] key 'height' not found` | 400 `resize: width/height must be > 0`（该端点两个字段本来就共用这一条） |
| `POST /quit` 带任何键 / body 不是对象 | 200（body 整个被忽略） | 400 |
| `POST /pass` body 不是对象 | 400 `pass: 需要 "name"（…）` | 400 `body must be a JSON object`（与其余端点同一条） |

**没变的部分**（逐字节核过）：`/shotburst` 的六条契约文案、`/camera` 的 `mode` 与
`yaw_at_frame` 两条、`/shot` 与 `/shotburst` 的互斥与空名两条、`/pass` 的三条手写检查、
`/param` 的 404 两条、所有**正常**请求的应答。九端点 46 个用例的 before/after 对拍：
除上表那 10 处，其余 36 处逐字节相同。

**行为等价读数**（本机，批 2 前后各捉一份）：出厂默认锚 `9dcc8099…` 与门关态锚 `0957692f…`
逐位相同且等于登记值；`just regress burst` 32 帧 → 1 种哈希、最小同相位周期 1 帧；
`just test` 589 用例全过（新增 7 条形状用例，`tests/test_burst_protocol.cppm` **一个字没改**
仍全过）；`just gates` 四步全绿。

**调用方核查**（切 400 之前逐个 grep 实际发出的键，全部已在 schema 声明）：
`tools/imgqa/engine.cpp`（`/camera` mode·position·target·fovDegrees·yawDegrees·yaw_at_frame，
`/param` label·value·page，`/shot` file·attachment·gbuf，`/shotburst` n·dir·prefix·attachment·
start_frame，`/quit` 空对象，`GET /`、`GET /state`）、`tools/regress.py`（`/shotburst` n·dir·
prefix，`/quit` 空对象，`GET /state`）、`tools/cpu_baseline.py`（`GET /`、`GET /state`、
`POST /quit` 无 body）。`tools/smoke.py` 与 `tests/` 不打 RPC；`rgl/` 内部只有 bind 前探端口的
`probe.Get("/")`（无 body）。切后以上每一处都重跑过：`just regress burst` 绿、
`just tools-check` 绿、探针里 imgqa 用到的每个端点的正常路径逐字节不变。

### 批 3 —— 其余端点字段 + 文档生成

补全剩余端点字段与语义说明（含"哪些字段是条件性的、哪些是运行时快照"这类注释）；新增
tools/templates/rpc_schema_md.inja → 生成人工可读的协议参考（生成物，拟放 rgl/generated/ 下）；
`docs/技术概念.md` §RPC 表格瘦身为语义 + 指针（删掉与 schema 重复的字段枚举）。

验收：生成参考与 schema 一致（规则扩面）；技术概念删改后文档指针检查全绿；参考文档样例先给
用户过目。

#### 批 3 落地记录（2026-10-19）

交付：9 个端点全部补 `response = { fields = … }`（字段清单逐条读处理函数实际回包得来：
`rpcRoutesJson` / `rpcHandleState` / `rpcHandleCamera` / `rpcWriteParamPage` / `rpcSettleShot` /
`rpcSettleBurst` / `rpcHandleResize` / `rpcHandlePass` / `rpcHandleQuit`，**没抄旧文档表**——
这条不是形式主义：`graph.presentPasses`/`dynamicPasses` 的类型（`string[]`，不是数字）只有读
`GraphStatus` 本体才拿得到，旧表那里本来就只写了 `{…}` 占位，凭记忆或凭表写都会写错）；
新增 `tools/templates/rpc_schema_md.inja` → 同一条管线多渲染
`rgl/generated/rpc_schema.md`（已提交产物）；`docs/技术概念.md` 端点表整删只留语义 + 指针；
`rules_rpc` 从四个面扩到六个面（面 5 = response 段自洽，面 6 = md ↔ schema；判据与盲区 →
`docs/风格检查规则.md` §10.9/§10.10）。逐条注入实验十四条的实测读数写在 §10.8（批 3 那张表），
本处只记**计划没写到而当场必须决策的七处**、一个待拍板分叉、与验收读数。

- **`response` 不进 `.cppm`**（计划 §3.4 只说"批 2 起追加字段名常量"，没说响应进哪份产物）。
  理由 = §3.3「没有消费点的字段不进生成物」的对称应用：C++ 侧没有任何代码读响应形状（回包由
  处理函数拼装，无运行期校验可挂），渲染进 `.cppm` 就是一堆没人读的 constexpr = 装饰。
  消费方 = `.md`（人工）与 `.json`（客户端）。对账不因此变弱：面 1 对 `.json` 是**整份深度
  比较不放过 response**，面 6 对 md 钉字段名集合与顺序；`.cppm` 侧把 `response` 加进
  `_CPPM_ONLY_IGNORE` 的口径是"该产物按构造不带它"（与 `manualReason` 同族），不是"没人对账"。
- **共享子结构放根级 `shapes`、按名引用**（计划没定嵌套怎么表达）。触发点 = camera 姿态被
  `/state` 与 `/camera` 共用：就地展开就是同一事实抄两遍（本项目哲学第 2 条的反面）。
  `kind ∈ {object, object[]}` 必带 `shape`、非 object 不许带、引用悬空与"没人引用的 shape"
  都由面 5 报 error。`/state` 的 `pages` 按计划 §3.1 边界②只登记粗粒度（name/entries），
  entries 的元素形状写在 note，不抄 param_semantics 的 label 词表。
- **条件性字段 = `optional` + `absentWhen`，不扩"分支形状"表达**（计划没定 `/pass` 两种回包
  形态怎么表达）。`/pass` 的 on 形态与 toggle_frames 形态回**不同键集**——用"每条字段标
  什么时候没有"的并集表达，而不是给约束语言加 variant/oneOf-shape。理由与批 2 止损线同一条：
  约束语言只收现状；并集 + absentWhen 已把"客户端能缺哪些"说全，加形态概念就是往 OpenAPI 方向滑。
- **响应 kind 词表与请求侧 `RpcFieldKind` 分开**（`boolean/uint32/number/string/vec3/string[]/
  object/object[]/json`，住 `config.RPC_RESPONSE_KINDS`）。请求那份是运行期校验档位，响应这份是
  文档口径；硬合并会让"加个文档类型"变成"改校验器"（风险 2 的另一种长大）。
- **面 1 追加根级对账**（schema ↔ `.json` 除 endpoints 外整份深度比较）。批 3 起了根级
  `shapes`，而端点级对账看不到它——手改 `.json` 的 shapes 就没人守了。根级比较让
  `module_name` / `version` / `shapes` / **未来新根键默认进比较范围**，与端点记录同口径
  （注入实验 E13 实测：`schema 根级.shapes[0].fields[0].name` 点名到位）。
- **md 对账把"顺序"也钉上**（任务只要求条数 + 字段名集合）。理由 = 面 1 批 2 的同款教训：
  只比集合，"两边各少同一维"与"顺序被手工挪过"都测不出来；md 按 schema 顺序渲染，正常生成
  不可能漂顺序，漂了就是有人手工编辑产物（E11 实测：换序红而集合绿）。
- **两处工具坑**（计划没预见到）：① inja 默认把**行首 `##` 当模板行语句**，markdown 二级标题
  天然撞它（`renderTemplate` 里 `set_line_statement("")` 关掉——本仓没有任何模板用行语句，
  关掉只会解禁行首 `##`）；② 任务书说"把那份独立 Lua 解析器扩到能读 response 嵌套"——批 2
  换的递归下降解析器**本来就吃任意深度**，本批实测直接读出 response/shapes（无需扩面）；
  真正会卡住的形态是 Lua 的 `..` 字符串拼接（两侧解析器都不认，schema 头部已写明是数据子集）。

**待拍板分叉（未实现，等裁决）**：**错误回包要不要也进 schema**。现状 = 统一错误体
`{"ok":false,"error":"…"}`（本体 `rpcErrorJson`）+ 各端点的错误条件散在处理函数里，md 参考的
「回包总形状」一节把错误体作为**全局事实写一次**（模板硬编码 + 指针），**没有**每端点的
错误枚举。两种走向：A 维持现状（错误形状不进表）；B 给每端点加 `errors = { {status, when,
error文案} }` 段。B 的代价：`when` 是运行期状态（409 抢槽、锚已过、门未开），schema 表达不了
判据本体，只能登记"人话描述 + 文案"——**schema 与处理函数之间无机器对账**（manualShape 同族
的残余面，且量大得多：粗数 20+ 条），收益 = 客户端能枚举错误而不读代码。A 的代价：客户端要
处理全部错误形态得读处理函数。**推荐 A**（§7「不追求 schema 描述一切」+ 错误条件天然漂），
若批 4 客户端确实需要错误枚举再立 B 的专项（文案先收进 schema 一个 `errorCatalog` 段、逐条
`TEST:` 钉住，比裸登记 errors 段可辩护）。本批按 A 落地，未预造 B。
 **裁决（主 agent，2026-10-19）= A**，理由与 B 的反对意见同一条：一份"声明了但没有任何机器在验"
 的 20+ 条错误清单，正是本项目反复立债的那个形态（"看起来已完工的半吊子"）。用户若要 B，推翻
 本段即可，专项要往哪儿加已经想好（`errorCatalog` 段 + 逐条 `TEST:`）。

**主 agent 亲验**（不采信 worker 自报读数，且**变异形态与 worker 所验不同**）：

- `just gates` 四步全绿；两条截图锚我重跑 = `9dcc8099` / `0957692f`（与登记值逐位相同）。
- **产物缺失不静默**：`mv rgl/generated/rpc_schema.md` → `checks --strict` rc=1，文案点名
  "磁盘上不存在"；还原 → rc=0。
- **护栏覆盖的真实语义要问准**：手改 md 不跑 reflect ⟹ `just build` **rc=0**。这不是批 3 的洞——
  对照实验：手改既有产物 `types_generated.cppm` 同样 rc=0 ⟹ `REFLECTION_PAIRS` 的 SHA1 护栏守的是
  **输入漂移**（改了 schema/模板没重跑），从不哈希产物。该性质对 md 实测成立：改
  `rgl/rpc_schema.lua` 不跑 reflect → `just build` **rc=1**（`reflection_check` FATAL）。
- **worker 没验的方向（更要紧的那一侧）**：schema **少声明**一个引擎真会回的字段。我注入删掉
  `/resize` 的 `frame` 声明并 `just reflect`（让 md 与 schema 一起变"自洽"）⟹ `checks --strict`
  **全绿**（面 6 结构上看不见），`tools/rpc_endpoints_live.py` **rc=1** 且点名
  `实际 = [frame,height,ok,width] / 声明全量 = [height,ok,width]`。⟹ 该脚本是这一侧唯一的网，而它
  当时**没有任何入口** = 只在有人记得敲时才存在 ⟹ 已补 `just rpc-live`（要真设备，刻意不进 gates：
  gates 的无设备性质是设计的一部分）。
- 抽查"字段清单来自读代码"这句：`GET /` 声明的 `ok`/`frame`/`routes` 与 `debug_rpc_payload.cppm`
  的 `rpcRoutesJson` 回包逐字相符；`/shotburst` 的 `ok`/`count`/`startFrame`/`files` 与
  `rpcSettleBurst` 的 resolve 逐字相符。
- 改动面核对：worker 未触碰 `frame_capture*.cppm` / `ecs_frame_diag.cppm` /
  `debug_rpc_exec_impl.cppm`（按 mtime 划面）；已跟踪的生成物 `git diff` 干净 ⟹ `renderTemplate`
  关掉 inja 行语句**没有**改动其他模板的产物字节。


**验收读数**（本机，2026-10-19）：`just gates` 四步全绿（build → test → tools-check →
checks --strict，逐条取退出码）；两个截图锚**逐位不变**且等于登记值（`just shot --name w1`
→ `9dcc8099…`；`--name w2 --set meshlet.assets=false --set meshlet.shadow_lod=-1` →
`0957692f…`；跑前 `pgrep -cx rgl` = 0）；`GET /` 的 routes 数组与 schema 记录顺序**逐条含序**
一致（批 1 既有行为不变）；`just doc-check` 0 警告；实机回包对拍（`tools/rpc_endpoints_live.py`，
本批新落）九端点 + `/pass` 两分支全绿——顶层键集合、`/state` 三个嵌套子结构与 pages[0]、
`/camera` 的 camera、条件性字段两条反向用例、`/quit` 干净退出 rc=0，全部与 schema 一致；
脚本自身的变异自验（schema 加一个处理函数不回的字段 → 脚本红且归因点名；还原 → 绿）。
判据变异自验十四条（面 5 七条 + 面 6 五条 + 根级一条 + 读不到必须报错一条）读数 → §10.8 批 3 表。

**没做完 / 故意没做**（如实登记）：① 上面那个错误回包分叉（等拍板，未实现）；② 批 4 客户端
读 `.json` 里 response 段的**消费代码**还不存在（数据已就位，消费在批 4）；③ 面 6 只钉 md 的
**结构事实**（条数/顺序/字段名），不钉 note/summary 文案——文案漂了 md 与 schema 仍"一致"，
这是刻意的（文档措辞不是契约，钉它 = 把改文案变成要跑 reflect 的事）；④ `GET /` 应答带字段
清单（能力发现）按计划排在批 5，本批没动回包内容。

### 批 4 —— 客户端工具（原任务 CLI 部分）

tools/rpc/ 新建（拟建；Python，HTTP 用系统 requests——仓库"Python 工具仅标准库"的规矩由用户
2026-10-19 明确放宽，工具自述里写明这一例外）；子命令、帮助、客户端超时从 schema 来；Justfile
加一条 rpc 菜单条目（参数透传）。顺带把原任务里的动作做完：工具驱动 headless 引擎实测热重载
（`camera.lua`/`scene.lua` 等配置改动 → 经 `/state` 观测生效），收尾走 `/quit` 干净退出。

### 批 5（可选）—— 能力发现与客户端收敛

`GET /` 应答扩展：从生成表直接带出各端点字段（远程/无仓库副本的客户端也能发现能力）；
把 `tools/imgqa/engine.cpp`、`tools/cpu_baseline.py`、`tools/regress.py` 的 RPC 用法逐步收敛到
工具客户端层，或至少让它们读同一份生成数据。网页 GUI 在此之后另立计划。

## 5. 检查规则与测试（换血后各守什么）

- rules_rpc 面 1：schema ↔ 生成物逐条**深度**对账（路由集合、方法、超时、批 2 起含嵌套的
  请求字段表），防"生成物被手改 / 只重生成一份"——`cmake/ReflectionManifest.cmake` 只哈希输入，
  产物被编辑它看不见（同 `tools/checks/rules_reflection.py` 的取向）。批 2 起 schema 是嵌套的，
  本模块的 Lua 读取因此换成递归下降表解析器（读不懂就抛，不静默少读一块）；
- rules_rpc 面 2：扫 rgl/ 全部 .cppm（排除 generated/）出现的注册/分发字面量即报错，批 2 起
  含"请求字段名写回字面量"（`body.at("…")` 那类）；同时要求消费文件确实 import 生成模块
  （找不到 = 这条面可能已经瞎了，出声）；
- rules_rpc 面 3（批 2）：schema 里声明的每个字段都必须有 `rpc_field::<端点>::<字段>` 引用点。
  这条是"字段名常量"不沦为装饰的机器凭据；
- rules_rpc 面 4（批 2）：标了 `manualShape` 的端点必须带非空 `manualReason`（止损线出口的
  配套护栏）。生成物侧另有编译期断言：`manualShape` 端点不许在 schema 里声明形状；
- 失效出声：schema 或生成物读不到 / 解析出 0 条 → 报错声明"本次未做任何对账"；
- 读数：每次构建一行（`schema N 条 / 生成物 N 条`），与既有各条的读数纪律一致；
- 单测 tests/test_rpc_contract.cppm（拟建；无设备，毫秒级）：生成表非空、路径唯一、id 唯一、
  超时大于 0；"路由回包的字面量解析回来 == 生成表"（同一次生成的两份产物自洽）；
- 行为等价锚：`tests/test_burst_protocol.cppm` 现有六条文案在批 1/批 2 全程不许改一个字。

## 6. 拍板结论（2026-10-19 用户逐条给）

| # | 事项 | **结论** | 落地影响 |
|---|---|---|---|
| 1 | schema 格式 | **Lua**（用户：「就是为了全局风格统一」） | 触发 §3.2 的两处扩展（`--lua` 入口 + JSON 生成物），**这是本计划唯一一处"选备选要多干活"的地方** |
| 2 | 生成器 | 复用 `reflection_tool render` | 零新工具 |
| 3 | 字段校验迁移范围 | **一次性全迁**（用户改选备选，原推荐是"只试刀 `/shotburst`"） | 批 2 范围放大到全部 9 端点；用户判断「理论上风险不大」的**前置 = 批 1 的截图逐位等价先立住**，所以批 2 必须在批 1 的 `just regress burst` 全绿之后才开工（见批 2 前置条） |
| 4 | 文档 | 批 3 生成参考 + 技术概念瘦身 | 推荐 |
| 5 | 未知字段 | **改 400 响亮拒绝** | ⚠ 这是**行为变更**不是重构：现在静默忽略的调用方会开始收到 400。已知调用方 = `tools/imgqa/engine.cpp`、`tools/cpu_baseline.py`、`regress.py`、`smoke.py`，批 2 要逐个核一遍再切（切前给一次全量冒烟） |
| 6 | §10 退役时机 | 批 1 同批换血 | 推荐 |
| 7 | 工具 HTTP 依赖 | 系统 requests | 推荐（规矩例外写进工具自述） |

**用户补的一句定方向的话**（原话）：「本计划核心目的就是收敛全部外部调试接口到统一」
⟹ 判据不是"批 1/2/3 做完"，而是"**引擎能做什么这件事只剩一份手写清单**"。凡在此之外的
第二份副本（含批 5 的客户端收敛）都算本计划未完成的部分。

<details><summary>用户手写拍板原文（保留，别改）</summary>

* 1. 我选择Lua，没别的理由，就是为了全局风格统一。
* 其他都选推荐。
* 但是讲道理3迁移范围，理论上风险不大，截图成功后直接全部迁移，子agent慢慢干。
* 本计划核心目的就是收敛全部外部调试接口到统一。

</details>
| # | 事项 | 推荐 | 备选 |
|---|---|---|---|
| 1 | schema 格式 | JSON（render 子命令直接吃、客户端用标准库 json 可读；先例 = `rgl/features_config.json` / `rgl/param_semantics.json`） | Lua（与着色器 schema 同形态，但要扩工具与客户端） |
| 2 | 生成器 | 复用 reflection_tool render | 新子命令/新工具（不建议：多一份要守的东西） |
| 3 | 字段校验迁移范围 | 批 2 只试刀 `/shotburst`，干净才推广 | 一次性全迁 / 完全不迁（只剩字段名常量的消费，残余面如批 2 所述） |
| 4 | 文档 | 批 3 生成参考 + 技术概念瘦身 | 保留手写表 + 加一条"表↔schema"对账规则 |
| 5 | 未知字段 | 批 2 改 400（响亮） | 维持静默忽略（现状） |
| 6 | §10 退役时机 | 批 1 同批换血（避免两条机制说同一件事） | 并存一期 |
| 7 | 工具 HTTP 依赖 | 系统 requests（已确认可用） | 标准库 urllib（`tools/cpu_baseline.py` 先例，能避开规矩例外） |

## 7. 边界与非目标

- 不改线程/时序模型、不改传输层行为：注册与分发是等价重写，批 1 验收就是"看得见的行为不变"。
- 不动着色器反射管线与其余既有检查规则；不把 tools/checks 变成生成器的消费者。
- 不引入 OpenAPI 等外部标准：这套约束语言只需要覆盖现状；将来客户端变多再评估。
- 不追求"schema 描述一切"：只收当前处理函数真正在查的形状；找不到消费者的事实不进。
- 传输中立：schema 与生成物只描述**能力**（有哪些命令、收什么、回什么），不描述**怎么送**。将来给 Blender 加实时通道（推送、长会话、预览帧流）时，做法是新增一个传输层模块 + schema 增"事件"段（引擎→客户端主动消息），命令面、生成物与现有 HTTP 通道都不动；形态届时再定（WebSocket / SSE / 本地套接字均可）。批量资产数据不走消息通道——继续用文件/共享内存交接 + 一条通知。本计划不预造这部分。
- 风险 1：schema 变装饰 ⟹ 每批验收都含"消费点变红"的注入实验；没有消费点的字段不进 schema。
- 风险 2：字段约束语言越做越大 ⟹ 只收现状；先试刀后推广。
- 风险 3：生成物里第一次出现 .md（参考文档）⟹ 批 3 先出样例给用户过目；不接受就退到 §6 第 4 条的备选。
- 附注：本文不抄端点名单（抄一次漂一次）——要看端点与字段，以 schema 本体与该进程的 `GET /` 应答为准。

## 8. 调研记录（本计划依据的实测，便于复核）

- 副本清点与 §10 看不到的地方：`rgl/debug_rpc_impl.cppm`（`DebugRpcServer::start`、等待函数）、
  `rgl/debug_rpc_exec_impl.cppm`（`rpcRoutesJson`、`rpcDispatch`、`rpcDispatchPost`）、
  `rgl/debug_rpc_exec.cppm`（接口注释）、`docs/技术概念.md` §RPC 调试通道、
  `docs/风格检查规则.md` §10、`tools/imgqa/engine.cpp`、`tools/cpu_baseline.py`、`tools/regress.py`。
- 管线可行性：`cmake/RglReflection.cmake` 里 features / param_semantics 两条 render 管线；
  `cmake/ReflectionManifest.cmake` 的输入哈希守卫；`tools/reflection_tool.cpp` 的 render 子命令；
  `tools/generator.cppm` 的 `renderTemplate`；生成物表头形态（`rgl/generated/param_semantics_generated.cppm`）。
- 行为锚：`tests/test_burst_protocol.cppm` 的六条错误文案与锚点裁决用例。
- 编译器兜底：根 CMakeLists 第一方 `-Werror` + `-Wcovered-switch-default`（缺 case 与多余 default 都会被拦下）。
- 债-125 现状（计划撰写时）：`docs/还债总账.md` §1 该行；当时的规则本体 = `tools/checks/rules_text.py`
  的 `rpc_route_checks`（批 1 已删除，换 `tools/checks/rules_rpc.py`）。

---

## 9. 附：`/shot` 端点被一次拼错打死（2026-10-19，批 2 期间实机撞出）

批 2 验 RPC 端点时 `POST /shot {"attachment":"object_id"}`（picking 门未开）拿到 **504**，
且此后每个 `/shot` 都吃 409。根因不在 RPC 层，在帧捕获的状态机：

`FrameCapturer::record` 把两种「未命中」揉进同一个条件（`slot != kRgNoAttachment ? rts.find(name) : nullptr`
为空 ⟹ 只 warn 不清请求），而「保持 pending 重试」只在**暂时**未就绪时成立。**名字不在编译好的图里**
是永久结论——图不会自己长出附件。于是每帧重演：warn 每帧一条（实测 46 秒 **1505** 条，而
`requestAttachment` 的接口注释原本承诺"warn 一次"）、`pendingShot` 永不复位 ⟹ 之后**每一个**截图请求
都 409，直到进程重启。`gbuf` 分支的 `captureRequest` 虽在 record 末尾被复位（不刷日志），
但同样没有失败通道 ⟹ 一样把端点等到死。

> ⚠ 原 `take()` 注释写的理由「record 未命中（acquire 跳帧）时请求须保持 pending 重试」
> **把两件事混成了一件事**：acquire 超时是 `drawFrame` 提前 return（`vkrenderer_impl` ③
> 「acquire 100ms 超时跳帧」），`record` 根本不被调用 ⟹ 请求天然保持 pending，**不需要靠
> "未命中时不清"来保**。已就地改正（错误版本不留，只留正确做法）。

### 修法（三处，缺一处就是半吊子）

1. **录制侧**（`frame_capture_impl.cppm`）：拆两种未命中——不在图里 = 永久失败（清请求 + 置失败值），
   在图里但 RT 未建 = 可重试但**限次**（`kCaptureRtRetryBudget = 8`，RT 因装饰条件永不建时同形缺陷）。
   三处失败点收进一个 `failWith` lambda：**原因字符串同时是日志文本和回给消费方的原因**——旧代码
   一个意思写两遍，结果两处都没兑现"warn 一次"。
2. **通道**（`frame_capture.cppm`）：`hasFailure()` / `takeFailure()`（取后即清，一条失败只兑现一个请求，
   不许留在里面毒化下一个合法请求）；`request*()` 清失败值。分类逻辑抽成**纯函数**
   `classifyCaptureMiss(inGraph, missesSoFar, budget)`——`record`/`collect` 要 `VkDevice`，
   而"该重试还是该判死"是纯逻辑，不拆出来就只能开设备复现。
3. **消费侧**（`debug_rpc_exec_impl.cppm` + `ecs_frame_diag.cppm`）：`rpcSettleShot` / `rpcSettleBurst`
   在 `hasFrameCapture()` 之前先查失败 ⟹ 立即 **400 带原因** + 复位 pending。llm 截图路径同型缺陷
   （`wanted()` 过 delay 后恒真，只等产物 = 进程永不退出）⟹ 同一批收：失败即 `AppExit` + 错误上抛。

### 实测读数（改前 → 改后，同机同构建档）

| 判据 | 改前 | 改后 |
|---|---|---|
| `/shot attachment=object_id`（图里没有） | 504 超时 | **400** `attachment 'object_id' is not in the compiled render graph` |
| 失败之后再来一个 `/shot` | 409 `another /shot is still pending`，到重启 | **200**（端点自愈） |
| `/shot gbuf=99`（越界，走 RPC 不走配置解析器） | 静默无产物 → 端点打死 | **400** `gbuf channel 99 out of range [0, 5)` |
| `/shotburst attachment=taa_out`（过白名单、图里没有） | 整批卡死 | **400** 带原因，之后 `/shot` 仍 200 |
| 一次失败请求的 warn 条数 | 1505 条 / 46 秒 | **1 条**（三条失败请求共 3 条） |
| llm 模式回读源不可用 | 永不退出 + 每帧一条 warn | 退出码 255 + 一行错误带原因（受控变异验，见下） |

逐位回归未受影响：出厂默认锚 `9dcc8099…`、门关态锚 `0957692f…` 改后**逐位不变**（本改动不碰渲染路径）。

### 沉淀地址

- `TEST: tests/test_frame_capture_miss.cppm`（5 例 623 断言，纯函数半边）。五条变异**全部亲验变红**：
  永久分支失效（68 红）/ `>=`→`>`（16）/ 去 `+1u`（16）/ 三元对调=一律判死（318）/ 默认预算 `+3`（2）。
  ⚠ 首轮变异脚本自己犯了 skill `rgl-test-discipline` 的两条：**`| tail` 吞退出码**（构建失败被读成
  "编译通过 + 测试全绿"）+ **`if (!inGraph)` 改成 `if (false)` 触发 `-Wunused-parameter`**（拿到的是
  编译红不是判据红）。两条都在 skill 里写着，是**读了没照做**，不是不知道。
- `GUARD:` 无新增机器护栏——`record()` 超 120 行的上限由 source_checks 点名（本批已压回，靠 `failWith`
  收口而不是靠删注释）。
- `DOC:` 状态机消费侧的教训**写不出无设备断言**（`FrameCapturer` 带 `vk::raii` 成员，无设备编不出来）
  ⟹ 落本节，判据 = 上表的实机读数。llm 那条用**受控变异**端到端跑通（临时把越界判据放宽成
  `captureRequest >= 0`，让每次 gbuf 请求都判永久失败）——生产条件本身由 RPC `gbuf=99` 独立验到，
  两侧合起来才算这条分支验过。

### 未收的尾巴（登记，别当已验）

- **一条没复现的 segfault**：`dmesg` 有 `rgl[...]: segfault at 2 ... error 4 in rgl[...]`（用户态读
  地址 0x2）。我在同构建上按同激励跑两次**均未复现**（进程存活、只出 504），而首次实验里"进程消失"
  是我自己 `pkill -f rpcfg` 杀的，不算证据。`collect()` 的拷贝循环有 `if (captureMapped)` 守卫，
  嫌疑未成立。**日志洪水修掉后再出现** ⟹ 拿这条指纹对拍，别再从头猜。
- **schema 侧的对应缺口 → 批 3**：`/shotburst` 的 `attachment` 声明了 `oneOf = { "swapchain", "taa_out" }`，
  `/shot` 的没有。但"哪个附件名合法"取决于**运行期的图与门**（本节的 `taa_out` 就是过白名单却不在图里），
  ⟹ 它不属于静态 `oneOf`，属批 3 要立的「响应形状 / 运行期能力」那一类，别在 schema 里抄一张会漂的名单。
