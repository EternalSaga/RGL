-- 渲染意图图（Phase 6，configs/render_graph.lua）
-- 声明 attachment/pass 拓扑，C++ 编译为可执行序列（barrier/布局转换自动推导）。
-- 本文件缺失或整段缺省 = 回落默认图（与下方内容等价）。结构规则与字段语义
-- 权威记录：rgl/render_graph.cppm（类型契约）+ render_graph_config.cppm
-- （Lua 格式）；加载 = sol2 沙箱（禁 dofile/require），枚举字符串写错 = 启动报错。
--
-- attachments: kind = "swapchain" | "depth" | "offscreen"（帧首丢弃规则：
--   swapchain/depth 每帧内容按 load 重建；offscreen 持久图跨帧追踪布局）
-- passes:      线性序 = 执行序；kind = "draw"（默认，画 FrameDrawList 段）| "blit"
--   | "drawLight"（固定全屏单 group draw，state 选条目："light"/"tonemap"）
--   state = 动态状态名（"opaque"/"skybox"/"transparent"/"tonemap"），draws = 绘制段名
--   outputs/inputs 条目: usage = colorWrite | depthWrite | sampleRead |
--   transferRead | transferWrite；load = "clear"|"load"，store = "store"|"dontCare"；
--   clear = {r,g,b,a}（颜色）/ clearDepth = f（深度），缺省 = 0.1 灰背景 / 1.0 远平面
-- 编译器校验（读先于写、同 pass 读写冲突、后续使用的写必须 store、swapchain 必须
-- 被写……）失败即启动报错，见 compileRenderGraph。
--
-- variant（Phase 7 路线选路，2026-09-08 替代 RGL_DEFERRED/RGL_OFFSCREEN 环境变量）：
--   "forward"（缺省）= 解析下方 attachments/passes（整段缺省 = 默认图）
--   "deferred"  = 内建 3-pass 延迟图（geometry→G-buffer + light 全屏逐灯→scene_hdr
--                 + tonemap 上屏，exclusive，下方 passes 被忽略；opaque 段管线换
--                 deferred_geometry，main 契约联动）
--   "offscreen" = 内建 offscreen 图变体（三段渲 scene_rt + composite blit 上屏，SDR）
--
-- Phase 8 批次 ①：HDR 累加链——opaque/skybox/transparent 渲入 scene_hdr
--（rgba16_sfloat offscreen RT），tonemap pass（drawLight）ACES 上屏 swapchain。
return {
    variant = "forward",   -- "forward" | "deferred" | "offscreen"
    attachments = {
        { name = "swapchain", kind = "swapchain" },
        { name = "depth", kind = "depth" },
        { name = "scene_hdr", kind = "offscreen", format = "rgba16_sfloat" },
        -- Phase 11 批 11.4：SSAO 遮蔽量（r8_unorm）。ao pass（state="ao"）读 depth
        -- 写本图，tonemap 采样施加（configs/scene.lua 的 ao.strength 控强度，
        -- 0 = 逐位旁路，故加这张图不改变既有输出）。
        { name = "ao", kind = "offscreen", format = "r8_unorm" },
    },
    passes = {
        { name = "opaque", state = "opaque", draws = "opaque",
          outputs = {
              { attachment = "scene_hdr", usage = "colorWrite", load = "clear", store = "store",
                clear = { 0.1, 0.1, 0.1, 1.0 } },
              { attachment = "depth", usage = "depthWrite", load = "clear", store = "store",
                clearDepth = 1.0 },
          } },
        { name = "skybox", state = "skybox", draws = "skybox",
          outputs = {
              { attachment = "scene_hdr", usage = "colorWrite", load = "load", store = "store" },
              { attachment = "depth", usage = "depthWrite", load = "load", store = "store" },
          } },
        { name = "transparent", state = "transparent", draws = "transparent",
          outputs = {
              { attachment = "scene_hdr", usage = "colorWrite", load = "load", store = "store" },
              -- store（勿改回 dontCare）：Phase 11 批 11.4 起 ao pass 以 sampleRead 读 depth，
              -- 编译器有"后续使用的写必须 store"校验，dontCare 会直接编译失败
              { attachment = "depth", usage = "depthWrite", load = "load", store = "store" },
          } },
        { name = "ao", kind = "drawLight", state = "ao",
          outputs = {
              { attachment = "ao", usage = "colorWrite", load = "clear", store = "store",
                clear = { 0, 0, 0, 0 } },
          },
          inputs = {
              { attachment = "depth", usage = "sampleRead" },
          } },
        { name = "tonemap", kind = "drawLight", state = "tonemap",
          outputs = {
              { attachment = "swapchain", usage = "colorWrite", load = "clear", store = "store" },
          },
          inputs = {
              { attachment = "scene_hdr", usage = "sampleRead" },
              { attachment = "ao", usage = "sampleRead" },
          } },
    },
}
