-- 相机与控制配置（启动时加载，字段均可选，缺省回落默认值）
return {
    mode = "trackball",  -- "trackball"（轨道环绕）或 "fps"（第一人称飞行）

    position = { 0.0, 0.0, 3.0 },
    target = { 0.0, 0.0, 0.0 },  -- 轨道中心（仅 trackball 用）
    fovDegrees = 45.0,
    near = 0.1,
    far = 10.0,

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
    -- true = 默认（1280x720，首帧即截）；table = 细字段：
    -- llm_debug = {
    --     delay_frames = 0,                -- 第 N 帧渲染完成后截图（0 = 首帧；动态场景预留）
    --     file = "screenshots/shot.png",   -- 缺省 screenshots/llm_<时间戳>.png（绝对路径打日志）
    --     size = { 1280, 720 },           -- 离屏窗口尺寸
    -- }
    -- 相机位置无额外字段：直接编辑上面的顶层 position/target/fovDegrees
    llm_debug = false,
}
