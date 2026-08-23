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
}
