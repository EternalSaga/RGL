-- 相机与控制配置（启动时加载，字段均可选，缺省回落默认值）
return {
    mode = "fps",  -- "trackball"（轨道环绕）或 "fps"（第一人称飞行）

    -- 机位约定：trackball 绕 target 公转，target = 模型包围球中心，position 距球心 ~1.4·(r/tan(fov/2))
    -- 整球入画留裕量。near/far 有 SceneBounds 时由 projectionSystem 自适应（忽略）。
    -- position = { 128.0, 73.0, 150.0 },  -- castle_of_loarre 机位：包围球 center=(-3.4,0.6,17.7) r=59.7，3/4 视角距 ~200
    -- target = { -3.4, 0.6, 17.7 },  -- 轨道中心（仅 trackball 用）
    -- position = { 380.0, 320.0, 380.0 },  -- kyoto_fushimi_inari_taisha_temple 机位（包围球 center≈(0,0,0) r=73.3）
    -- ── SSR 参考机位（2026-10-10 用户提供，街道平视）── 看 SSR 效果**用这个，别用下面那个
    -- 交付机位**：交付机位大半画面是环境贴图照片，湿石板路只占 1.48%；本机位湿材质占 22.4%。
    -- ⚠ 启用时要**同时把上面 mode 改成 "trackball"**——`target` 仅 trackball 消费（fps 模式
    --   朝向走 yaw/pitch 状态，写 target 无效 = 静默到不了这个视角）。
    -- position = { -0.075051, 0.060435, -0.021944 },  -- SSR 参考机位（fov 45 不变）
    -- target = { -0.078430, -0.013424, 0.192736 },  -- ↑ 配套 target（启用时两行一起放开）
    -- 生效机位仍是交付机位：换它 = 改默认配置截图 = **红线 A 需重立**，故此处只留注释。
    position = { 1.1, 0.75, 1.05 },  -- Phase 7 D1 Bistro：贴近建筑簇检查拼装/贴图（世界 r≈1.17）
    target = { 0.14, 0.12, 0.02 },  -- 轨道中心 = Bistro 世界包围球心
    -- position = { -2617.0, 1343.0, 2550.0 },  -- cottage 机位（4K 黑屏 UAF 复验，2026-08-28；包围球 center=(-182,546,115) r=1352）
    -- target = { -182.0, 300.0, 115.0 },  -- 轨道中心（仅 trackball 用）
    -- position = { 8.0, 6.0, 10.0 },  -- Phase 5 透明排序：transparent.glb 4 盒（原点 ±6 内）
    -- position = { 1.69, 2.5, -19.46 },  -- 排序验证机位：绿盒(远,α0.25)与黄盒(近,α0.25)视线共线重叠
    -- target = { 0.0, 0.0, 0.0 },  -- 轨道中心（仅 trackball 用）
    -- position = { 70.0, 85.0, 65.0 },  -- 草模型机位：包围球 center=(10,24,-9) r=81.5，球外 ~110
    -- target = { 10.0, 24.0, -9.0 },  -- 草模型包围球中心
    fovDegrees = 45.0,
    near = 0.1,
    far = 8000.0,

    -- 帧驱动 yaw（诊断激励门，默认关 = 逐位今天）：frame_yaw ≠ 0（rad/帧）时
    -- yaw = 基准 + frame_yaw × 帧号（**纯函数**，与帧时长无关 ⟹ 跨运行/跨进程可复现的
    -- 相机运动序列，imgqa temporal-motion 的候选与超采样参考才能逐帧对齐；
    -- dt 累积随帧时长漂 = 不可复现）。仅 fps 模式生效；基准被外部写入（RPC /camera）
    -- 时自动重定基（跨进程只差常数帧偏移，判据的 argmin 对齐吸收）。
    -- frame_yaw = 0.00349066,  -- 0.2°/帧 ≈ 门框机位 3–5px/帧
    -- 🔴 本键从"注释掉的示例"改成**真键 + 默认 0.0**（债-140 动机位批）：`smoke.py --set` 是
    --    **替换**语义（要求键在文件里恰好出现 1 次），键只存在于注释里 ⟹ 注入直接 FAIL。
    --    0.0 = 关 = 与"没有这个键"逐位同（代码侧缺字段本就回落 0，见 camera_config_impl），
    --    所以这不是新增行为，只是把已有的诊断门**接上注入面**。
    frame_yaw = 0.0,

    trackball = {
        sensitivity = 0.005,  -- rad/px
        zoom = 0.03,          -- 每格滚轮的轨道半径相对缩放（半径 ×(1∓zoom)，2026-08-28 改：加法式在远机位失效）
    },
    fps = {
        speed = 0.4,          -- WASD 移动速度，单位/秒
        sensitivity = 0.003,  -- rad/px
    },

    keys = {                  -- 键位绑定（可选，缺省 WASD；单字母/数字或 Up/Down/Left/Right/Space/Enter）
        forward = "W",
        back = "S",
        left = "A",
        right = "D",
    },

    -- 2026-09-08：llm_debug/llm_shot 截图段已迁至 configs/runtime.lua（截图属运行模式，
    -- 与相机语义无关）。本文件回归纯相机姿态/输入。相机位置仍编辑顶层 position/target/fovDegrees。
}
