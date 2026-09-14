-- 相机与控制配置（启动时加载，字段均可选，缺省回落默认值）
return {
    mode = "fps",  -- "trackball"（轨道环绕）或 "fps"（第一人称飞行）

    -- 机位约定：trackball 绕 target 公转，target = 模型包围球中心，position 距球心 ~1.4·(r/tan(fov/2))
    -- 整球入画留裕量。near/far 有 SceneBounds 时由 projectionSystem 自适应（忽略）。
    -- position = { 128.0, 73.0, 150.0 },  -- castle_of_loarre 机位：包围球 center=(-3.4,0.6,17.7) r=59.7，3/4 视角距 ~200
    -- target = { -3.4, 0.6, 17.7 },  -- 轨道中心（仅 trackball 用）
    -- position = { 380.0, 320.0, 380.0 },  -- kyoto_fushimi_inari_taisha_temple 机位（包围球 center≈(0,0,0) r=73.3）
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
