return {
    headless = true,
    headless_size = { 1280, 720 },
    win_size = { 800, 600 },
    show_after = -1,
    present_mode = "immediate",
    ui_font = "",
    ui_font_size = 18.0,
    -- 批 15.2B：湿面 G-buffer 调制**钉死关**。本靶的 backdrop 走 deferred_geometry 写 G-buffer，而调制就住在这条路上（闭式混合式的
    -- dst 因此必须是已知量）。闭式解不认识湿面调制 ⟹ 不钉则 albedo 压暗/粗糙度端点改写
    -- 材质观测值，四个判据区一起失准。strength=0 = 恒等旁路（逐位 no-op）。
    -- ⚠ 2026-10-18 本靶切 deferred 后这道门从「双保险」变成**承重**：湿面调制只住在
    -- deferred_geometry.frag.slang（已删除的 forward 路根本不经过该文件），k=0 走恒等旁路分支。
    wet = {
        strength = 0.0,
    },
    llm_debug = true,
    llm_shot = {
        file = "screenshots/blend_sort.png",
        size = { 1280, 720 },
        delay_frames = 8,
    },
}
