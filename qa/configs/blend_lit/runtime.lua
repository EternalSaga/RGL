return {
    headless = true,
    headless_size = { 1280, 720 },
    win_size = { 800, 600 },
    show_after = -1,
    present_mode = "immediate",
    ui_font = "",
    ui_font_size = 18.0,

    -- 抗锯齿**关**。两条理由，第二条是承重的：
    -- ① TAA/FSR 会把 pane 边界的像素与邻域混合，区域归因（"这块像素属于哪个 pane"）
    --    在边界上失去一一对应；
    -- ② 更关键：透明段**不写深度**（docs/透明混合管线_plan.md §4），velocity 因此不可信，
    --    开 TAA 等于给"双臂像素差"这个读数叠一个跨帧历史项 —— 重放时差值会随历史长度漂。
    -- aa="off" 让交付图 = 当帧光栅结果，一次启动一个确定读数。
    aa = "off",

    -- 湿面 G-buffer 调制**钉死关**。调制只住在 deferred_geometry.frag.slang，本靶的
    -- backdrop 与 OPAQUE 臂的三支 pane 都走那条路；strength=0 = 恒等旁路（逐位 no-op）。
    -- 对本靶它是**双保险**（两臂同乘，不会造出假的"双臂不同"），但别删：不钉则
    -- albedo 压暗/粗糙度端点改写材质观测值，"背景板竖条"的读数跨重放就不稳。
    wet = {
        strength = 0.0,
    },

    -- ── 自截自退 + RPC 读条数：两个门**同时开**，这是本靶读数的形态 ──────────────
    -- llm_debug=true ⟹ 引擎在第 delay_frames 帧自己截图、然后退出；imgqa 侧不需要发
    -- /shot（llm_debug 开着时 /shot 会被 409 拒，probe.cpp 为此专门把它改写成 false）。
    -- 本靶**不**改写：截图就要走自截，而透明段条数走 RPC GET /state 的 render.transparentDraws。
    -- 两者的时序关系是实测出来的（不是推的）：RPC 服务在进程起后约 2.1 s 绑上端口，
    -- 请求只在帧边界被排空 ⟹ /state 的读数落在第 2…delay_frames 帧；delay_frames=8 时
    -- 「绑上端口 → 退出」的窗口约 20 ms，实测 8/8 次各读到 3–6 条（帧号 2…9），0 次空手。
    -- ⚠ 消费侧必须要求 frame ≥ 2：第 1 帧的读数是装配期默认的空表（见 rpc_schema.lua 注）。
    llm_debug = true,
    llm_shot = {
        file = "screenshots/blend_lit.png",
        size = { 1280, 720 },
        -- 固定帧号 = 读数可重放。**为什么这里是 2000 而 blend_sort 是 8**：本靶多一路 RPC
        -- 读数，而「RPC 绑上端口 → 进程退出」的窗口宽度 ≈ delay_frames / 帧率。本机 headless
        -- 稳态约 2500 fps，实测窗口：delay_frames=8 → ~20 ms（8/8 次各读到 3–6 条，0 次空手，
        -- 但余量只有一档机器速度）；=2000 → ~0.8 s（同机实测读到 30–52 条）。
        -- 调小省的是 0.8 秒墙钟，赔的是「换台慢机器就一条也读不到」——不划算。
        -- 像素侧无代价：spin=false + aa=off ⟹ 第 8 帧与第 2000 帧逐位相同（同配置两跑
        -- 逐位对账已实测通过，读数见 tools/imgqa/blend_lit.cpp 头注的实测表）。
        delay_frames = 2000,
    },
}
