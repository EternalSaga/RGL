-- rgl/rpc_schema.lua —— 引擎外部调试接口的能力清单（唯一真相源）
--
-- 消费方与生成物：`docs/rpc_单源_plan.md` §3.2/§3.3。一句话——本文件决定
-- 「有哪些端点、什么方法、各自怎么超时、各收哪些请求字段、各回哪些响应字段」，生成物
-- `rgl/generated/rpc_schema_generated.cppm`（C++ 路由表）、`rgl/generated/rpc_schema.json`
-- （非 C++ 客户端）与 `rgl/generated/rpc_schema.md`（人工可读协议参考）承载它，
-- 服务端注册 / 分发 / 超时 / 路由回包 / 请求形状校验与外部客户端全部吃生成物。
-- **改本文件后必须 `just reflect`**（构建开头的输入哈希守卫会拦）。
--
-- 一条记录 = 一处事实，路由字段含义：
--   id          路由身份，直接成为生成物里 `RpcRouteId` 的枚举子 ⟹ 必须是合法 C++ 标识符
--               （拼错 = 生成物编不过，这是故意的）；
--   method      上线方法（"GET" / "POST"）；生成物里的 `RpcRouteMethod` 只有这两档，
--               写第三档 = 生成物编不过；
--   path        路径（httplib 全串匹配）；表内不得重复（生成物有编译期唯一性断言）；
--   timeout_ms  该端点的等待超时（毫秒，必须 > 0）——帧边界没跑到就 504，见传输层；
--   summary     一句说明，给文档参考与客户端帮助用（不进任何应答体）。
--   manualShape 该端点的**形状检查留在处理函数**（见下"止损线"），生成物只出字段名常量；
--   manualReason 为什么表达不出来 —— manualShape = true 时必填（checks 侧强制非空）。
--
-- ── 请求字段（批 2 起）：`request = { fields = { … } }` ─────────────────────────────
-- 一条字段记录 = 处理函数**真正在查**的一条形状事实。字段含义：
--   name       字段名，同时成为生成物里的常量名（`rpc_field::<id>::<name>`）⟹ 必须是合法
--              C++ 标识符；改它 = 引用它的处理函数编译不过（这就是"单源承重"的形态）。
--   kind       类型档：`string` / `integer` / `number` / `boolean` / `vec3` / `any`。
--              `integer` = 整数（不含浮点），`number` = 任意数值，`vec3` = 三个数值组成的数组。
--              **省略 = 不查类型**（该字段的类型失效今天仍由处理函数的 `get<T>()` 抛出）。
--   required   必填（缺字段 → 报 error）。
--   min / max  数值**闭**区间（只在整数档上有意义）。
--   nonEmpty   字符串非空。
--   oneOf      取值白名单（只在值是字符串时比较；非字符串交给 kind 档或处理函数）。
--   exclusive  与另一字段互斥（两者同时出现 = 客户端 bug，响亮拒，不静默取一个）。
--   error      文案。缺字段 / 类型不符 / 越界 三条失效共用它（本项目既有文案就是这个形态：
--              `'n' must be an integer in [1, 1024]` 同时覆盖"没给/给了字符串/越界"）。
--   emptyError / oneOfError / exclusiveError
--              分别覆盖 nonEmpty / oneOf / exclusive 那一条的文案（不写 = 用 error）。
--              为什么要有覆盖档：`/shotburst` 的 `attachment` 一个字段有四条不同文案，
--              而文案是行为契约（`tests/test_burst_protocol.cppm` 逐条钉住"不同字段必须给
--              不同串"）——只给一条 = 迁移当场破契约。
--
-- 检查顺序是**契约的一部分**（同一条请求只能报一个错，谁先红就是谁的归因）：
--   端点内按 fields 的书写顺序逐条查；单字段内 fixed 顺序 =
--   exclusive → required（缺字段）→ kind → nonEmpty → oneOf → min/max。
-- 全部字段查完才查**未知字段**（未在本表声明的键 → 400；这是批 2 拍板 5 的行为变更：
-- 以前静默忽略，现在响亮拒。理由：客户端拼错字段名会拿到 200 + 一个"什么都没改"的应答，
-- 而"看起来生效了其实没生效"正是本项目反复踩的那类假绿）。
--
-- ⚠ 字符串里**别放** `"` 与 `\` 以外的控制字符：模板经 `cpp_escape` 写进 C++ 字面量，
--   引号与反斜杠会被转义（能过），换行会变成 `\n`（文案里本来也不该有换行）。
--
-- ── 响应字段（批 3 起）：`response = { fields = { … } }` 与根级 `shapes` ────────────────
-- 描述**成功回包**（失败回包是统一错误体 `{"ok":false,"error":"<一句原因>"}`，本体在
-- `rpcErrorJson`，各端点不重复登记）。消费方 = 人工参考 `rpc_schema.md` 与 `.json` 产物；
-- **C++ 侧不消费**（回包由处理函数拼装，没有运行期校验）⟹ 本段是文档面，机器对账分工：
-- rules_rpc 面 5 查本段自洽（每个端点必须有 response、kind 在词表内、shape 指得到、
-- optional 必带 absentWhen），面 6 查 `rpc_schema.md` 与本段一致，面 1 的深度对账顺带覆盖
-- `.json` 里的本段。**「schema 与处理函数实际回包一致」不在机器对账范围**（与请求侧同口径，
-- 那条靠端点冒烟；改回包字段记得同步这里，`tools/rpc_endpoints_live.py` 逐端点对拍键集）。字段含义：
--   name        回包**顶层键**（嵌套结构经 shape 引用，见下）。
--   kind        取值形态：`boolean` / `uint32` / `number` / `string` / `vec3` /
--               `string[]` / `object` / `object[]` / `json`。
--               `json` = 类型随上下文而异（如 /param 的 value 回显随目标条目 kind 变）。
--   shape       kind 为 object / object[] 时**必填**，指向根级 `shapes` 里的共享子结构名。
--               为什么用引用而不是就地展开：同一结构会被多个端点回（camera 姿态 = /state 与
--               /camera 共用），就地抄两份就是会漂的第二份副本——共享结构只登记一次。
--   optional    条件性字段（不是每个成功回包都有）。
--   absentWhen  什么时候没有它（optional = true 时**必填**——对客户端来说「能缺哪些」
--               与「有哪些」是同一份契约的两半，只登记前者不登记后者等于没登记）。
--   note        语义说明（单位、口径、读数时刻、生效时机）。
-- 根级 `shapes` = 共享响应子结构清单：`{ name = …, note = …, fields = { … } }`，
-- 字段记录与上面同构（object 类同样可以再引 shape）。只被一个端点用的结构也放这里——
-- 「放 shapes」是为了让 md 把每个结构只渲染一次，不是为了省行数。
--
-- 止损线（`docs/rpc_单源_plan.md` §4 批 2）：现有约束语言只表达**无条件**的必填/类型/范围/
-- 白名单/互斥。某端点的形状若需要条件性必填这类表达不了的形态，**该端点单独退回手写**并标
-- manualShape + manualReason——不许为了"全迁"这个数字去扩约束语言（计划风险 2）。
-- 这类端点的残余面：字段名仍有常量消费、未知字段闸门照跑，但"schema 说的必填/范围"与
-- 处理函数手写检查之间**无机器对账**。
--
-- 顺序即契约：本文件的记录顺序 = 注册顺序 = `GET /` 回包里 routes 数组的顺序。
-- 端点与字段的名单**只在本文件**（别处抄一份就会漂）——要看现状就读它，或问进程的 `GET /`。

return {
    module_name = "rpc_schema_generated",
    version = 1,
    endpoints = {
        {
            id = "routes", method = "GET", path = "/", timeout_ms = 2000,
            summary = "路由与能力发现（frame = 装配态帧计数，纯观测）",
            -- 回包本体 = rpcRoutesJson（rlbase，单测逐条钉它逐条来自生成表）。
            response = { fields = {
                { name = "ok", kind = "boolean", note = "恒 true；失败走统一错误体" },
                { name = "frame", kind = "uint32", note = "装配态帧计数（纯观测，客户端可拿它核对读数时刻）" },
                { name = "routes", kind = "string[]",
                  note = "元素 = 方法 + 空格 + 路径（如 GET /state），顺序 = 注册顺序 = 本文件记录顺序" },
            } },
        },
        {
            id = "state", method = "GET", path = "/state", timeout_ms = 2000,
            summary = "相机姿态 + 全部参数页 + 渲染/图/能力段快照（label 发现面）",
            -- 回包本体 = rpcHandleState。三个子结构都放 shapes：camera 与 /camera 共用，
            -- render/graph 单端点用但也进 shapes（md 因此每个结构只渲染一次）。
            response = { fields = {
                { name = "ok", kind = "boolean", note = "恒 true" },
                { name = "frame", kind = "uint32", note = "装配态帧计数" },
                { name = "camera", kind = "object", shape = "camera", note = "当前相机姿态" },
                { name = "pages", kind = "object[]", shape = "statePage",
                  note = "每个参数页一条（label 发现面，/param 按 label 寻址）" },
                { name = "render", kind = "object", shape = "stateRender",
                  note = "窗口/渲染尺寸与生效倍率（像素类判据的口径记录）" },
                { name = "graph", kind = "object", shape = "stateGraph",
                  note = "渲染图快照（开关是否真的生效的唯一观测面：无选中时 highlight 零 draw，画面本来就不该变）" },
                { name = "capabilities", kind = "object", shape = "stateCapabilities",
                  note = "渲染能力快照（docs/能力观测_plan.md 批 1）：内容一律从编译产物现算 = 「这一帧实际在画什么」；与 graph 段是两个口径（那边读声明，来源问题见 stateGraph.note 与计划 §1.1）" },
                { name = "fsr", kind = "object", shape = "stateFsr",
                  note = "FSR 生效快照（能力观测批 2）：FSR 是图外 compute、不在编译图里 ⟹ 它的判据吃不到 capabilities 段，单给一段。申报侧（gateOpen/temporalConfig）与生效侧（usable/temporalEffective/尺寸/jitter）分列，「申报了没生效」的差就是本段的读数对象" },
            } },
        },
        {
            id = "camera", method = "POST", path = "/camera", timeout_ms = 2000,
            summary = "写相机姿态（任意子集，缺字段不改；角度一律度）",
            -- 顺序 = 处理函数 rpcHandleCamera 的套用顺序（position 必须先于 target：fps 下的
            -- 朝向反推要用到新 position）。yaw_at_frame 的**状态**判据（frame_yaw 为 0 无从钉、
            -- 锚点已过 = 409）不在这里——那是运行期快照，不是请求形状。
            request = { fields = {
                { name = "mode", kind = "string", oneOf = { "trackball", "fps" },
                  error = "mode must be \"trackball\" or \"fps\"" },
                { name = "position", kind = "vec3", error = "position must be [x, y, z]" },
                { name = "target", kind = "vec3", error = "target must be [x, y, z]" },
                { name = "up", kind = "vec3", error = "up must be [x, y, z]" },
                { name = "yawDegrees", kind = "number", error = "yawDegrees must be a number" },
                { name = "pitchDegrees", kind = "number", error = "pitchDegrees must be a number" },
                { name = "fovDegrees", kind = "number", error = "fovDegrees must be a number" },
                { name = "yaw_at_frame", kind = "integer", min = 1, max = 4294967295,
                  error = "'yaw_at_frame' must be an integer in [1, 2^32)" },
            } },
            -- 回包本体 = rpcHandleCamera（成功路径）。
            response = { fields = {
                { name = "ok", kind = "boolean", note = "恒 true" },
                { name = "camera", kind = "object", shape = "camera",
                  note = "本请求套用后的姿态回显（不是应答到达时的姿态，引擎还在跑帧）" },
                { name = "yawPinnedFrame", kind = "uint32", optional = true,
                  absentWhen = "请求未带 yaw_at_frame（带了但校验失败是 400/409，没有成功回包）",
                  note = "回显自证：客户端核对锚点帧号（没回显 = 没钉）" },
            } },
        },
        {
            id = "param", method = "POST", path = "/param", timeout_ms = 2000,
            summary = "按 label 写一个面板参数（page/material 可选寻址）",
            -- `value` 的**类型**不在这里查：它随目标条目的 kind 而异（slider/int = 数值，
            -- color/drag3 = 三元数组），判据本体在 rpcWriteParamPage（它才知道 label 是什么档）。
            -- 顺序 = 处理函数 rpcHandleParam 的读取顺序（label → material → value → page）。
            request = { fields = {
                { name = "label", kind = "string", required = true,
                  error = "'label' must be a string" },
                { name = "material", kind = "integer", error = "'material' must be an integer" },
                { name = "value", required = true, error = "'value' is required" },
                { name = "page", kind = "string", error = "'page' must be a string" },
            } },
            -- 回包本体 = rpcWriteParamPage 的成功分支（label 未知 / page 未知是 404，
            -- value 类型不符是 400，都没有成功回包）。
            response = { fields = {
                { name = "ok", kind = "boolean", note = "恒 true" },
                { name = "label", kind = "string", note = "回显请求" },
                { name = "value", kind = "json",
                  note = "回显请求的 value；类型随目标条目 kind（slider/int = 数值，color/drag3 = 三元数组）" },
            } },
        },
        {
            id = "shot", method = "POST", path = "/shot", timeout_ms = 10000,
            summary = "帧捕获写盘（帧首收单、本帧回读、下帧结算，故给 10s）",
            -- attachment 与 gbuf 都是"回读哪个源"，同时给必是客户端 bug，静默取一个会拍出
            -- 用户没要的那张图。本端点的 attachment **没有**取值白名单（任意图内附件名都收，
            -- 消费方 = imgqa 的 object_id dump）——与 /shotburst 同名字段不同形状，
            -- 这正是"字段按端点声明、不按名字全局声明"的理由。
            request = { fields = {
                { name = "file", kind = "string", error = "'file' must be a string" },
                { name = "gbuf", kind = "integer", error = "'gbuf' must be an integer" },
                { name = "attachment", kind = "string", exclusive = "gbuf", nonEmpty = true,
                  error = "'attachment' must be a string",
                  exclusiveError = "'attachment' and 'gbuf' are mutually exclusive",
                  emptyError = "'attachment' must be a non-empty name" },
            } },
            -- 回包本体 = rpcSettleShot 的成功分支（跨帧兑现；409 抢槽 / 400 回读源不可用 /
            -- 500 写盘失败都没有成功回包）。
            response = { fields = {
                { name = "ok", kind = "boolean", note = "恒 true" },
                { name = "file", kind = "string",
                  note = "已写盘 PNG 的绝对路径（writeCapturePng 的返回值；gbuf 通道的名字由写盘方定为 screenshots/gbuffer_<通道>.png，不采用请求里的 file）" },
            } },
        },
        {
            id = "shotburst", method = "POST", path = "/shotburst", timeout_ms = 120000,
            summary = "连续 N 帧采集（整批交齐才应答，N 上限 1024 帧 + 逐张写盘，故给 120s）",
            -- 这六条文案是**行为契约**：tests/test_burst_protocol.cppm 逐条钉住"九条反向输入
            -- 收敛到这五条、一条不多一条不少"，迁移前后逐字不变。检查顺序 = 拆分前
            -- parseBurstRequest 的顺序（n → dir → prefix → attachment → gbuf → start_frame）。
            request = { fields = {
                { name = "n", kind = "integer", required = true, min = 1, max = 1024,
                  error = "'n' must be an integer in [1, 1024]" },
                { name = "dir", kind = "string", required = true, nonEmpty = true,
                  error = "'dir' must be a non-empty path" },
                { name = "prefix", kind = "string", error = "'prefix' must be a string" },
                { name = "attachment", kind = "string", exclusive = "gbuf", nonEmpty = true,
                  oneOf = { "swapchain", "taa_out" },
                  error = "'attachment' must be a string",
                  exclusiveError = "'attachment' and 'gbuf' are mutually exclusive",
                  emptyError = "'attachment' must be a non-empty name",
                  oneOfError = "'attachment' only supports swapchain | taa_out" },
                { name = "gbuf", kind = "integer", error = "'gbuf' must be an integer" },
                { name = "start_frame", kind = "integer", min = 1, max = 4294967295,
                  error = "'start_frame' must be an integer in [1, 2^32)" },
            } },
            -- 回包本体 = rpcSettleBurst 的交齐分支（整批交齐才兑现；锚已过 409 /
            -- 回读源不可用 400 / 写盘失败 500 都没有成功回包）。
            response = { fields = {
                { name = "ok", kind = "boolean", note = "恒 true（整批交齐才有一条应答）" },
                { name = "count", kind = "uint32", note = "采集帧数 = 请求的 n" },
                { name = "startFrame", kind = "uint32",
                  note = "首张实际落帧（回显自证：带 start_frame 的调用侧核对它等于锚点；锚帧被 acquire 跳帧时这里才是真相）" },
                { name = "files", kind = "string[]", note = "绝对路径，逐帧一条（顺序 = 帧序）" },
            } },
        },
        {
            id = "resize", method = "POST", path = "/resize", timeout_ms = 2000,
            summary = "headless 改交换链尺寸（冒烟用；窗口模式出声拒）",
            -- 两个字段共用一条文案（拆分前就是 `width == 0 || height == 0` 一条判断）：
            -- 报"哪个字段坏了"要先把两条判断拆开，那是为了文案好看而改控制流，不值。
            request = { fields = {
                { name = "width", kind = "integer", required = true, min = 1,
                  error = "resize: width/height must be > 0" },
                { name = "height", kind = "integer", required = true, min = 1,
                  error = "resize: width/height must be > 0" },
            } },
            -- 回包本体 = rpcHandleResize（窗口模式直接拒绝 = 400，没有成功回包）。
            response = { fields = {
                { name = "ok", kind = "boolean", note = "恒 true" },
                { name = "width", kind = "uint32", note = "回显请求（真正的重建由下一帧 resizeSystem 完成）" },
                { name = "height", kind = "uint32", note = "回显请求" },
                { name = "frame", kind = "uint32", note = "收单时刻的帧计数" },
            } },
        },
        {
            id = "pass", method = "POST", path = "/pass", timeout_ms = 2000,
            summary = "pass 级动态开关（复合事务的安全点入口；toggle_frames = 逐帧翻转）",
            -- 🔴 止损线（本文件头部"止损线"段）：`on` 是**条件性必填**——只在没给
            -- toggle_frames 时才要。现有约束语言只表达无条件必填/类型/范围/白名单/互斥，
            -- 为这一条端点去加"requiredUnless"就是把字段约束语言往 OpenAPI 方向做（计划
            -- 风险 2 原话）。⟹ 本端点的三条形状检查（name 必填、toggle_frames > 0、
            -- on 与 toggle_frames 二选一）**留在处理函数**，字段名仍吃生成常量、未知字段
            -- 闸门照跑。残余面：schema 与那三条手写检查之间无机器对账。
            manualShape = true,
            manualReason = "on 是条件性必填（只在未给 toggle_frames 时才要），条件性必填不在约束语言里",
            request = { fields = {
                { name = "name" },
                { name = "toggle_frames" },
                { name = "on" },
            } },
            -- 回包本体 = rpcHandlePass 的两个分支（on 形态与 toggle_frames 形态回**不同**的
            -- 键集，条件性逐条标在 absentWhen；name 缺失 / 二选一都没给 = 400，翻转已在跑 = 409）。
            response = { fields = {
                { name = "ok", kind = "boolean", note = "恒 true" },
                { name = "name", kind = "string", note = "回显 pass 名" },
                { name = "frame", kind = "uint32", note = "收单时刻的帧计数（生效在之后的安全点，要读生效后的值再发一次 /state）" },
                { name = "on", kind = "boolean", optional = true,
                  absentWhen = "toggle_frames 形态（那条分支不回 on）", note = "回显请求的目标状态（on 形态）" },
                { name = "toggleFrames", kind = "uint32", optional = true,
                  absentWhen = "on 形态（只有 toggle_frames 形态回）", note = "回显请求的翻转帧数" },
                { name = "startFrame", kind = "uint32", optional = true,
                  absentWhen = "on 形态（只有 toggle_frames 形态回）", note = "第一个翻转帧 = 收单帧 + 1" },
                { name = "passes", kind = "uint32", optional = true,
                  absentWhen = "toggle_frames 形态（只有 on 形态回）", note = "收单时刻的 pass 数（不是生效后的读数）" },
                { name = "recompiles", kind = "uint32", optional = true,
                  absentWhen = "toggle_frames 形态（只有 on 形态回）", note = "收单时刻的累计重编译次数" },
            } },
        },
        {
            id = "quit", method = "POST", path = "/quit", timeout_ms = 2000,
            summary = "置 AppExit = 下个帧边界干净退出（自动化测试收尾用）",
            -- 无 request 段 = **一个字段都不收**：POST 带任何键都算未知字段 → 400。
            -- 调用方发的都是空对象（`{}` 或无 body），不受影响。
            response = { fields = {
                { name = "ok", kind = "boolean", note = "恒 true（应答先兑现再置位，本帧 HTTP 线程仍拿得到 200）" },
                { name = "frame", kind = "uint32", note = "收单时刻的帧计数（进程在**下个**帧边界退出）" },
            } },
        },
    },

    -- ── 共享响应子结构（批 3）─────────────────────────────────────────────────────────
    -- 被 response.fields 的 shape 键引用；md 参考把每个结构只渲染一次。
    -- 字段清单出处 = 拼装该结构的函数本体（camera → rpcCameraJson，
    -- statePage/stateRender/stateGraph → rpcHandleState + rpcEntriesJson）。
    shapes = {
        {
            name = "camera",
            note = "相机姿态快照（rpcCameraJson）。角度一律度（同 camera.lua 的 fovDegrees 语义）。生效字段随 mode 而异：trackball 看 position/target/up，fps 看 position/yaw/pitch，另一组照样回显（fps 下 target 经 cameraAimAt 与 yaw/pitch 保持一致）",
            fields = {
                { name = "mode", kind = "string", note = "trackball | fps" },
                { name = "position", kind = "vec3" },
                { name = "target", kind = "vec3" },
                { name = "up", kind = "vec3" },
                { name = "yawDegrees", kind = "number" },
                { name = "pitchDegrees", kind = "number" },
                { name = "fovDegrees", kind = "number" },
            },
        },
        {
            name = "stateRender",
            note = "像素口径（批 14.3：像素类判据读的是后缓冲截图，renderScale ≠ 1 时画面是内部网格放大来的；三个值都给，工具不必猜换算）",
            fields = {
                { name = "windowWidth", kind = "uint32", note = "窗口物理像素" },
                { name = "windowHeight", kind = "uint32", note = "窗口物理像素" },
                { name = "renderWidth", kind = "uint32", note = "渲染网格像素（= swapchain 尺寸 × scale）" },
                { name = "renderHeight", kind = "uint32", note = "渲染网格像素" },
                { name = "scale", kind = "number", note = "生效的 render_scale" },
            },
        },
        {
            name = "stateGraph",
            note = "渲染图快照（Renderer::graphStatus，现取）",
            fields = {
                { name = "passes", kind = "uint32", note = "当前 desc 里的 pass 数（插拔的直接读数）" },
                { name = "presentPasses", kind = "string[]", note = "当前在场的 pass 名（顺序 = 执行序）" },
                { name = "dynamicPasses", kind = "string[]", note = "在册可开关的 pass 名（含已撤回的；/pass 的 name 从这里取）" },
                { name = "recompiles", kind = "uint32", note = "累计重编译次数（出厂档判据 = 0）" },
                { name = "lastRecompileUs", kind = "number", note = "最近一次重编译耗时（微秒；0 = 还没重编译过）" },
            },
        },
        {
            name = "stateCapabilities",
            note = "渲染能力快照（rpcCapabilitiesJson 拼装，能力观测_plan 批 1）。内容一律从编译产物现算 = 「这一帧实际在画什么」；与 stateGraph 是两个口径（那边读声明 desc()，本段读编译产物，两者只在脏窗口分歧）",
            fields = {
                { name = "frame", kind = "uint32", note = "本快照属于哪一帧（运行期图可被 /pass 热插拔，没有帧号无法复现；= 回包顶层 frame，同一次快照）" },
                { name = "dirty", kind = "boolean", note = "true = 声明侧有变更尚未落图（申报后、重编译前，或重编译失败正逐帧重试）——此时 attachments/passes 是执行器当前编译图的读数，声明侧已是另一份" },
                { name = "attachments", kind = "object[]", shape = "capabilityAttachment", note = "编译图附件逐条（顺序 = slot 下标）" },
                { name = "passes", kind = "object[]", shape = "capabilityPass", note = "编译图 pass 逐条（顺序 = 执行序）" },
            },
        },
        {
            name = "stateFsr",
            note = "FSR 生效快照（rpcFsrJson 拼装，能力观测_plan 批 2）。FSR = 图外 compute，编译图里没有它的事实（capabilities 段按 §3.1 只装图事实），故单列一段。gateOpen/temporalConfig = 申报侧（SceneEnv，装配期回填的配置原值）；usable/temporalEffective/render*/upscale*/jitter* = 生效侧（Renderer/FsrUpscaler/SceneEnv 现状）。⚠ 「velocity 附件在图」推不出「FSR 接起来了」——设备轴 shader16 降级档就是反例，两侧必须分开报",
            fields = {
                { name = "gateOpen", kind = "boolean", note = "runtime.lua aa == \"fsr\" 门开（配置事实；与有没有真的接起来无关）" },
                { name = "usable", kind = "boolean", note = "FSR context 真的构造并接进图了（= gateOpen ∧ velocity 在图 ∧ 设备 shader16 ∧ 输入附件齐备；gateOpen ∧ ¬usable = 设备轴降级，原因见启动日志「aa=\"fsr\" 开门但接不起来」行）" },
                { name = "renderWidth", kind = "uint32", optional = true,
                  absentWhen = "usable = false（context 未构造，没有「vendor 实收尺寸」这个事实可报；报 0 会把「没构造」与「构造在 0 宽上」塌成同一个值）",
                  note = "FsrUpscaler 构造/重建时实收的渲染宽（像素；与 render 段 renderWidth 相等是「接线没错」的交叉判据，两条读数来自不同代码路径）" },
                { name = "renderHeight", kind = "uint32", optional = true,
                  absentWhen = "usable = false（同 renderWidth）",
                  note = "vendor 实收渲染高" },
                { name = "upscaleWidth", kind = "uint32", optional = true,
                  absentWhen = "usable = false（同 renderWidth）",
                  note = "vendor 实收上屏宽（= 构造入参 swapchain extent）" },
                { name = "upscaleHeight", kind = "uint32", optional = true,
                  absentWhen = "usable = false（同 renderWidth）",
                  note = "vendor 实收上屏高" },
                { name = "temporalEffective", kind = "boolean", note = "vendor 时域累积历史真的在跑（= usable ∧ 互斥裁决通过；¬usable 恒 false——FSR 没跑就没有时域）" },
                { name = "temporalConfig", kind = "boolean", note = "配置键 fsr.temporal 原值（申报侧；与 temporalEffective 的差 = 「静默换臂」的窗口，判据读生效侧）" },
                { name = "jitterWired", kind = "boolean", note = "Halton 亚像素抖动接进了投影并申报给 FSR（= usable ∧ 配置 fsr.jitter，wireFsrJitter 的裁决结果）" },
                { name = "jitterPhase", kind = "uint32", note = "抖动序列周期（相位计数；0 = 未接。口径 = fsrJitterPhaseCount(渲染宽, 上屏宽)，与 vendor 内部自算的 JitterSequenceLength 必须同值）" },
            },
        },
        {
            name = "capabilityAttachment",
            note = "能力段的一条附件（来源 = CompiledRenderGraph::attachments；名单唯一来自图，rtBuilt 只是逐个问缓存，缓存不反向提供名单）",
            fields = {
                { name = "name", kind = "string", note = "附件名（= RT 缓存的键；/shot 的 attachment 按它寻址）" },
                { name = "kind", kind = "string", note = "swapchain | depth | offscreen | depthOffscreen | buffer（拼写 = RGL::kindName(RgAttachmentKind) 枚举名，不是 Lua 配置词表）" },
                { name = "format", kind = "string", note = "声明拼写原样：『inherit』（随 swapchain/depth 现有格式）或具名格式词（rgba16_sfloat 等）；解析后的 vk::Format 是另一件事，本段不报" },
                { name = "width", kind = "uint32", note = "宽（像素）：图属 RT = resolvedRtExtent（独立 extent 优先，跟随档 = 渲染网格 × extentScale，与执行器同一函数）；swapchain/depth = 渲染网格；buffer 档无图像尺寸恒 0" },
                { name = "height", kind = "uint32", note = "高（像素），口径同 width" },
                { name = "rtBuilt", kind = "boolean", optional = true,
                  absentWhen = "kind ∈ swapchain | depth | buffer（这三档不占 RT 缓存，没有『建没建』可问）",
                  note = "这张 RT 今天实际建了没有 = RenderTargetCache 活动表在不在（稳态图内图属 RT 每帧 ensure，应 true；false = 首帧前或退役窗口）" },
            },
        },
        {
            name = "capabilityPass",
            note = "能力段的一条 pass（来源 = CompiledRenderGraph::passes，不是 desc()——两来源的分歧只有脏窗口可观测，机器判据 = tests/test_rpc_capabilities.cppm 的脏窗口用例）",
            fields = {
                { name = "name", kind = "string", note = "pass 名" },
                { name = "kind", kind = "string", note = "drawMesh | blit | drawLight（拼写 = kindName(RgPassKind) 枚举名；Lua 配置表里 drawMesh 档拼 draw，那是配置别名，不是本表）" },
                { name = "state", kind = "string", note = "动态状态名（同时是 ShaderRegistry 固定条目名）；blit 档恒空串（blit 无动态状态）" },
                { name = "order", kind = "uint32", note = "执行序 = 编译产物下标（骨架版编译器不重排、不消除死 pass）" },
            },
        },
        {
            name = "statePage",
            note = "一个参数页。粗粒度登记（docs/rpc_单源_plan.md §3.1 边界②）：entries 的细目归 param_semantics 生成管线与面板控件，本表不抄 label 词表",
            fields = {
                { name = "name", kind = "string", note = "页名（/param 的 page 字段的寻址值）" },
                { name = "entries", kind = "json",
                  note = "条目数组，元素形状随 kind ∈ slider | int | color | drag3 | text 而异（text 无 label；min/max/speed 随 kind 出现与否）；value 经条目 get 回调现读源头 = 当前真实值（判据本体 = rpcEntriesJson）" },
            },
        },
    },
}
