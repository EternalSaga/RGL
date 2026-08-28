-- 相机与控制配置（启动时加载，字段均可选，缺省回落默认值）
return {
    mode = "trackball",  -- "trackball"（轨道环绕）或 "fps"（第一人称飞行）

    -- Phase 3 测试机位：cottage 包围球 center=(-182,546,115) r=1352，
    -- 3/4 前右上视角、距 ~3700（整栋入画）。near/far 有 SceneBounds 时由 projectionSystem 自适应（忽略）。
    -- position = { -2617.0, 1343.0, 2550.0 },
    -- target = { -182.0, 300.0, 115.0 },  -- 轨道中心（仅 trackball 用）
    position = { 70.0, 85.0, 65.0 },  -- 草模型机位：包围球 center=(10,24,-9) r=81.5，球外 ~110
    target = { 10.0, 24.0, -9.0 },  -- 草模型包围球中心
    fovDegrees = 45.0,
    near = 0.1,
    far = 8000.0,

    trackball = {
        sensitivity = 0.005,  -- rad/px
        zoom = 0.03,          -- 每格滚轮的轨道半径变化
    },
    fps = {
        speed = 3.0,          -- WASD 移动速度，单位/秒
        sensitivity = 0.003,  -- rad/px
    },

    keys = {                  -- 键位绑定（可选，缺省 WASD；单字母/数字或 Up/Down/Left/Right/Space/Enter）
        forward = "W",
        back = "S",
        left = "A",
        right = "D",
    },

    -- llm_debug 一次性截图（agent 自主工作：离屏隐藏窗渲染 → 截图 → 退出，默认关）
    -- true = 默认（1280x720，首帧即截）；table = 细字段（file/size/delay_frames）
    -- 相机位置无额外字段：直接编辑上面的顶层 position/target/fovDegrees
    -- Phase 3 纹理验证：一次性截图（NVIDIA/RADV 跨驱动比对用）；交互运行改回 false
    -- 4K 黑屏排查（2026-08-27）：spectacle 截屏已抓到 GPUVM fault（TCP 权限 fault），
    -- 恢复 llm_debug 跑矩阵（fault 日志 = 判据）
    llm_debug = false,

    -- {
    --    file = "screenshots/phase3_texture.png",
    --    size = { 2048, 2048 },  -- 4K 黑屏排查：纹理(4096) > 目标(2048) 是否仍坏
    --},
}
